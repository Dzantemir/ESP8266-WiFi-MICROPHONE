#ifndef AT_INTERNAL_H
#define AT_INTERNAL_H

/*
 * Internal shared declarations for the AT command subsystem.
 *
 * Public API (at_cmd_init) is in at_cmd.h. This header is the contract
 * between at_cmd.c (UART driver state, dispatch table, at_process_line)
 * and at_handlers.c (the cmd_* handler implementations).
 *
 * Split performed in R3-B; see worklog. The handler bodies are byte-
 * for-byte unchanged from the original monolithic at_cmd.c.
 */

#include <stdbool.h>

#include "driver/uart.h"
#include "esp_err.h"

/* board_config.h defines BATTERY_ENABLED (used for the cmd_batt guard). */
#include "board_config.h"

/* ---- Handler function type ----
 * Each AT+NAME command is a single table row + one handler with this
 * signature. The dispatch in at_process_line() (at_cmd.c) enforces the
 * takes_args rule for AT+NAME=value; handlers themselves decide query
 * vs bare behavior. */
typedef esp_err_t (*cmd_handler_fn_t)(uart_port_t uart, bool is_query, const char *args);

/* ---- Command table entry ---- */
typedef struct {
    const char *name;           /* e.g. "WIFI" for AT+WIFI */
    cmd_handler_fn_t handler;   /* called with (uart, is_query, args) */
    bool takes_args;            /* true if AT+NAME=value form is valid */
} at_cmd_entry_t;

/* The command table (defined in at_cmd.c, used by at_process_line).
 * at_handlers.c does not iterate this table; it only supplies the
 * function pointers the table references. */
extern const at_cmd_entry_t AT_COMMANDS[];

/* ---- UART helpers (defined in at_cmd.c, used by at_handlers.c) ----
 * These wrap uart_write_bytes() on UART_NUM_0 and the UART driver state
 * lives in at_cmd.c, so the implementations stay there. Declared here so
 * the handlers in at_handlers.c can call them. */
void at_send_str(const char *str);
void at_send_ok(void);
void at_send_error(void);
void at_send_data(const char *fmt, ...);

/* ---- Handler forward declarations (defined in at_handlers.c) ---- */

/* cmd_at — bare "AT" test command (no args, no is_query). Called directly
 * from at_process_line as a special case (outside the table). Returns OK. */
void cmd_at(void);

/* Dispatch handlers referenced by AT_COMMANDS[] in at_cmd.c.
 * Each dispatches to the appropriate cmd_X_query / cmd_X_set / cmd_X_impl
 * based on (is_query, args). Bodies unchanged from R2-C. */
esp_err_t cmd_rst(uart_port_t uart, bool is_query, const char *args);
esp_err_t cmd_gmr(uart_port_t uart, bool is_query, const char *args);
esp_err_t cmd_help(uart_port_t uart, bool is_query, const char *args);
esp_err_t cmd_wifi(uart_port_t uart, bool is_query, const char *args);
esp_err_t cmd_host(uart_port_t uart, bool is_query, const char *args);
esp_err_t cmd_port(uart_port_t uart, bool is_query, const char *args);
esp_err_t cmd_txpwr(uart_port_t uart, bool is_query, const char *args);
esp_err_t cmd_rate(uart_port_t uart, bool is_query, const char *args);
esp_err_t cmd_bits(uart_port_t uart, bool is_query, const char *args);
esp_err_t cmd_fmt(uart_port_t uart, bool is_query, const char *args);
esp_err_t cmd_ch(uart_port_t uart, bool is_query, const char *args);
esp_err_t cmd_status(uart_port_t uart, bool is_query, const char *args);
esp_err_t cmd_factory(uart_port_t uart, bool is_query, const char *args);
esp_err_t cmd_gain(uart_port_t uart, bool is_query, const char *args);
esp_err_t cmd_agc(uart_port_t uart, bool is_query, const char *args);
esp_err_t cmd_codec(uart_port_t uart, bool is_query, const char *args);
esp_err_t cmd_xport(uart_port_t uart, bool is_query, const char *args);
esp_err_t cmd_wch(uart_port_t uart, bool is_query, const char *args);
esp_err_t cmd_timing(uart_port_t uart, bool is_query, const char *args);
esp_err_t cmd_hotrestart(uart_port_t uart, bool is_query, const char *args);
esp_err_t cmd_log(uart_port_t uart, bool is_query, const char *args);
#if BATTERY_ENABLED
esp_err_t cmd_batt(uart_port_t uart, bool is_query, const char *args);
#endif

#endif /* AT_INTERNAL_H */
