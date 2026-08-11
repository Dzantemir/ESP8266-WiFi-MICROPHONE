/*
 * AT command parser for ESP8266 RTOS SDK v3.4.
 *
 * Uses UART0 for both AT command input and log output. Full duplex:
 * AT commands arrive on RX, log output goes to TX.
 *
 * Command format:
 *   AT              -> OK
 *   AT+CMD?         -> +CMD:value\r\nOK
 *   AT+CMD=val      -> OK (or ERROR)
 *
 * All commands must be terminated with \r\n (or just \r).
 *
 * Auto-save: AT+WIFI, AT+PORT, AT+TXPWR, AT+RATE, AT+BITS, AT+FMT, AT+CH
 * automatically save to NVS. No separate AT+SAVE command needed.
 *
 * AT+TXPWR applies immediately (no restart needed).
 * AT+WIFI applies immediately if WiFi driver is running.
 * AT+PORT requires restart (AT+RST) — service port can't be changed on the fly.
 *
 * Audio parameters (AT+RATE, AT+BITS, AT+FMT, AT+CH) apply on next stream start.
 *
 * File layout (R3-B split, see worklog):
 *   at_cmd.c       — UART driver state, at_cmd_init, at_task_fn,
 *                    AT_COMMANDS dispatch table, at_process_line, and the
 *                    at_send_* UART helpers.
 *   at_handlers.c  — all cmd_* handler implementations (referenced by
 *                    AT_COMMANDS[]) and their handler-only helpers.
 *   at_internal.h  — shared typedefs (cmd_handler_fn_t, at_cmd_entry_t),
 *                    at_send_* declarations, and cmd_* forward declarations.
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

/* Shared declarations with at_handlers.c (handler typedefs, AT_COMMANDS
 * extern, at_send_* prototypes, cmd_* forward decls). */
#include "at_internal.h"

static const char *TAG = "at_cmd";

#define UART_NUM UART_NUM_0
/* FIX (M25): see FIXES.md — RX queue depth = UART_BUF_SIZE * 2 = 1024 bytes. */
#define UART_BUF_SIZE 512
#define CMD_BUF_SIZE 256

/* ---- Forward declarations ---- */
static void at_task_fn(void *arg);
static void at_process_line(const char *line, int len);
/* at_send_str / at_send_ok / at_send_error / at_send_data are declared
 * non-static in at_internal.h (called from at_handlers.c); their
 * definitions live below. */

static volatile bool s_running = false;

esp_err_t at_cmd_init(void)
{
    uart_config_t uart_cfg = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    esp_err_t err = uart_param_config(UART_NUM, &uart_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "UART param config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_driver_install(UART_NUM, UART_BUF_SIZE * 2, 0, 0, NULL, 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "UART driver install failed: %s", esp_err_to_name(err));
        return err;
    }

    s_running = true;
    /* FIX (2-E LOW): see FIXES.md — check xTaskCreate return. */
    BaseType_t tr = xTaskCreate(at_task_fn, "at_cmd", TASK_STACK_AT,
                                NULL, TASK_PRIO_AT, NULL);
    if (tr != pdPASS) {
        ESP_LOGE(TAG, "Failed to create AT task (ret=%d)", (int)tr);
        /* FIX (4-E LOW #13): see FIXES.md — cleanup UART driver on task-create failure. */
        uart_driver_delete(UART_NUM);
        s_running = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "AT command interface initialized (UART0, %d 8N1)", UART_BAUD_RATE);
    return ESP_OK;
}

/* ---- UART helpers ----
 * Non-static (declared in at_internal.h) so at_handlers.c can call them.
 * The UART driver state lives in this file (UART_NUM macro), so the
 * implementations stay here. */

void at_send_str(const char *str)
{
    uart_write_bytes(UART_NUM, str, strlen(str));
}

void at_send_ok(void)
{
    at_send_str("\r\nOK\r\n");
}

void at_send_error(void)
{
    at_send_str("\r\nERROR\r\n");
}

void at_send_data(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0)
    {
        /* FIX (C1): see FIXES.md — cap write length to buffer size on truncation. */
        size_t write_len = ((size_t)n < sizeof(buf)) ? (size_t)n : sizeof(buf) - 1;
        uart_write_bytes(UART_NUM, buf, write_len);
        /* FIX (GROK-26): see FIXES.md — append ellipsis marker on truncation. */
        if ((size_t)n > sizeof(buf) - 1 && write_len >= 3)
        {
            uart_write_bytes(UART_NUM, "...[truncated]\r\n", 16);
        }
    }
}

