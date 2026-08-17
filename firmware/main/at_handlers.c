/*
 * AT command handler implementations for ESP8266 RTOS SDK v3.4.
 *
 * This file was split out of at_cmd.c in R3-B (see worklog). It contains
 * all cmd_* handler functions and their handler-only helpers. The UART
 * driver state, command dispatch table, at_process_line(), and the
 * at_send_* helpers remain in at_cmd.c.
 *
 * Function bodies are byte-for-byte unchanged from the original
 * monolithic at_cmd.c — only their file location and `static` qualifier
 * (for the dispatch handlers referenced by AT_COMMANDS[]) changed.
 *
 * Layout:
 *   1. Forward declarations of static helpers (cmd_X_impl, cmd_X_query,
 *      cmd_X_set, parse_wifi_args, log_putchar_noop).
 *   2. cmd_at — bare "AT" test (no args; called from at_process_line).
 *   3. Dispatch handlers (cmd_rst .. cmd_log, cmd_batt) — referenced by
 *      AT_COMMANDS[] in at_cmd.c.
 *   4. Impl/query/set functions and handler-only static helpers, in the
 *      original order.
 */

/* ---- System / SDK includes ---- */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <strings.h> /* strcasecmp */
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
/* FIX (AUDIT-MEDIUM): see FIXES.md — explicit include for tcpip_adapter API. */
#include "tcpip_adapter.h"

/* ---- Project includes ---- */
#include "board_config.h"
#include "at_cmd.h"
#include "config_mgr.h"
#include "svc_port.h"
#include "svc_protocol.h"
#include "wifi_sta.h"
#include "i2s_capture.h"
#include "battery.h"
#include "stream_control.h"
#include "stream_mode.h"

/* Shared declarations with at_cmd.c (handler typedefs, at_send_*, cmd_*
 * forward decls). */
#include "at_internal.h"

static const char *TAG = "at_cmd";

/* ---- Forward declarations of static helpers ----
 * These impls/query/set functions are only called from the dispatch
 * handlers in this file, so they remain static. Forward declarations
 * are needed because the dispatch handlers are defined above the impls
 * (matching the original file order). */

/* Internal impls (called by the dispatch handlers; bodies unchanged). */
static void cmd_rst_impl(void);
static void cmd_gmr_impl(void);
static void cmd_help_impl(void);
static void cmd_status_impl(void);
static void cmd_factory_impl(void);
static void cmd_hotrestart_impl(void);
#if BATTERY_ENABLED
static void cmd_batt_query_impl(void);
#endif

/* Query/set impls (kept as-is; called by the dispatch handlers). */
static void cmd_wifi_query(void);
static void cmd_wifi_set(const char *args);
static void cmd_host_query(void);
static void cmd_host_set(const char *args);
static void cmd_port_query(void);
static void cmd_port_set(const char *args);
static void cmd_txpwr_query(void);
static void cmd_txpwr_set(const char *args);
static void cmd_bits_query(void);
static void cmd_bits_set(const char *args);
static void cmd_fmt_query(void);
static void cmd_fmt_set(const char *args);
static void cmd_ch_query(void);
static void cmd_ch_set(const char *args);
static void cmd_rate_query(void);
static void cmd_rate_set(const char *args);
static void cmd_gain_query(void);
static void cmd_gain_set(const char *args);
static void cmd_agc_query(void);
static void cmd_agc_set(const char *args);
static void cmd_codec_query(void);
static void cmd_codec_set(const char *args);
static void cmd_wch_query(void);
static void cmd_wch_set(const char *args);
static void cmd_xport_query(void);
static void cmd_xport_set(const char *args);
static void cmd_timing_query(void);
static void cmd_timing_set(const char *args);
/* FIX (UART0/B9): see FIXES.md — AT+LOG mutes ESP_LOG output on UART0. */
static void cmd_log_set(const char *args);
static void cmd_log_query(void);

/* ---- Command implementations ---- */

void cmd_at(void)
{
    at_send_ok();
}

/* ---- Dispatch handlers (R2-C) ----
 * Each handler dispatches to the appropriate query/set impl based on
 * is_query and args. The dispatch in at_process_line() enforces the
 * takes_args rule for AT+NAME=value, so handlers for non-arg commands
 * only need to reject AT+NAME? (is_query=true). For arg-taking commands,
 * bare AT+NAME (is_query=false, args=NULL) is rejected by the handler.
 * Adding a new command = 1 row in AT_COMMANDS + 1 handler here. */

/* Non-arg commands: AT+NAME (bare) runs the impl; AT+NAME? → ERROR here;
 * AT+NAME=val → ERROR by the dispatch (takes_args=false). */
esp_err_t cmd_rst(uart_port_t uart, bool is_query, const char *args)
{
    (void)uart;
    (void)args;
    if (is_query)
    {
        at_send_error();
        return ESP_OK;
    }
    cmd_rst_impl();
    return ESP_OK;
}

esp_err_t cmd_gmr(uart_port_t uart, bool is_query, const char *args)
{
    (void)uart;
    (void)args;
    if (is_query)
    {
        at_send_error();
        return ESP_OK;
    }
    cmd_gmr_impl();
    return ESP_OK;
}

esp_err_t cmd_help(uart_port_t uart, bool is_query, const char *args)
{
    (void)uart;
    (void)args;
    if (is_query)
    {
        at_send_error();
        return ESP_OK;
    }
    cmd_help_impl();
    return ESP_OK;
}

esp_err_t cmd_status(uart_port_t uart, bool is_query, const char *args)
{
    (void)uart;
    (void)args;
    if (is_query)
    {
        at_send_error();
        return ESP_OK;
    }
    cmd_status_impl();
    return ESP_OK;
}

esp_err_t cmd_factory(uart_port_t uart, bool is_query, const char *args)
{
    (void)uart;
    (void)args;
    if (is_query)
    {
        at_send_error();
        return ESP_OK;
    }
    cmd_factory_impl();
    return ESP_OK;
}

esp_err_t cmd_hotrestart(uart_port_t uart, bool is_query, const char *args)
{
    (void)uart;
    (void)args;
    if (is_query)
    {
        at_send_error();
        return ESP_OK;
    }
    cmd_hotrestart_impl();
    return ESP_OK;
}

/* Arg-taking commands: AT+NAME? → query impl, AT+NAME=val → set impl,
 * AT+NAME (bare) → ERROR (preserves original if/else semantics).
 * is_query and args are mutually exclusive (enforced by the dispatch
 * parser), so the order of these checks does not matter. */
esp_err_t cmd_wifi(uart_port_t uart, bool is_query, const char *args)
{
    (void)uart;
    if (is_query)
    {
        cmd_wifi_query();
        return ESP_OK;
    }
    if (args)
    {
        cmd_wifi_set(args);
        return ESP_OK;
    }
    at_send_error();
    return ESP_OK;
}

esp_err_t cmd_host(uart_port_t uart, bool is_query, const char *args)
{
    (void)uart;
    if (is_query)
    {
        cmd_host_query();
        return ESP_OK;
    }
    if (args)
    {
        cmd_host_set(args);
        return ESP_OK;
    }
    at_send_error();
    return ESP_OK;
}

