/*
 * TCP transport for audio streaming — implementation.
 *
 * ESP = listener (TCP server). Accepts one connection, streams audio with
 * length-prefix framing. Blocking send with SO_SNDTIMEO (backpressure flows
 * through task queues; avoids deadlock of non-blocking + select()).
 *
 * Framing (server/eassp_server.bas): each frame = [u16 len BE][16-byte
 * pkt_header][payload]; len = 16 + payload_len (<= 1400, fits u16).
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
#include "tcp_stream.h"
#include "svc_port.h" /* svc_port_get_server_ip() */
#include "socket_util.h"

static const char *TAG = "tcp_stream";

/* Named mutex timeouts (replaces magic 50/200 literals). */
#define SEND_MUTEX_TIMEOUT_MS 50    /* is_ready: short, drop-frame-friendly */
#define DEINIT_MUTEX_TIMEOUT_MS 200 /* init_listen reuse path: teardown */
/* TCP_EAGAIN_MAX_RETRIES lives in board_network.h (FR-TCP #22) so the
 * cross-validation #error in board_config.h can see it. */

/* Listening socket (accept). */
static int s_listen_sock = -1;
/* Active client socket (connect accepted). -1 = no client. */
static int s_client_sock = -1;
/* Port we're listening on (for logging). */
static uint16_t s_listen_port = 0;
/* Accept task handle. */
static TaskHandle_t s_accept_task = NULL;
/* s_running is read by the accept task in a loop and written by deinit() from
 * another task. Mark volatile so the compiler doesn't hoist the read out of
 * while(s_running) (FreeRTOS convention for cross-task flags). */
static volatile bool s_running = false;

/* Mutex guarding s_client_sock (Bug #2). Accept task and send path both take
 * it; send() holds it across the blocking send() to prevent fd-recycle races
 * (MEDIUM #9). */
static SemaphoreHandle_t s_client_mutex = NULL;

