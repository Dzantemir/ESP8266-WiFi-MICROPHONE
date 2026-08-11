#ifndef BOARD_NETWORK_H
#define BOARD_NETWORK_H

/* Service port, transport modes, TCP/UDP/timeout config.
 * Extracted from board_config.h (R3-C). */

#include "sdkconfig.h"
#include <stdint.h>
#include <stdbool.h>

/* ====================================================================
 *  Service Port (EASSP) — configured via Kconfig
 * ==================================================================== */

#ifdef CONFIG_STREAMER_SVC_PORT
#define SVC_PORT_DEFAULT         CONFIG_STREAMER_SVC_PORT
#else
#define SVC_PORT_DEFAULT         3950
#endif

#ifdef CONFIG_STREAMER_SVC_WATCHDOG_TIMEOUT_MS
#define SVC_WATCHDOG_TIMEOUT_MS  CONFIG_STREAMER_SVC_WATCHDOG_TIMEOUT_MS
#else
#define SVC_WATCHDOG_TIMEOUT_MS  15000
#endif

#ifdef CONFIG_STREAMER_SVC_INFO_INTERVAL_MS
#define SVC_INFO_INTERVAL_MS     CONFIG_STREAMER_SVC_INFO_INTERVAL_MS
#else
#define SVC_INFO_INTERVAL_MS     1000
#endif

/* Broadcast announcements — controlled by Kconfig STREAMER_SVC_ANNOUNCE_ENABLED. */
#ifdef CONFIG_STREAMER_SVC_ANNOUNCE_ENABLED
#define SVC_ANNOUNCE_ENABLED     1
#else
#define SVC_ANNOUNCE_ENABLED     0
#endif

#ifdef CONFIG_STREAMER_SVC_ANNOUNCE_MIN_MS
#define SVC_ANNOUNCE_MIN_MS      CONFIG_STREAMER_SVC_ANNOUNCE_MIN_MS
#else
#define SVC_ANNOUNCE_MIN_MS      1000
#endif

#ifdef CONFIG_STREAMER_SVC_ANNOUNCE_MAX_MS
#define SVC_ANNOUNCE_MAX_MS      CONFIG_STREAMER_SVC_ANNOUNCE_MAX_MS
#else
#define SVC_ANNOUNCE_MAX_MS      5000
#endif

#ifdef CONFIG_STREAMER_SVC_RECV_BUF_SIZE
#define SVC_RECV_BUF_SIZE        CONFIG_STREAMER_SVC_RECV_BUF_SIZE
#else
#define SVC_RECV_BUF_SIZE        256
#endif

#ifdef CONFIG_STREAMER_SVC_RECONFIGURE_STOP_TIMEOUT_MS
#define SVC_RECONFIGURE_STOP_TIMEOUT_MS  CONFIG_STREAMER_SVC_RECONFIGURE_STOP_TIMEOUT_MS
#else
#define SVC_RECONFIGURE_STOP_TIMEOUT_MS  2000
#endif

/* ====================================================================
 *  Transport modes
 * ====================================================================
 * Transport mode for audio stream.
 *   0 = UDP   — datagram, no delivery/order guarantees.
 *   1 = TCP   — ESP = listener (server connects). Framing: [u16 len][hdr 16B][payload].
 *                TCP_NODELAY + non-blocking send (drop on overflow, like UDP).
 *   2 = RawTX — broadcast raw 802.11 frames (receiver in monitor mode).
 * Applied at AT+HOTRESTART (UDP<->TCP) or AT+RST (RAWTX transitions). */
#define TRANSPORT_MODE_UDP    0
#define TRANSPORT_MODE_TCP    1
#define TRANSPORT_MODE_RAWTX  2

#ifdef CONFIG_STREAMER_TRANSPORT_MODE
#define TRANSPORT_MODE_DEFAULT CONFIG_STREAMER_TRANSPORT_MODE
#else
#define TRANSPORT_MODE_DEFAULT TRANSPORT_MODE_UDP
#endif

/* ====================================================================
 *  TCP transport tuning (only used when transport_mode == TRANSPORT_MODE_TCP)
 * ====================================================================
 *   SEND_TIMEOUT_MS — blocking send() timeout; on expiry the connection is
 *     closed and accept task takes a new connect. LAN: 1000-2000, bad WiFi: 3000-5000.
 *   ACCEPT_TASK_STACK — accept task stack. 1024 = minimum. */
#ifdef CONFIG_STREAMER_TCP_SEND_TIMEOUT_MS
#define TCP_SEND_TIMEOUT_MS CONFIG_STREAMER_TCP_SEND_TIMEOUT_MS
#else
#define TCP_SEND_TIMEOUT_MS 2000
#endif

