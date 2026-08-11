/*
 * UDP transport — sends encoded audio packets to a receiver via a standard
 * UDP socket (lwIP). Requires WiFi AP association (router).
 *
 * Independent module — does not share state with tcp_stream.c or
 * rawtx_stream.c. Raw 802.11 TX is in rawtx_stream.c.
 *
 * Concurrency: s_state_mutex guards s_sock / s_ready / s_dest. send() holds
 * the mutex across sendto() to prevent fd-recycle races (see Bug #2 / MEDIUM #24).
 */

/* ---- System / SDK includes ---- */
#include <string.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "esp_log.h"

/* ---- Project includes ---- */
#include "board_config.h"
#include "udp_stream.h"
#include "socket_util.h"

static const char *TAG = "udp";

/* Named mutex timeouts (replaces magic 50/200 literals). */
#define SEND_MUTEX_TIMEOUT_MS       50   /* send / is_ready: drop frame if contended */
#define DEINIT_MUTEX_TIMEOUT_MS     200  /* init / deinit: teardown path */

static int s_sock = -1;
static struct sockaddr_in s_dest;
static bool s_ready = false;

/* Mutex guarding s_sock / s_ready / s_dest. Created on first init (via
 * ensure_mutex, idempotent); NEVER destroyed — kept for the lifetime of the
 * module so deinit/init cycles can reuse it without racing on
 * delete-while-held (FR-SVC #4, #19). All access points still check for NULL
 * before taking (defensive — s_state_mutex is only NULL before the first
 * init call, but the NULL-check guards against init-failure / never-inited
 * states). */
static SemaphoreHandle_t s_state_mutex = NULL;

/* Create the mutex on first use. Idempotent. Returns false on alloc failure. */
static bool ensure_mutex(void)
{
    if (!s_state_mutex)
    {
        s_state_mutex = xSemaphoreCreateMutex();
        if (!s_state_mutex)
        {
            ESP_LOGE(TAG, "ensure_mutex: xSemaphoreCreateMutex failed");
            return false;
        }
    }
    return true;
}

/* UDP payload upper bound: 1400 leaves headroom for WiFi encap and matches
 * the RAWTX cap. Larger payloads trigger IP fragmentation (any fragment lost
 * -> whole datagram lost, amplifying loss on congested WiFi). */
#define UDP_MAX_PAYLOAD 1400

esp_err_t udp_stream_init(uint32_t host_ip, uint16_t host_port)
{
    /* FIX (FR-SVC #4, #19): ensure_mutex() is idempotent — if s_state_mutex
     * already exists (from a previous init, or kept across a deinit since
     * deinit no longer deletes it), don't recreate it. This eliminates the
     * delete-while-held race in deinit entirely: the mutex is created once
     * and lives for the module lifetime. */
    if (!ensure_mutex())
    {
        ESP_LOGE(TAG, "init: mutex alloc failed");
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(DEINIT_MUTEX_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGE(TAG, "init: state_mutex timeout");
        return ESP_ERR_TIMEOUT;
    }

    /* Unconditionally reset state (MEDIUM #23): previously the cleanup branch
     * was only entered when (s_ready && s_sock >= 0), so an inconsistent state
     * (s_ready==true && s_sock<0) skipped cleanup. */
    if (s_sock >= 0)
    {
        ESP_LOGW(TAG, "init: closing old socket (s_ready=%d) first", s_ready);
        socket_close_safe(&s_sock);
    }
    s_ready = false;

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0)
    {
        int saved_errno = errno;  /* capture before ESP_LOGE (Task 6-D #3) */
        ESP_LOGE(TAG, "socket: errno=%d", saved_errno);
        xSemaphoreGive(s_state_mutex);
        return ESP_FAIL;
    }

#if CFG_UDP_SEND_TIMEOUT_MS_ENABLED
    (void)socket_set_send_timeout_ms(s_sock, UDP_SEND_TIMEOUT_MS);
#endif

/* If CONFIG_ESP8266_WIFI_QOS_ENABLED is undefined, IP_TOS is silently skipped
 * — not all SDK builds expose QoS hooks. Intentional. */
#ifdef CONFIG_ESP8266_WIFI_QOS_ENABLED
    (void)socket_set_tos_ef(s_sock);
#endif

    memset(&s_dest, 0, sizeof(s_dest));
    s_dest.sin_family = AF_INET;
    s_dest.sin_port = htons(host_port);
    s_dest.sin_addr.s_addr = host_ip; /* already network byte order */

    ESP_LOGI(TAG, "UDP -> %d.%d.%d.%d:%u",
             (int)(host_ip & 0xFF), (int)((host_ip >> 8) & 0xFF),
             (int)((host_ip >> 16) & 0xFF), (int)((host_ip >> 24) & 0xFF),
             (unsigned)host_port);

    /* Set s_dest BEFORE s_ready=true so a racing send() never sees ready=true
     * with a stale dest. Strictly ordered under the mutex. */
    s_ready = true;

    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

esp_err_t udp_stream_deinit(void)
{
    /* Close under the mutex so a racing send() can't capture s_sock and then
     * sendto() on a closed fd (Bug #2).
     * FIX (FR-SVC #4, #19): do NOT delete s_state_mutex — keep it for the
     * lifetime of the module. Deleting a mutex that another task might hold
     * is UB; the previous NULL-first-then-delete pattern still left a
     * Give→NULL gap during which a concurrent acquirer could end up holding
     * a soon-to-be-deleted mutex. The mutex is created once in init() (via
     * ensure_mutex, idempotent) and reused across deinit/init cycles, which
     * eliminates the delete-while-held race entirely. The cost is a single
     * persistent mutex allocation (~80 bytes) for the module lifetime — a
     * fair trade for correctness. */
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(DEINIT_MUTEX_TIMEOUT_MS)) == pdTRUE)
    {
        socket_close_safe(&s_sock);
        s_ready = false;
        xSemaphoreGive(s_state_mutex);
        /* Do NOT delete s_state_mutex — keep it for the lifetime of the module.
         * Deleting a mutex that another task might hold is UB. The mutex is
         * created once in init() (idempotent via ensure_mutex) and reused
         * across deinit/init cycles. */
    }
    else
    {
        /* Mutex busy — can't safely close s_sock (fd-recycle race with send task).
         * Just set s_ready=false so send task stops. The socket will be closed
         * by the next udp_stream_init() under the mutex. */
        ESP_LOGW(TAG, "deinit: mutex busy — s_ready=false, socket left for next init");
        s_ready = false;
    }
    return ESP_OK;
}

