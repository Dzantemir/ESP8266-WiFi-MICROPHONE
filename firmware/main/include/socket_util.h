#ifndef SOCKET_UTIL_H
#define SOCKET_UTIL_H

#include <stdbool.h>
#include <stdint.h>

/* Set SO_SNDTIMEO on a socket. Returns 0 on success, -1 on error (errno set). */
int socket_set_send_timeout_ms(int sock, uint32_t timeout_ms);

/* Set IP_TOS to IPTOS_DSCP_EF (voice priority). Returns 0 on success. */
int socket_set_tos_ef(int sock);

/* Set SO_REUSEADDR. Returns 0 on success. */
int socket_set_reuseaddr(int sock);

/* Set TCP keepalive (idle/intvl/cnt in seconds). Returns 0 on success. */
int socket_set_keepalive(int sock, uint32_t idle_sec, uint32_t intvl_sec, uint32_t cnt);

/* Set SO_LINGER (on/off, timeout_sec). Returns 0 on success. */
int socket_set_linger(int sock, bool on, uint32_t timeout_sec);

/* Set TCP_NODELAY (disable Nagle). Returns 0 on success. */
int socket_set_nodelay(int sock);

/* Safely close a socket: shutdown + close, set fd to -1. */
void socket_close_safe(int *sock);

#endif /* SOCKET_UTIL_H */