esp_err_t cmd_port(uart_port_t uart, bool is_query, const char *args)
{
    (void)uart;
    if (is_query)
    {
        cmd_port_query();
        return ESP_OK;
    }
    if (args)
    {
        cmd_port_set(args);
        return ESP_OK;
    }
    at_send_error();
    return ESP_OK;
}

esp_err_t cmd_txpwr(uart_port_t uart, bool is_query, const char *args)
{
    (void)uart;
    if (is_query)
    {
        cmd_txpwr_query();
        return ESP_OK;
    }
    if (args)
    {
        cmd_txpwr_set(args);
        return ESP_OK;
    }
    at_send_error();
    return ESP_OK;
}

esp_err_t cmd_rate(uart_port_t uart, bool is_query, const char *args)
{
    (void)uart;
    if (is_query)
    {
        cmd_rate_query();
        return ESP_OK;
    }
    if (args)
    {
        cmd_rate_set(args);
        return ESP_OK;
    }
    at_send_error();
    return ESP_OK;
}

esp_err_t cmd_bits(uart_port_t uart, bool is_query, const char *args)
{
    (void)uart;
    if (is_query)
    {
        cmd_bits_query();
        return ESP_OK;
    }
    if (args)
    {
        cmd_bits_set(args);
        return ESP_OK;
    }
    at_send_error();
    return ESP_OK;
}

esp_err_t cmd_fmt(uart_port_t uart, bool is_query, const char *args)
{
    (void)uart;
    if (is_query)
    {
        cmd_fmt_query();
        return ESP_OK;
    }
    if (args)
    {
        cmd_fmt_set(args);
        return ESP_OK;
    }
    at_send_error();
    return ESP_OK;
}

esp_err_t cmd_ch(uart_port_t uart, bool is_query, const char *args)
{
    (void)uart;
    if (is_query)
    {
        cmd_ch_query();
        return ESP_OK;
    }
    if (args)
    {
        cmd_ch_set(args);
        return ESP_OK;
    }
    at_send_error();
    return ESP_OK;
}

esp_err_t cmd_gain(uart_port_t uart, bool is_query, const char *args)
{
    (void)uart;
    if (is_query)
    {
        cmd_gain_query();
        return ESP_OK;
    }
    if (args)
    {
        cmd_gain_set(args);
        return ESP_OK;
    }
    at_send_error();
    return ESP_OK;
}

esp_err_t cmd_agc(uart_port_t uart, bool is_query, const char *args)
{
    (void)uart;
    if (is_query)
    {
        cmd_agc_query();
        return ESP_OK;
    }
    if (args)
    {
        cmd_agc_set(args);
        return ESP_OK;
    }
    at_send_error();
    return ESP_OK;
}

esp_err_t cmd_codec(uart_port_t uart, bool is_query, const char *args)
{
    (void)uart;
    if (is_query)
    {
        cmd_codec_query();
        return ESP_OK;
    }
    if (args)
    {
        cmd_codec_set(args);
        return ESP_OK;
    }
    at_send_error();
    return ESP_OK;
}

esp_err_t cmd_xport(uart_port_t uart, bool is_query, const char *args)
{
    (void)uart;
    if (is_query)
    {
        cmd_xport_query();
        return ESP_OK;
    }
    if (args)
    {
        cmd_xport_set(args);
        return ESP_OK;
    }
    at_send_error();
    return ESP_OK;
}

esp_err_t cmd_wch(uart_port_t uart, bool is_query, const char *args)
{
    (void)uart;
    if (is_query)
    {
        cmd_wch_query();
        return ESP_OK;
    }
    if (args)
    {
        cmd_wch_set(args);
        return ESP_OK;
    }
    at_send_error();
    return ESP_OK;
}

esp_err_t cmd_timing(uart_port_t uart, bool is_query, const char *args)
{
    (void)uart;
    if (is_query)
    {
        cmd_timing_query();
        return ESP_OK;
    }
    if (args)
    {
        cmd_timing_set(args);
        return ESP_OK;
    }
    at_send_error();
    return ESP_OK;
}

esp_err_t cmd_log(uart_port_t uart, bool is_query, const char *args)
{
    (void)uart;
    /* AT+LOG=0/1 or AT+LOG? — is_query and args are mutually exclusive
     * (enforced by the dispatch parser), mirroring the original logic. */
    if (is_query)
    {
        cmd_log_query();
        return ESP_OK;
    }
    if (args)
    {
        cmd_log_set(args);
        return ESP_OK;
    }
    at_send_error();
    return ESP_OK;
}

#if BATTERY_ENABLED
/* BATT: takes_args=false but AT+BATT? AND AT+BATT (bare) both query.
 * AT+BATT=val is rejected by the dispatch (takes_args=false). */
esp_err_t cmd_batt(uart_port_t uart, bool is_query, const char *args)
{
    (void)uart;
    (void)is_query;
    (void)args;
    cmd_batt_query_impl();
    return ESP_OK;
}
#endif

static void cmd_rst_impl(void)
{
    at_send_str("\r\nOK\r\nRestarting...\r\n");
    vTaskDelay(pdMS_TO_TICKS(100)); /* flush UART */

    /* FIX (C2): see FIXES.md.  FIX (AUDIT-H8): see FIXES.md. */
    if (streaming_is_active())
    {
        streaming_request_stop();
        /* Poll streaming_is_active() until main loop finishes stop_streaming()
         * (max wait 5s = 50 × 100ms). */
        for (int i = 0; i < 50 && streaming_is_active(); i++)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (streaming_is_active())
        {
            ESP_LOGW(TAG, "AT+RST: stream still active after 5s - rebooting anyway "
                          "(SDK will tear down hardware)");
        }
    }

    /* Reboot. The SDK tears down WiFi/I2S/transport as part of reboot,
     * which is safe even if the main loop is mid-start_streaming(). */
    vTaskDelay(pdMS_TO_TICKS(100)); /* let hardware settle */
    esp_restart();
}

static void cmd_gmr_impl(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    /* FIX FR-AT #15: see FIXES.md — gate frame_ms on streaming_frame_ms_known()
     * so GMR doesn't print a stale frame_ms when no stream has ever started. */
    unsigned fms = streaming_frame_ms_known() ? (unsigned)streaming_get_frame_ms() : 0;
    at_send_data("+GMR:ESP8266 ADPCM Streamer " FIRMWARE_VERSION "\r\n");
    at_send_data("+GMR:SDK ESP8266_RTOS_SDK v3.4\r\n");
    at_send_data("+GMR:Codec DVI4 IMA ADPCM (RFC 3551)\r\n");
    at_send_data("+GMR:Audio %u Hz, %u ms frames\r\n",
                 (unsigned)cfg.sample_rate, fms);
    at_send_data("+GMR:Mic INMP441 I2S\r\n");
    at_send_ok();
}