bool udp_stream_is_ready(void)
{
    /* Read under mutex for acquire/release semantics (bool r/w is atomic on
     * Xtensa, but the mutex provides the barrier a bare read doesn't). */
    bool r = false;
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(SEND_MUTEX_TIMEOUT_MS)) == pdTRUE)
    {
        r = s_ready;
        xSemaphoreGive(s_state_mutex);
    }
    else
    {
        r = s_ready;  /* best-effort fallback */
    }
    return r;
}

esp_err_t udp_stream_send(const uint8_t *data, size_t len)
{
    if (!data || !len)
        return ESP_ERR_INVALID_ARG;
    if (len > UDP_MAX_PAYLOAD)
    {
        ESP_LOGW(TAG, "send: payload %u > %u, rejecting",
                 (unsigned)len, (unsigned)UDP_MAX_PAYLOAD);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Hold the state mutex across sendto() (MEDIUM #24 / LOW #38) so deinit()
     * cannot recycle the socket fd while a send is in flight. Contention is
     * minimal (send is per audio frame; deinit is rare). */
    int sock;
    struct sockaddr_in dest;
    if (!(s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(SEND_MUTEX_TIMEOUT_MS)) == pdTRUE))
    {
        return ESP_ERR_INVALID_STATE;  /* drop frame rather than unsync send */
    }

    sock = s_sock;
    dest = s_dest;
    if (sock < 0 || !s_ready)
    {
        xSemaphoreGive(s_state_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    int sendto_ret = sendto(sock, data, len, MSG_DONTWAIT,
                            (struct sockaddr *)&dest, sizeof(dest));
    xSemaphoreGive(s_state_mutex);

    if (sendto_ret < 0)
    {
        int saved_errno = errno;
        ESP_LOGW(TAG, "sendto failed: errno=%d len=%u", saved_errno, (unsigned)len);
        /* EHOSTUNREACH is 118 in newlib but may differ in some lwIP builds —
         * keep the || 118 fallback (HIGH #13). */
        if (saved_errno == ENOMEM)
            vTaskDelay(pdMS_TO_TICKS(50));
        else if (saved_errno == ENODEV)
            vTaskDelay(pdMS_TO_TICKS(500));
        else if (saved_errno == EHOSTUNREACH || saved_errno == 118)
            vTaskDelay(pdMS_TO_TICKS(200));
        else if (saved_errno != EBADF && saved_errno != ENOTCONN)
            vTaskDelay(pdMS_TO_TICKS(100));

        return ESP_FAIL;
    }

    return ESP_OK;
}
