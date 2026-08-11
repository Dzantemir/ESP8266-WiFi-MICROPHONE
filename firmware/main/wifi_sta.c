/* ---- System / SDK includes ---- */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
/* ---- Project includes ---- */
#include "board_config.h"
#include "wifi_sta.h"
#include "svc_port.h"
#include "stream_control.h"
#include "tcp_stream.h"
/* FIX FR-AT #13: see FIXES.md — stream_mode_current_transport() used by
 * wifi_sta_reconfigure() to refuse reconfiguration in RawTX mode.
 * FIX (F3-B #4): config_get_copy() used by wifi_sta_reconfigure() to also
 * check the NVS-persisted transport (handles pending RawTX switch). */
#include "stream_mode.h"
#include "config_mgr.h"

static const char *TAG = "wifi_sta";

#define WIFI_EVT_CONNECTED (1 << 0)
#define WIFI_EVT_GOT_IP (1 << 1)
#define WIFI_EVT_STA_STARTED (1 << 2)
#define WIFI_EVT_RECONNECT_REQ (1 << 3)
#define WIFI_EVT_EXIT (1 << 4)
#define WIFI_EVT_APPLY_CACHED (1 << 5)

#define WIFI_EVT_REINIT_SOCKET (1 << 6)

static EventGroupHandle_t s_wifi_evt = NULL;
static SemaphoreHandle_t s_backoff_mtx = NULL;
static uint32_t s_backoff_ms = WIFI_RECONNECT_BACKOFF_MIN_MS;
static bool s_initialized = false;
static bool s_wifi_hw_initialized = false;
static char s_hostname[32] = {0}; /* saved for tcpip_adapter_set_hostname in STA_START */

static TaskHandle_t s_reconnect_task = NULL;
#define RECONNECT_TASK_STACK 2048
#define RECONNECT_TASK_PRIO 4

static volatile bool s_intentional_disconnect = false;

static bool s_have_cached_ip = false;
static tcpip_adapter_ip_info_t s_cached_ip_info;

extern esp_err_t wifi_set_user_fixed_rate(uint8_t enable_mask, uint8_t rate);
#define FIXED_RATE_MASK_STA 0x01
#define WIFI_RATE_11B 0x03

#define WIFI_TX_POWER_DBM_TO_QDBM(dbm) ((dbm) * 4)

static void wifi_reconnect_task(void *arg);

typedef struct
{
    int8_t tx_power_x;
    volatile esp_err_t config_err;
} tx_ctx_t;