static void cmd_help_impl(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);

    at_send_str("\r\n+HELP:--- AT Command List ---\r\n");
    at_send_str("+HELP:AT             - test connection\r\n");
    at_send_str("+HELP:AT+RST         - restart device\r\n");
    at_send_str("+HELP:AT+GMR         - show version\r\n");
    at_send_str("+HELP:AT+HELP        - this help\r\n");
    at_send_str("+HELP:AT+WIFI?       - show WiFi settings\r\n");
    at_send_str("+HELP:AT+WIFI=s,pwd  - set WiFi (auto-save, applied immediately)\r\n");
    at_send_str("+HELP:                  quoted form: AT+WIFI=\"ssid,with,comma\",\"pwd\"\r\n");
    at_send_str("+HELP:AT+HOST?       - show DHCP hostname\r\n");
    at_send_str("+HELP:AT+HOST=name   - set hostname (max 23 chars, auto-save, restart)\r\n");
    at_send_str("+HELP:AT+PORT?       - show service/discovery port\r\n");
    at_send_str("+HELP:AT+PORT=n      - set service port (auto-save, restart required)\r\n");
    at_send_str("+HELP:AT+TXPWR?      - show WiFi TX power\r\n");
    at_send_str("+HELP:AT+TXPWR=n     - set TX power in dBm (see AT+TXPWR? for range)\r\n");
    at_send_str("+HELP:AT+RATE?       - show sample rate\r\n");
    at_send_str("+HELP:AT+RATE=n      - set rate 8000/11025/16000/22050/32000/44100/48000\r\n");
    at_send_str("+HELP:AT+GAIN?       - show digital gain (0-64)\r\n");
    at_send_str("+HELP:AT+GAIN=n      - set gain 0-64 (0=bypass, 32=+30dB, 16bit:use 4-8, HOTRESTART to apply)\r\n");
    at_send_str("+HELP:AT+AGC?        - show AGC mode (0-8 presets)\r\n");
    at_send_str("+HELP:AT+AGC=0..8    - set AGC preset (0=off 1=studio 2=podcast 3=balanced 4=fast 5=noisy 6=music 7=limiter 8=surv, HOTRESTART to apply)\r\n");
    at_send_str("+HELP:AT+CODEC?      - show codec (0=ADPCM, 1=PCM)\r\n");
    at_send_str("+HELP:AT+CODEC=0|1   - set codec (0=adpcm 1=pcm, auto-save, hotrestart)\r\n");
    at_send_str("+HELP:AT+WCH?        - show wifi channel (1-14)\r\n");
    at_send_str("+HELP:AT+WCH=n       - set wifi channel 1-14 (auto-save, raw TX only, AT+HOTRESTART to apply)\r\n");
    at_send_str("+HELP:AT+XPORT?      - show transport (0=UDP 1=TCP 2=RawTX)\r\n");
    at_send_str("+HELP:AT+XPORT=0|1|2 - set transport (AT+RST to apply)\r\n");
    at_send_str("+HELP:AT+TIMING?     - show I2S RX input delays (sd,ws,bck)\r\n");
    at_send_str("+HELP:AT+TIMING=s,w,b - set I2S RX delays 0-3 each (auto-save, hotrestart)\r\n");
#if BATTERY_ENABLED
    at_send_str("+HELP:AT+BATT?       - show battery voltage and charge level\r\n");
#endif
    at_send_str("+HELP:AT+BITS?       - show I2S bits per sample\r\n");
    at_send_str("+HELP:AT+BITS=16|24  - set I2S bits (auto-save, hotrestart to apply)\r\n");
    at_send_str("+HELP:AT+FMT?        - show I2S communication format\r\n");
    at_send_str("+HELP:AT+FMT=0|1     - 0=Philips I2S  1=LSB (auto-save, hotrestart to apply)\r\n");
    at_send_str("+HELP:AT+CH?         - show I2S channel format\r\n");
    at_send_str("+HELP:AT+CH=0|1|2    - 0=left 1=right 2=stereo (auto-save, hotrestart to apply)\r\n");
    at_send_str("+HELP:AT+STATUS      - full device status\r\n");
    at_send_str("+HELP:AT+HOTRESTART   - restart stream to apply audio changes (no reboot)\r\n");
    at_send_str("+HELP:AT+FACTORY     - factory reset (restart required)\r\n");
    at_send_str("+HELP:--- Audio Parameters ---\r\n");
    at_send_data("+HELP:  Sample rate: %u Hz (use AT+RATE to change)\r\n", (unsigned)cfg.sample_rate);
    /* FIX FR-AT #15: see FIXES.md — gate frame_ms on streaming_frame_ms_known(). */
    {
        unsigned fms = streaming_frame_ms_known() ? (unsigned)streaming_get_frame_ms() : 0;
        at_send_data("+HELP:  Frame duration: %u ms\r\n", fms);
    }
    at_send_data("+HELP:  Bits: %u-bit (use AT+BITS to change)\r\n", cfg.bits_per_sample);
    at_send_data("+HELP:  Gain: %u (use AT+GAIN to change, 0=bypass)\r\n", (unsigned)cfg.gain);
    {
        const agc_preset_t *p = (cfg.agc_mode < AGC_MODE_COUNT) ? &AGC_PRESETS[cfg.agc_mode] : &AGC_PRESETS[0];
        at_send_data("+HELP:  AGC: %u %s (use AT+AGC to change, 0-8)\r\n",
                     (unsigned)cfg.agc_mode, p->name);
    }
    at_send_data("+HELP:  CODEC: %u (use AT+CODEC to change, 0=ADPCM 1=PCM)\r\n", (unsigned)cfg.codec_mode);
    {
        const char *xname = "UDP";
        if (cfg.transport_mode == TRANSPORT_MODE_TCP)
            xname = "TCP";
        else if (cfg.transport_mode == TRANSPORT_MODE_RAWTX)
            xname = "Raw 802.11 TX";
        at_send_data("+HELP:  Transport: %u %s (use AT+XPORT to change, 0=UDP 1=TCP 2=RawTX)\r\n",
                     (unsigned)cfg.transport_mode, xname);
    }
    if (cfg.transport_mode == TRANSPORT_MODE_RAWTX)
    {
        at_send_data("+HELP:  WCH: %u (use AT+WCH to change, 1-14)\r\n", (unsigned)cfg.wifi_channel);
    }
    at_send_ok();
}

static void cmd_wifi_query(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    at_send_data("+WIFI:ssid=\"%s\"\r\n", cfg.wifi_ssid);
    /* FIX (H11): see FIXES.md — mask the WiFi password (no AT auth). */
    size_t plen = strlen(cfg.wifi_password);
    at_send_data("+WIFI:password=<%u chars, hidden>\r\n", (unsigned)plen);
    at_send_ok();
}

/* FIX (2-E HIGH #11): see FIXES.md.  FIX (4-E LOW): see FIXES.md.
 * Parse AT+WIFI=ssid,password (quoted form allows commas; \" escapes). Returns 0/-1. */