/* ---- AT task ---- */

static void at_task_fn(void *arg)
{
    char cmd_buf[CMD_BUF_SIZE];
    int pos = 0;
    /* FIX (GROK-25): see FIXES.md — one-shot flag for overflow reports. */
    bool overflow_reported = false;
    /* FIX (GROK-G11-16): see FIXES.md — discard rest of over-long line. */
    bool overflow_discard = false;

    at_send_str("\r\n=== ESP8266 ADPCM Streamer ===\r\n");
    at_send_str("AT command interface ready (115200 8N1)\r\n");
    at_send_str("Type AT+HELP for command list\r\n");

    while (s_running)
    {
        uint8_t ch;
        int n = uart_read_bytes(UART_NUM, &ch, 1, pdMS_TO_TICKS(100));
        if (n <= 0)
            continue;

        /* Echo off (silent). */

        if (ch == '\r' || ch == '\n')
        {
            /* Don't dispatch if we were discarding an over-long line. */
            if (pos > 0 && !overflow_discard)
            {
                cmd_buf[pos] = '\0';
                at_process_line(cmd_buf, pos);
            }
            pos = 0;
            overflow_reported = false;
            overflow_discard = false;
            continue;
        }

        if (overflow_discard)
            continue;

        if (pos < CMD_BUF_SIZE - 1)
        {
            cmd_buf[pos++] = (char)ch;
        }
        else
        {
            /* Overflow — discard char, wait for line end.
             * FIX (GROK-25): see FIXES.md.  FIX (GROK-G11-16): see FIXES.md. */
            if (!overflow_reported)
            {
                at_send_data("+ERR:command too long (max %d chars), discarded\r\n",
                             (int)(CMD_BUF_SIZE - 1));
                overflow_reported = true;
                overflow_discard = true;
            }
            continue;
        }
    }

    vTaskDelete(NULL);
}

/* ---- Command table (R2-C) ----
 * One row per AT+NAME command. takes_args=true means AT+NAME=value is valid;
 * false means only AT+NAME (bare) and/or AT+NAME? are accepted (the handler
 * decides which). Adding a command = 1 row + 1 handler.
 *
 * The handler function pointers (cmd_*) are defined in at_handlers.c and
 * declared in at_internal.h. */

const at_cmd_entry_t AT_COMMANDS[] = {
    { "RST",        cmd_rst,        false },
    { "GMR",        cmd_gmr,        false },
    { "HELP",       cmd_help,       false },
    { "WIFI",       cmd_wifi,       true  },
    { "PORT",       cmd_port,       true  },
    { "TXPWR",      cmd_txpwr,      true  },
    { "RATE",       cmd_rate,       true  },
    { "BITS",       cmd_bits,       true  },
    { "FMT",        cmd_fmt,        true  },
    { "CH",         cmd_ch,         true  },
    { "STATUS",     cmd_status,     false },
    { "FACTORY",    cmd_factory,    false },
    { "HOST",       cmd_host,       true  },
    { "GAIN",       cmd_gain,       true  },
    { "AGC",        cmd_agc,        true  },
    { "CODEC",      cmd_codec,      true  },
    { "XPORT",      cmd_xport,      true  },
    { "WCH",        cmd_wch,        true  },
    { "TIMING",     cmd_timing,     true  },
    { "HOTRESTART", cmd_hotrestart, false },
    { "LOG",        cmd_log,        true  },
#if BATTERY_ENABLED
    { "BATT",       cmd_batt,       false },
#endif
};
#define AT_COMMAND_COUNT (sizeof(AT_COMMANDS) / sizeof(AT_COMMANDS[0]))

/* ---- Command processing ---- */