static tx_ctx_t s_ctx = {0};

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT)
    {
        switch (id)
        {

        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "WIFI_EVENT_STA_START - connecting...");

            esp_err_t err = esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
            if (err != ESP_OK)
            {
                ESP_LOGW(TAG, "esp_wifi_set_ps failed: %s", esp_err_to_name(err));
                s_ctx.config_err = err;
            }

            wifi_country_t country_config = {
                .cc = "RU",
                .schan = 1,
                .nchan = 13,
                .policy = WIFI_COUNTRY_POLICY_AUTO,
                .max_tx_power = WIFI_TX_POWER_DBM_TO_QDBM(WIFI_TX_POWER_MAX),
            };

            err = esp_wifi_set_country(&country_config);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "esp_wifi_set_country failed: %s", esp_err_to_name(err));
                s_ctx.config_err = err;
            }

            if (s_ctx.tx_power_x > WIFI_TX_POWER_MAX)
                s_ctx.tx_power_x = WIFI_TX_POWER_MAX;
            err = esp_wifi_set_max_tx_power(WIFI_TX_POWER_DBM_TO_QDBM(s_ctx.tx_power_x));
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "esp_wifi_set_max_tx_power failed: %s", esp_err_to_name(err));
                s_ctx.config_err = err;
            }

            ESP_LOGI(TAG, "TX power set to %u dBm", (unsigned)s_ctx.tx_power_x);

            err = esp_wifi_set_inactive_time(ESP_IF_WIFI_STA, 10);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "esp_wifi_set_inactive_time failed: %s", esp_err_to_name(err));
                s_ctx.config_err = err;
            }

            /* Set DHCP hostname (for consistency, even in raw mode). */
            if (s_hostname[0])
            {
                err = tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, s_hostname);
                if (err != ESP_OK)
                {
                    ESP_LOGW(TAG, "tcpip_adapter_set_hostname failed: %s", esp_err_to_name(err));
                    s_ctx.config_err = err;
                }
                else
                {
                    ESP_LOGI(TAG, "Hostname set to '%s'", s_hostname);
                }
            }

            /* FIX (HIGH #8): see FIXES.md */
            xEventGroupSetBits(s_wifi_evt, WIFI_EVT_RECONNECT_REQ);

            break;

        case WIFI_EVENT_STA_DISCONNECTED:
        {
            wifi_event_sta_disconnected_t *evt = data;
            /* FIX (LOW #21): see FIXES.md */
            if (evt == NULL)
            {
                ESP_LOGW(TAG, "STA_DISCONNECTED event with NULL data");
                break;
            }
            xEventGroupClearBits(s_wifi_evt, WIFI_EVT_CONNECTED | WIFI_EVT_GOT_IP);

            if (streaming_is_active())
            {
                ESP_LOGW(TAG, "STA_DISCONNECTED reason %d - stopping stream + reconnect",
                         evt->reason);
                streaming_request_stop();
            }
            else
            {
                ESP_LOGW(TAG, "STA_DISCONNECTED reason %d - reconnect scheduled",
                         evt->reason);
            }

            if (s_intentional_disconnect)
            {
                s_intentional_disconnect = false;
                ESP_LOGI(TAG, "  (intentional disconnect - no reconnect this cycle)");
                break;
            }

            xEventGroupSetBits(s_wifi_evt, WIFI_EVT_RECONNECT_REQ);
            break;
        }

        case WIFI_EVENT_STA_CONNECTED:

            if (s_have_cached_ip)
            {
                ESP_LOGI(TAG, "STA_CONNECTED - cached IP available, signaling task");
                xEventGroupSetBits(s_wifi_evt, WIFI_EVT_APPLY_CACHED);
            }
            else
            {
                ESP_LOGI(TAG, "STA_CONNECTED - waiting for DHCP (first boot)");
            }

            svc_port_reset_watchdog();
            break;

        case WIFI_EVENT_STA_AUTHMODE_CHANGE:
        {

            wifi_event_sta_authmode_change_t *auth = data;
            if (!auth)
            {
                ESP_LOGW(TAG, "STA_AUTHMODE_CHANGE: event data NULL (old SDK?) - ignoring");
                break;
            }

            ESP_LOGI(TAG, "STA_AUTHMODE_CHANGE: %d -> %d",
                     (int)auth->old_mode, (int)auth->new_mode);

            bool old_encrypted = (auth->old_mode != WIFI_AUTH_OPEN &&
                                  auth->old_mode != WIFI_AUTH_WEP);
            bool new_encrypted = (auth->new_mode != WIFI_AUTH_OPEN &&
                                  auth->new_mode != WIFI_AUTH_WEP);

            if (old_encrypted && !new_encrypted)
            {
                ESP_LOGW(TAG, "  DOWNGRADE to unencrypted auth mode %d - "
                              "stopping stream + forcing reconnect",
                         (int)auth->new_mode);

                if (streaming_is_active())
                    streaming_request_stop();

                xEventGroupClearBits(s_wifi_evt, WIFI_EVT_CONNECTED | WIFI_EVT_GOT_IP);

                /* FIX (HIGH #8, P2-HIGH #1): see FIXES.md */
                s_intentional_disconnect = true;
                xEventGroupSetBits(s_wifi_evt, WIFI_EVT_RECONNECT_REQ);
            }
            else if (auth->old_mode != auth->new_mode)
            {

                ESP_LOGI(TAG, "  encrypted->encrypted transition - stream continues");
            }

            break;
        }

        default:
            break;
        }
    }
    else if (base == IP_EVENT)
    {
        if (id == IP_EVENT_STA_GOT_IP)
        {
            ip_event_got_ip_t *evt = data;
            if (evt == NULL)
            {
                ESP_LOGW(TAG, "GOT_IP: event data NULL - ignoring");
                return;
            }
            ESP_LOGI(TAG, "GOT_IP: " IPSTR, IP2STR(&evt->ip_info.ip));

            /* FIX F-B #2: take a consistent snapshot of the cached IP state
             * under the mutex — previously s_have_cached_ip and
             * s_cached_ip_info were read unlocked and could be torn by a
             * concurrent writer (APPLY_CACHED / deinit). */
            bool have_cached = false;
            tcpip_adapter_ip_info_t cached;
            memset(&cached, 0, sizeof(cached));
            if (s_backoff_mtx && xSemaphoreTake(s_backoff_mtx, portMAX_DELAY) == pdTRUE)
            {
                have_cached = s_have_cached_ip;
                cached = s_cached_ip_info;
                xSemaphoreGive(s_backoff_mtx);
            }

            if ((xEventGroupGetBits(s_wifi_evt) & WIFI_EVT_GOT_IP) && have_cached &&
                evt->ip_info.ip.addr == cached.ip.addr &&
                evt->ip_info.gw.addr == cached.gw.addr &&
                evt->ip_info.netmask.addr == cached.netmask.addr)
            {
                ESP_LOGI(TAG, "GOT_IP duplicate (same IP, already processed) - skipping");
                return;
            }

            if (have_cached &&
                evt->ip_info.ip.addr != cached.ip.addr)
            {
                ESP_LOGW(TAG, "IP changed: cached " IPSTR " -> new " IPSTR " - restarting stream",
                         IP2STR(&cached.ip),
                         IP2STR(&evt->ip_info.ip));

                if (streaming_is_active())
                    streaming_request_stop();
                xEventGroupClearBits(s_wifi_evt, WIFI_EVT_CONNECTED | WIFI_EVT_GOT_IP);
            }

            /* FIX (MEDIUM #30): see FIXES.md — uses the snapshotted cached
             * value for the same race-safety reason as above. */
            bool ip_changed = !have_cached ||
                              (memcmp(&cached, &evt->ip_info,
                                      sizeof(cached)) != 0);

            /* FIX (HIGH #9): see FIXES.md */
            if (s_backoff_mtx && xSemaphoreTake(s_backoff_mtx, portMAX_DELAY) == pdTRUE)
            {
                s_cached_ip_info = evt->ip_info;
                s_have_cached_ip = true;
                s_backoff_ms = WIFI_RECONNECT_BACKOFF_MIN_MS;
                xSemaphoreGive(s_backoff_mtx);
            }

            if (ip_changed)
            {
                xEventGroupSetBits(s_wifi_evt, WIFI_EVT_REINIT_SOCKET);
            }

            xEventGroupSetBits(s_wifi_evt, WIFI_EVT_CONNECTED | WIFI_EVT_GOT_IP);
        }
        else if (id == IP_EVENT_STA_LOST_IP)
        {

            ESP_LOGW(TAG, "IP_EVENT_STA_LOST_IP - IP lost, re-applying cached IP");
            xEventGroupClearBits(s_wifi_evt, WIFI_EVT_CONNECTED | WIFI_EVT_GOT_IP);
            if (streaming_is_active())
                streaming_request_stop();
            xEventGroupSetBits(s_wifi_evt, WIFI_EVT_APPLY_CACHED);
        }
    }
}