static int parse_wifi_args(const char *args, char *ssid, size_t ssid_sz,
                           char *pwd, size_t pwd_sz)
{
    const char *p = args;
    size_t i;

    if (*p == '"')
    {
        /* Quoted SSID — handle \" escapes. */
        p++; /* skip opening quote */
        i = 0;
        while (*p && *p != '"')
        {
            /* FIX (4-E LOW): see FIXES.md — check buffer limit before write. */
            if (i >= ssid_sz - 1)
                return -1; /* SSID too long */
            if (*p == '\\' && p[1] == '"')
            {
                ssid[i++] = '"';
                p += 2;
            }
            else
            {
                ssid[i++] = *p++;
            }
        }
        if (*p != '"')
            return -1; /* unterminated quote */
        p++;           /* skip closing quote */
        ssid[i] = '\0';
    }
    else
    {
        /* Unquoted SSID — no commas allowed (comma is the separator). */
        const char *end = strchr(p, ',');
        size_t len;
        if (end)
        {
            len = (size_t)(end - p);
        }
        else
        {
            /* No comma — SSID is the entire remaining string (open network). */
            len = strlen(p);
        }
        if (len == 0 || len >= ssid_sz)
            return -1;
        memcpy(ssid, p, len);
        ssid[len] = '\0';
        p = end;
    }

    /* Password is optional: absent = open network. */
    pwd[0] = '\0';
    if (p == NULL || *p != ',')
        return 0; /* no comma → no password → open network */
    p++;          /* skip comma */

    /* FIX (4-E LOW): see FIXES.md — skip leading whitespace before unquoted pwd. */
    while (*p == ' ' || *p == '\t')
        p++;

    if (*p == '"')
    {
        /* Quoted password — handle \" escapes. */
        p++; /* skip opening quote */
        i = 0;
        while (*p && *p != '"')
        {
            if (i >= pwd_sz - 1)
                return -1; /* password too long */
            if (*p == '\\' && p[1] == '"')
            {
                pwd[i++] = '"';
                p += 2;
            }
            else
            {
                pwd[i++] = *p++;
            }
        }
        if (*p != '"')
            return -1; /* unterminated quote */
        p++;           /* skip closing quote */
        pwd[i] = '\0';
        /* Validate: only whitespace or end-of-string allowed after quoted password */
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p != '\0')
            return -1; /* trailing garbage */
    }
    else
    {
        /* Unquoted password — rest of string */
        size_t len = strlen(p);
        /* Strip trailing whitespace (spaces, tabs, CR, LF) */
        while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t' || p[len - 1] == '\r' || p[len - 1] == '\n'))
            len--;
        if (len >= pwd_sz)
            return -1;
        memcpy(pwd, p, len);
        pwd[len] = '\0';
    }

    return 0;
}

static void cmd_wifi_set(const char *args)
{
    /* Format: ssid,password  (or "ssid","password" for SSIDs with commas) */
    /* FIX (M23): see FIXES.md — check input length before truncating. */
    if (strlen(args) >= 127)
    {
        at_send_data("+ERR:input too long (max 127 chars total)\r\n");
        at_send_error();
        return;
    }

    /* FIX (2-E HIGH #11): see FIXES.md — use parse_wifi_args for quoted SSIDs. */
    char ssid[33];
    char pwd[65];
    if (parse_wifi_args(args, ssid, sizeof(ssid), pwd, sizeof(pwd)) != 0)
    {
        memset(pwd, 0, sizeof(pwd));
        at_send_data("+ERR:format is ssid,password (or \"ssid\",\"password\")\r\n");
        at_send_error();
        return;
    }

    if (!ssid[0])
    {
        memset(pwd, 0, sizeof(pwd));
        at_send_data("+ERR:ssid required\r\n");
        at_send_error();
        return;
    }

    /* Password is optional: empty = open network.
     * If present, WPA2 mandates 8-63 characters. */
    if (pwd[0])
    {
        size_t plen = strlen(pwd);
        if (plen < 8 || plen > 63)
        {
            memset(pwd, 0, sizeof(pwd));
            at_send_data("+ERR:password must be 8-63 chars (got %u)\r\n",
                         (unsigned)plen);
            at_send_error();
            return;
        }
    }
    bool is_open = !pwd[0];
    esp_err_t err = config_set_wifi(ssid, pwd);
    if (err != ESP_OK)
    {
        memset(pwd, 0, sizeof(pwd));
        at_send_data("+ERR:config_set_wifi failed\r\n");
        at_send_error();
        return;
    }

    /* Apply immediately — reconnect to the new AP. */
    err = wifi_sta_reconfigure(ssid, pwd);
    memset(pwd, 0, sizeof(pwd));
    if (err != ESP_OK)
    {
        at_send_data("+WIFI:saved but reconfigure failed (restart required)\r\n");
        at_send_ok();
        return;
    }

    if (is_open)
        at_send_data("+WIFI:set to ssid=\"%s\" OPEN (saved, reconnecting)\r\n", ssid);
    else
        at_send_data("+WIFI:set to ssid=\"%s\" (saved, reconnecting)\r\n", ssid);
    at_send_ok();
}

static void cmd_host_query(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    at_send_data("+HOST:\"%s\"\r\n", cfg.hostname);
    at_send_ok();
}

static void cmd_host_set(const char *args)
{
    /* Format: hostname (alphanumeric + hyphens, 1-32 chars) */
    if (!args || !args[0])
    {
        at_send_data("+ERR:hostname required\r\n");
        at_send_error();
        return;
    }
    if (strlen(args) >= 24)
    {
        at_send_data("+ERR:hostname too long (max 23 chars)\r\n");
        at_send_error();
        return;
    }

    esp_err_t err = config_set_hostname(args);
    if (err != ESP_OK)
    {
        at_send_data("+ERR:invalid hostname (use alphanumeric + hyphens)\r\n");
        at_send_error();
        return;
    }

    at_send_data("+HOST:set to \"%s\" (saved, restart required)\r\n", args);
    at_send_ok();
}

static void cmd_port_query(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    at_send_data("+PORT:%u\r\n", (unsigned)cfg.svc_port);
    at_send_ok();
}

static void cmd_port_set(const char *args)
{
    char *endptr = NULL;
    long port = strtol(args, &endptr, 10);
    if (endptr == args || *endptr != '\0' || port < 1 || port > 65535)
    {
        at_send_data("+ERR:port must be 1-65535\r\n");
        at_send_error();
        return;
    }

    esp_err_t err = config_set_svc_port((uint16_t)port);
    if (err != ESP_OK)
    {
        at_send_data("+ERR:config_set_svc_port failed\r\n");
        at_send_error();
        return;
    }

    at_send_data("+PORT:set to %u (saved, restart required)\r\n", (unsigned)port);
    at_send_ok();
}

static void cmd_txpwr_query(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    at_send_data("+TXPWR:%u dBm (max %u)\r\n", (unsigned)cfg.tx_power, (unsigned)WIFI_TX_POWER_MAX);
    at_send_ok();
}

