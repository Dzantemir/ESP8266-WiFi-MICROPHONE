/*
 * Common socket option helpers — centralizes the setsockopt boilerplate that
 * was duplicated between udp_stream.c and tcp_stream.c (~80 lines removed).
 *
 * Each helper:
 *   - captures errno immediately after the failing call (ESP_LOGW may invoke
 *     UART syscalls that clobber it),
 *   - logs via ESP_LOGW on failure,
 *   - returns 0 on success, -1 on failure.
 */

/* ---- System / SDK includes ---- */
#include <errno.h>
#include "lwip/sockets.h"
#include "esp_log.h"
/* ---- Project includes ---- */
#include "board_config.h" 
#include "socket_util.h"

#ifndef IPTOS_DSCP_EF
#define IPTOS_DSCP_EF 0xB8
#endif

static const char *TAG = "socket_util";

int socket_set_send_timeout_ms(int sock, uint32_t timeout_ms)
{
    struct timeval tv = {
        .tv_sec = (time_t)(timeout_ms / 1000u),
        .tv_usec = (long)((timeout_ms % 1000u) * 1000u),
    };
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
        int saved_errno = errno;
        ESP_LOGW(TAG, "SO_SNDTIMEO failed: errno=%d", saved_errno);
        return -1;
    }
    return 0;
}

int socket_set_tos_ef(int sock)
{
    int tos = IPTOS_DSCP_EF;
    if (setsockopt(sock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) != 0) {
        int saved_errno = errno;
        ESP_LOGW(TAG, "IP_TOS failed: errno=%d", saved_errno);
        return -1;
    }
    return 0;
}

int socket_set_reuseaddr(int sock)
{
    int reuse = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        int saved_errno = errno;
        ESP_LOGW(TAG, "SO_REUSEADDR failed: errno=%d", saved_errno);
        return -1;
    }
    return 0;
}

int socket_set_keepalive(int sock, uint32_t idle_sec, uint32_t intvl_sec, uint32_t cnt)
{
    int keepalive = 1;
    int keepidle  = (int)idle_sec;
    int keepintvl = (int)intvl_sec;
    int keepcnt   = (int)cnt;
    int rc = 0;

    if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)) != 0) {
        int saved_errno = errno;
        ESP_LOGW(TAG, "SO_KEEPALIVE failed: errno=%d", saved_errno);
        rc = -1;
    }
    if (setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle)) != 0) {
        int saved_errno = errno;
        ESP_LOGW(TAG, "TCP_KEEPIDLE failed: errno=%d", saved_errno);
        rc = -1;
    }
    if (setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl)) != 0) {
        int saved_errno = errno;
        ESP_LOGW(TAG, "TCP_KEEPINTVL failed: errno=%d", saved_errno);
        rc = -1;
    }
    if (setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt)) != 0) {
        int saved_errno = errno;
        ESP_LOGW(TAG, "TCP_KEEPCNT failed: errno=%d", saved_errno);
        rc = -1;
    }
    return rc;
}

int socket_set_linger(int sock, bool on, uint32_t timeout_sec)
{
    struct linger ling;
    ling.l_onoff  = on ? 1 : 0;
    ling.l_linger = (int)timeout_sec;
    if (setsockopt(sock, SOL_SOCKET, SO_LINGER, &ling, sizeof(ling)) != 0) {
        int saved_errno = errno;
        ESP_LOGW(TAG, "SO_LINGER failed: errno=%d", saved_errno);
        return -1;
    }
    return 0;
}

int socket_set_nodelay(int sock)
{
    int flag = 1;
    if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) != 0) {
        int saved_errno = errno;
        ESP_LOGW(TAG, "TCP_NODELAY failed: errno=%d", saved_errno);
        return -1;
    }
    return 0;
}

void socket_close_safe(int *sock)
{
    if (!sock || *sock < 0)
        return;
    shutdown(*sock, SHUT_RDWR);
    close(*sock);
    *sock = -1;
}