static void wifi_reconnect_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Reconnect task started");

    while (s_initialized)
    {
        /* FIX (HIGH #2): see FIXES.md */
        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_evt,
            WIFI_EVT_RECONNECT_REQ | WIFI_EVT_APPLY_CACHED |
                WIFI_EVT_REINIT_SOCKET | WIFI_EVT_EXIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

        if (!s_initialized || (bits & WIFI_EVT_EXIT))
            break;

        if (bits & WIFI_EVT_REINIT_SOCKET)
        {
            xEventGroupClearBits(s_wifi_evt, WIFI_EVT_REINIT_SOCKET);
            svc_port_update_broadcast();
            if (svc_port_is_running())
            {
                esp_err_t rerr = svc_port_reinit_socket();
                if (rerr != ESP_OK)
                    ESP_LOGW(TAG, "svc_port_reinit_socket: %s", esp_err_to_name(rerr));
            }

            esp_err_t terr = tcp_stream_reinit_listener();
            if (terr != ESP_OK)
                ESP_LOGW(TAG, "tcp_stream_reinit_listener: %s", esp_err_to_name(terr));

            /* FIX (CRITICAL #1): see FIXES.md */
        }

        if (bits & WIFI_EVT_APPLY_CACHED)
        {
            xEventGroupClearBits(s_wifi_evt, WIFI_EVT_APPLY_CACHED);
            /* HIGH #9: take a consistent snapshot of the cached IP state. */
            bool have = false;
            tcpip_adapter_ip_info_t cached;
            memset(&cached, 0, sizeof(cached));
            if (s_backoff_mtx && xSemaphoreTake(s_backoff_mtx, portMAX_DELAY) == pdTRUE)
            {
                have = s_have_cached_ip;
                cached = s_cached_ip_info;
                xSemaphoreGive(s_backoff_mtx);
            }

            if (have)
            {
                ESP_LOGI(TAG, "Applying cached IP " IPSTR, IP2STR(&cached.ip));
                tcpip_adapter_dhcpc_stop(TCPIP_ADAPTER_IF_STA);
                esp_err_t ip_err = tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_STA,
                                                             &cached);
                if (ip_err == ESP_OK)
                {
                    svc_port_update_broadcast();
                    if (s_backoff_mtx && xSemaphoreTake(s_backoff_mtx, portMAX_DELAY) == pdTRUE)
                    {
                        s_backoff_ms = WIFI_RECONNECT_BACKOFF_MIN_MS;
                        xSemaphoreGive(s_backoff_mtx);
                    }
                    xEventGroupSetBits(s_wifi_evt, WIFI_EVT_CONNECTED | WIFI_EVT_GOT_IP);
                    ESP_LOGI(TAG, "Cached IP applied - GOT_IP signaled");
                }
                else
                {
                    ESP_LOGW(TAG, "set_ip_info failed: %s - falling back to DHCP",
                             esp_err_to_name(ip_err));
                    /* FIX F-B #1: DHCP was stopped above (line ~324); restart
                     * it so the interface can re-acquire an address instead
                     * of being left with no IP at all. */
                    esp_err_t dh_err = tcpip_adapter_dhcpc_start(TCPIP_ADAPTER_IF_STA);
                    if (dh_err != ESP_OK && dh_err != ESP_ERR_TCPIP_ADAPTER_DHCP_ALREADY_STARTED)
                    {
                        ESP_LOGW(TAG, "dhcpc_start fallback failed: %s",
                                 esp_err_to_name(dh_err));
                    }
                }
            }
            else
            {
                /* FIX FR-AT #10: see FIXES.md — LOST_IP arrived but we have no
                 * cached IP to re-apply. The APPLY_CACHED branch above only
                 * handles the "have cached" case; previously the no-cached
                 * path silently did nothing, leaving the interface with no IP
                 * and no reconnect attempt. Request a reconnect so DHCP retries
                 * via the standard connect path. */
                ESP_LOGW(TAG, "LOST_IP but no cached IP - requesting reconnect");
                xEventGroupSetBits(s_wifi_evt, WIFI_EVT_RECONNECT_REQ);
            }
        }

        if (bits & WIFI_EVT_RECONNECT_REQ)
        {
            xEventGroupClearBits(s_wifi_evt, WIFI_EVT_RECONNECT_REQ);

            /* FIX (HIGH #1, F-B #5): see FIXES.md */
            if (s_intentional_disconnect)
            {
                ESP_LOGI(TAG, "intentional disconnect before reconnect");
                esp_wifi_disconnect();
                /* Don't clear s_intentional_disconnect here — the
                 * STA_DISCONNECTED handler (wifi_event_handler, case
                 * WIFI_EVENT_STA_DISCONNECTED) will clear it when the event
                 * fires. The 50ms delay below gives the driver time to
                 * process the disconnect. */
                vTaskDelay(pdMS_TO_TICKS(50));
                /* Safety net (F-B #5): if STA_DISCONNECTED never fires within
                 * 500ms, clear the flag ourselves so it doesn't stay stuck
                 * and accidentally suppress a legitimate later disconnect. */
                if (s_intentional_disconnect)
                {
                    vTaskDelay(pdMS_TO_TICKS(500));
                    if (s_intentional_disconnect)
                    {
                        ESP_LOGW(TAG, "intentional_disconnect flag stuck - clearing");
                        s_intentional_disconnect = false;
                    }
                }
            }

            uint32_t delay_ms = WIFI_RECONNECT_BACKOFF_MIN_MS;
            if (s_backoff_mtx && xSemaphoreTake(s_backoff_mtx, portMAX_DELAY) == pdTRUE)
            {
                delay_ms = s_backoff_ms;
                s_backoff_ms <<= 1;
                if (s_backoff_ms > WIFI_RECONNECT_BACKOFF_MAX_MS)
                    s_backoff_ms = WIFI_RECONNECT_BACKOFF_MAX_MS;
                xSemaphoreGive(s_backoff_mtx);
            }

            ESP_LOGW(TAG, "Reconnect in %u ms", (unsigned)delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            if (!s_initialized)
                break;

            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
                xEventGroupSetBits(s_wifi_evt, WIFI_EVT_RECONNECT_REQ);
            }
        }
    }

    ESP_LOGI(TAG, "Reconnect task exiting");
    s_reconnect_task = NULL;
    vTaskDelete(NULL);
}