static void cmd_txpwr_set(const char *args)
{
    char *endptr = NULL;
    long power = strtol(args, &endptr, 10);
    if (endptr == args || *endptr != '\0' || power < WIFI_TX_POWER_MIN || power > WIFI_TX_POWER_MAX)
    {
        at_send_data("+ERR:txpwr must be %d-%d dBm\r\n", WIFI_TX_POWER_MIN, WIFI_TX_POWER_MAX);
        at_send_error();
        return;
    }

    esp_err_t err = config_set_tx_power((uint8_t)power);
    if (err != ESP_OK)
    {
        at_send_data("+ERR:config_set_tx_power failed\r\n");
        at_send_error();
        return;
    }

    /* Apply immediately. */
    wifi_sta_set_tx_power((uint8_t)power);

    at_send_data("+TXPWR:set to %u dBm (saved, applied)\r\n", (unsigned)power);
    at_send_ok();
}

static void cmd_bits_query(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    at_send_data("+BITS:%u\r\n", cfg.bits_per_sample);
    at_send_ok();
}

static void cmd_bits_set(const char *args)
{
    char *endptr = NULL;
    long bits = strtol(args, &endptr, 10);
    if (endptr == args || *endptr != '\0' || (bits != 16 && bits != 24))
    {
        at_send_data("+ERR:bits must be 16 or 24\r\n");
        at_send_error();
        return;
    }

    esp_err_t err = config_set_bits_per_sample((uint8_t)bits);
    if (err != ESP_OK)
    {
        at_send_data("+ERR:config_set_bits failed\r\n");
        at_send_error();
        return;
    }

    at_send_data("+BITS:set to %d (saved, use AT+HOTRESTART to apply)\r\n", (int)bits);
    at_send_ok();
}

static void cmd_fmt_query(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    at_send_data("+FMT:%d (%s)\r\n", cfg.comm_format,
                 cfg.comm_format == I2S_CFMT_LSB ? "LSB" : "Philips");
    at_send_ok();
}

static void cmd_fmt_set(const char *args)
{
    char *endptr = NULL;
    long fmt = strtol(args, &endptr, 10);
    if (endptr == args || *endptr != '\0' || (fmt != I2S_CFMT_PHILIPS && fmt != I2S_CFMT_LSB))
    {
        at_send_data("+ERR:fmt must be 0 (Philips) or 1 (LSB)\r\n");
        at_send_error();
        return;
    }

    esp_err_t err = config_set_comm_format((uint8_t)fmt);
    if (err != ESP_OK)
    {
        at_send_data("+ERR:config_set_fmt failed\r\n");
        at_send_error();
        return;
    }

    at_send_data("+FMT:set to %d (%s, saved, use AT+HOTRESTART to apply)\r\n",
                 (int)fmt, fmt == I2S_CFMT_LSB ? "LSB" : "Philips");
    at_send_ok();
}

static void cmd_ch_query(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    /* Show user-friendly 0/1/2 (as in AT+CH=n), not raw 4/3/0 SDK values. */
    int user_val;
    const char *desc;
    switch (cfg.channel_format)
    {
    case I2S_CHFMT_LEFT:
        user_val = 0;
        desc = "left";
        break;
    case I2S_CHFMT_RIGHT:
        user_val = 1;
        desc = "right";
        break;
    case I2S_CHFMT_STEREO:
        user_val = 2;
        desc = "stereo";
        break;
    default:
        user_val = -1;
        desc = "unknown";
        break;
    }
    at_send_data("+CH:%d (%s, %u ch)\r\n", user_val, desc,
                 (unsigned)channel_format_to_count(cfg.channel_format));
    at_send_ok();
}

static void cmd_ch_set(const char *args)
{
    char *endptr = NULL;
    long ch = strtol(args, &endptr, 10);
    /* FIX (GROK-15): see FIXES.md — validate entire argument was consumed. */
    if (endptr == args || *endptr != '\0')
    {
        at_send_data("+ERR:ch must be a number (0, 1, or 2)\r\n");
        at_send_error();
        return;
    }
    uint8_t fmt;
    switch (ch)
    {
    case 0:
        fmt = I2S_CHFMT_LEFT;
        break;
    case 1:
        fmt = I2S_CHFMT_RIGHT;
        break;
    case 2:
        fmt = I2S_CHFMT_STEREO;
        break;
    default:
        at_send_data("+ERR:ch must be 0 (left), 1 (right), or 2 (stereo)\r\n");
        at_send_error();
        return;
    }

    esp_err_t err = config_set_channel_format(fmt);
    if (err != ESP_OK)
    {
        at_send_data("+ERR:config_set_ch failed\r\n");
        at_send_error();
        return;
    }

    /* Update svc_port::s_channels immediately so the next DISCOVER gets the
     * correct channel count (otherwise INFO packets carry the old value). */
    int new_ch = channel_format_to_count(fmt);
    svc_port_set_channels((uint8_t)new_ch);

    at_send_data("+CH:set to %d (saved, use AT+HOTRESTART to apply)\r\n", (int)ch);
    at_send_ok();
}

static void cmd_rate_query(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    at_send_data("+RATE:%u\r\n", (unsigned)cfg.sample_rate);
    at_send_ok();
}

static void cmd_rate_set(const char *args)
{
    char *endptr = NULL;
    long rate = strtol(args, &endptr, 10);
    if (endptr == args || *endptr != '\0')
    {
        at_send_data("+ERR:rate must be a number\r\n");
        at_send_error();
        return;
    }
    if (!sample_rate_is_valid((uint32_t)rate))
    {
        at_send_data("+ERR:rate must be 8000/11025/16000/22050/32000/44100/48000\r\n");
        at_send_error();
        return;
    }

    esp_err_t err = config_set_sample_rate((uint32_t)rate);
    if (err != ESP_OK)
    {
        at_send_data("+ERR:config_set_sample_rate failed\r\n");
        at_send_error();
        return;
    }

    at_send_data("+RATE:set to %u (saved, use AT+HOTRESTART to apply)\r\n", (unsigned)rate);
    at_send_ok();
}

static void cmd_gain_query(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    at_send_data("+GAIN:%u\r\n", (unsigned)cfg.gain);
    at_send_ok();
}

static void cmd_gain_set(const char *args)
{
    char *endptr = NULL;
    long gain = strtol(args, &endptr, 10);
    if (endptr == args || *endptr != '\0' || gain < 0 || gain > 64)
    {
        at_send_data("+ERR:gain must be 0-64 (0=bypass, 32=+30dB)\r\n");
        at_send_error();
        return;
    }

    esp_err_t err = config_set_gain((uint8_t)gain);
    if (err != ESP_OK)
    {
        at_send_data("+ERR:config_set_gain failed\r\n");
        at_send_error();
        return;
    }

    at_send_data("+GAIN:set to %u (saved, use AT+HOTRESTART to apply)\r\n", (unsigned)gain);
    at_send_ok();
}

static void cmd_agc_query(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    if (cfg.agc_mode < AGC_MODE_COUNT)
    {
        const agc_preset_t *p = &AGC_PRESETS[cfg.agc_mode];
        at_send_data("+AGC:%u (%s, attack=%u, release=%u)\r\n",
                     (unsigned)cfg.agc_mode, p->name,
                     (unsigned)p->attack, (unsigned)p->release);
    }
    else
    {
        at_send_data("+AGC:%u (unknown)\r\n", (unsigned)cfg.agc_mode);
    }
    at_send_ok();
}

