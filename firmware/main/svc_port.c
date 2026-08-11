/*
 * Service UDP port - EASSP protocol implementation.
 *
 * Listens on UDP:3950 for DISCOVER and CONFIGURE commands.
 * Sends INFO responses and periodic announcements.
 * Watchdog auto-stops streaming if server stops sending DISCOVER heartbeats.
 *
 * Uses POSIX sockets (lwip/sockets.h) on ESP8266 RTOS SDK v3.4.
 */

/* ---- System / SDK includes ---- */
#include <string.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "tcpip_adapter.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

/* ---- Project includes ---- */
#include "board_config.h"
#include "svc_port.h"
#include "svc_protocol.h"
#include "config_mgr.h"
#include "stream_mode.h"    /* FIX (AUDIT-XPORT-AUTOAPPLY): stream_mode_current_transport() */
#include "stream_control.h" /* FIX (LOW): streaming_get_frame_ms / streaming_get_channels / streaming_frame_ms_known — replaces the extern decls. */
#include "lwip/def.h"       /* ntohs() */

static const char *TAG = "svc_port";

/* ---- State machine ---- */
typedef enum
{
    SVC_STOPPED = 0,
    SVC_IDLE = 1,
    SVC_STREAMING = 2,
} svc_state_t;

/* ---- Module state ---- */
static int s_sock = -1;
static uint16_t s_port = 0; /* bound port (for reinit after WiFi reconnect) */
static SemaphoreHandle_t s_mutex = NULL;
static SemaphoreHandle_t s_reinit_mutex = NULL;

static svc_state_t s_state = SVC_STOPPED;
static TaskHandle_t s_task_handle = NULL;
static uint32_t s_packets_sent = 0;

/* FIX (F2-SVC #11): the s_channels static is GONE — it was write-only
 * dead state. build_info_payload() uses streaming_get_channels()
 * directly (since the B3/channels-desync fix), so the value set here
 * was never read. svc_port_set_channels() is kept as a no-op for API
 * compatibility with stream_mode.c / at_handlers.c callers. */
static volatile uint8_t s_error_code = SVC_ERR_NONE;
static bool s_watchdog_fired = false; /* L8: one-shot log suppression */
static uint16_t s_seq_counter = 0;

/* FIX (GROK-19): see FIXES.md */
static volatile uint8_t s_error_pending = SVC_ERR_NONE;

static ip_addr_t s_server_ip; /* audio destination */
static uint16_t s_server_port;
static ip_addr_t s_server_svc_addr; /* service port (for INFO replies) */
static uint16_t s_server_svc_port;

static uint8_t s_mac[6];
static ip_addr_t s_broadcast_addr;

static EventGroupHandle_t s_stream_evt_grp = NULL;

static TickType_t s_last_discover_ticks = 0;

/* EHOSTUNREACH suppression - avoid log spam while WiFi is associating. */
static bool s_no_route_logged = false;

/* ---- Forward declarations ---- */
static void svc_task_fn(void *arg);
static void handle_discover(const svc_header_t *hdr,
                            const ip_addr_t *src_addr, uint16_t src_port);
static void handle_configure(const svc_header_t *hdr, const uint8_t *payload,
                             const ip_addr_t *src_addr, uint16_t src_port);

static void handle_stop(const svc_header_t *hdr,
                        const ip_addr_t *src_addr, uint16_t src_port);
static void send_info(uint16_t req_seq, const ip_addr_t *dest, uint16_t port);
static void build_info_payload(svc_info_payload_t *info);
static void send_to(const uint8_t *data, size_t len,
                    const ip_addr_t *dest, uint16_t port);
static TickType_t now_ticks(void);

static esp_err_t svc_port_reinit_socket_impl(void);

/* ---- Helpers ---- */

static TickType_t now_ticks(void)
{
    return xTaskGetTickCount();
}

/* Atomically reserve and return the next INFO sequence number. */
static uint16_t next_info_seq(void)
{
    uint16_t seq;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    seq = s_seq_counter++;
    xSemaphoreGive(s_mutex);
    return seq;
}

/* FIX (F-A #7): compute the subnet-directed broadcast address from
 * the current STA IP/netmask. Pure helper — does NOT touch
 * s_broadcast_addr. Callers must store the result under s_mutex so the
 * announce loop in svc_task_fn (which reads s_broadcast_addr under
 * s_mutex) sees a consistent value.
 *
 * If WiFi has no IP yet (netif not ready), falls back to IPADDR_BROADCAST
 * (255.255.255.255). This is safe but less efficient — announce packets
 * flood the entire L2 segment instead of the local subnet. Updated to
 * subnet-directed broadcast once IP is acquired (called on GOT_IP). */
static void compute_broadcast_addr(ip_addr_t *out)
{
    tcpip_adapter_ip_info_t ip_info;
    if (tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip_info) == ESP_OK)
    {
        ip_addr_t ip, mask;
        ip.addr = ip_info.ip.addr;
        mask.addr = ip_info.netmask.addr;
        /* Subnet-directed broadcast: IP | ~netmask */
        out->addr = ip.addr | ~mask.addr;
        /* Logging moved to update_broadcast_addr (only logs on change). */
    }
    else
    {
        out->addr = IPADDR_BROADCAST;
        /* Logging moved to update_broadcast_addr. */
    }
}

/* Refresh s_broadcast_addr from the current STA IP/netmask.
 * FIX (F-A #7): the s_broadcast_addr write happens under s_mutex so
 * concurrent readers (svc_task_fn announce loop) see a consistent
 * value. */
static void update_broadcast_addr(void)
{
    ip_addr_t computed;
    compute_broadcast_addr(&computed);
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        /* Only log if the broadcast address actually changed (avoids duplicate
         * log lines when GOT_IP handler and reinit_socket both call this). */
        if (s_broadcast_addr.addr != computed.addr)
        {
            s_broadcast_addr = computed;
            if (computed.addr == IPADDR_BROADCAST)
            {
                ESP_LOGW(TAG, "No IP yet, using 255.255.255.255 for broadcast");
            }
            else
            {
                ESP_LOGI(TAG, "Broadcast: " IPSTR, IP2STR(&computed));
            }
        }
        xSemaphoreGive(s_mutex);
    }
    else
    {
        s_broadcast_addr = computed;
    }
}