/* ---- Accept task: waits for incoming connect, replaces current client ---- */
static void tcp_accept_task_fn(void *arg)
{
    (void)arg;
    struct sockaddr_in client_addr;

    while (s_running)
    {
        socklen_t addr_len = sizeof(client_addr);
        int new_sock = accept(s_listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (new_sock < 0)
        {
            if (s_running)
            {
                int saved_errno = errno; /* capture before ESP_LOGW (Task 6-D #4) */
                ESP_LOGW(TAG, "accept failed: errno=%d", saved_errno);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* H1: authorize the client. Without this, ANY host on the LAN could
         * connect and hijack/backpressure the stream. Only accept the most
         * recent CONFIGURE's source IP. */
        uint32_t allowed_ip = 0;
        bool have_server = svc_port_get_server_ip(&allowed_ip);
        if (!have_server || client_addr.sin_addr.s_addr != allowed_ip)
        {
            ESP_LOGW(TAG, "rejecting unauthorized TCP client %d.%d.%d.%d:%d",
                     (int)(client_addr.sin_addr.s_addr & 0xFF),
                     (int)((client_addr.sin_addr.s_addr >> 8) & 0xFF),
                     (int)((client_addr.sin_addr.s_addr >> 16) & 0xFF),
                     (int)((client_addr.sin_addr.s_addr >> 24) & 0xFF),
                     (int)ntohs(client_addr.sin_port));
            socket_close_safe(&new_sock);
            continue;
        }

#if CFG_TCP_NODELAY_ENABLED
        (void)socket_set_nodelay(new_sock);
#endif

#ifdef CONFIG_ESP8266_WIFI_QOS_ENABLED
        (void)socket_set_tos_ef(new_sock);
#endif

#if CFG_TCP_SEND_TIMEOUT_MS_ENABLED
        (void)socket_set_send_timeout_ms(new_sock, TCP_SEND_TIMEOUT_MS);
#endif

#if CFG_TCP_KEEPALIVE_ENABLED
        (void)socket_set_keepalive(new_sock,
                                   CFG_TCP_KEEPIDLE_SEC,
                                   CFG_TCP_KEEPINTVL_SEC,
                                   CFG_TCP_KEEPCNT);
#endif

        /* ESP-RECONNECT-LEAK: SO_LINGER with linger=0 forces RST close (no
         * TIME_WAIT) -> prevents heap exhaustion during rapid reconnect
         * cycles. Default ON for streaming audio (we don't care about
         * in-flight data on close). */
#if CFG_TCP_LINGER_ENABLED
        (void)socket_set_linger(new_sock, true, 0);
#endif

        /* Bug #2: close old client and install new one ATOMICALLY under the
         * mutex. New order: under mutex, shutdown+close old FIRST (any
         * in-flight send() on old fails fast), THEN publish new_sock.
         *
         * AUDIT-H1: re-check s_running under the mutex BEFORE installing —
         * if deinit() won the race, refuse the new client and exit.
         *
         * F-D #8: timeout raised from 100ms to 500ms so a send() in flight
         * (which holds this mutex for up to (TCP_EAGAIN_MAX_RETRIES + 1) x
         * SO_SNDTIMEO = ~2s under heavy backpressure, F2-TCP #2) gets a
         * fair chance to finish on a NORMAL short send before we refuse the
         * new client. With RETRIES=0 (F2-TCP #2) the worst-case send time
         * (2s) is now LONGER than this 500ms timeout — accept may still
         * refuse a new client during a stuck send, but that's correct by
         * design (refuse + retry next iteration).
         * Tradeoff: a stuck peer blocks accept for 500ms instead of 100ms;
         * new client then gets refused (socket_close_safe + continue) -- the
         * next accept loop iteration retries. */
        int old;
        if (s_client_mutex && xSemaphoreTake(s_client_mutex, pdMS_TO_TICKS(500)) == pdTRUE)
        {
            if (!s_running)
            {
                xSemaphoreGive(s_client_mutex);
                ESP_LOGW(TAG, "accept: s_running cleared during accept, refusing new client");
                socket_close_safe(&new_sock);
                break;
            }
            old = s_client_sock;
            if (old >= 0)
            {
                ESP_LOGI(TAG, "new client, closing old connection (sock=%d)", old);
                socket_close_safe(&old);
            }
            s_client_sock = new_sock;
            xSemaphoreGive(s_client_mutex);
        }
        else
        {
            ESP_LOGW(TAG, "accept: client_mutex timeout -- refusing new client");
            socket_close_safe(&new_sock);
            continue;
        }

        ESP_LOGI(TAG, "client connected: %d.%d.%d.%d:%d (sock=%d)",
                 (int)(client_addr.sin_addr.s_addr & 0xFF),
                 (int)((client_addr.sin_addr.s_addr >> 8) & 0xFF),
                 (int)((client_addr.sin_addr.s_addr >> 16) & 0xFF),
                 (int)((client_addr.sin_addr.s_addr >> 24) & 0xFF),
                 (int)ntohs(client_addr.sin_port), new_sock);
    }

    /* s_accept_task is a TaskHandle_t (32-bit pointer) — atomic write on Xtensa.
     * No mutex needed (s_client_mutex protects s_client_sock, not s_accept_task). */
    s_accept_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t tcp_stream_init_listen(uint16_t port)
{
    /* REUSE LISTENING SOCKET across stop→start cycles. ESP8266 lwIP keeps the
     * listening socket in TIME_WAIT/CLOSE_WAIT even after close()+shutdown(),
     * causing EADDRINUSE on the next bind(). Listener + accept task are
     * created ONCE on first init and destroyed only in deinit() (full
     * teardown, e.g. transport-mode change). Stream restart only closes the
     * active client connection. */

    /* Already listening on the requested port? Listening socket + accept task
     * alive — just reset client state. */
    if (s_listen_sock >= 0 && s_listen_port == port && s_accept_task)
    {
        /* H2: close any stale client connection under the mutex. Previously
         * s_client_sock was closed without taking the mutex, racing with
         * tcp_stream_send which may have snapshotted it. */
        if (s_client_mutex && xSemaphoreTake(s_client_mutex, pdMS_TO_TICKS(DEINIT_MUTEX_TIMEOUT_MS)) == pdTRUE)
        {
            socket_close_safe(&s_client_sock);
            xSemaphoreGive(s_client_mutex);
        }
        else
        {
            ESP_LOGW(TAG, "init_listen reuse: client_mutex timeout - client not closed");
        }
        ESP_LOGI(TAG, "TCP reusing listening socket on port %u (accept task alive)",
                 (unsigned)port);
        return ESP_OK;
    }

    /* Different port, or first init, or previous deinit — full teardown. */
    if (s_listen_sock >= 0 || s_accept_task)
    {
        tcp_stream_deinit();
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    s_listen_port = port;
    s_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listen_sock < 0)
    {
        int saved_errno = errno; /* capture before ESP_LOGE (Task 6-D #4) */
        ESP_LOGE(TAG, "socket() failed: errno=%d", saved_errno);
        return ESP_FAIL;
    }

    /* Bug #2: create the client mutex once, on first init. */
    if (!s_client_mutex)
    {
        s_client_mutex = xSemaphoreCreateMutex();
        if (!s_client_mutex)
        {
            ESP_LOGE(TAG, "Failed to create client mutex");
            socket_close_safe(&s_listen_sock);
            return ESP_ERR_NO_MEM;
        }
    }

#ifdef CONFIG_LWIP_SO_REUSE
    (void)socket_set_reuseaddr(s_listen_sock);
#endif

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(port);

    if (bind(s_listen_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0)
    {
        int saved_errno = errno; /* capture before ESP_LOGE (Task 6-D #4) */
        ESP_LOGE(TAG, "bind() port=%u failed: errno=%d", (unsigned)port, saved_errno);
        socket_close_safe(&s_listen_sock);
        return ESP_FAIL;
    }

    if (listen(s_listen_sock, 1) < 0)
    {
        int saved_errno = errno; /* capture before ESP_LOGE (Task 6-D #4) */
        ESP_LOGE(TAG, "listen() failed: errno=%d", saved_errno);
        socket_close_safe(&s_listen_sock);
        return ESP_FAIL;
    }

    /* Start accept task. NOTE: TCP_ACCEPT_TASK_STACK (default 1024) is tight;
     * if you see stack overflow, increase via menuconfig. Recommended min 1536. */
    s_running = true;
    s_client_sock = -1;
    BaseType_t ok = xTaskCreate(tcp_accept_task_fn, "tcp_accept", TCP_ACCEPT_TASK_STACK, NULL,
                                TCP_ACCEPT_TASK_PRIO, &s_accept_task);
    if (ok != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create accept task");
        socket_close_safe(&s_listen_sock);
        s_running = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "TCP listening on port %u (waiting for client connect)", (unsigned)port);
    return ESP_OK;
}

esp_err_t tcp_stream_deinit(void)
{
    s_running = false;

    /* shutdown() forces accept() in the accept task to return immediately
     * (close() alone doesn't always wake it on ESP8266 lwIP). Critical:
     * without it the listening socket stays in LISTEN and the next bind()
     * fails with EADDRINUSE on quick stop→start cycles. */
    socket_close_safe(&s_listen_sock);

    /* Bug #2 / MEDIUM #32: close client under the mutex so send() can't race
     * on a half-closed fd. Block indefinitely — send path never holds it for
     * long, and deinit is a teardown path. */
    if (s_client_mutex)
    {
        /* Bound the wait: send() holds the mutex at most
         * TCP_FRAME_SEND_DEADLINE_MS (2500ms). Add 500ms margin. */
        if (xSemaphoreTake(s_client_mutex,
                           pdMS_TO_TICKS(TCP_FRAME_SEND_DEADLINE_MS + 500)) == pdTRUE)
        {
            socket_close_safe(&s_client_sock);
            xSemaphoreGive(s_client_mutex);
        }
        else
        {
            /* Send is stuck beyond the deadline — shutdown() should have
             * unblocked it (called from tcp_stream_abort before deinit).
             * If we still can't take the mutex, force-close without it.
             * The accept task also can't take the mutex while send holds it,
             * so fd recycling is impossible in this window. */
            ESP_LOGW(TAG, "deinit: client_mutex timeout — force-closing socket");
            int sock = s_client_sock;
            if (sock >= 0)
            {
                shutdown(sock, SHUT_RDWR);
                close(sock);
            }
            s_client_sock = -1;
        }
    }
    else
    {
        socket_close_safe(&s_client_sock); /* best-effort, no mutex (shouldn't happen post-init) */
    }

    /* Give the accept task time to notice s_running=false and exit via
     * vTaskDelete(NULL). shutdown() unblocks accept() so the task loops back,
     * sees s_running=false, and deletes itself.
     * FW#4 / GROK-3: poll up to 2s for clean exit; force-delete if stuck
     * (lwIP-state risk, but smaller than leaking the task — reboot recommended). */
    if (s_accept_task)
    {
        /* Poll for clean exit. The listening socket was already shutdown+closed
         * above by socket_close_safe(&s_listen_sock), which unblocks accept(). */
        for (int i = 0; i < 20 && s_accept_task; i++)
            vTaskDelay(pdMS_TO_TICKS(100));
        if (s_accept_task)
        {
            ESP_LOGW(TAG, "tcp_stream: accept task did not exit in 2s - "
                          "force-deleting (reboot recommended to clean lwIP state)");
            vTaskDelete(s_accept_task);
            s_accept_task = NULL;
        }
    }

    s_listen_port = 0;
    return ESP_OK;
}

/* WiFi reconnect: re-create the TCP listening socket after WiFi
 * disconnect/reconnect. When WiFi disconnects, lwIP destroys the netif; the
 * old listening socket becomes a zombie (bound to a destroyed interface) and
 * never receives new connections. Without this, the server cannot connect
 * after a WiFi drop until reboot. Safe to call when TCP is not active. */
esp_err_t tcp_stream_reinit_listener(void)
{
    if (s_listen_sock < 0)
        return ESP_OK;

    uint16_t port = s_listen_port;
    ESP_LOGI(TAG, "Reinitializing TCP listening socket after WiFi reconnect (port %u)",
             (unsigned)port);

    /* Unblock any in-flight send() before deinit, otherwise deinit may wait
     * on s_client_mutex held by a blocked send() for up to SO_SNDTIMEO. */
    tcp_stream_abort();

    tcp_stream_deinit();
    vTaskDelay(pdMS_TO_TICKS(50));
    return tcp_stream_init_listen(port);
}

void tcp_stream_close_client(void)
{
    /* Close ONLY the active client connection. Listener + accept task stay
     * alive so the next init_listen() can reuse them (avoids EADDRINUSE).
     * Bug #2 / MEDIUM #32: block indefinitely on the mutex (was 200ms timeout
     * with an unsynchronized fallback). */
    if (s_client_mutex)
    {
        if (xSemaphoreTake(s_client_mutex,
                           pdMS_TO_TICKS(TCP_FRAME_SEND_DEADLINE_MS + 500)) != pdTRUE)
        {
            ESP_LOGW(TAG, "close_client: mutex timeout — force-closing");
            int sock = s_client_sock;
            if (sock >= 0)
            {
                shutdown(sock, SHUT_RDWR);
                close(sock);
            }
            s_client_sock = -1;
            return;
        }
        socket_close_safe(&s_client_sock);
        xSemaphoreGive(s_client_mutex);
    }
    else
    {
        socket_close_safe(&s_client_sock); /* best-effort, no mutex (shouldn't happen post-init) */
    }
}

/* F2-TCP (#1.1): Abort: shutdown the active client socket to unblock any
 * send() in progress. Called by teardown_pipeline BEFORE waiting for the TX
 * task to exit, so a send() blocked on SO_SNDTIMEO (up to 2s) returns
 * immediately with ENOTCONN/EPIPE and the TX task can self-exit cleanly
 * within the stop timeout. Without this, stop_streaming() can wait the
 * full STREAM_STOP_TIMEOUT_TCP_MS and then force-delete the TX task while
 * it still holds s_client_mutex — orphaning the mutex and corrupting lwIP
 * state.
 *
 * Best-effort: tries to take s_client_mutex briefly (50ms). If taken,
 * shutdown() is called while holding the mutex, then the mutex is released.
 * If the mutex is busy (send() holds it), shutdown() is called without the
 * mutex — this is safe because shutdown() on a stale/recycled fd is harmless,
 * and the accept task also cannot take the mutex while send() holds it,
 * so fd recycling is impossible in that window. */
void tcp_stream_abort(void)
{
    /* Take mutex if possible (so accept task can't recycle fd while we
     * shutdown). If mutex busy, send() holds it — accept also can't take
     * it, so fd recycling is impossible. Either way, shutdown is safe. */
    bool took = (s_client_mutex &&
                 xSemaphoreTake(s_client_mutex, pdMS_TO_TICKS(50)) == pdTRUE);
    int sock = s_client_sock;
    if (sock >= 0)
    {
        shutdown(sock, SHUT_RDWR); /* unblocks send() immediately */
    }
    if (took)
    {
        xSemaphoreGive(s_client_mutex);
    }
}

bool tcp_stream_is_ready(void)
{
    /* M6: take the mutex for acquire/release semantics, mirroring
     * udp_stream_is_ready. On Xtensa the int read is atomic, but without a
     * barrier the tx task may briefly observe a stale value. */
    bool r = false;
    if (s_client_mutex && xSemaphoreTake(s_client_mutex, pdMS_TO_TICKS(SEND_MUTEX_TIMEOUT_MS)) == pdTRUE)
    {
        r = (s_client_sock >= 0);
        xSemaphoreGive(s_client_mutex);
    }
    else
    {
        r = (s_client_sock >= 0); /* best-effort fallback */
    }
    return r;
}

/* Static frame buffer — single-threaded (only tcp_stream_send calls this, and
 * it holds s_client_mutex for its full duration, so no concurrent entry).
 * TCP_MAX_PAYLOAD = 1400 to match UDP/RAWTX (LOW #37). */
#define TCP_MAX_PAYLOAD 1400
static uint8_t s_frame_buf[2 + TCP_MAX_PAYLOAD];

esp_err_t tcp_stream_send(const uint8_t *data, size_t len)
{
    if (!data || !len)
        return ESP_ERR_INVALID_ARG;
    if (len > TCP_MAX_PAYLOAD)
        return ESP_ERR_INVALID_SIZE; /* AUDIT-MEDIUM / LOW #37 */

    /* MEDIUM #9 / LOW #38: hold s_client_mutex across the entire send() loop
     * so the accept task cannot shutdown+close the fd and recycle its number
     * for a new client while send() is in flight. MEDIUM #10: removed the
     * unsynchronized timeout-fallback paths that could clobber a new client. */
    if (!s_client_mutex)
        return ESP_ERR_INVALID_STATE; /* not initialized */
    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    int sock = s_client_sock;
    if (sock < 0)
    {
        xSemaphoreGive(s_client_mutex);
        return ESP_ERR_INVALID_STATE; /* T6: state, not arg, error */
    }

    /* Build the FULL frame: [u16 length BE][data]. Blocking send (SO_SNDTIMEO)
     * — TCP stack drains in background; backpressure flows through task
     * queues. s_frame_buf is safe under the mutex (no concurrent entry). */
    s_frame_buf[0] = (uint8_t)((len >> 8) & 0xFF);
    s_frame_buf[1] = (uint8_t)(len & 0xFF);
    memcpy(s_frame_buf + 2, data, len);
    size_t frame_len = 2 + len;

    size_t sent = 0;
    /* F3-A HIGH #1: total frame-send deadline. The EAGAIN retry counter
     * alone is insufficient — a slow-reader client that does partial reads
     * (send() returns >0 each time, eagain_retries resets to 0 at line ~505)
     * can keep s_client_mutex held indefinitely via many short sends, each
     * bounded only by SO_SNDTIMEO. t0 marks the start of the whole frame
     * send so we can detect this and bail out (closing the client). Must
     * be < STREAM_STOP_TIMEOUT_TCP_MS (3000) so stop_streaming() never
     * force-deletes the TX task while it still holds this mutex. */
    TickType_t t0 = xTaskGetTickCount();
    /* Task 6-D #2: bound EAGAIN retries so a peer that is alive (ACKing) but
     * never reads cannot hold s_client_mutex forever (would deadlock accept/
     * deinit/close_client). F2-TCP (#2): worst-case mutex hold =
     * (TCP_EAGAIN_MAX_RETRIES + 1) * SO_SNDTIMEO. With RETRIES=0 that's
     * 1 * 2s = 2s max (one send attempt, no EAGAIN retry) — strictly under
     * STREAM_STOP_TIMEOUT_TCP_MS=3s so stop_streaming() never force-deletes
     * the TX task while it still holds this mutex.
     * Reset on successful send — bound is on CONSECUTIVE EAGAINs, not total. */
    int eagain_retries = 0;
    while (sent < frame_len)
    {
        /* Total frame-send deadline: if exceeded, close client and return.
         * Protects against slow-reader clients that do partial reads, which
         * bypass the EAGAIN counter (eagain_retries resets on each partial send). */
        if ((xTaskGetTickCount() - t0) > pdMS_TO_TICKS(TCP_FRAME_SEND_DEADLINE_MS))
        {
            ESP_LOGW(TAG, "send: frame deadline %dms exceeded, closing client", TCP_FRAME_SEND_DEADLINE_MS);
            /* We hold s_client_mutex — accept task can't recycle fd.
             * socket_close_safe does shutdown+close+NULL. */
            socket_close_safe(&s_client_sock);
            xSemaphoreGive(s_client_mutex);
            return ESP_ERR_TIMEOUT;
        }
        int w = send(sock, s_frame_buf + sent, frame_len - sent, 0);
        if (w < 0)
        {
            int saved_errno = errno;
            if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK)
            {
                if (++eagain_retries > TCP_EAGAIN_MAX_RETRIES)
                {
                    ESP_LOGW(TAG, "send: EAGAIN max retries exceeded (sent=%u/%u) -- closing client",
                             (unsigned)sent, (unsigned)frame_len);
                    if (s_client_sock == sock)
                    {
                        socket_close_safe(&s_client_sock);
                    }
                    xSemaphoreGive(s_client_mutex);
                    return ESP_ERR_TIMEOUT;
                }
                continue;
            }
            /* EPIPE/ENOTCONN/ECONNRESET/etc.: accept task already replaced
             * this fd and closed it under us, or peer closed. LOW #36:
             * close-then-clear order under the mutex. */
            ESP_LOGW(TAG, "send() failed: errno=%d (sent=%u/%u) -- closing client",
                     saved_errno, (unsigned)sent, (unsigned)frame_len);
            if (s_client_sock == sock)
            {
                socket_close_safe(&s_client_sock);
            }
            /* If s_client_sock != sock, accept() already closed the old fd and
             * installed a new one. Do NOT close sock — already closed, fd may
             * be reused. */
            xSemaphoreGive(s_client_mutex);
            return ESP_FAIL;
        }
        if (w == 0)
        {
            ESP_LOGW(TAG, "send() returned 0 -- connection closed by peer");
            if (s_client_sock == sock)
            {
                socket_close_safe(&s_client_sock);
            }
            xSemaphoreGive(s_client_mutex);
            return ESP_FAIL;
        }
        sent += (size_t)w;
        eagain_retries = 0;
    }

    xSemaphoreGive(s_client_mutex);
    return ESP_OK;
}
