#ifndef PIPELINE_INTERNAL_H
#define PIPELINE_INTERNAL_H

/*
 * pipeline_internal.h
 * ===================
 *
 * Private interface shared between the four files that used to live in
 * main.c before the R3-A structural refactor:
 *
 *   main.c           — app_main, boot, main event loop, wifi_boot_retry_or_sleep
 *   pipeline.c       — stream lifecycle (start/stop/teardown) + pipeline tasks
 *                      (i2s/adpcm/pcm/tx) + pool management
 *   supervisor.c     — supervisor_task_fn (software watchdog)
 *   stream_control.c — streaming_* public API + STREAM_EVT_* event group
 *
 * The streaming_* API is also declared publicly in stream_control.h —
 * this header only adds the cross-file plumbing (externs for statics
 * owned by each file, plus the few non-static helper functions called
 * across boundaries).
 *
 * Layout rule: every static variable that needs to be visible across
 * these files is declared `extern` here and defined (without `extern`)
 * in exactly one of the four .c files. Static functions used by only
 * one file stay `static` in that file; static functions used across
 * files lose the `static` qualifier and are prototyped here.
 */

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_err.h"

/*
 * STREAM_EVT_* bits live in svc_port.h (they are owned by the service
 * port — svc_port.c sets them, main.c clears/waits on them). Pull that
 * header in here so all four pipeline files see the same definitions.
 */
#include "svc_port.h" /* STREAM_EVT_START_REQ / STOP_REQ / ACTIVE */

/* ---- Pipeline task indices ---------------------------------------- */

typedef enum
{
    TASK_IDX_I2S = 0,
    TASK_IDX_ADPCM = 1,
    TASK_IDX_UDP = 2,
    TASK_IDX_COUNT = 3
} task_idx_t;

/* ---- Defined in stream_control.c --------------------------------- */

/* Event group carrying STREAM_EVT_START_REQ / STOP_REQ / ACTIVE.
 * Created by stream_control_init() at boot, owned by stream_control.c. */
extern EventGroupHandle_t s_stream_evt_grp;

/* ---- Defined in pipeline.c --------------------------------------- */

/* Runtime audio parameters — written by start_streaming(), read by the
 * pipeline tasks and (for s_channels / s_frame_ms / s_frame_ms_known)
 * by the stream_control.c accessors. */
extern uint8_t  s_channels;            /* 1 or 2, set at stream start     */
extern uint32_t s_frame_ms;            /* frame duration in ms            */
extern bool     s_frame_ms_known;      /* false until first successful start */

/* Set by streaming_request_restart() (stream_control.c), cleared by
 * start_streaming() (pipeline.c) once the transport-change decision
 * has been applied or refused. */
extern bool s_pending_transport_apply;

/* Task bookkeeping — also used by supervisor.c for stack high-water checks. */
extern TaskHandle_t      s_task_handles[TASK_IDX_COUNT];
extern SemaphoreHandle_t s_task_handles_mutex;

/* Boot-time WiFi state — set by app_main after the first successful
 * ops->wifi_init(), checked by start_streaming() to skip re-init. */
extern bool s_wifi_initialized;

/* ---- Defined in supervisor.c (only when supervisor is compiled in) -- */

#ifdef CONFIG_STREAMER_SUPERVISOR_ENABLED
extern volatile uint32_t s_supervisor_i2s_count;
extern volatile uint32_t s_supervisor_tx_count;
extern volatile uint32_t s_supervisor_tx_consecutive_drops;
extern volatile TickType_t s_supervisor_stream_start_tick;
#endif /* CONFIG_STREAMER_SUPERVISOR_ENABLED */

/* ---- Function prototypes ----------------------------------------- */

/* pipeline.c */
esp_err_t start_streaming(void);
void     stop_streaming(void);

/* supervisor.c */
void supervisor_task_fn(void *arg);

/* stream_control.c */
/* streaming_is_active / streaming_request_stop / streaming_request_restart /
 * streaming_get_frame_ms / streaming_frame_ms_known / streaming_get_channels
 * are declared in the public header stream_control.h. */
esp_err_t stream_control_init(void); /* creates s_stream_evt_grp */

/* main.c */
void wifi_boot_retry_or_sleep(uint8_t transport_mode);

#endif /* PIPELINE_INTERNAL_H */