/* ---- Public API ---- */
esp_err_t svc_port_init(uint16_t port, void *stream_evt_grp)
{
    bool already_running;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        already_running = (s_state != SVC_STOPPED);
        xSemaphoreGive(s_mutex);
    }
    else
    {
        already_running = (s_state != SVC_STOPPED);
    }
    if (already_running)
    {
        ESP_LOGW(TAG, "Already running");
        return ESP_ERR_INVALID_STATE;
    }

    s_port = port;
    s_stream_evt_grp = (EventGroupHandle_t)stream_evt_grp;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex)
    {
        ESP_LOGE(TAG, "Failed to create mutex");
        goto init_fail;
    }

    /* SINGLE-FLIGHT: create the reinit guard mutex. */
    s_reinit_mutex = xSemaphoreCreateMutex();
    if (!s_reinit_mutex)
    {
        ESP_LOGE(TAG, "Failed to create reinit mutex");
        goto init_fail;
    }

    esp_err_t mac_err = esp_wifi_get_mac(ESP_IF_WIFI_STA, s_mac);
    if (mac_err != ESP_OK)
    {
        ESP_LOGW(TAG, "esp_wifi_get_mac failed: %s - INFO will report 00:...",
                 esp_err_to_name(mac_err));
        memset(s_mac, 0, sizeof(s_mac));
    }
    else
    {
        ESP_LOGI(TAG, "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                 s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
    }

    update_broadcast_addr();

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0)
    {
        ESP_LOGE(TAG, "socket failed: errno=%d", errno);
        goto init_fail;
    }

#ifdef CONFIG_LWIP_SO_REUSE
    int enable = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
#endif

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0)
    {
        ESP_LOGE(TAG, "bind failed: errno=%d", errno);
        goto init_fail;
    }

#if CFG_UDP_RECEIVE_TIMEOUT_MS_ENABLED
    struct timeval tv_rcv = {.tv_sec = UDP_RECEIVE_TIMEOUT_MS / 1000,
                             .tv_usec = (UDP_RECEIVE_TIMEOUT_MS % 1000) * 1000};
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv_rcv, sizeof(tv_rcv));
#endif

#if CFG_UDP_SEND_TIMEOUT_MS_ENABLED
    struct timeval tv_snd = {.tv_sec = UDP_SEND_TIMEOUT_MS / 1000,
                             .tv_usec = (UDP_SEND_TIMEOUT_MS % 1000) * 1000};
    setsockopt(s_sock, SOL_SOCKET, SO_SNDTIMEO, &tv_snd, sizeof(tv_snd));
#endif

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_server_ip.addr = 0;
    s_server_port = 0;
    s_server_svc_addr.addr = 0;
    s_server_svc_port = 0;
    s_packets_sent = 0;
    s_error_code = 0;
    s_error_pending = SVC_ERR_NONE;
    s_last_discover_ticks = 0;
    s_no_route_logged = false;
    s_seq_counter = 0;
    s_state = SVC_IDLE;
    xSemaphoreGive(s_mutex);

    BaseType_t res = xTaskCreate(svc_task_fn, "svc_port", TASK_STACK_SVC,
                                 NULL, TASK_PRIO_SVC, &s_task_handle);
    if (res != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create task");
        goto init_fail;
    }

    uint32_t boot_frame_ms = streaming_frame_ms_known()
                                 ? streaming_get_frame_ms()
                                 : 0;
    ESP_LOGI(TAG, "Service port active on UDP:%u (audio: %u ms%s, rate from config)",
             (unsigned)port, (unsigned)boot_frame_ms,
             boot_frame_ms == 0 ? " (not yet computed)" : "");
    return ESP_OK;

init_fail:
    s_state = SVC_STOPPED;
    s_task_handle = NULL;
    if (s_sock >= 0)
    {
        close(s_sock);
        s_sock = -1;
    }
    if (s_mutex)
    {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    /* SINGLE-FLIGHT: clean up reinit mutex on init failure. */
    if (s_reinit_mutex)
    {
        vSemaphoreDelete(s_reinit_mutex);
        s_reinit_mutex = NULL;
    }
    return ESP_FAIL;
}

bool svc_port_is_running(void)
{
    /* FIX (AUDIT-H6): see FIXES.md — read s_state under the mutex. */
    bool running = false;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        running = (s_state != SVC_STOPPED);
        xSemaphoreGive(s_mutex);
    }
    else
    {
        running = (s_state != SVC_STOPPED); /* best-effort fallback */
    }
    return running;
}

/* SINGLE-FLIGHT: public entry point. Takes s_reinit_mutex with timeout=0
 * (try-lock). If another reinit is already in progress, returns ESP_OK
 * immediately ("someone else is handling it"). This prevents
 * wifi_reconnect_task and svc_task_fn periodic retry from both creating
 * sockets and leaking one of them. */
esp_err_t svc_port_reinit_socket(void)
{
    if (!s_reinit_mutex)
    {
        ESP_LOGE(TAG, "reinit: reinit_mutex not created");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_reinit_mutex, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "reinit: already in progress - skipping");
        return ESP_OK;
    }

    esp_err_t err = svc_port_reinit_socket_impl();

    xSemaphoreGive(s_reinit_mutex);
    return err;
}

void svc_port_notify_streaming_started(void)
{
    if (!s_mutex)
        return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state = SVC_STREAMING;
    s_last_discover_ticks = now_ticks();
    /* FIX (AUDIT-MEDIUM): see FIXES.md */
    s_error_code = SVC_ERR_NONE;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Streaming started - DISCOVER watchdog active (%d s timeout), "
                  "periodic INFO every %d s",
             SVC_WATCHDOG_TIMEOUT_MS / 1000, SVC_INFO_INTERVAL_MS / 1000);
}

void svc_port_notify_streaming_stopped(void)
{
    if (!s_mutex)
        return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state = SVC_IDLE;
    xSemaphoreGive(s_mutex);
    s_watchdog_fired = false; /* L8: reset for next stream */
    ESP_LOGI(TAG, "Streaming stopped - announcements resumed");
}

void svc_port_reset_watchdog(void)
{
    if (!s_mutex)
        return;
    /* FIX (C2): see FIXES.md */
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        ESP_LOGW(TAG, "reset_watchdog: mutex timeout - skipping");
        return;
    }
    /* Only reset if we're streaming (watchdog only runs in that state).
     * If idle, this is a no-op. */
    if (s_state == SVC_STREAMING)
    {
        s_last_discover_ticks = now_ticks();
    }
    xSemaphoreGive(s_mutex);
}

void svc_port_notify_stop_complete(void)
{
    /* No-op: stop completion detected via streaming_is_active() in handle_configure. */
}

void svc_port_update_stats(uint32_t packets_sent)
{
    if (!s_mutex)
        return;
    /* FIX (C2): see FIXES.md */
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        ESP_LOGW(TAG, "update_stats: mutex timeout - skipping");
        return;
    }
    s_packets_sent = packets_sent;
    xSemaphoreGive(s_mutex);
}

bool svc_port_get_stream_dest(uint32_t *host, uint16_t *port)
{
    if (!host || !port)
        return false;
    if (!s_mutex)
        return false;
    bool valid;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *host = s_server_ip.addr;
    *port = s_server_port;
    valid = (s_server_ip.addr != 0 && s_server_port != 0);
    xSemaphoreGive(s_mutex);
    return valid;
}

