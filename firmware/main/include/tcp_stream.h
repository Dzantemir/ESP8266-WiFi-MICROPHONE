#ifndef TCP_STREAM_H
#define TCP_STREAM_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * TCP transport for audio streaming.
 *
 * ESP = listener (TCP server). After CONFIGURE, opens a listening socket on
 * stream_port and waits for the receiver to connect. One active connection
 * at a time.
 *
 * Framing (TCP has no message boundaries):
 *   Each frame = [u16 length BE][16-byte pkt_header][payload]
 *   length = 16 + payload_len (<= 1400, fits u16).
 *   Receiver: read(2) length -> read(length) -> parse as UDP-like packet.
 *
 * Backpressure:
 *   send() is BLOCKING with SO_SNDTIMEO=2s. If the receiver is slow, send
 *   blocks -> ADPCM queue fills -> I2S drops frames (natural backpressure,
 *   as in UDP). Avoids the deadlock that non-blocking send + select() caused.
 *
 * Lifecycle:
 *   tcp_stream_init_listen(port)   — open listener, accept in background
 *   tcp_stream_is_ready()          — true if an active client is connected
 *   tcp_stream_send(data, len)     — send a frame (framing + blocking)
 *   tcp_stream_close_client()      — close ONLY the client (listener stays)
 *   tcp_stream_deinit()            — close everything (listener + client + task)
 */

/* Open a listening socket on `port` and start accepting connections in a
 * background task. On a new connect, the old connection is closed (1 client
 * at a time). `port` comes from CONFIGURE (semantically reused, like UDP). */
esp_err_t tcp_stream_init_listen(uint16_t port);

/* Close listener + active connection + stop the accept task. */
esp_err_t tcp_stream_deinit(void);

/* FIX (WiFi reconnect): re-create the listening socket after WiFi
 * disconnect/reconnect — the old one is a zombie bound to a destroyed netif.
 * No-op if TCP is not initialized (s_listen_sock < 0). */
esp_err_t tcp_stream_reinit_listener(void);

/* Close ONLY the active client connection (keep listener + accept task).
 * Used on stream stop so the listening socket stays alive for fast restart
 * (no EADDRINUSE). */
void tcp_stream_close_client(void);

/* F2-TCP (#1.1): Abort any in-flight blocking send() by shutting down the
 * active client socket. Called by teardown_pipeline() BEFORE waiting for the
 * TX task to exit, so a send() blocked on SO_SNDTIMEO returns immediately
 * and the TX task can self-exit within the stop timeout (instead of being
 * force-deleted while holding s_client_mutex). No-op if no client is
 * connected. Safe to call multiple times. */
void tcp_stream_abort(void);

/* true if there is an active client connection ready for send. */
bool tcp_stream_is_ready(void);

/* Send an audio frame. data = [pkt_header 16B][payload], len = 16+payload.
 * Adds a 2-byte length prefix (BE) and writes to the socket (blocking,
 * SO_SNDTIMEO=2s). On timeout/disconnect, closes the client socket (the
 * accept task will pick up a new one). */
esp_err_t tcp_stream_send(const uint8_t *data, size_t len);

#endif /* TCP_STREAM_H */