/* FR-TCP (#1, #22) + F2-TCP (#2): max CONSECUTIVE EAGAIN/EWOULDBLOCK retries
 * in tcp_stream_send() AFTER the first EAGAIN. Total worst-case send() mutex
 * hold time = (TCP_EAGAIN_MAX_RETRIES + 1) * TCP_SEND_TIMEOUT_MS (the "+1"
 * accounts for the first send attempt, which is always allowed regardless of
 * the retry cap — see the loop in tcp_stream_send: `if (++eagain_retries >
 * TCP_EAGAIN_MAX_RETRIES)` fires AFTER the Nth EAGAIN).
 *
 * F2-TCP (#2): value lowered 1 -> 0. With RETRIES=1 the off-by-one in the
 * `++retries > MAX` check actually allowed 2 send attempts (4s worst-case),
 * which EXCEEDS STREAM_STOP_TIMEOUT_TCP_MS=3s — a stuck peer would let
 * stop_streaming() force-delete the TX task while it still held
 * s_client_mutex. RETRIES=0 allows exactly one send attempt (2s max), no
 * EAGAIN retry — well under the 3s stop timeout. The accept-task mutex-take
 * timeout of 500ms (F-D #8) is also now longer than the worst-case send, so
 * accept no longer refuses new clients during a stuck send.
 *
 * The cross-validation #error in board_config.h uses (RETRIES + 1) to match
 * the actual worst-case send() time (NOT just RETRIES * TIMEOUT). */
#define TCP_EAGAIN_MAX_RETRIES      0   /* 0 = one send attempt (2s max), no EAGAIN retry */

/* F3-A HIGH #1: total max wall-clock time to send one complete frame (sum of
 * all send() calls for the frame, including partial sends and EAGAIN retries).
 * The EAGAIN retry counter alone is insufficient — a slow-reader client that
 * does partial reads (send() returns >0 each time, eagain_retries resets)
 * can keep s_client_mutex held indefinitely via many short sends, each bounded
 * only by TCP_SEND_TIMEOUT_MS. This deadline bounds the WHOLE frame send.
 * Must be < STREAM_STOP_TIMEOUT_TCP_MS so stop_streaming() never force-deletes
 * the TX task while it still holds s_client_mutex (see cross-validation #error
 * in board_config.h). */
#define TCP_FRAME_SEND_DEADLINE_MS   2500  /* total max time to send one frame;
                                           * must be < STREAM_STOP_TIMEOUT_TCP_MS (3000) */

#ifdef CONFIG_STREAMER_TCP_ACCEPT_TASK_STACK
#define TCP_ACCEPT_TASK_STACK CONFIG_STREAMER_TCP_ACCEPT_TASK_STACK
#else
#define TCP_ACCEPT_TASK_STACK 1024
#endif

#ifdef CONFIG_STREAMER_TASK_PRIO_TCP_ACCEPT
#define TCP_ACCEPT_TASK_PRIO CONFIG_STREAMER_TASK_PRIO_TCP_ACCEPT
#else
#define TCP_ACCEPT_TASK_PRIO 4
#endif

/* ====================================================================
 *  TCP / UDP socket options
 * ====================================================================
 * ---- TCP socket options ----
 * Macro names use CFG_ prefix to avoid collisions with lwIP socket option
 * constants (TCP_NODELAY, TCP_KEEPIDLE, TCP_KEEPINTVL, TCP_KEEPCNT are all
 * defined in lwIP/sockets.h). */
#ifdef CONFIG_STREAMER_TCP_NODELAY_ENABLED
#define CFG_TCP_NODELAY_ENABLED    1
#else
#define CFG_TCP_NODELAY_ENABLED    0
#endif

#ifdef CONFIG_STREAMER_UDP_SEND_TIMEOUT_MS_ENABLED
#define CFG_UDP_SEND_TIMEOUT_MS_ENABLED  1
#else
#define CFG_UDP_SEND_TIMEOUT_MS_ENABLED  0
#endif

#ifdef CONFIG_STREAMER_TCP_SEND_TIMEOUT_MS_ENABLED
#define CFG_TCP_SEND_TIMEOUT_MS_ENABLED  1
#else
#define CFG_TCP_SEND_TIMEOUT_MS_ENABLED  0
#endif