static void cmd_agc_set(const char *args)
{
    char *endptr = NULL;
    long val = strtol(args, &endptr, 10);
    if (endptr == args || *endptr != '\0' || val < 0 || val >= AGC_MODE_COUNT)
    {
        at_send_data("+ERR:agc must be 0-8 (0=OFF 1=Studio 2=Podcast 3=Balanced "
                     "4=Fast 5=Noisy 6=Music 7=Limiter 8=Surveillance)\r\n");
        at_send_error();
        return;
    }

    esp_err_t err = config_set_agc_mode((uint8_t)val);
    if (err != ESP_OK)
    {
        at_send_data("+ERR:config_set_agc_mode failed\r\n");
        at_send_error();
        return;
    }

    const agc_preset_t *p = &AGC_PRESETS[val];
    at_send_data("+AGC:set to %ld (%s, attack=%u, release=%u, "
                 "saved, use AT+HOTRESTART to apply)\r\n",
                 val, p->name, (unsigned)p->attack, (unsigned)p->release);
    at_send_ok();
}

static void cmd_codec_query(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    const char *name = (cfg.codec_mode == CODEC_MODE_PCM) ? "PCM" : "ADPCM";
    const char *desc = (cfg.codec_mode == CODEC_MODE_PCM)
                           ? "raw 16/24-bit, no compression"
                           : "DVI4 IMA, 4 bits/sample";
    at_send_data("+CODEC:%u (%s - %s)\r\n",
                 (unsigned)cfg.codec_mode, name, desc);
    at_send_ok();
}

static void cmd_codec_set(const char *args)
{
    char *endptr = NULL;
    long val = strtol(args, &endptr, 10);
    if (endptr == args || *endptr != '\0' ||
        val < CODEC_MODE_ADPCM || val > CODEC_MODE_PCM)
    {
        at_send_data("+ERR:codec must be 0 (ADPCM) or 1 (PCM)\r\n");
        at_send_error();
        return;
    }

    esp_err_t err = config_set_codec_mode((uint8_t)val);
    if (err != ESP_OK)
    {
        at_send_data("+ERR:config_set_codec_mode failed\r\n");
        at_send_error();
        return;
    }

    const char *name = (val == CODEC_MODE_PCM) ? "PCM (raw 16/24-bit)"
                                               : "ADPCM (DVI4 IMA)";
    at_send_data("+CODEC:set to %ld (%s, saved, use AT+HOTRESTART to apply)\r\n",
                 val, name);
    at_send_ok();
}

static void cmd_wch_query(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    at_send_data("+WCH:%u (channel %u)\r\n",
                 (unsigned)cfg.wifi_channel, (unsigned)cfg.wifi_channel);
    at_send_ok();
}

static void cmd_wch_set(const char *args)
{
    char *endptr = NULL;
    long val = strtol(args, &endptr, 10);
    if (endptr == args || *endptr != '\0' || val < 1 || val > 14)
    {
        at_send_data("+ERR:wch must be 1-14\r\n");
        at_send_error();
        return;
    }

    esp_err_t err = config_set_wifi_channel((uint8_t)val);
    if (err != ESP_OK)
    {
        at_send_data("+ERR:config_set_wifi_channel failed\r\n");
        at_send_error();
        return;
    }

    at_send_data("+WCH:set to %ld (saved, use AT+HOTRESTART to apply)\r\n", val);
    at_send_ok();
}

/* AT+XPORT? — current transport (0=UDP, 1=TCP, 2=RawTX).
 * AT+XPORT=n — set, auto-save to NVS, apply via HOTRESTART (UDP<->TCP) or RST (RAWTX). */
static void cmd_xport_query(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    const char *name;
    switch (cfg.transport_mode)
    {
    case TRANSPORT_MODE_TCP:
        name = "TCP";
        break;
    case TRANSPORT_MODE_RAWTX:
        name = "Raw 802.11 TX";
        break;
    case TRANSPORT_MODE_UDP:
    default:
        name = "UDP";
        break;
    }
    at_send_data("+XPORT:%u (%s)\r\n", (unsigned)cfg.transport_mode, name);
    at_send_ok();
}

static void cmd_xport_set(const char *args)
{
    char *endptr = NULL;
    long val = strtol(args, &endptr, 10);
    if (endptr == args || *endptr != '\0' ||
        val < 0 || val > TRANSPORT_MODE_RAWTX)
    {
        at_send_data("+ERR:xport must be 0=UDP, 1=TCP, 2=RawTX\r\n");
        at_send_error();
        return;
    }

    esp_err_t err = config_set_transport_mode((uint8_t)val);
    if (err != ESP_OK)
    {
        at_send_data("+ERR:config_set_transport_mode failed\r\n");
        at_send_error();
        return;
    }

    /* FIX (2-E MEDIUM #44): see FIXES.md — HOTRESTART for UDP<->TCP, RST for RAWTX. */
    at_send_data("+XPORT:set to %ld (saved, use AT+RST)\r\n", val);
    at_send_ok();
}

/* AT+TIMING? — show current I2S RX delays.
 * AT+TIMING=sd,ws,bck — set (0..3 each, saved to NVS).
 * Applied on next AT+HOTRESTART or stream start. */
static void cmd_timing_query(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    at_send_data("+TIMING:sd=%u,ws=%u,bck=%u\r\n",
                 (unsigned)cfg.i2s_timing_sd_delay,
                 (unsigned)cfg.i2s_timing_ws_delay,
                 (unsigned)cfg.i2s_timing_bck_delay);
    at_send_ok();
}