/* FIX (H1): see FIXES.md */
bool svc_port_get_server_ip(uint32_t *ip)
{
    if (!ip || !s_mutex)
        return false;
    bool valid;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *ip = s_server_svc_addr.addr;
    valid = (s_server_svc_addr.addr != 0);
    xSemaphoreGive(s_mutex);
    return valid;
}

void svc_port_set_channels(uint8_t channels)
{
    /* FIX (F2-SVC #11): no-op. The s_channels static was write-only dead
     * state — build_info_payload() uses streaming_get_channels() directly
     * (B3/channels-desync fix). Kept as a no-op for API compatibility
     * with the stream_mode ops table and at_handlers.c callers. */
    (void)channels;
}

void svc_port_set_error(uint8_t error_code)
{
    if (!s_mutex)
        return;

    /* FIX (C2): see FIXES.md — timeout instead of portMAX_DELAY. */
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        /* FIX (F-A #9): PARTIAL fix — best-effort volatile writes so the
         * error is still reported. s_error_code is read under mutex by
         * build_info_payload(); a racing read may see the old or new
         * value (uint8_t store is atomic on Xtensa), but the worst case
         * is one INFO packet with stale error_code, which the next INFO
         * will correct. */
        ESP_LOGW(TAG, "set_error: mutex timeout - error %u set best-effort",
                 (unsigned)error_code);
        s_error_code = error_code; /* best-effort volatile write */
        s_error_pending = error_code;
        return;
    }
    s_error_code = error_code;
    /* FIX (GROK-19): see FIXES.md */
    s_error_pending = error_code;
    xSemaphoreGive(s_mutex);
}

void svc_port_clear_error(void)
{
    if (!s_mutex)
        return;
    /* FIX (C2): see FIXES.md */
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        ESP_LOGW(TAG, "clear_error: mutex timeout - error flag not cleared");
        return;
    }
    s_error_code = SVC_ERR_NONE;
    /* FIX (GROK-3.5): see FIXES.md */
    s_error_pending = SVC_ERR_NONE;
    xSemaphoreGive(s_mutex);
}

/* FIX (F2-SVC #8): set error_code ONLY if no error is currently set.
 * This preserves any higher-priority error (I2S, CODEC, MEMORY) already
 * reported by an upstream task. The unconditional svc_port_set_error()
 * above is kept for callers that explicitly want to overwrite (e.g. the
 * watchdog path, which sets SVC_ERR_WATCHDOG unconditionally). Used by
 * pipeline.c's stream_task_fn send-fail path so a transient NETWORK
 * error doesn't clobber a concurrent I2S/CODEC/MEMORY error. */
void svc_port_set_error_if_none(uint8_t error_code)
{
    if (!s_mutex)
        return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        if (s_error_code == SVC_ERR_NONE)
        {
            s_error_code = error_code;
            s_error_pending = error_code;
        }
        xSemaphoreGive(s_mutex);
    }
    else
    {
        /* Best-effort: only set if no current error. uint8_t store is
         * atomic on Xtensa; worst case is one INFO packet with stale
         * error_code, which the next INFO will correct (same reasoning
         * as the F-A #9 best-effort path in svc_port_set_error). */
        if (s_error_code == SVC_ERR_NONE)
        {
            s_error_code = error_code;
            s_error_pending = error_code;
        }
    }
}

/* FIX (FR-SVC #7): per-code error clearing. Clears s_error_code / s_error_pending
 * ONLY if the currently-active error matches `error_code`. This lets callers
 * clear a specific error (e.g. SVC_ERR_NETWORK after a successful TX) without
 * clobbering an unrelated upstream error (e.g. SVC_ERR_I2S set by the i2s
 * task in the meantime). The unconditional svc_port_clear_error() is kept for
 * cases where a full clear is intentional (e.g. on streaming start). */
void svc_port_clear_error_code(uint8_t error_code)
{
    if (!s_mutex)
        return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        if (s_error_code == error_code)
        {
            s_error_code = SVC_ERR_NONE;
            s_error_pending = SVC_ERR_NONE;
        }
        xSemaphoreGive(s_mutex);
    }
}

/* Called by wifi_sta.c on IP_EVENT_STA_GOT_IP to refresh broadcast addr. */
void svc_port_update_broadcast(void)
{
    /* FIX (LOW #28): see FIXES.md */
    bool running = false;
    if (s_mutex != NULL)
    {
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            running = (s_state != SVC_STOPPED);
            xSemaphoreGive(s_mutex);
        }
        else
        {
            running = (s_state != SVC_STOPPED); /* best-effort fallback */
        }
    }
    if (!running)
        return;
    update_broadcast_addr();
}

void svc_port_get_status(svc_port_status_t *status)
{
    if (!status)
        return;
    memset(status, 0, sizeof(*status));
    if (!s_mutex)
        return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    status->running = (s_state != SVC_STOPPED);
    status->streaming = (s_state == SVC_STREAMING);
    status->error_code = s_error_code;
    /* Refresh MAC dynamically: if WiFi was not initialized when svc_port_init()
     * ran, s_mac may still be all zeros. esp_wifi_get_mac() is cheap and safe
     * to call here (INFO packets are sent at most a few times per second). */
    {
        uint8_t mac_tmp[6];
        if (esp_wifi_get_mac(WIFI_IF_STA, mac_tmp) == ESP_OK)
        {
            memcpy(status->mac, mac_tmp, 6);
        }
        else
        {
            memcpy(status->mac, s_mac, 6);
        }
    }
    status->server_stream_ip = s_server_ip.addr;
    status->server_stream_port = s_server_port;
    status->server_svc_ip = s_server_svc_addr.addr;
    status->server_svc_port = s_server_svc_port;
    status->packets_sent = s_packets_sent;
    if (s_state == SVC_STREAMING)
    {
        uint32_t elapsed_ms = (uint32_t)((now_ticks() - s_last_discover_ticks) * portTICK_PERIOD_MS);
        int32_t remaining = (int32_t)SVC_WATCHDOG_TIMEOUT_MS - (int32_t)elapsed_ms;
        status->watchdog_remaining_ms = (remaining > 0) ? remaining : 0;
    }
    else
    {
        status->watchdog_remaining_ms = -1;
    }
    xSemaphoreGive(s_mutex);
}

/* ---- Internal: send helpers ---- */