typedef struct
{
    int8_t tx_power_x;
    uint8_t channel;
    volatile esp_err_t config_err;
} raw_tx_ctx_t;

static raw_tx_ctx_t s_raw_ctx = {0};

static void wifi_raw_event_handler(void *arg, esp_event_base_t base,
                                   int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base != WIFI_EVENT || id != WIFI_EVENT_STA_START)
        return;

    ESP_LOGI(TAG, "WIFI_EVENT_STA_START - configuring radio (raw TX mode)");

    esp_err_t err = esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "esp_wifi_set_ps failed: %s", esp_err_to_name(err));
        s_raw_ctx.config_err = err;
    }

    err = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_protocol failed: %s", esp_err_to_name(err));
        if (s_raw_ctx.config_err == ESP_OK)
            s_raw_ctx.config_err = err;
    }

    err = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_bandwidth failed: %s", esp_err_to_name(err));
        s_raw_ctx.config_err = err;
    }

    wifi_country_t country_config = {
        .cc = "JP",
        .schan = 1,
        .nchan = 14,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
        .max_tx_power = WIFI_TX_POWER_DBM_TO_QDBM(WIFI_TX_POWER_MAX),
    };

    err = esp_wifi_set_country(&country_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_country failed: %s", esp_err_to_name(err));
        s_raw_ctx.config_err = err;
    }

    err = esp_wifi_set_channel(s_raw_ctx.channel,
                               WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_channel(%d) failed: %s",
                 s_raw_ctx.channel, esp_err_to_name(err));
        s_raw_ctx.config_err = err;
    }
    err = wifi_set_user_fixed_rate(FIXED_RATE_MASK_STA, WIFI_RATE_11B);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "wifi_set_user_fixed_rate failed: %s - "
                      "continuing with default rate",
                 esp_err_to_name(err));
        s_raw_ctx.config_err = err;
    }
    else
    {
        ESP_LOGI(TAG, "Raw TX: protocol=11B, rate=11 Mbps (fixed)");
    }

    if (s_raw_ctx.tx_power_x > WIFI_TX_POWER_MAX)
        s_raw_ctx.tx_power_x = WIFI_TX_POWER_MAX;
    err = esp_wifi_set_max_tx_power(WIFI_TX_POWER_DBM_TO_QDBM(s_raw_ctx.tx_power_x));
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_max_tx_power failed: %s", esp_err_to_name(err));
        s_raw_ctx.config_err = err;
    }

    ESP_LOGI(TAG, "TX power set to %u dBm", (unsigned)s_raw_ctx.tx_power_x);

    /* Set DHCP hostname (for consistency, even in raw mode). */
    if (s_hostname[0])
    {
        err = tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, s_hostname);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "tcpip_adapter_set_hostname failed: %s", esp_err_to_name(err));
            s_raw_ctx.config_err = err;
        }
        else
        {
            ESP_LOGI(TAG, "Hostname set to '%s'", s_hostname);
        }
    }

    if (s_wifi_evt)
        xEventGroupSetBits(s_wifi_evt, WIFI_EVT_STA_STARTED);
}

