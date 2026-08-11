/*
 * Raw 802.11 TX transport — broadcasts raw WiFi data frames directly into
 * the air on the current channel (no router/AP association needed).
 * Receiver must be in Monitor Mode on the same channel.
 *
 * Independent module — does not share state with udp_stream.c or tcp_stream.c.
 */

/* ---- System / SDK includes ---- */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "esp_log.h"

/* ---- Project includes ---- */
#include "board_config.h" 
#include "rawtx_stream.h"

static const char *TAG = "rawtx";

/* 802.11 MAC header for Data frames (24 bytes).
 * Layout (IEEE 802.11-2016, §9.2.4):
 *   bytes 0-1:   Frame Control
 *   bytes 2-3:   Duration/ID
 *   bytes 4-9:   Address 1 (Receiver / DA)
 *   bytes 10-15: Address 2 (Transmitter / SA)
 *   bytes 16-21: Address 3 (BSSID)
 *   bytes 22-23: Sequence Control (12-bit seq num + 4-bit frag num)
 *
 * For independent raw TX (no AP association):
 *   ToDS=0, FromDS=0, all addresses broadcast except SA (our MAC). */
#define WIFI_HDR_LEN 24
static uint8_t s_wifi_hdr[WIFI_HDR_LEN];

/* Sequence number for 802.11 frames (12-bit, wraps at 4096).
 * Incremented per transmitted MSDU to allow receiver dedup. */
static uint16_t s_wifi_seq = 0;

/* AUDIT-LOW: s_ready is read by stream_task_fn (via transport_is_ready) from
 * a different task than the one that writes it (rawtx_stream_init/deinit from
 * main loop). Mark volatile for the same reason as s_running in tcp_stream.c. */
static volatile bool s_ready = false;

/* AUDIT-MEDIUM: single source of truth for the payload size cap. Was a magic
 * 1400 duplicated in s_frame_buf declaration and the len check. */
#define RAWTX_MAX_PAYLOAD 1400

/* Static frame buffer — single-threaded (only stream_task_fn calls
 * rawtx_stream_send via transport_send), so no mutex needed. Same approach as
 * tcp_stream.c. Avoids per-packet malloc/free which fragments the heap at 66+
 * allocs/sec. Max frame = 24 (wifi hdr) + 1400 (payload) = 1424 bytes. */
static uint8_t s_frame_buf[WIFI_HDR_LEN + RAWTX_MAX_PAYLOAD];

esp_err_t rawtx_stream_init(void)
{
    if (s_ready)
    {
        rawtx_stream_deinit();
        /* vTaskDelay(50ms) was here — removed: esp_wifi_80211_tx() is
         * synchronous (copies the frame into the driver's TX pool before
         * returning), so there is no async cleanup to wait for. */
    }

    /* Build 802.11 MAC header for a Data frame sent as an independent
     * station (not associated with any AP).
     *
     * Frame Control (2 bytes, little-endian):
     *   byte 0 = 0x08: Protocol=0, Type=2 (Data), Subtype=0
     *   byte 1 = 0x00: ToDS=0, FromDS=0 (independent frame, no DS)
     *
     *   IMPORTANT: byte 1 bit 0 is ToDS. Setting 0x01 means ToDS=1
     *   ("frame going TO the distribution system" = STA->AP), which is
     *   only valid when associated with an AP. For raw broadcast TX with
     *   no AP, ToDS MUST be 0. Using ToDS=1 produces malformed frames
     *   that monitors may drop or flag as invalid.
     *
     * Addressing (ToDS=0, FromDS=0):
     *   addr1 (DA)    = broadcast (we want everyone to receive it)
     *   addr2 (SA)    = our MAC (so receivers know who sent it)
     *   addr3 (BSSID) = broadcast (we're not in any BSS)
     *
     * Sequence Control: starts at 0, incremented per packet in
     * rawtx_stream_send() so receivers can dedup. */
    memset(s_wifi_hdr, 0, sizeof(s_wifi_hdr));
    s_wifi_hdr[0] = 0x08; /* FC byte 0: Data frame (type=2, subtype=0) */
    s_wifi_hdr[1] = 0x00; /* FC byte 1: ToDS=0, FromDS=0 (independent TX) */

    /* addr1 (bytes 4-9): Receiver Address = Broadcast */
    memset(&s_wifi_hdr[4], 0xFF, 6);

    /* addr2 (bytes 10-15): Transmitter Address = our MAC.
     * AUDIT-H4: do NOT fall back to a hardcoded MAC. If multiple devices hit
     * this path, they all transmit with the same SA -> 802.11 dedup by
     * (SA, seq) collides and on-air interference results. Fail init instead. */
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK)
    {
        memcpy(&s_wifi_hdr[10], mac, 6);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to get MAC - cannot init raw TX (unique SA required)");
        return ESP_ERR_INVALID_STATE;
    }

    /* addr3 (bytes 16-21): BSSID = Broadcast.
     * For ToDS=0/FromDS=0, addr3 is the BSSID. Since we're not in any BSS,
     * use broadcast — not our MAC. */
    memset(&s_wifi_hdr[16], 0xFF, 6);

    /* bytes 22-23: Sequence Control — starts at 0, incremented per packet. */
    s_wifi_seq = 0;

    ESP_LOGI(TAG, "Raw 802.11 TX mode active (broadcast, ToDS=0, FromDS=0)");

    s_ready = true;
    return ESP_OK;
}

