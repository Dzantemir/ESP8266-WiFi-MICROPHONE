#ifndef BOARD_TASKS_H
#define BOARD_TASKS_H

/* Task priorities, stack sizes, supervisor config, AT command interface.
 * Extracted from board_config.h (R3-C). */

#include "sdkconfig.h"
#include <stdint.h>
#include <stdbool.h>

/* ====================================================================
 *  Pipeline / Task Configuration
 * ==================================================================== */

#ifdef CONFIG_STREAMER_TASK_PRIO_I2S
#define TASK_PRIO_I2S       CONFIG_STREAMER_TASK_PRIO_I2S
#else
#define TASK_PRIO_I2S       5
#endif

#ifdef CONFIG_STREAMER_TASK_PRIO_ADPCM
#define TASK_PRIO_ADPCM     CONFIG_STREAMER_TASK_PRIO_ADPCM
#else
#define TASK_PRIO_ADPCM     3
#endif

#ifdef CONFIG_STREAMER_TASK_PRIO_UDP
#define TASK_PRIO_UDP       CONFIG_STREAMER_TASK_PRIO_UDP
#else
#define TASK_PRIO_UDP       2
#endif

#ifdef CONFIG_STREAMER_TASK_PRIO_AT
#define TASK_PRIO_AT        CONFIG_STREAMER_TASK_PRIO_AT
#else
#define TASK_PRIO_AT        1
#endif

#ifdef CONFIG_STREAMER_TASK_PRIO_SVC
#define TASK_PRIO_SVC       CONFIG_STREAMER_TASK_PRIO_SVC
#else
#define TASK_PRIO_SVC       2
#endif

#ifdef CONFIG_STREAMER_TASK_STACK_I2S
#define TASK_STACK_I2S      CONFIG_STREAMER_TASK_STACK_I2S
#else
#define TASK_STACK_I2S      3584
#endif

#ifdef CONFIG_STREAMER_TASK_STACK_ADPCM
#define TASK_STACK_ADPCM    CONFIG_STREAMER_TASK_STACK_ADPCM
#else
#define TASK_STACK_ADPCM    2560
#endif

#ifdef CONFIG_STREAMER_TASK_STACK_UDP
#define TASK_STACK_UDP      CONFIG_STREAMER_TASK_STACK_UDP
#else
#define TASK_STACK_UDP      3072
#endif

#ifdef CONFIG_STREAMER_TASK_STACK_AT
#define TASK_STACK_AT       CONFIG_STREAMER_TASK_STACK_AT
#else
#define TASK_STACK_AT       3584
#endif

#ifdef CONFIG_STREAMER_TASK_STACK_SVC
#define TASK_STACK_SVC      CONFIG_STREAMER_TASK_STACK_SVC
#else
#define TASK_STACK_SVC      3584
#endif

#ifdef CONFIG_STREAMER_TASK_STACK_SUPERVISOR
#define TASK_STACK_SUPERVISOR  CONFIG_STREAMER_TASK_STACK_SUPERVISOR
#else
#define TASK_STACK_SUPERVISOR  2048
#endif

#ifdef CONFIG_STREAMER_TASK_PRIO_SUPERVISOR
#define TASK_PRIO_SUPERVISOR   CONFIG_STREAMER_TASK_PRIO_SUPERVISOR
#else
#define TASK_PRIO_SUPERVISOR   1
#endif

#ifdef CONFIG_STREAMER_SUPERVISOR_MIN_HEAP
#define SUPERVISOR_MIN_HEAP_BYTES   CONFIG_STREAMER_SUPERVISOR_MIN_HEAP
#else
#define SUPERVISOR_MIN_HEAP_BYTES   15360
#endif

#ifdef CONFIG_STREAMER_SUPERVISOR_STALL_TIMEOUT
#define SUPERVISOR_STALL_TIMEOUT_MS CONFIG_STREAMER_SUPERVISOR_STALL_TIMEOUT
#else
#define SUPERVISOR_STALL_TIMEOUT_MS 15000
#endif

#ifdef CONFIG_STREAMER_SUPERVISOR_MIN_STACK
#define SUPERVISOR_MIN_STACK_BYTES  CONFIG_STREAMER_SUPERVISOR_MIN_STACK
#else
#define SUPERVISOR_MIN_STACK_BYTES  256
#endif

#ifdef CONFIG_STREAMER_SUPERVISOR_CHECK_INTERVAL
#define SUPERVISOR_CHECK_INTERVAL_MS CONFIG_STREAMER_SUPERVISOR_CHECK_INTERVAL
#else
#define SUPERVISOR_CHECK_INTERVAL_MS 2000
#endif

/* ====================================================================
 *  AT Command Interface
 * ==================================================================== */

#ifdef CONFIG_STREAMER_AT_CMD_ENABLED
#define AT_CMD_ENABLED      1
#else
#define AT_CMD_ENABLED      0
#endif

#ifdef CONFIG_STREAMER_UART_BAUD_RATE
#define UART_BAUD_RATE      CONFIG_STREAMER_UART_BAUD_RATE
#else
#define UART_BAUD_RATE      115200
#endif

#endif /* BOARD_TASKS_H */