static esp_err_t wifi_hw_init(void)
{
    esp_netif_init();

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return err;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        /* FIX (LOW): see FIXES.md */
        esp_wifi_deinit();
        return err;
    }

    /* FIX (MEDIUM): see FIXES.md */
    s_wifi_hw_initialized = true;
    return ESP_OK;
}

/* FIX (HIGH #4): see FIXES.md — memcpy with explicit length to avoid
 * truncating full-length SSIDs/passwords. */
static void wifi_sta_set_credentials(wifi_config_t *cfg, const char *ssid,
                                     const char *password)
{
    size_t ssid_len = strlen(ssid);
    if (ssid_len > sizeof(cfg->sta.ssid))
        ssid_len = sizeof(cfg->sta.ssid);
    memcpy(cfg->sta.ssid, ssid, ssid_len);
    if (ssid_len < sizeof(cfg->sta.ssid))
        cfg->sta.ssid[ssid_len] = '\0';
    size_t pwd_len = strlen(password);
    if (pwd_len > sizeof(cfg->sta.password))
        pwd_len = sizeof(cfg->sta.password);
    memcpy(cfg->sta.password, password, pwd_len);
    if (pwd_len < sizeof(cfg->sta.password))
        cfg->sta.password[pwd_len] = '\0';
}