static void send_to(const uint8_t *data, size_t len,
                    const ip_addr_t *dest, uint16_t port)
{
    /* FIX (S6): see FIXES.md */
    if (!s_mutex)
        return;
    if (len == 0)
        return;

    /* FIX (F-A #5): hold s_mutex ACROSS sendto() so the fd cannot be
     * closed/swapped out from under us by svc_port_reinit_socket()
     * (which also takes s_mutex while swapping s_sock). The previous
     * snapshot-then-release pattern raced against reinit_socket: the
     * fd could be close()'d between our xSemaphoreGive and the sendto,
     * producing EBADF / use-after-close. */
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
        return;
    int sock = s_sock;
    if (sock < 0)
    {
        xSemaphoreGive(s_mutex);
        return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = dest->addr;

    /* FIX (F-A #3): MSG_DONTWAIT prevents sendto() from blocking when
     * the socket buffer is full (no SO_SNDTIMEO configured when
     * CFG_UDP_SEND_TIMEOUT_MS_ENABLED is unset). */
    ssize_t sent = sendto(sock, data, len, MSG_DONTWAIT,
                          (struct sockaddr *)&addr, sizeof(addr));
    int saved_errno = errno; /* capture before xSemaphoreGive (context switch) */
    xSemaphoreGive(s_mutex);
    if (sent < 0)
    {
        /* FIX (AUDIT-MEDIUM): see FIXES.md */
#ifndef ENETUNREACH
#define ENETUNREACH 118
#endif
#ifndef EHOSTUNREACH
#define EHOSTUNREACH 118
#endif
        if (saved_errno == ENETUNREACH || saved_errno == EHOSTUNREACH)
        {
            if (!s_no_route_logged)
            {
                ESP_LOGW(TAG, "sendto: no route (WiFi associating?)");
                s_no_route_logged = true;
            }
        }
        else
        {
            ESP_LOGW(TAG, "sendto failed: errno=%d", saved_errno);
        }
    }
    else
    {
        s_no_route_logged = false;
    }
}

static void build_info_payload(svc_info_payload_t *info)
{
    memset(info, 0, sizeof(*info));

    device_config_t cfg;
    config_get_copy(&cfg);

    info->codec_id = (cfg.codec_mode == CODEC_MODE_PCM) ? CODEC_ID_PCM : CODEC_ID_ADPCM;
    info->sample_rate = cfg.sample_rate;
    /* FIX (LOW #32, F-A #6): see FIXES.md — report 0 if frame_ms has
     * not yet been computed (before the first stream starts) so INFO
     * packets don't prematurely claim the default 20 ms. Mirrors the
     * boot-log guard in svc_port_init. */
    info->frame_ms = streaming_frame_ms_known()
                         ? (uint8_t)streaming_get_frame_ms()
                         : 0;
    info->bits_per_sample = cfg.bits_per_sample;
    /* FIX (AUDIT-XPORT-AUTOAPPLY): see FIXES.md */
    info->transport_mode = stream_mode_current_transport();

    /* FIX (B3/channels-desync): see FIXES.md */
    uint8_t channels_snapshot = streaming_get_channels();

    /* FIX (L22): see FIXES.md */
    svc_state_t state_local;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    state_local = s_state;
    info->status = (state_local == SVC_STREAMING) ? SVC_STATUS_STREAMING : SVC_STATUS_IDLE;
    info->error = s_error_code;
    info->packets_sent = s_packets_sent;
    info->channels = channels_snapshot;
    /* Refresh MAC dynamically: if WiFi was not initialized when svc_port_init()
     * ran, s_mac may still be all zeros. esp_wifi_get_mac() is cheap and safe
     * to call here (INFO packets are sent at most a few times per second). */
    {
        uint8_t mac_tmp[6];
        if (esp_wifi_get_mac(WIFI_IF_STA, mac_tmp) == ESP_OK)
        {
            memcpy(info->mac, mac_tmp, 6);
        }
        else
        {
            memcpy(info->mac, s_mac, 6);
        }
    }
    xSemaphoreGive(s_mutex);

    /* Only override status to ERROR when streaming.
     * When IDLE (after stop), leftover error codes from the stop process
     * (e.g. send() fail on closed socket) should NOT be shown as ERROR -
     * the device is simply idle. */
    if (info->error != SVC_ERR_NONE && state_local == SVC_STREAMING)
    {
        info->status = SVC_STATUS_ERROR;
    }

    info->free_heap = esp_get_free_heap_size();

    wifi_ap_record_t ap_info;
    int8_t rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
    {
        rssi = ap_info.rssi;
    }
    info->wifi_rssi = rssi;

    /* FIX (AUDIT-LOW): see FIXES.md */
    strncpy(info->firmware, FIRMWARE_VERSION, sizeof(info->firmware) - 1);
    info->firmware[sizeof(info->firmware) - 1] = '\0';

    /* v2.2: hostname for display in receiver UI (NUL-terminated, max 32 chars). */
    strncpy(info->hostname, cfg.hostname, sizeof(info->hostname) - 1);
    info->hostname[sizeof(info->hostname) - 1] = '\0';
}

static void send_info(uint16_t req_seq, const ip_addr_t *dest, uint16_t port)
{
    svc_info_payload_t info;
    build_info_payload(&info);

    uint8_t buf[SVC_HEADER_SIZE + sizeof(svc_info_payload_t)];

    /* Use req_seq directly for the header sequence number.
     * Callers source the seq: replies echo the request's seq; periodic /
     * error INFO uses next_info_seq(). FIX: previously send_info ALSO
     * incremented s_seq_counter itself, causing a double increment. */
    svc_header_t hdr;
    svc_header_init(&hdr, SVC_CMD_INFO, req_seq, sizeof(svc_info_payload_t));
    memcpy(buf, &hdr, SVC_HEADER_SIZE);
    memcpy(buf + SVC_HEADER_SIZE, &info, sizeof(svc_info_payload_t));

    send_to(buf, sizeof(buf), dest, port);
}

/* ---- Sender validation (zero-day defense) ----
 * When STREAMING, only the current streaming server may STOP / reset
 * watchdog / re-CONFIGURE.
 *
 * SECURITY CAVEAT — EASSP has NO real authentication (Fix F-A #10):
 *   - Source IP is spoofable on most LANs (no ARP binding to MAC).
 *   - No MAC verification, no nonce/replay protection, no HMAC/signature.
 *   - sender_is_current_server() only does an IP-equality check against
 *     s_server_svc_addr.
 * This is a BEST-EFFORT check, useful as hardening on a TRUSTED LAN
 * only — do NOT rely on it as a security boundary on a hostile network
 * or across the open internet. When NOT streaming, ANY sender is
 * accepted (so initial discovery/configure works). */
static bool sender_is_current_server(const ip_addr_t *src_addr)
{
    /* If not streaming, accept any sender (initial discovery/configure). */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool streaming = (s_state == SVC_STREAMING);
    ip_addr_t server_svc = s_server_svc_addr;
    xSemaphoreGive(s_mutex);
    if (!streaming)
        return true;
    /* Streaming: only accept from the server that configured us. */
    return (src_addr->addr == server_svc.addr);
}

/* ---- Internal: command handlers ---- */

static void handle_discover(const svc_header_t *hdr,
                            const ip_addr_t *src_addr, uint16_t src_port)
{
    ESP_LOGI(TAG, "DISCOVER from " IPSTR ":%u (seq=%u, plen=%u)",
             IP2STR(src_addr), (unsigned)src_port,
             (unsigned)hdr->seq, (unsigned)hdr->payload_len);

    /* SECURITY: when streaming, only the current server's DISCOVER resets
     * the watchdog. A spoofed DISCOVER from another host is ignored (no
     * watchdog reset, no INFO response). This prevents an attacker from
     * keeping a dead stream alive or probing device state. */
    if (!sender_is_current_server(src_addr))
    {
        ESP_LOGW(TAG, "DISCOVER from non-server " IPSTR " - ignored (streaming)",
                 IP2STR(src_addr));
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_last_discover_ticks = now_ticks();
    s_server_svc_addr = *src_addr;
    s_server_svc_port = src_port;
    xSemaphoreGive(s_mutex);

    /* FIX (FR-SVC #20): always send INFO response (both idle and streaming).
     * During streaming this gives the server an immediate heartbeat ack
     * instead of waiting for the next periodic INFO (~1s). */
    send_info(hdr->seq, src_addr, src_port);
}

/* FIX (F2-SVC #7): arm START_REQ atomically with the s_packets_sent
 * reset. Previously the in-drain START_REQ set (when a CONFIGURE is
 * received during the drain loop) did NOT reset s_packets_sent, racing
 * with the new stream's update_stats calls — the new stream could
 * briefly see a stale non-zero counter, or worse, the late post-drain
 * reset could zero out the new stream's already-incremented counts.
 * Routing ALL START_REQ sets in handle_configure through this helper
 * guarantees the reset happens BEFORE the bit is set, so the new
 * stream always starts at 0. The original FR-SVC #9 fix noted this
 * in-drain race as out-of-scope; F2-SVC closes it. */
static void arm_start_req(void)
{
    if (!s_stream_evt_grp)
        return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_packets_sent = 0;
    xSemaphoreGive(s_mutex);
    xEventGroupSetBits(s_stream_evt_grp, STREAM_EVT_START_REQ);
}

static void handle_configure(const svc_header_t *hdr, const uint8_t *payload,
                             const ip_addr_t *src_addr, uint16_t src_port)
{
    bool start_already_armed = false;

    ESP_LOGI(TAG, "CONFIGURE from " IPSTR ":%u", IP2STR(src_addr), (unsigned)src_port);

    /* FIX (F-A #1): reject CONFIGURE from any non-current-server sender.
     * Safe for IDLE state because sender_is_current_server() returns true
     * when not streaming (so initial discovery/configure still works).
     * See the EASSP-auth SECURITY CAVEAT above sender_is_current_server(). */
    if (!sender_is_current_server(src_addr))
    {
        ESP_LOGW(TAG, "CONFIGURE from non-server " IPSTR " - ignored (streaming)",
                 IP2STR(src_addr));
        return;
    }

    const svc_configure_payload_t *cfg = (const svc_configure_payload_t *)payload;

    if (cfg->stream_port == 0)
    {
        ESP_LOGW(TAG, "CONFIGURE: invalid stream port");
        return;
    }
    bool stream_active = streaming_is_active(); // FIX: учитываем окно ACTIVE→SVC_STREAMING

    /* CONFIGURE is just a "Start Stream" trigger.
     * The ESP is the audio authority - it streams exactly what is in its NVS
     * config (set by AT+CH). The server learns the channel count from the INFO
     * packet and adapts its playback (WaveOut) accordingly. We intentionally
     * IGNORE any channel count in the CONFIGURE payload to avoid mismatches. */

    uint16_t new_port = ntohs(cfg->stream_port);

    /* FIX (LOW): see FIXES.md */
    bool same;
    bool need_stop;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    same = (s_state == SVC_STREAMING || stream_active) &&
           s_server_ip.addr == src_addr->addr &&
           s_server_port == new_port;
    if (same)
    {
        s_last_discover_ticks = now_ticks();
        s_server_svc_addr = *src_addr;
        s_server_svc_port = src_port;
        xSemaphoreGive(s_mutex);
        ESP_LOGI(TAG, "Already streaming to same destination - resetting watchdog");
        send_info(hdr->seq, src_addr, src_port);
        return;
    }

    /* Different destination - store it.
     * NOTE: s_packets_sent is reset AFTER the old stream is stopped below,
     * not here. If we reset it here while the old stream is still running,
     * stream_task_fn keeps calling svc_port_update_stats(sent) on every
     * packet and overwrites our 0, so INFO packets during the transition
     * would show stale counts. */
    s_server_ip.addr = src_addr->addr;
    s_server_port = new_port;
    s_server_svc_addr = *src_addr;
    s_server_svc_port = src_port;
    s_last_discover_ticks = now_ticks();
    /* Snapshot need_stop (was s_state == SVC_STREAMING) in the SAME
     * critical section — no need to release+reacquire the mutex. */
    need_stop = (s_state == SVC_STREAMING || stream_active);
    xSemaphoreGive(s_mutex);

    /* FIX (MEDIUM #5, MEDIUM #6): see FIXES.md */
    bool stop_requested = false;

    if (need_stop && s_stream_evt_grp)
    {
        /* FIX (H3): see FIXES.md — set STOP_REQ and poll streaming_is_active(). */
        xEventGroupSetBits(s_stream_evt_grp, STREAM_EVT_STOP_REQ);

        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(SVC_RECONFIGURE_STOP_TIMEOUT_MS);
        bool stopped = false;
        while (xTaskGetTickCount() < deadline)
        {
            /* FIX (TOCTOU-1): recvfrom под мьютексом. MSG_DONTWAIT гарантирует
             * неблокирующий возврат, поэтому мьютекс удерживается микросекунды.
             * Это устраняет гонку с svc_port_reinit_socket_impl(), который
             * закрывает/заменяет s_sock под тем же мьютексом.
             * Паттерн идентичен send_to() (FIX F-A #5). */
            uint8_t drain_buf[SVC_RECV_BUF_SIZE];
            struct sockaddr_in drain_addr;
            socklen_t drain_len = sizeof(drain_addr);
            ssize_t rlen = -1;

            if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
            {
                if (s_sock >= 0)
                {
                    rlen = recvfrom(s_sock, drain_buf, sizeof(drain_buf),
                                    MSG_DONTWAIT,
                                    (struct sockaddr *)&drain_addr, &drain_len);
                }
                xSemaphoreGive(s_mutex);
            }
            else
            {
                /* Мьютекс занят — пропускаем итерацию (не гонимся за stale fd). */
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }

            if (rlen < 0)
            {
                /* Нет данных или сокет невалиден — просто ждём. */
                vTaskDelay(pdMS_TO_TICKS(50));
                /* rlen == -1 → ниже идёт if (rlen > 0), который не сработает.
                 * Переходим к проверке streaming_is_active(). */
            }
            if (rlen > 0)
            {
                /* FIX (AUDIT-H7, MEDIUM #36, MEDIUM #5, MEDIUM #6): see FIXES.md */
                if (rlen >= (ssize_t)SVC_HEADER_SIZE)
                {
                    svc_header_t dhdr;
                    memcpy(&dhdr, drain_buf, SVC_HEADER_SIZE);
                    if (dhdr.magic[0] == EASSP_MAGIC0 &&
                        dhdr.magic[1] == EASSP_MAGIC1 &&
                        dhdr.version == EASSP_VER)
                    {
                        if (dhdr.cmd == SVC_CMD_STOP)
                        {
                            /* FIX (F-A #2): verify sender — only honor
                             * CMD_STOP from the current streaming server.
                             * A spoofed CMD_STOP would otherwise kill the
                             * stream mid-reconfigure. */
                            ip_addr_t drain_ip = {.addr = drain_addr.sin_addr.s_addr};
                            if (!sender_is_current_server(&drain_ip))
                            {
                                ESP_LOGW(TAG, "CMD_STOP during drain from non-server - ignored");
                            }
                            else
                            {
                                /* FIX (MEDIUM #5): see FIXES.md */
                                stop_requested = true;
                                ESP_LOGW(TAG, "CMD_STOP received during reconfigure "
                                              "stop-wait — will honor (no restart)");
                                if (s_stream_evt_grp)
                                    xEventGroupSetBits(s_stream_evt_grp,
                                                       STREAM_EVT_STOP_REQ);
                            }
                        }
                        else if (dhdr.cmd == SVC_CMD_CONFIGURE)
                        {
                            /* FIX (F-A #2): verify sender — only honor
                             * CONFIGURE during drain from the current
                             * streaming server. A spoofed CONFIGURE could
                             * otherwise redirect the stream to an attacker.
                             * Sender rejected → no destination update AND
                             * no START_REQ re-arm. */
                            ip_addr_t drain_ip = {.addr = drain_addr.sin_addr.s_addr};
                            if (!sender_is_current_server(&drain_ip))
                            {
                                ESP_LOGW(TAG, "CONFIGURE during drain from non-server - ignored");
                            }
                            else
                            {
                                /* FIX (MEDIUM #6): see FIXES.md */
                                if (rlen >= (ssize_t)(SVC_HEADER_SIZE + 2))
                                {
                                    uint16_t new_drain_port;
                                    memcpy(&new_drain_port,
                                           drain_buf + SVC_HEADER_SIZE, 2);
                                    new_drain_port = ntohs(new_drain_port);
                                    if (new_drain_port != 0)
                                    {
                                        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE)
                                        {
                                            s_server_ip.addr = drain_addr.sin_addr.s_addr;
                                            s_server_port = new_drain_port;
                                            s_server_svc_addr.addr = drain_addr.sin_addr.s_addr;
                                            s_server_svc_port = ntohs(drain_addr.sin_port);
                                            xSemaphoreGive(s_mutex);
                                        }
                                        ESP_LOGI(TAG, "CONFIGURE updated during drain: port=%u",
                                                 (unsigned)new_drain_port);
                                    }
                                    else
                                    {
                                        ESP_LOGW(TAG, "CONFIGURE during drain: invalid stream_port=0 — ignoring");
                                    }
                                }
                                else
                                {
                                    ESP_LOGW(TAG, "CONFIGURE during drain: payload truncated (rlen=%d) — keeping previous destination",
                                             (int)rlen);
                                }
                                /* Re-arm START_REQ so the latest destination wins.
                                 * FIX (Task 6-B): see FIXES.md — guard with
                                 * stop_requested so an in-drain CMD_STOP wins.
                                 * FIX (F2-SVC #7): use arm_start_req() so
                                 * s_packets_sent is reset BEFORE the START_REQ
                                 * bit is set — closes the in-drain START_REQ
                                 * race noted as out-of-scope in the original
                                 * FR-SVC #9 fix. */
                                if (!stop_requested && !start_already_armed)
                                {
                                    arm_start_req();
                                    start_already_armed = true;
                                }
                                else
                                {
                                    ESP_LOGI(TAG, "CONFIGURE during drain — "
                                                  "stop requested, not "
                                                  "re-arming START");
                                }
                            }
                        }
                        /* For any valid EASSP packet (incl. DISCOVER),
                         * refresh the watchdog timestamp. */
                        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
                        {
                            s_last_discover_ticks = now_ticks();
                            xSemaphoreGive(s_mutex);
                        }
                    }
                }
                else
                {
                    /* Too short to be an EASSP header — just refresh the
                     * watchdog timestamp best-effort. */
                    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
                    {
                        s_last_discover_ticks = now_ticks();
                        xSemaphoreGive(s_mutex);
                    }
                }
            }
            if (!streaming_is_active())
            {
                stopped = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (stopped)
            ESP_LOGI(TAG, "Previous stream stopped - starting new stream");
        else
            ESP_LOGW(TAG, "Stop not completed within %d ms - proceeding anyway",
                     SVC_RECONFIGURE_STOP_TIMEOUT_MS);
    }

    /* Send INFO response. */
    send_info(hdr->seq, src_addr, src_port);

    /* Request main loop to start streaming. */
    device_config_t dev_cfg;
    config_get_copy(&dev_cfg);

    ESP_LOGI(TAG, "Requesting stream start: " IPSTR ":%u (%u Hz, %u ms, NVS ch=%u)",
             IP2STR(src_addr), (unsigned)new_port,
             (unsigned)dev_cfg.sample_rate, (unsigned)streaming_get_frame_ms(),
             (unsigned)channel_format_to_count(dev_cfg.channel_format));

    /* FIX (F2-SVC #7): arm START_REQ via the helper, which resets
     * s_packets_sent to 0 BEFORE setting the bit. The previous standalone
     * reset (FR-SVC #9) is now redundant — arm_start_req() does both.
     * This also closes the in-drain START_REQ race (the in-drain path
     * above now uses the same helper, so both START_REQ sets reset the
     * counter atomically). */
    if (!stop_requested && !start_already_armed)
    {
        arm_start_req();
    }
    else
    {
        ESP_LOGI(TAG, "CMD_STOP received during reconfigure — not restarting");
    }
}

/* ---- CMD_STOP: explicit stream stop from server ---- */
static void handle_stop(const svc_header_t *hdr,
                        const ip_addr_t *src_addr, uint16_t src_port)
{
    ESP_LOGI(TAG, "CMD_STOP from " IPSTR ":%u - stopping stream",
             IP2STR(src_addr), (unsigned)src_port);

    /* SECURITY: only the current streaming server can stop the stream.
     * Without this, any host on the network can send CMD_STOP and kill
     * the stream. When IDLE, CMD_STOP is a no-op anyway (nothing to stop),
     * but we still validate to avoid logging noise from spoofers. */
    if (!sender_is_current_server(src_addr))
    {
        ESP_LOGW(TAG, "CMD_STOP from non-server " IPSTR " - ignored",
                 IP2STR(src_addr));
        return;
    }

    /* Request main loop to stop streaming. */
    if (s_stream_evt_grp)
    {
        xEventGroupSetBits(s_stream_evt_grp, STREAM_EVT_STOP_REQ);
    }

    /* FIX (M11): see FIXES.md */

    /* Send INFO response reflecting the current state. */
    send_info(hdr->seq, src_addr, src_port);
}

/* ---- Internal: service task ---- */

static void svc_task_fn(void *arg)
{
    ESP_LOGI(TAG, "Service port task started");

    uint8_t recv_buf[SVC_RECV_BUF_SIZE];
    TickType_t last_info = 0;
#if SVC_ANNOUNCE_ENABLED
    uint32_t announce_ms = SVC_ANNOUNCE_MIN_MS;
    TickType_t last_announce = 0;
#endif

    while (1)
    {
        /* Exit check. */
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        svc_state_t st = s_state;
        xSemaphoreGive(s_mutex);
        if (st == SVC_STOPPED)
            break;

        /* FIX (F3-B #3): see FIXES.md — If s_sock is invalid (reinit failed),
         * periodically retry reinit instead of spinning on EBADF forever.
         * After 5 failed retries in svc_port_reinit_socket, s_sock stays -1
         * and recvfrom would return EBADF every loop iteration. This check
         * retries reinit every 5s and skips the recvfrom on invalid socket. */
        static TickType_t last_reinit_attempt = 0;
        /* Check s_sock under mutex (avoids false reinit when reinit just completed). */
        int sock_snapshot = -1;
        if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            sock_snapshot = s_sock;
            xSemaphoreGive(s_mutex);
        }
        else
        {
            sock_snapshot = s_sock; /* best-effort */
        }
        if (sock_snapshot < 0)
        {
            TickType_t now = xTaskGetTickCount();
            if ((now - last_reinit_attempt) > pdMS_TO_TICKS(5000))
            {
                ESP_LOGW(TAG, "svc_task: s_sock invalid, retrying reinit");
                last_reinit_attempt = now;
                svc_port_reinit_socket();
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        ssize_t len = -1;
        int saved_errno = 0;

        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            sock_snapshot = s_sock;
            if (sock_snapshot >= 0)
            {
                len = recvfrom(sock_snapshot, recv_buf, sizeof(recv_buf),
                               MSG_DONTWAIT, (struct sockaddr *)&src, &slen);
                saved_errno = errno;
            }
            xSemaphoreGive(s_mutex);
        }
        else
        {
            /* Mutex busy — skip this iteration entirely.
             * Better than racing on a stale fd. */
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (len < 0)
            errno = saved_errno; /* restore for any downstream checks */

        if (len >= SVC_HEADER_SIZE)
        {
            svc_header_t hdr;
            memcpy(&hdr, recv_buf, SVC_HEADER_SIZE);

            if (hdr.magic[0] == EASSP_MAGIC0 && hdr.magic[1] == EASSP_MAGIC1 &&
                hdr.version == EASSP_VER)
            {

                ip_addr_t ip = {.addr = src.sin_addr.s_addr};
                uint16_t port = ntohs(src.sin_port);
                const uint8_t *payload = recv_buf + SVC_HEADER_SIZE;
                size_t avail = (size_t)len - SVC_HEADER_SIZE;

                /* SECURITY: validate payload_len (uint16 max 65535, buffer
                 * is SVC_RECV_BUF_SIZE). Reject malformed/malicious packets
                 * early. FIX (AUDIT-MEDIUM): see FIXES.md — cast to size_t
                 * to avoid sign-compare warning. */
                if ((size_t)hdr.payload_len > sizeof(recv_buf) - SVC_HEADER_SIZE)
                {
                    ESP_LOGW(TAG, "RX payload_len %u > buf capacity %u - rejected",
                             (unsigned)hdr.payload_len,
                             (unsigned)(sizeof(recv_buf) - SVC_HEADER_SIZE));
                }
                else if ((size_t)hdr.payload_len <= avail)
                {
                    if (hdr.cmd == SVC_CMD_DISCOVER)
                        handle_discover(&hdr, &ip, port);
                    else if (hdr.cmd == SVC_CMD_CONFIGURE && hdr.payload_len >= CFG_PAYLOAD_SZ)
                        handle_configure(&hdr, payload, &ip, port);
                    else if (hdr.cmd == SVC_CMD_CONFIGURE)
                        /* FIX (L23): see FIXES.md */
                        ESP_LOGW(TAG, "RX CONFIGURE plen=%u too short (need %u)",
                                 (unsigned)hdr.payload_len, (unsigned)CFG_PAYLOAD_SZ);
                    else if (hdr.cmd == SVC_CMD_STOP)
                        handle_stop(&hdr, &ip, port);
                    else
                        ESP_LOGW(TAG, "RX cmd=0x%02X plen=%u (unknown/unhandled)",
                                 (unsigned)hdr.cmd, (unsigned)hdr.payload_len);
                }
                else
                {
                    ESP_LOGW(TAG, "RX truncated: cmd=0x%02X plen=%u avail=%u",
                             (unsigned)hdr.cmd, (unsigned)hdr.payload_len, (unsigned)avail);
                }
            }
            else if (len > 0)
            {
                ESP_LOGW(TAG, "RX bad magic/ver: %02X %02X %02X (len=%d)",
                         (unsigned)hdr.magic[0], (unsigned)hdr.magic[1],
                         (unsigned)hdr.version, (int)len);
            }
        }

        /* Periodic: INFO / watchdog / announce - read state once. */
        TickType_t now = now_ticks();
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        st = s_state;
        ip_addr_t svc_addr = s_server_svc_addr;
        uint16_t svc_port = s_server_svc_port;
        uint32_t elapsed = (uint32_t)(now - s_last_discover_ticks) * portTICK_PERIOD_MS;
        ip_addr_t bcast = s_broadcast_addr;
        /* FIX (GROK-19): see FIXES.md */
        uint8_t pending_err = s_error_pending;
        if (pending_err != SVC_ERR_NONE)
            s_error_pending = SVC_ERR_NONE;
        xSemaphoreGive(s_mutex);

        /* Flush a pending error immediately (regardless of state) so the
         * server is notified of e.g. I2S underrun / encode failure without
         * waiting for the next periodic INFO interval. */
        if (pending_err != SVC_ERR_NONE && svc_addr.addr != 0)
        {
            uint16_t s = next_info_seq();
            send_info(s, &svc_addr, svc_port);
            ESP_LOGI(TAG, "flushed pending error %u via INFO", (unsigned)pending_err);
        }

        if (st == SVC_STREAMING)
        {
            /* Periodic INFO. */
            if ((uint32_t)(now - last_info) * portTICK_PERIOD_MS >= SVC_INFO_INTERVAL_MS)
            {
                last_info = now;
                if (svc_addr.addr)
                {
                    uint16_t s = next_info_seq();
                    send_info(s, &svc_addr, svc_port);
                }
            }
            /* Watchdog. */
            if (elapsed >= SVC_WATCHDOG_TIMEOUT_MS)
            {
                /* L8: log only once per watchdog fire (was flooding every 100ms
                 * until stop_streaming completes, ~30 lines in ~3s). */
                if (!s_watchdog_fired)
                {
                    ESP_LOGW(TAG, "Watchdog expired (%u ms) - stopping", (unsigned)elapsed);
                    s_watchdog_fired = true;
                }
                if (s_stream_evt_grp)
                    xEventGroupSetBits(s_stream_evt_grp, STREAM_EVT_STOP_REQ);
                /* FIX (MEDIUM #41): see FIXES.md */
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                /* Only the error code is set here; state stays SVC_STREAMING. */
                s_error_code = SVC_ERR_WATCHDOG;
                xSemaphoreGive(s_mutex);
            }
        }
#if SVC_ANNOUNCE_ENABLED
        else if (st == SVC_IDLE)
        {
            if ((uint32_t)(now - last_announce) * portTICK_PERIOD_MS >= announce_ms)
            {
                last_announce = now;
                uint32_t range = SVC_ANNOUNCE_MAX_MS - SVC_ANNOUNCE_MIN_MS;
                announce_ms = SVC_ANNOUNCE_MIN_MS + (esp_random() % (range + 1));
                if (bcast.addr)
                {
                    /* FIX (M9): see FIXES.md */
                    uint16_t s = next_info_seq();
                    send_info(s, &bcast, s_port);
                    /* FIX (DIAG-ANNOUNCE): see FIXES.md */
                    static uint32_t announce_log_counter = 0;
                    if ((++announce_log_counter % 5) == 1)
                    {
                        ESP_LOGI(TAG, "announce #%u -> " IPSTR ":%u (state=IDLE)",
                                 (unsigned)announce_log_counter,
                                 IP2STR(&bcast), (unsigned)s_port);
                    }
                }
            }
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "Service port task exiting");

    /* FIX (GROK-1): see FIXES.md — clear task handle BEFORE vTaskDelete. */
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        s_task_handle = NULL;
        xSemaphoreGive(s_mutex);
    }
    else
    {
        s_task_handle = NULL;
    }
    vTaskDelete(NULL);
}

/* SINGLE-FLIGHT: internal implementation. All original logic unchanged.
 * Called ONLY from svc_port_reinit_socket() under s_reinit_mutex. */
static esp_err_t svc_port_reinit_socket_impl(void)
{
    bool is_stopped;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        is_stopped = (s_state == SVC_STOPPED);
        xSemaphoreGive(s_mutex);
    }
    else
    {
        is_stopped = (s_state == SVC_STOPPED);
    }
    if (is_stopped)
        return ESP_ERR_INVALID_STATE;

    if (s_port == 0)
        return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "Reinitializing UDP socket after WiFi reconnect (port %u)",
             (unsigned)s_port);

    int old_sock = -1;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        old_sock = s_sock;
        s_sock = -1;
        xSemaphoreGive(s_mutex);
    }
    else
    {
        ESP_LOGE(TAG, "reinit: mutex busy on close - aborting");
        return ESP_FAIL;
    }

    if (old_sock >= 0)
    {
        shutdown(old_sock, SHUT_RDWR);
        close(old_sock);
    }

    int new_sock = -1;
    for (int i = 0; i < 5; i++)
    {
        new_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (new_sock >= 0)
            break;
        int saved_errno = errno;
        ESP_LOGW(TAG, "reinit: socket() failed (errno=%d), retry %d/5",
                 saved_errno, i + 1);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (new_sock < 0)
    {
        ESP_LOGE(TAG, "reinit: socket() failed after 5 retries - service degraded");
        return ESP_FAIL;
    }

#ifdef CONFIG_LWIP_SO_REUSE
    int enable = 1;
    if (setsockopt(new_sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0)
    {
        int saved_errno = errno;
        ESP_LOGW(TAG, "reinit: SO_REUSEADDR failed (errno=%d)", saved_errno);
    }
#endif

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(s_port);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int bind_ok = -1;
    for (int i = 0; i < 5; i++)
    {
        bind_ok = bind(new_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr));
        if (bind_ok == 0)
            break;
        int saved_errno = errno;
        ESP_LOGW(TAG, "reinit: bind() failed (errno=%d), retry %d/5",
                 saved_errno, i + 1);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (bind_ok < 0)
    {
        ESP_LOGE(TAG, "reinit: bind() failed after 5 retries - service degraded");
        close(new_sock);
        return ESP_FAIL;
    }

#if CFG_UDP_RECEIVE_TIMEOUT_MS_ENABLED
    struct timeval tv_rcv = {.tv_sec = UDP_RECEIVE_TIMEOUT_MS / 1000,
                             .tv_usec = (UDP_RECEIVE_TIMEOUT_MS % 1000) * 1000};
    if (setsockopt(new_sock, SOL_SOCKET, SO_RCVTIMEO, &tv_rcv, sizeof(tv_rcv)) < 0)
    {
        int saved_errno = errno;
        ESP_LOGW(TAG, "reinit: SO_RCVTIMEO failed (errno=%d)", saved_errno);
    }
#endif

#if CFG_UDP_SEND_TIMEOUT_MS_ENABLED
    struct timeval tv_snd = {.tv_sec = UDP_SEND_TIMEOUT_MS / 1000,
                             .tv_usec = (UDP_SEND_TIMEOUT_MS % 1000) * 1000};
    if (setsockopt(new_sock, SOL_SOCKET, SO_SNDTIMEO, &tv_snd, sizeof(tv_snd)) < 0)
    {
        int saved_errno = errno;
        ESP_LOGW(TAG, "reinit: SO_SNDTIMEO failed (errno=%d)", saved_errno);
    }
#endif

    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        s_sock = new_sock;
        s_no_route_logged = false;
        xSemaphoreGive(s_mutex);
    }
    else
    {
        ESP_LOGE(TAG, "reinit: mutex busy on swap - closing new socket");
        close(new_sock);
        return ESP_FAIL;
    }

    update_broadcast_addr();

    if (old_sock == new_sock)
    {
        ESP_LOGI(TAG, "UDP socket reinitialized (fd %d, recycled by lwIP)", new_sock);
    }
    else
    {
        ESP_LOGI(TAG, "UDP socket reinitialized (fd %d -> %d)", old_sock, new_sock);
    }
    return ESP_OK;
}