static void cmd_timing_set(const char *args)
{
    /* Format: sd,ws,bck — three integers 0..3 separated by commas. */
    /* FIX (M24): see FIXES.md — buf 64 to fit "INT_MAX,INT_MAX,INT_MAX". */
    char buf[64];
    /* FIX FR-AT #16: see FIXES.md — bounds-check the input length BEFORE
     * strncpy so an over-long AT+TIMING= line is rejected cleanly instead
     * of silently truncating to the first 63 chars (which would parse as
     * a valid-looking but wrong set of values). */
    if (strlen(args) >= sizeof(buf))
    {
        at_send_data("+ERR:input too long\r\n");
        at_send_error();
        return;
    }
    strncpy(buf, args, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *p = buf;
    char *endptr = NULL;
    long sd = strtol(p, &endptr, 10);
    if (endptr == p || *endptr != ',')
    {
        at_send_data("+ERR:format is sd,ws,bck (each 0-3)\r\n");
        at_send_error();
        return;
    }
    p = endptr + 1;
    long ws = strtol(p, &endptr, 10);
    if (endptr == p || *endptr != ',')
    {
        at_send_data("+ERR:format is sd,ws,bck (each 0-3)\r\n");
        at_send_error();
        return;
    }
    p = endptr + 1;
    long bck = strtol(p, &endptr, 10);
    if (endptr == p || *endptr != '\0')
    {
        at_send_data("+ERR:format is sd,ws,bck (each 0-3)\r\n");
        at_send_error();
        return;
    }

    if (sd < 0 || sd > I2S_TIMING_DELAY_MAX ||
        ws < 0 || ws > I2S_TIMING_DELAY_MAX ||
        bck < 0 || bck > I2S_TIMING_DELAY_MAX)
    {
        at_send_data("+ERR:each value must be 0-%d\r\n", I2S_TIMING_DELAY_MAX);
        at_send_error();
        return;
    }

    esp_err_t err = config_set_i2s_timing((uint8_t)sd, (uint8_t)ws, (uint8_t)bck);
    if (err != ESP_OK)
    {
        at_send_data("+ERR:config_set_i2s_timing failed\r\n");
        at_send_error();
        return;
    }

    at_send_data("+TIMING:set to sd=%ld,ws=%ld,bck=%ld (saved, use AT+HOTRESTART to apply)\r\n",
                 sd, ws, bck);
    at_send_ok();
}

#if BATTERY_ENABLED
static void cmd_batt_query_impl(void)
{
    /* AT+BATT? or AT+BATT — last measured battery voltage + percent.
     * FIX (H13): see FIXES.md — use cached value (avoids 750ms blocking read).
     * FIX FR-AT #17: see FIXES.md — never call battery_get_voltage_mv() from
     * the AT task. It blocks ~750ms on the ADC mutex and is documented in
     * battery.h as AT-unsafe. If there is no cached reading yet (just-booted,
     * monitor task hasn't sampled), return "+BATT:no reading yet" instead of
     * falling back to a fresh blocking read. */
    uint32_t v_mv = battery_get_last_mv();
    if (v_mv == 0)
    {
        at_send_data("+BATT:no reading yet\r\n");
        at_send_ok();
        return;
    }
    uint8_t pct = battery_get_percent();

    if (v_mv < BATT_BAD_MV)
    {
        at_send_data("+BATT:%u mV (invalid reading, divider disconnected?)\r\n",
                     (unsigned)v_mv);
    }
    else
    {
        const char *state;
        if (v_mv < BATT_CRITICAL_MV)
        {
            state = "CRITICAL - deep sleep pending";
        }
        else if (v_mv < BATT_START_MV)
        {
            state = "LOW";
        }
        else
        {
            state = "OK";
        }
        at_send_data("+BATT:%u mV (%u%%, %s)\r\n",
                     (unsigned)v_mv, (unsigned)pct, state);
    }
    at_send_ok();
}
#endif

static void cmd_hotrestart_impl(void)
{
    /* AT+HOTRESTART — restart the stream without rebooting. Stops the current
     * stream and starts a new one with current NVS config, so audio parameter
     * changes (AT+RATE/BITS/FMT/CH/GAIN/AGC/CODEC/WCH) take effect without a
     * ~1s reboot.
     *
     * AT+XPORT behavior: UDP<->TCP applied on the fly (only sockets swapped).
     * Any RAWTX transition is NOT applied by HOTRESTART (start_streaming keeps
     * the OLD transport and logs a warning) — AT+RST required.
     * Server-initiated stop+start does NOT apply a pending AT+XPORT change.
     * Only works when the stream is active (otherwise no-op). */
    if (!streaming_is_active())
    {
        /* FIX (L32): see FIXES.md — return ERROR when request did nothing. */
        at_send_data("+ERR:HOTRESTART:stream not active - params saved, "
                     "will apply on next start\r\n");
        at_send_error();
        return;
    }
    if (streaming_request_restart())
    {
        at_send_data("+HOTRESTART:stream restart requested (stop+start)\r\n");
        at_send_ok();
    }
    else
    {
        at_send_data("+ERR:stream control not initialized\r\n");
        at_send_error();
    }
}

static void cmd_status_impl(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    svc_port_status_t st;
    svc_port_get_status(&st);

    extern void mxr_dump(void);
    mxr_dump();

    at_send_data("+STATUS:firmware=" FIRMWARE_VERSION "\r\n");
    at_send_data("+STATUS:wifi_ssid=\"%s\"\r\n", cfg.wifi_ssid);
    at_send_data("+STATUS:hostname=\"%s\"\r\n", cfg.hostname);
    at_send_data("+STATUS:wifi_connected=%s\r\n", wifi_sta_is_connected() ? "YES" : "NO");
    if (wifi_sta_is_connected())
    {
        tcpip_adapter_ip_info_t ip_info;
        if (tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip_info) == ESP_OK)
        {
            at_send_data("+STATUS:ip=%d.%d.%d.%d\r\n",
                         (int)((ip_info.ip.addr >> 0) & 0xFF),
                         (int)((ip_info.ip.addr >> 8) & 0xFF),
                         (int)((ip_info.ip.addr >> 16) & 0xFF),
                         (int)((ip_info.ip.addr >> 24) & 0xFF));
        }
        else
        {
            at_send_data("+STATUS:ip=error\r\n");
        }
    }
    at_send_data("+STATUS:svc_port=%u\r\n", (unsigned)cfg.svc_port);
    at_send_data("+STATUS:svc_running=%s\r\n", st.running ? "YES" : "NO");
    at_send_data("+STATUS:svc_protocol=EASSP v%d (0xEA%02X)\r\n", EASSP_VER, EASSP_MAGIC1);
    at_send_data("+STATUS:svc_commands=DISCOVER,CONFIGURE,STOP\r\n");
    at_send_data("+STATUS:mac=%02X:%02X:%02X:%02X:%02X:%02X\r\n",
                 (unsigned)st.mac[0], (unsigned)st.mac[1], (unsigned)st.mac[2],
                 (unsigned)st.mac[3], (unsigned)st.mac[4], (unsigned)st.mac[5]);

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
    {
        at_send_data("+STATUS:wifi_rssi=%d dBm\r\n", ap_info.rssi);
    }
    at_send_data("+STATUS:wifi_tx_power=%u dBm\r\n", (unsigned)cfg.tx_power);
    at_send_data("+STATUS:streaming=%s\r\n", st.streaming ? "YES" : "NO");
    at_send_data("+STATUS:sample_rate=%u\r\n", (unsigned)cfg.sample_rate);
    /* FIX FR-AT #15: see FIXES.md — gate frame_ms on streaming_frame_ms_known()
     * so STATUS doesn't print a stale frame_ms after stop_streaming. */
    {
        unsigned fms = streaming_frame_ms_known() ? (unsigned)streaming_get_frame_ms() : 0;
        at_send_data("+STATUS:frame_ms=%u (computed)\r\n", fms);
    }
    at_send_data("+STATUS:bits_per_sample=%u\r\n", (unsigned)cfg.bits_per_sample);
    at_send_data("+STATUS:gain=%u (0=bypass, 32=+30dB)\r\n", (unsigned)cfg.gain);
    {
        const agc_preset_t *ap = (cfg.agc_mode < AGC_MODE_COUNT) ? &AGC_PRESETS[cfg.agc_mode] : &AGC_PRESETS[0];
        at_send_data("+STATUS:agc=%u (%s, attack=%u, release=%u)\r\n",
                     (unsigned)cfg.agc_mode, ap->name,
                     (unsigned)ap->attack, (unsigned)ap->release);
    }
    at_send_data("+STATUS:codec=%u (%s)\r\n", (unsigned)cfg.codec_mode,
                 cfg.codec_mode == CODEC_MODE_PCM ? "PCM" : "ADPCM");
    {
        const char *xname = "UDP";
        if (cfg.transport_mode == TRANSPORT_MODE_TCP)
            xname = "TCP";
        else if (cfg.transport_mode == TRANSPORT_MODE_RAWTX)
            xname = "Raw 802.11 TX";
        at_send_data("+STATUS:transport_config=%u (%s)\r\n",
                     (unsigned)cfg.transport_mode, xname);
    }
    /* FIX FR-AT #14: see FIXES.md — also show the ACTIVE transport (may
     * differ from the NVS-configured one when an AT+XPORT change is pending
     * and has not yet been applied by AT+HOTRESTART/AT+RST, or when a
     * RAWTX transition was rejected by start_streaming). */
    {
        uint8_t active = stream_mode_current_transport();
        const char *xname = "UDP";
        if (active == TRANSPORT_MODE_TCP)
            xname = "TCP";
        else if (active == TRANSPORT_MODE_RAWTX)
            xname = "Raw 802.11 TX";
        at_send_data("+STATUS:transport_active=%u (%s)\r\n",
                     (unsigned)active, xname);
    }
    if (cfg.transport_mode == TRANSPORT_MODE_RAWTX)
    {
        at_send_data("+STATUS:wifi_channel=%u\r\n", (unsigned)cfg.wifi_channel);
    }
    at_send_data("+STATUS:i2s_timing=sd=%u,ws=%u,bck=%u\r\n",
                 (unsigned)cfg.i2s_timing_sd_delay,
                 (unsigned)cfg.i2s_timing_ws_delay,
                 (unsigned)cfg.i2s_timing_bck_delay);
#if BATTERY_ENABLED
    {
        uint32_t batt_mv = battery_get_last_mv();
        uint8_t batt_pct = battery_get_percent();
        if (batt_mv == 0)
        {
            at_send_data("+STATUS:battery=not measured yet\r\n");
        }
        else
        {
            at_send_data("+STATUS:battery=%u mV (%u%%)\r\n",
                         (unsigned)batt_mv, (unsigned)batt_pct);
        }
    }
#else
    at_send_data("+STATUS:battery=disabled (menuconfig)\r\n");
#endif
    at_send_data("+STATUS:comm_format=%d (%s)\r\n", cfg.comm_format,
                 cfg.comm_format == I2S_CFMT_LSB ? "LSB" : "Philips");
    {
        const char *ch_desc;
        switch (cfg.channel_format)
        {
        case I2S_CHFMT_LEFT:
            ch_desc = "left";
            break;
        case I2S_CHFMT_RIGHT:
            ch_desc = "right";
            break;
        case I2S_CHFMT_STEREO:
            ch_desc = "stereo";
            break;
        default:
            ch_desc = "unknown";
            break;
        }
        at_send_data("+STATUS:channel_format=%d (%s, %u ch)\r\n",
                     cfg.channel_format, ch_desc,
                     (unsigned)channel_format_to_count(cfg.channel_format));
    }
    {
        /* Bitrate depends on codec: ADPCM=4 bits/sample, PCM=bits_per_sample. */
        unsigned bits_per_codec = (cfg.codec_mode == CODEC_MODE_PCM)
                                      ? cfg.bits_per_sample
                                      : 4;
        at_send_data("+STATUS:bitrate=%u\r\n",
                     (unsigned)(cfg.sample_rate * bits_per_codec *
                                channel_format_to_count(cfg.channel_format)));
    }
    at_send_data("+STATUS:error=%s (%d)\r\n",
                 st.error_code == 0 ? "NONE" : st.error_code == 1 ? "MEMORY"
                                           : st.error_code == 2   ? "I2S"
                                           : st.error_code == 3   ? "CODEC"
                                           : st.error_code == 4   ? "NETWORK"
                                           : st.error_code == 5   ? "WATCHDOG"
                                                                  : "UNKNOWN",
                 st.error_code);
    if (st.streaming)
    {
        at_send_data("+STATUS:watchdog=%d ms remaining\r\n", st.watchdog_remaining_ms);
    }
    else
    {
        at_send_data("+STATUS:watchdog=OFF (not streaming)\r\n");
    }
    at_send_data("+STATUS:packets_sent=%u\r\n", (unsigned)st.packets_sent);
    at_send_data("+STATUS:free_heap=%u\r\n", (unsigned)esp_get_free_heap_size());
    at_send_ok();
}

