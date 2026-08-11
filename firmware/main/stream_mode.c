/*
 * Stream Mode Implementations
 * ===========================
 *
 * Three ops tables: UDP, TCP, Raw 802.11 TX. Each groups ALL mode-specific
 * behavior into a single place.
 *
 * main.c (and pipeline tasks) calls stream_mode_ops() / transport_*() and
 * is completely unaware of which transport is active.
 *
 * Transport modules are independent:
 *   udp_stream.c    — UDP socket (lwIP)
 *   tcp_stream.c    — TCP listener + blocking send with framing
 *   rawtx_stream.c  — Raw 802.11 TX via esp_wifi_80211_tx()
 */

/* CONCURRENCY: s_active_ops and s_active_transport are set once in
 * stream_mode_init() at boot. start_streaming() may call stream_mode_init()
 * again on HOTRESTART (UDP<->TCP switch), but this happens in the main task
 * context, and transport_*() wrappers are only called from the main task
 * and pipeline tasks (which are stopped during HOTRESTART). On ESP8266
 * (single-core, 32-bit aligned pointer writes are atomic), the lock-free
 * read in transport_*() wrappers is safe in practice. For multi-core ports,
 * add a mutex or use _Atomic. */

/* ---- System / SDK includes ---- */
#include "esp_log.h"

/* ---- Project includes ---- */
#include "board_config.h"
#include "stream_mode.h"
#include "wifi_sta.h"
#include "svc_port.h"
#include "udp_stream.h"
#include "tcp_stream.h"
#include "rawtx_stream.h"

static const char *TAG = "stream_mode";

/* ====================================================================
 * UDP Mode - connect to AP, send audio via UDP socket to a server.
 * Uses service port (EASSP) for discovery/control.
 * ==================================================================== */

static esp_err_t udp_wifi_init(const device_config_t *cfg)
{
    esp_err_t err = wifi_sta_init(cfg->wifi_ssid, cfg->wifi_password,
                                  cfg->hostname, cfg->tx_power);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "UDP: WiFi STA init failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

static esp_err_t udp_wifi_wait_ready(const device_config_t *cfg)
{
    if (!wifi_sta_is_connected())
    {
        ESP_LOGW(TAG, "UDP: WiFi not connected, waiting up to %d ms",
                 WIFI_CONNECT_TIMEOUT_MS);
        esp_err_t err = wifi_sta_wait_connected(WIFI_CONNECT_TIMEOUT_MS);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "UDP: WiFi connect timeout");
            return ESP_ERR_INVALID_STATE;
        }
    }
    ESP_LOGI(TAG, "UDP: WiFi connected");
    return ESP_OK;
}