/* Poll for reconnect task exit (100 ms granularity). */
static void await_reconnect_task_exit(uint32_t timeout_ms)
{
    for (uint32_t i = 0; i < timeout_ms / 100; i++)
    {
        if (s_reconnect_task == NULL)
            break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t wifi_sta_init(const char *ssid, const char *password,
                        const char *hostname, uint8_t tx_power)
{
    esp_err_t err = ESP_FAIL;

    if (s_initialized)
    {
        wifi_sta_deinit();
    }
    if (!ssid || !password)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!hostname || !hostname[0])
    {
        hostname = WIFI_HOSTNAME_DEFAULT;
    }

    s_wifi_evt = xEventGroupCreate();
    s_backoff_mtx = xSemaphoreCreateMutex();
    if (!s_wifi_evt || !s_backoff_mtx)
    {
        ESP_LOGE(TAG, "Failed to create event group / mutex");
        goto fail_init_noinit;
    }
    s_backoff_ms = WIFI_RECONNECT_BACKOFF_MIN_MS;
    s_intentional_disconnect = false;

    s_ctx.tx_power_x = tx_power;
    s_ctx.config_err = ESP_OK;

    err = wifi_hw_init();
    if (err != ESP_OK)
        goto fail_init_noinit;

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     wifi_event_handler, NULL);
    if (err != ESP_OK)
        goto fail_init;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                     wifi_event_handler, NULL);
    if (err != ESP_OK)
        goto fail_init;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                     wifi_event_handler, NULL);
    if (err != ESP_OK)
        goto fail_init;

    wifi_config_t wifi_cfg = {0};
    wifi_sta_set_credentials(&wifi_cfg, ssid, password);

    /* HIGH #10: check return value of esp_wifi_set_config. */
    err = esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        goto fail_init;
    }

    /* Save hostname for tcpip_adapter_set_hostname, which is called in
     * WIFI_EVENT_STA_START handler (netif is ready there; here it returns
     * ESP_ERR_TCPIP_ADAPTER_IF_NOT_READY). */
    strncpy(s_hostname, hostname, sizeof(s_hostname) - 1);
    s_hostname[sizeof(s_hostname) - 1] = '\0';

    /* FIX (CRITICAL #2): see FIXES.md */
    s_initialized = true;

    /* FIX (MEDIUM #29): see FIXES.md */
    BaseType_t tr = xTaskCreate(wifi_reconnect_task, "wifi_recon", RECONNECT_TASK_STACK,
                                NULL, RECONNECT_TASK_PRIO, &s_reconnect_task);
    if (tr != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create reconnect task");
        s_reconnect_task = NULL;
        s_initialized = false;
        err = ESP_FAIL;
        goto fail_init;
    }

    err = esp_wifi_start();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        s_initialized = false;
        if (s_wifi_evt)
            xEventGroupSetBits(s_wifi_evt, WIFI_EVT_EXIT);
        if (s_reconnect_task)
        {
            await_reconnect_task_exit(3000);
            if (s_reconnect_task)
            {
                ESP_LOGW(TAG, "wifi init fail: reconnect task did not exit, force-deleting");
                vTaskDelete(s_reconnect_task);
                s_reconnect_task = NULL;
            }
        }
        esp_wifi_stop();
        esp_wifi_deinit();
        s_wifi_hw_initialized = false;
        goto fail_init;
    }

    memset(&wifi_cfg, 0, sizeof(wifi_cfg));

    /* FIX (MEDIUM #28): see FIXES.md */
    if (s_ctx.config_err != ESP_OK)
    {
        ESP_LOGW(TAG, "wifi config error pending: %s",
                 esp_err_to_name(s_ctx.config_err));
    }

    ESP_LOGI(TAG, "WiFi STA initialized (UDP mode), connecting to %s...", ssid);
    return ESP_OK;

fail_init:
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_LOST_IP, wifi_event_handler);
    /* FIX (MEDIUM): see FIXES.md */
    if (s_wifi_hw_initialized)
    {
        esp_wifi_deinit();
        s_wifi_hw_initialized = false;
    }
fail_init_noinit:
    if (s_backoff_mtx)
    {
        vSemaphoreDelete(s_backoff_mtx);
        s_backoff_mtx = NULL;
    }
    if (s_wifi_evt)
    {
        vEventGroupDelete(s_wifi_evt);
        s_wifi_evt = NULL;
    }
    return err != ESP_OK ? err : ESP_FAIL;
}