static void at_process_line(const char *line, int len)
{
    if (len == 0)
        return;

    /* Strip leading whitespace. */
    while (*line == ' ' || *line == '\t')
    {
        line++;
        len--;
    }

    /* FIX (AUDIT-LOW): see FIXES.md — re-check len after strip. */
    if (len == 0)
        return;

    if (strcasecmp(line, "AT") == 0)
    {
        cmd_at();
        return;
    }
    if (strncasecmp(line, "AT+", 3) != 0)
    {
        at_send_error();
        return;
    }

    /* Parse "AT+CMD?" or "AT+CMD=args". */
    const char *p = line + 3;
    /* F3-A LOW #7: strip leading whitespace between "AT+" and the command
     * name. Without this, "AT+ RATE=16000" → cmd_name=" RATE" and
     * strcasecmp(" RATE","RATE") fails → "+ERR:unknown command". The
     * leading-line strip above only handled whitespace BEFORE "AT+". */
    while (*p == ' ' || *p == '\t') p++;
    const char *eq = strchr(p, '=');
    const char *q = strchr(p, '?');

    char cmd_name[32];
    int cmd_len;
    bool is_query = false;
    /* FIX (2-E MEDIUM #43): see FIXES.md — args is non-const for in-place strip. */
    char *args = NULL;

    if (q && (!eq || q < eq))
    {
        cmd_len = (int)(q - p);
        is_query = true;
    }
    else if (eq)
    {
        cmd_len = (int)(eq - p);
        args = (char *)(eq + 1);
        /* FIX (2-E MEDIUM #43): see FIXES.md.  FIX (4-E LOW #14): see FIXES.md. */
        if (args[0] != '"')
        {
            char *end = args + strlen(args);
            while (end > args && (end[-1] == ' ' || end[-1] == '\t' ||
                                  end[-1] == '\r' || end[-1] == '\n'))
            {
                *--end = '\0';
            }
        }
    }
    else
    {
        cmd_len = (int)strlen(p);
    }

    if (cmd_len <= 0 || cmd_len >= (int)sizeof(cmd_name))
    {
        at_send_error();
        return;
    }
    memcpy(cmd_name, p, cmd_len);
    cmd_name[cmd_len] = '\0';

    /* FIX FR-AT #11: see FIXES.md — strip trailing whitespace from the
     * command name. Without this, "AT+RATE ?" yields cmd_name="RATE " and
     * the strcasecmp against "RATE" fails. Walk backwards from the end of
     * cmd_name (already terminated at the boundary point by the logic above)
     * and overwrite spaces/tabs. */
    {
        size_t name_len = strlen(cmd_name);
        while (name_len > 0 &&
               (cmd_name[name_len - 1] == ' ' || cmd_name[name_len - 1] == '\t'))
        {
            cmd_name[--name_len] = '\0';
        }
        if (name_len == 0)
        {
            at_send_error();
            return;
        }
    }

    /* FIX FR-AT #11: see FIXES.md — skip leading whitespace in args. Without
     * this, "AT+RATE= 16000" passes args=" 16000" to the handler unstripped.
     * Done AFTER the existing trailing-whitespace strip so quoted args (which
     * skip the trailing strip above) still get their leading spaces skipped
     * only when the value begins with whitespace before the quote. */
    if (args)
    {
        while (*args == ' ' || *args == '\t')
            args++;
    }

    /* Table-driven dispatch (R2-C).
     * Find the matching command entry; the dispatch enforces the
     * takes_args rule for AT+NAME=value, while each handler decides
     * query vs bare-name behavior (preserving the per-command semantics
     * of the original if/else cascade). */
    for (int i = 0; i < (int)AT_COMMAND_COUNT; i++)
    {
        if (strcasecmp(cmd_name, AT_COMMANDS[i].name) != 0)
            continue;

        const at_cmd_entry_t *entry = &AT_COMMANDS[i];
        if (is_query)
        {
            /* AT+NAME? — query path. Handler rejects query for non-arg
             * commands that don't support it (RST/GMR/HELP/STATUS/etc.). */
            entry->handler(UART_NUM, true, NULL);
        }
        else if (args)
        {
            /* AT+NAME=value — set path. Reject for takes_args=false. */
            if (!entry->takes_args)
            {
                at_send_error();
            }
            else
            {
                entry->handler(UART_NUM, false, args);
            }
        }
        else
        {
            /* AT+NAME (bare) — handler decides (query or error). */
            entry->handler(UART_NUM, false, NULL);
        }
        return;
    }

    /* Unknown command. */
    at_send_data("+ERR:unknown command \"%s\"\r\n", cmd_name);
    at_send_error();
}
