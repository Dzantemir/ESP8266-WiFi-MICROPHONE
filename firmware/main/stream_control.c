/*
 * stream_control.c
 * ================
 *
 * Public streaming_* API (declared in include/stream_control.h) plus
 * the FreeRTOS EventGroup that carries STREAM_EVT_START_REQ /
 * STREAM_EVT_STOP_REQ / STREAM_EVT_ACTIVE.
 *
 * Split out of main.c in the R3-A structural refactor. The actual
 * pipeline lifecycle (start_streaming / stop_streaming / teardown) lives
 * in pipeline.c; this file only owns the EventGroup and the small
 * accessor / request functions used by AT commands, svc_port, etc.
 *
 * Active state is encoded in the STREAM_EVT_ACTIVE bit of
 * s_stream_evt_grp (set by start_streaming, cleared by stop_streaming /
 * teardown_pipeline). streaming_is_active() is the canonical check used
 * by every pipeline task loop and by svc_port.
 */

/* ---- System / SDK includes ---- */
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"

/* ---- Project includes ---- */
#include "board_config.h"     /* channel_format_to_count */
#include "config_mgr.h"       /* config_get_copy, device_config_t */
#include "pipeline_internal.h"
#include "stream_control.h"   /* public streaming_* API (this file's impl) */

static const char *TAG = "stream_ctrl";

/* ---- Stream control EventGroup ----------------------------------- */

/* Defined here; declared extern in pipeline_internal.h. Created by
 * stream_control_init() at boot. Read/written from main.c (main loop),
 * pipeline.c (teardown/start) and here (the streaming_* accessors). */
EventGroupHandle_t s_stream_evt_grp = NULL;

/* stream_control_init: create the stream control EventGroup.
 * Called from app_main before any module that might touch the
 * streaming_* API. Returns ESP_OK on success, ESP_FAIL on allocation
 * failure (caller should reboot). */
esp_err_t stream_control_init(void)
{
    if (s_stream_evt_grp)
        return ESP_OK; /* idempotent */

    s_stream_evt_grp = xEventGroupCreate();
    if (!s_stream_evt_grp)
    {
        ESP_LOGE(TAG, "Failed to create stream event group");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ---- Stream control API (for AT commands, svc_port, ...) -------- */

bool streaming_is_active(void)
{
    if (!s_stream_evt_grp)
        return false;
    return (xEventGroupGetBits(s_stream_evt_grp) & STREAM_EVT_ACTIVE) != 0;
}

bool streaming_request_stop(void)
{
    if (!s_stream_evt_grp)
        return false;
    xEventGroupSetBits(s_stream_evt_grp, STREAM_EVT_STOP_REQ);
    return true;
}

bool streaming_request_restart(void)
{
    if (!s_stream_evt_grp)
        return false;
    /* Only restart if currently streaming. If the stream is already stopped
     * (e.g., after CMD_STOP from server), don't auto-start - the saved NVS
     * params will apply naturally on the next stream start. This prevents
     * HOTRESTART from overriding an intentional stop. */
    if (!streaming_is_active())
        return false;
    /* FIX (AUDIT-XPORT-AUTOAPPLY): see FIXES.md */
    s_pending_transport_apply = true;
    /* FIX (H5): see FIXES.md */
    xEventGroupSetBits(s_stream_evt_grp,
                       STREAM_EVT_STOP_REQ | STREAM_EVT_START_REQ);
    return true;
}

/* ---- Frame / channel accessors (read pipeline.c state) --------- */

uint32_t streaming_get_frame_ms(void)
{
    /* FIX (L25, GROK-21): see FIXES.md */
    return s_frame_ms;
}

bool streaming_frame_ms_known(void)
{
    return s_frame_ms_known;
}

/* FIX (B3/channels-desync): see FIXES.md */
uint8_t streaming_get_channels(void)
{
    if (streaming_is_active())
        return s_channels;
    /* IDLE: return the config (pending) channel count from NVS. */
    device_config_t cfg;
    config_get_copy(&cfg);
    return channel_format_to_count(cfg.channel_format);
}