static void cmd_factory_impl(void)
{
    esp_err_t err = config_factory_reset();
    if (err != ESP_OK)
    {
        at_send_data("+ERR:factory reset failed\r\n");
        at_send_error();
        return;
    }

    at_send_str("\r\n+FACTORY:defaults restored, rebooting...\r\n");
    at_send_ok();

    /* FIX (2-E HIGH #12): see FIXES.md — reboot so hostname/port changes apply. */
    vTaskDelay(pdMS_TO_TICKS(200)); /* let UART flush the +OK response */
    esp_restart();                  /* never returns */
    /* unreachable */
}

/* FIX (UART0/B9): see FIXES.md. */
/* AT+LOG=0 redirects ALL log output via esp_log_set_putchar() (ESP8266 RTOS
 * SDK v3.4 API; not esp_log_set_vprintf which is ESP-IDF only). AT+LOG=1
 * restores the original putchar. AT responses use uart_write_bytes() directly,
 * NOT esp_log — unaffected by mute. */

/* No-op log putchar — swallows ALL log output character by character. */
static int log_putchar_noop(int ch)
{
    (void)ch;
    return ch;
}

/* Saved original putchar (SDK default, writes to UART0). Captured at first
 * AT+LOG=0 call, reused for AT+LOG=1 restore. */
static putchar_like_t s_orig_putchar = NULL;
static bool s_logs_muted = false;

static void cmd_log_set(const char *args)
{
    char *endptr = NULL;
    long val = strtol(args, &endptr, 10);
    if (endptr == args || *endptr != '\0')
    {
        at_send_data("+ERR:LOG must be 0 (mute) or 1 (unmute)\r\n");
        at_send_error();
        return;
    }

    if (val == 0)
    {
        /* Mute: save original putchar on first call, then redirect to no-op. */
        if (!s_logs_muted)
        {
            s_orig_putchar = esp_log_set_putchar(log_putchar_noop);
            s_logs_muted = true;
        }
        at_send_data("+LOG:logs muted (putchar redirected to noop)\r\n");
        at_send_ok();
    }
    else if (val == 1)
    {
        /* Unmute: restore the original putchar (writes to UART0). */
        if (s_logs_muted && s_orig_putchar)
        {
            esp_log_set_putchar(s_orig_putchar);
            s_logs_muted = false;
        }
        at_send_data("+LOG:logs restored\r\n");
        at_send_ok();
    }
    else
    {
        at_send_data("+ERR:LOG must be 0 (mute) or 1 (unmute)\r\n");
        at_send_error();
    }
}

static void cmd_log_query(void)
{
    at_send_data("+LOG:%s\r\n", s_logs_muted ? "0 (muted)" : "1 (enabled)");
    at_send_ok();
}