esp_err_t wifi_sta_init_raw(uint8_t channel, uint8_t tx_power)
{
    if (s_initialized)
    {
        wifi_sta_deinit();
    }
    if (channel < 1 || channel > 14)
    {
        return ESP_ERR_INVALID_ARG;
    }

    s_wifi_evt = xEventGroupCreate();
    if (!s_wifi_evt)
    {
        return ESP_ERR_NO_MEM;
    }

    s_backoff_mtx = xSemaphoreCreateMutex();
    if (!s_backoff_mtx)
    {
        vEventGroupDelete(s_wifi_evt);
        s_wifi_evt = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_intentional_disconnect = false;

    s_raw_ctx.tx_power_x = tx_power;
    s_raw_ctx.channel = channel;
    s_raw_ctx.config_err = ESP_OK;

    esp_err_t err = wifi_hw_init();
    if (err != ESP_OK)
        goto fail_raw_init;

    err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START,
                                     wifi_raw_event_handler, NULL);
    if (err != ESP_OK)
        goto fail_raw_init_after_hw;

    err = esp_wifi_start();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        goto fail_raw_init_after_handler;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_evt,
                                           WIFI_EVT_STA_STARTED,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(WIFI_RAW_START_TIMEOUT_MS));
    if (!(bits & WIFI_EVT_STA_STARTED))
    {
        ESP_LOGE(TAG, "Timeout (%d ms) waiting for WIFI_EVENT_STA_START "
                      "(raw TX) - radio did not come up",
                 WIFI_RAW_START_TIMEOUT_MS);
        err = ESP_ERR_TIMEOUT;
        goto fail_raw_init_after_start;
    }

    if (s_raw_ctx.config_err != ESP_OK)
    {
        ESP_LOGE(TAG, "Raw TX radio configuration failed: %s",
                 esp_err_to_name(s_raw_ctx.config_err));
        err = s_raw_ctx.config_err;
        goto fail_raw_init_after_start;
    }

    xEventGroupSetBits(s_wifi_evt, WIFI_EVT_CONNECTED);

    s_initialized = true;
    ESP_LOGI(TAG, "WiFi initialized (Raw 802.11 TX mode, channel %d, "
                  "protocol=11B, rate=11 Mbps)",
             channel);
    return ESP_OK;

fail_raw_init_after_start:
    esp_wifi_stop();
fail_raw_init_after_handler:
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_START,
                                 wifi_raw_event_handler);
fail_raw_init_after_hw:
    esp_wifi_deinit();
    s_wifi_hw_initialized = false;
fail_raw_init:
    if (s_backoff_mtx)
    {
        vSemaphoreDelete(s_backoff_mtx);
        s_backoff_mtx = NULL;
    }
    if (s_wifi_evt)
    {
        vEventGroupDelete(s_wifi_evt);
        s_wifi_evt = NULL;
    }
    return err;
}