esp_err_t rawtx_stream_deinit(void)
{
    s_ready = false;
    return ESP_OK;
}

bool rawtx_stream_is_ready(void)
{
    return s_ready;
}

esp_err_t rawtx_stream_send(const uint8_t *data, size_t len)
{
    if (!data || !len)
        return ESP_ERR_INVALID_ARG;
    /* LOW: !s_ready is a STATE error (transport not initialized), not an
     * argument error. Returning INVALID_ARG misled callers. */
    if (!s_ready)
        return ESP_ERR_INVALID_STATE;

    /* Raw 802.11 TX: prepend MAC header and send via esp_wifi_80211_tx.
     * Buffer: [wifi_hdr 24B][data len bytes]. Uses static s_frame_buf
     * (no per-packet malloc — see comment above). */
    /* L11: reject oversized payloads instead of silently truncating. Silent
     * truncation drops the tail of the audio packet — receiver decodes
     * garbage. main.c's MTU guard should prevent this, but a bug there would
     * be masked here. */
    if (len > RAWTX_MAX_PAYLOAD)
    {
        ESP_LOGW(TAG, "payload %u > %u, rejecting",
                 (unsigned)len, (unsigned)RAWTX_MAX_PAYLOAD);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t total_len = WIFI_HDR_LEN + len;

    memcpy(s_frame_buf, s_wifi_hdr, WIFI_HDR_LEN);

    /* Update Sequence Control (bytes 22-23) for this packet.
     * Layout (little-endian): 12-bit seq num + 4-bit frag num.
     *   byte 22 = (frag << 0) | (seq_lo << 4)  - frag=0, seq_lo = seq & 0xF
     *   byte 23 = seq_hi = (seq >> 4) & 0xFF
     * seq wraps at 4096 (12-bit). We never fragment, so frag stays 0.
     * AUDIT-LOW: explicit 12-bit wrap to match the on-air field width. */
    uint16_t seq = s_wifi_seq;
    s_frame_buf[22] = (uint8_t)((seq & 0x0F) << 4);   /* frag=0 | seq_lo */
    s_frame_buf[23] = (uint8_t)((seq >> 4) & 0xFF); /* seq_hi */

    memcpy(s_frame_buf + WIFI_HDR_LEN, data, len);

    /* GROK-24: esp_wifi_80211_tx() in ESP8266 RTOS SDK v3.x enqueues a COPY
     * of the frame into the WiFi driver's TX pool before returning, so the
     * caller's buffer may be reused immediately. s_frame_buf is static and
     * reused per call — safe today. IF a future SDK switches to zero-copy TX,
     * s_frame_buf must become per-call malloc'd or guarded by a TX-done
     * semaphore. Verify with a back-to-back stress test on SDK upgrade.
     *
     * Single-threaded contract: only stream_task_fn calls this (via
     * transport_send), so no mutex is needed for the static buffer. */
    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, s_frame_buf, total_len, false);

    /* Increment seq ONLY after successful TX — avoids holes in the sequence
     * when esp_wifi_80211_tx() fails (dropped frames don't increment seq). */
    if (err == ESP_OK) {
        s_wifi_seq = (uint16_t)((s_wifi_seq + 1) & 0x0FFF);
    }

    if (err != ESP_OK)
    {
        /* L12: return the actual error so the caller can log it. */
        return err;
    }
    return ESP_OK;
}
