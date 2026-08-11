#ifndef BOARD_WIFI_H
#define BOARD_WIFI_H

/* WiFi defaults, TX power, boot retry, channels.
 * Extracted from board_config.h (R3-C). */

#include "sdkconfig.h"
#include <stdint.h>
#include <stdbool.h>

/* ====================================================================
 *  WiFi defaults — configured via Kconfig, overridable via AT+WIFI
 * ==================================================================== */

#ifdef CONFIG_STREAMER_WIFI_SSID
#define WIFI_SSID_DEFAULT        CONFIG_STREAMER_WIFI_SSID
#else
#define WIFI_SSID_DEFAULT        "YOUR_WIFI_SSID"
#endif

#ifdef CONFIG_STREAMER_WIFI_PASSWORD
#define WIFI_PASSWORD_DEFAULT    CONFIG_STREAMER_WIFI_PASSWORD
#else
#define WIFI_PASSWORD_DEFAULT    "12345678"
#endif

#ifdef CONFIG_ESP8266_PHY_MAX_WIFI_TX_POWER
#define WIFI_TX_POWER_DEFAULT    CONFIG_ESP8266_PHY_MAX_WIFI_TX_POWER
#define WIFI_TX_POWER_MAX        CONFIG_ESP8266_PHY_MAX_WIFI_TX_POWER
#else
#define WIFI_TX_POWER_DEFAULT    20
#define WIFI_TX_POWER_MAX        20
#endif
#define WIFI_TX_POWER_MIN        0

#ifdef CONFIG_STREAMER_WIFI_HOSTNAME
#define WIFI_HOSTNAME_DEFAULT    CONFIG_STREAMER_WIFI_HOSTNAME
#else
#define WIFI_HOSTNAME_DEFAULT    "esp-streamer"
#endif

#ifdef CONFIG_STREAMER_WIFI_CONNECT_TIMEOUT_MS
#define WIFI_CONNECT_TIMEOUT_MS  CONFIG_STREAMER_WIFI_CONNECT_TIMEOUT_MS
#else
#define WIFI_CONNECT_TIMEOUT_MS  15000
#endif

/* ---- WiFi Boot Retry (deep sleep on connect failure) ---- */
#ifdef CONFIG_STREAMER_WIFI_BOOT_RETRY_ENABLED
#define WIFI_BOOT_RETRY_ENABLED     1
#else
#define WIFI_BOOT_RETRY_ENABLED     0
#endif

#ifdef CONFIG_STREAMER_WIFI_BOOT_RETRY_ATTEMPTS
#define WIFI_BOOT_RETRY_ATTEMPTS    CONFIG_STREAMER_WIFI_BOOT_RETRY_ATTEMPTS
#else
#define WIFI_BOOT_RETRY_ATTEMPTS    3
#endif

#ifdef CONFIG_STREAMER_WIFI_BOOT_SLEEP_MINUTES
#define WIFI_BOOT_SLEEP_MINUTES     CONFIG_STREAMER_WIFI_BOOT_SLEEP_MINUTES
#else
#define WIFI_BOOT_SLEEP_MINUTES     2
#endif

#ifdef CONFIG_STREAMER_WIFI_BOOT_SLEEP_MODE
#define WIFI_BOOT_SLEEP_MODE        CONFIG_STREAMER_WIFI_BOOT_SLEEP_MODE
#else
#define WIFI_BOOT_SLEEP_MODE        0
#endif

#ifdef CONFIG_STREAMER_WIFI_RECONNECT_BACKOFF_MIN_MS
#define WIFI_RECONNECT_BACKOFF_MIN_MS  CONFIG_STREAMER_WIFI_RECONNECT_BACKOFF_MIN_MS
#else
#define WIFI_RECONNECT_BACKOFF_MIN_MS  1000
#endif

#ifdef CONFIG_STREAMER_WIFI_RECONNECT_BACKOFF_MAX_MS
#define WIFI_RECONNECT_BACKOFF_MAX_MS  CONFIG_STREAMER_WIFI_RECONNECT_BACKOFF_MAX_MS
#else
#define WIFI_RECONNECT_BACKOFF_MAX_MS  15000
#endif

/* Raw 802.11 TX mode: max wait for WIFI_EVENT_STA_START after esp_wifi_start()
 * before giving up. esp_wifi_start() is async; we block on the STA_START event
 * (posted once the radio is calibrated) so the first esp_wifi_80211_tx() call
 * succeeds. Typical bring-up ~100-300 ms; 3 s is a safety net. */
#define WIFI_RAW_START_TIMEOUT_MS  3000

#endif /* BOARD_WIFI_H */