esp_err_t wifi_sta_deinit(void)
{
    if (!s_initialized)
        return ESP_OK;

    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_LOST_IP, wifi_event_handler);
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_START, wifi_raw_event_handler);

    vTaskDelay(pdMS_TO_TICKS(50));

    s_intentional_disconnect = true;

    /* FIX (LOW #25): see FIXES.md */
    esp_err_t dc = esp_wifi_disconnect();
    if (dc != ESP_OK && dc != ESP_ERR_WIFI_NOT_STARTED)
    {
        ESP_LOGD(TAG, "esp_wifi_disconnect in deinit: %s (expected in raw mode)",
                 esp_err_to_name(dc));
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    s_initialized = false;
    if (s_wifi_evt)
        xEventGroupSetBits(s_wifi_evt, WIFI_EVT_EXIT);

    if (s_reconnect_task)
    {
        await_reconnect_task_exit(3000);
        if (s_reconnect_task)
        {
            ESP_LOGW(TAG, "wifi_sta_deinit: reconnect task did not exit, force-deleting");
            vTaskDelete(s_reconnect_task);
            s_reconnect_task = NULL;
        }
    }

    esp_wifi_stop();
    esp_wifi_deinit();
    s_wifi_hw_initialized = false;

    if (s_wifi_evt)
    {
        vEventGroupDelete(s_wifi_evt);
        s_wifi_evt = NULL;
    }
    if (s_backoff_mtx)
    {
        /* FIX (LOW #24): see FIXES.md */
        if (xSemaphoreTake(s_backoff_mtx, pdMS_TO_TICKS(500)) == pdTRUE)
        {
            s_have_cached_ip = false;
            memset(&s_cached_ip_info, 0, sizeof(s_cached_ip_info));
            xSemaphoreGive(s_backoff_mtx);
            /* Temp-pointer pattern: NULL the global first so any concurrent
             * reader sees NULL and skips take. Then delete via local. */
            SemaphoreHandle_t tmp = s_backoff_mtx;
            s_backoff_mtx = NULL;
            vSemaphoreDelete(tmp);
        }
        else
        {
            static uint8_t leak_count = 0;
            leak_count++;
            ESP_LOGW(TAG, "backoff mutex held — leaking #%u (UB-safe, reboot to reclaim)",
                     leak_count);
            s_have_cached_ip = false;
            s_backoff_mtx = NULL;
            /* Если утечек > 3, вероятно системная проблема — ребут */
            if (leak_count >= 3)
            {
                ESP_LOGE(TAG, "Too many mutex leaks — rebooting");
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
        }
    }
    else
    {
        s_have_cached_ip = false;
        memset(&s_cached_ip_info, 0, sizeof(s_cached_ip_info));
    }
    s_intentional_disconnect = false;

    ESP_LOGI(TAG, "WiFi deinitialized");
    return ESP_OK;
}

esp_err_t wifi_sta_wait_connected(uint32_t timeout_ms)
{
    if (!s_wifi_evt)
        return ESP_ERR_INVALID_STATE;
    EventBits_t bits = xEventGroupWaitBits(s_wifi_evt,
                                           WIFI_EVT_CONNECTED,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_EVT_CONNECTED) ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool wifi_sta_is_connected(void)
{
    if (!s_wifi_evt)
        return false;
    return (xEventGroupGetBits(s_wifi_evt) & WIFI_EVT_CONNECTED) != 0;
}

void wifi_sta_set_tx_power(uint8_t tx_power)
{
    if (tx_power > WIFI_TX_POWER_MAX)
        tx_power = WIFI_TX_POWER_MAX;

    /* FIX F-B #3: keep the cached tx_power_x in sync so a subsequent
     * WIFI_EVENT_STA_START (after a reconnect) re-applies the new value.
     * Both the normal STA and the raw-TX contexts are updated. */
    s_ctx.tx_power_x = tx_power;
    s_raw_ctx.tx_power_x = tx_power;

    esp_err_t err = esp_wifi_set_max_tx_power(WIFI_TX_POWER_DBM_TO_QDBM(tx_power));
    if (err != ESP_OK)
        ESP_LOGW(TAG, "esp_wifi_set_max_tx_power(%u) failed: %s",
                 (unsigned)tx_power, esp_err_to_name(err));
    else
        ESP_LOGI(TAG, "TX power set to %u dBm", (unsigned)tx_power);
}

esp_err_t wifi_sta_reconfigure(const char *ssid, const char *password)
{
    /* LOW #22: distinguish "invalid argument" from "invalid state". */
    if (!ssid || !password)
        return ESP_ERR_INVALID_ARG;
    if (!s_initialized)
        return ESP_ERR_INVALID_STATE;

    /* FIX FR-AT #13 + F3-B #4: see FIXES.md — RawTX mode uses a different
     * WiFi init path (raw 802.11 frame injection, no STA association).
     * Reconfigure via esp_wifi_disconnect()/esp_wifi_connect() would corrupt
     * the raw radio state. Require AT+RST (full reboot) to apply WiFi
     * credential changes in RawTX mode.
     *
     * Check BOTH active transport AND NVS config — user may have saved RawTX
     * via AT+XPORT=2 but not yet HOTRESTART'd. In that case, active is still
     * UDP but NVS says RawTX, and reconfigure would break the pending switch. */
    device_config_t cfg;
    config_get_copy(&cfg);
    if (stream_mode_current_transport() == TRANSPORT_MODE_RAWTX ||
        cfg.transport_mode == TRANSPORT_MODE_RAWTX)
    {
        ESP_LOGW(TAG, "reconfigure not supported in RawTX mode (active or NVS) - use AT+RST");
        return ESP_ERR_NOT_SUPPORTED;
    }

    wifi_config_t wifi_cfg = {0};
    wifi_sta_set_credentials(&wifi_cfg, ssid, password);

    esp_err_t err = esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_cfg);
    if (err != ESP_OK)
        return err;

    if (s_backoff_mtx && xSemaphoreTake(s_backoff_mtx, portMAX_DELAY) == pdTRUE)
    {
        s_backoff_ms = WIFI_RECONNECT_BACKOFF_MIN_MS;
        xSemaphoreGive(s_backoff_mtx);
    }
    else
    {
        s_backoff_ms = WIFI_RECONNECT_BACKOFF_MIN_MS;
    }

    s_intentional_disconnect = true;

    /* FIX F-B #4: disconnect first to ensure a clean driver state before
     * applying the new config + connect. The STA_DISCONNECTED event this
     * generates is suppressed by s_intentional_disconnect (cleared by the
     * STA_DISCONNECTED handler, with a fallback clear below). */
    esp_wifi_disconnect();
    /* Give the driver time to emit STA_DISCONNECTED (which clears the flag).
     * We wait up to 250ms for the flag to clear; if it doesn't (event never
     * fired), clear it ourselves so we don't leak it. */
    for (int i = 0; i < 25 && s_intentional_disconnect; i++)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (s_intentional_disconnect)
    {
        ESP_LOGW(TAG, "reconfigure: intentional_disconnect not cleared by event - clearing");
        s_intentional_disconnect = false;
    }

    err = esp_wifi_connect();
    /* Clear the flag unconditionally — STA_DISCONNECTED handler also clears it
     * (defense-in-depth), but we clear here as fallback in case the event
     * doesn't fire (e.g. connect fails immediately). */
    s_intentional_disconnect = false;

    memset(&wifi_cfg, 0, sizeof(wifi_cfg));

    return err;
}