/* F2-TCP (#4): TCP send timeout (SO_SNDTIMEO via socket_set_send_timeout_ms)
 * is the ONLY thing that bounds how long tcp_stream_send() can block while
 * holding s_client_mutex. Without it, a peer that stops reading (TCP window
 * full) blocks send() forever — accept/deinit/close_client all need the same
 * mutex and deadlock, AND stop_streaming() force-deletes the TX task while
 * it still holds the mutex (orphaning the mutex and corrupting lwIP state
 * on the next start_streaming). FR-TCP #21 already defaulted the Kconfig
 * to `y`, but a user can still set it to `n` in menuconfig — this guard
 * makes that impossible. The F2-TCP abort/reboot safety net (Fix 1) only
 * fires when stop_streaming() is called; without a send timeout, a stuck
 * peer deadlocks accept/deinit mid-stream (not just on stop), which no
 * amount of abort can recover from. */
#if !CFG_TCP_SEND_TIMEOUT_MS_ENABLED
#warning "TCP send timeout MUST be enabled (STREAMER_TCP_SEND_TIMEOUT_MS_ENABLED=y) - without it, send() can block forever holding the mutex, causing deadlock on stop"
#endif

#ifdef CONFIG_STREAMER_UDP_RECEIVE_TIMEOUT_MS_ENABLED
#define CFG_UDP_RECEIVE_TIMEOUT_MS_ENABLED  1
#else
#define CFG_UDP_RECEIVE_TIMEOUT_MS_ENABLED  0
#endif

/* ---- TCP keepalive ---- */
#ifdef CONFIG_STREAMER_TCP_KEEPALIVE_ENABLED
#define CFG_TCP_KEEPALIVE_ENABLED  1
#else
#define CFG_TCP_KEEPALIVE_ENABLED  0
#endif

#ifdef CONFIG_STREAMER_TCP_KEEPIDLE
#define CFG_TCP_KEEPIDLE_SEC       CONFIG_STREAMER_TCP_KEEPIDLE
#else
#define CFG_TCP_KEEPIDLE_SEC       10
#endif

#ifdef CONFIG_STREAMER_TCP_KEEPINTVL
#define CFG_TCP_KEEPINTVL_SEC      CONFIG_STREAMER_TCP_KEEPINTVL
#else
#define CFG_TCP_KEEPINTVL_SEC      3
#endif

#ifdef CONFIG_STREAMER_TCP_KEEPCNT
#define CFG_TCP_KEEPCNT            CONFIG_STREAMER_TCP_KEEPCNT
#else
#define CFG_TCP_KEEPCNT            3
#endif

#ifdef CONFIG_STREAMER_TCP_LINGER_ENABLED
#define CFG_TCP_LINGER_ENABLED     1
#else
#define CFG_TCP_LINGER_ENABLED     0
#endif

/* ====================================================================
 *  Network / UDP
 * ==================================================================== */

#ifdef CONFIG_STREAMER_UDP_SEND_TIMEOUT_MS
#define UDP_SEND_TIMEOUT_MS CONFIG_STREAMER_UDP_SEND_TIMEOUT_MS
#else
#define UDP_SEND_TIMEOUT_MS 2000
#endif

#ifdef CONFIG_STREAMER_UDP_RECEIVE_TIMEOUT_MS
#define UDP_RECEIVE_TIMEOUT_MS CONFIG_STREAMER_UDP_RECEIVE_TIMEOUT_MS
#else
#define UDP_RECEIVE_TIMEOUT_MS 2000
#endif

/* ====================================================================
 *  Stream stop timeouts (split per-transport — FIX split, see FIXES.md)
 * ====================================================================
 * PCM/ADPCM pool sizes are computed at runtime in start_streaming() from
 * free heap and frame_ms — see main.c. No compile-time setting needed. */

/* FIX (split): see FIXES.md — separate stop timeouts for UDP and TCP. */
#ifdef CONFIG_STREAMER_STREAM_STOP_TIMEOUT_UDP_MS
#define STREAM_STOP_TIMEOUT_UDP_MS  CONFIG_STREAMER_STREAM_STOP_TIMEOUT_UDP_MS
#else
/* Default: must be > UDP_SEND_TIMEOUT_MS (2000). */
#define STREAM_STOP_TIMEOUT_UDP_MS  3000
#endif

#ifdef CONFIG_STREAMER_STREAM_STOP_TIMEOUT_TCP_MS
#define STREAM_STOP_TIMEOUT_TCP_MS  CONFIG_STREAMER_STREAM_STOP_TIMEOUT_TCP_MS
#else
/* Default: must be > TCP_SEND_TIMEOUT_MS (2000). */
#define STREAM_STOP_TIMEOUT_TCP_MS  3000
#endif

/* I2S DMA buffer dimensions are DERIVED from samples_per_frame at runtime
 * (see i2s_capture_init). One PCM frame = 4 DMA buffers; DMA buffer count
 * gives ~256ms of headroom. */

#endif /* BOARD_NETWORK_H */