static esp_err_t udp_get_stream_dest(uint32_t *host, uint16_t *port)
{
    if (!svc_port_get_stream_dest(host, port))
    {
        ESP_LOGE(TAG, "UDP: no CONFIGURE received - no stream destination");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static void udp_set_channels(uint8_t channels)
{
    /* F3-A MEDIUM #5: no-op — svc_port_set_channels() is now a no-op;
     * channels come from streaming_get_channels() in build_info_payload.
     * Kept for API compat (vtable contract requires a set_channels slot). */
    svc_port_set_channels(channels);
}

static esp_err_t udp_svc_port_init(EventGroupHandle_t evt_grp, uint16_t port)
{
    if (svc_port_is_running())
    {
        return ESP_OK;
    }
    return svc_port_init(port, evt_grp);
}

static esp_err_t udp_transport_init(uint32_t host, uint16_t port)
{
    return udp_stream_init(host, port);
}

static void udp_on_stream_started(void)
{
    svc_port_clear_error();
    svc_port_notify_streaming_started();
}

static void udp_on_stream_stopped(void)
{
    svc_port_notify_streaming_stopped();
    svc_port_notify_stop_complete();
}

/* Thin void wrappers for close_client (avoids casting esp_err_t(*)(void)
 * to void(*)(void) — technically UB, though it works in practice). */
static void udp_close_client(void)
{
    udp_stream_deinit();
}

/* F2-TCP (#1.2): UDP sendto() is non-blocking (or bounded by UDP_SEND_TIMEOUT_MS
 * when enabled, which is short). No abort hook needed. */
static void udp_abort_send(void)
{
    /* no-op */
}

static const stream_mode_ops_t s_udp_ops = {
    .name = "UDP",
    .wifi_init = udp_wifi_init,
    .wifi_wait_ready = udp_wifi_wait_ready,
    .get_stream_dest = udp_get_stream_dest,
    .set_channels = udp_set_channels,
    .svc_port_init = udp_svc_port_init,
    .transport_init = udp_transport_init,
    .is_ready = udp_stream_is_ready,
    .send = udp_stream_send,
    .close_client = udp_close_client,
    .abort_send = udp_abort_send,
    .deinit = udp_stream_deinit,
    .on_stream_started = udp_on_stream_started,
    .on_stream_stopped = udp_on_stream_stopped,
    .needs_wifi_association = true,
    .auto_start = false,
    .uses_svc_port = true,
};

/* ====================================================================
 * Raw 802.11 TX Mode - no AP, broadcast raw WiFi frames on a fixed channel.
 * Receiver must be in Monitor Mode. No service port, no discovery.
 * ==================================================================== */

static esp_err_t rawtx_wifi_init(const device_config_t *cfg)
{
    return wifi_sta_init_raw(cfg->wifi_channel, cfg->tx_power);
}

static esp_err_t rawtx_wifi_wait_ready(const device_config_t *cfg)
{
    /* Radio readiness is already ensured inside rawtx_wifi_init() ->
     * wifi_sta_init_raw(): it blocks until the STA_START event handler has
     * finished running set_channel/set_protocol(11B)/set_fixed_rate(11M). */
    (void)cfg;
    return ESP_OK;
}

static esp_err_t rawtx_get_stream_dest(uint32_t *host, uint16_t *port)
{
    /* Raw TX broadcasts to FF:FF:FF:FF:FF:FF — no destination to resolve.
     * Zero the outputs so callers see a defined value (AUDIT-MEDIUM). */
    if (host) *host = 0;
    if (port) *port = 0;
    return ESP_OK;
}

static void rawtx_set_channels(uint8_t channels)
{
    /* No service port in raw TX mode. */
    (void)channels;
}

static esp_err_t rawtx_svc_port_init(EventGroupHandle_t evt_grp, uint16_t port)
{
    /* No service port in raw TX mode. */
    (void)evt_grp;
    (void)port;
    return ESP_OK;
}

static esp_err_t rawtx_transport_init(uint32_t host, uint16_t port)
{
    /* host/port are unused - raw TX builds its own 802.11 header. */
    (void)host;
    (void)port;
    return rawtx_stream_init();
}

static void rawtx_on_stream_started(void)
{
    /* No service port to notify. */
}

static void rawtx_on_stream_stopped(void)
{
    /* No service port to notify. */
}

static void rawtx_close_client(void)
{
    rawtx_stream_deinit();
}

/* F2-TCP (#1.2): RawTX send (esp_wifi_80211_tx) is non-blocking and has no
 * peer to backpressure it. No abort hook needed. */
static void rawtx_abort_send(void)
{
    /* no-op */
}

static const stream_mode_ops_t s_rawtx_ops = {
    .name = "Raw 802.11 TX",
    .wifi_init = rawtx_wifi_init,
    .wifi_wait_ready = rawtx_wifi_wait_ready,
    .get_stream_dest = rawtx_get_stream_dest,
    .set_channels = rawtx_set_channels,
    .svc_port_init = rawtx_svc_port_init,
    .transport_init = rawtx_transport_init,
    .is_ready = rawtx_stream_is_ready,
    .send = rawtx_stream_send,
    .close_client = rawtx_close_client,
    .abort_send = rawtx_abort_send,
    .deinit = rawtx_stream_deinit,
    .on_stream_started = rawtx_on_stream_started,
    .on_stream_stopped = rawtx_on_stream_stopped,
    .needs_wifi_association = false,
    .auto_start = true,
    .uses_svc_port = false,
};

/* ====================================================================
 * TCP Mode - connect to AP, ESP = TCP listener, server connects to us.
 * Uses service port (EASSP) for discovery/control — same as UDP.
 * Differs from UDP only in transport_init: opens TCP listening socket
 * instead of UDP send socket. stream_port from CONFIGURE = TCP port
 * we listen on (server connects to ESP_IP:stream_port).
 * ==================================================================== */

static esp_err_t tcp_wifi_init(const device_config_t *cfg)
{
    esp_err_t err = wifi_sta_init(cfg->wifi_ssid, cfg->wifi_password,
                                  cfg->hostname, cfg->tx_power);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "TCP: WiFi STA init failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

static esp_err_t tcp_wifi_wait_ready(const device_config_t *cfg)
{
    if (!wifi_sta_is_connected())
    {
        ESP_LOGW(TAG, "TCP: WiFi not connected, waiting up to %d ms",
                 WIFI_CONNECT_TIMEOUT_MS);
        esp_err_t err = wifi_sta_wait_connected(WIFI_CONNECT_TIMEOUT_MS);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "TCP: WiFi connect timeout");
            return ESP_ERR_INVALID_STATE;
        }
    }
    ESP_LOGI(TAG, "TCP: WiFi connected");
    return ESP_OK;
}

/* TCP reuses UDP's discovery (get_stream_dest, set_channels, svc_port_init,
 * on_stream_started, on_stream_stopped) — the service port and EASSP
 * protocol are identical. Only transport_init differs. */

static esp_err_t tcp_transport_init(uint32_t host, uint16_t port)
{
    /* host arg from CONFIGURE is ignored (we = listener, not client).
     * port = stream_port from CONFIGURE = TCP port we listen on. */
    (void)host;
    return tcp_stream_init_listen(port);
}

static const stream_mode_ops_t s_tcp_ops = {
    .name = "TCP",
    .wifi_init = tcp_wifi_init,
    .wifi_wait_ready = tcp_wifi_wait_ready,
    .get_stream_dest = udp_get_stream_dest,   /* same discovery as UDP */
    .set_channels = udp_set_channels,
    .svc_port_init = udp_svc_port_init,
    .transport_init = tcp_transport_init,
    .is_ready = tcp_stream_is_ready,
    .send = tcp_stream_send,
    .close_client = tcp_stream_close_client,
    .abort_send = tcp_stream_abort,           /* F2-TCP (#1.2) */
    .deinit = tcp_stream_deinit,
    .on_stream_started = udp_on_stream_started,
    .on_stream_stopped = udp_on_stream_stopped,
    .needs_wifi_association = true,
    .auto_start = false,
    .uses_svc_port = true,
};

/* ====================================================================
 * Active mode selection
 * ==================================================================== */

static const stream_mode_ops_t *s_active_ops = &s_udp_ops;
/* Remember the transport mode so main.c can pick the right STREAM_STOP_TIMEOUT_*_MS
 * (UDP vs TCP). RawTX uses UDP value as a safe default.
 *
 * Concurrency contract for s_active_ops / s_active_transport (read without a
 * mutex in the transport_*() wrappers below): see the CONCURRENCY note at the
 * top of this file. Short version: stream_mode_init() runs at boot AND on the
 * HOTRESTART path (from main task, with pipeline tasks stopped); on ESP8266
 * (single-core, atomic aligned-pointer writes) the lock-free read is safe. */
static uint8_t s_active_transport = TRANSPORT_MODE_UDP;

void stream_mode_init(const device_config_t *cfg)
{
    switch (cfg->transport_mode)
    {
    case TRANSPORT_MODE_TCP:
        s_active_ops = &s_tcp_ops;
        s_active_transport = TRANSPORT_MODE_TCP;
        break;
    case TRANSPORT_MODE_RAWTX:
        s_active_ops = &s_rawtx_ops;
        s_active_transport = TRANSPORT_MODE_RAWTX;
        break;
    default:
        /* MEDIUM #25: normalize any non-TCP/non-RAWTX value (including
         * TRANSPORT_MODE_UDP and any invalid enum) to UDP. Keeps
         * s_active_ops and s_active_transport consistent so main.c picks
         * the right STREAM_STOP_TIMEOUT_*_MS. Logs a warning only for
         * genuinely invalid modes. */
        s_active_ops = &s_udp_ops;
        s_active_transport = TRANSPORT_MODE_UDP;
        if (cfg->transport_mode != TRANSPORT_MODE_UDP) {
            ESP_LOGW(TAG, "invalid transport_mode %d, defaulting to UDP",
                     (int)cfg->transport_mode);
        }
        break;
    }
    ESP_LOGI(TAG, "Stream mode: %s", s_active_ops->name);
}

const stream_mode_ops_t *stream_mode_ops(void)
{
    return s_active_ops;
}

uint8_t stream_mode_current_transport(void)
{
    return s_active_transport;
}

const stream_mode_ops_t *stream_mode_ops_for(uint8_t transport_mode)
{
    switch (transport_mode)
    {
    case TRANSPORT_MODE_TCP:   return &s_tcp_ops;
    case TRANSPORT_MODE_RAWTX: return &s_rawtx_ops;
    default:                   return &s_udp_ops;
    }
}



/* ---- Transport-agnostic wrappers (pure vtable dispatch, no if-branches) ---- */

bool transport_is_ready(void)
{
    return s_active_ops->is_ready();
}

esp_err_t transport_send(const uint8_t *data, size_t len)
{
    return s_active_ops->send(data, len);
}

esp_err_t transport_deinit(void)
{
    return s_active_ops->deinit();
}

void transport_close_client(void)
{
    s_active_ops->close_client();
}

/* F2-TCP (#1.2): abort any in-flight blocking send() so the TX task can
 * self-exit within the stop timeout. No-op for UDP/RAWTX (see vtable docs). */
void transport_abort_send(void)
{
    s_active_ops->abort_send();
}
