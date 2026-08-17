/*
 * pipeline.c
 * ==========
 *
 * Audio pipeline lifecycle + tasks + pools.
 *
 * Split out of main.c in the R3-A structural refactor. This file owns:
 *
 *   - All pipeline static state (queues, pools, encoders, runtime audio
 *     parameters, task handles, done semaphores, s_pending_transport_apply,
 *     s_wifi_initialized). Variables visible to other files (s_channels,
 *     s_frame_ms, s_frame_ms_known, s_task_handles, s_task_handles_mutex,
 *     s_pending_transport_apply, s_wifi_initialized) are declared extern
 *     in pipeline_internal.h; the rest stay file-local `static`.
 *   - start_streaming() / stop_streaming() / teardown_pipeline()
 *   - The four pipeline task functions: i2s_task_fn, adpcm_task_fn,
 *     pcm_task_fn, stream_task_fn (sender).
 *   - Pool-management helpers (pcm_frame_alloc, adpcm_frame_alloc,
 *     drain_and_delete_queue, wait_for_task_exit).
 *
 * Stream control (active flag, START_REQ / STOP_REQ bits) lives in
 * stream_control.c; this file reads/clears the ACTIVE bit via the
 * extern s_stream_evt_grp. The supervisor liveness counters live in
 * supervisor.c and are written from the I2S / TX tasks here.
 *
 * Architecture (preserved verbatim from the original main.c):
 *   I2S Task -> PCM Queue -> ADPCM/PCM Task -> ADPCM Queue -> TX Task
 *
 * Clean shutdown: pipeline tasks exit their loops when STREAM_EVT_ACTIVE
 * is cleared, give per-task done semaphores, and self-delete. The I2S
 * task uses a short i2s_read timeout (computed from DMA buffer capacity)
 * so it re-checks the active flag frequently - enables fast clean stop
 * without force-deletion (which would leave the I2S driver mutex locked
 * and deadlock i2s_driver_uninstall).
 */

/* ---- System / SDK includes ---- */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_log.h"

/* ---- Project includes ---- */
#include "board_config.h"
#include "config_mgr.h"
#include "wifi_sta.h"
#include "svc_port.h"
#include "svc_protocol.h"
#include "i2s_capture.h"
#include "adpcm_encoder.h"
#include "tpdf_dither.h"
#include "packet_format.h"
#include "stream_mode.h"
#include "stream_control.h"
#include "pipeline_internal.h"

static const char *TAG = "pipeline";

/* ---- Magic numbers (named for clarity) ---- */
#define UDP_MTU_BYTES 1400
#define POOL_BUDGET_FRACTION 2 /* 40% (2/5) of free heap for both pools */
#define POOL_BUDGET_DIVISOR 5
#define PCM_POOL_MIN 3
#define PCM_POOL_MAX 8
#define ADPCM_POOL_MIN 4
#define ADPCM_POOL_MAX 16
#define STOP_POLL_ITERATIONS 150
#define STOP_POLL_DELAY_MS 200

/* ---- Frame types with flexible array members ---- */

/* FIX (GROK-22): see FIXES.md */
typedef struct
{
    int num_samples;
    uint8_t samples_raw[];
} pcm_frame_t;

static inline int16_t *pcm_samples16(pcm_frame_t *f) { return (int16_t *)f->samples_raw; }
static inline int32_t *pcm_samples32(pcm_frame_t *f) { return (int32_t *)f->samples_raw; }
static inline const int16_t *pcm_samples16_const(const pcm_frame_t *f) { return (const int16_t *)f->samples_raw; }
static inline const int32_t *pcm_samples32_const(const pcm_frame_t *f) { return (const int32_t *)f->samples_raw; }

typedef struct
{
    uint16_t data_len;
    uint16_t seq_num;
    uint32_t timestamp_ms;
    uint8_t data[];
} adpcm_frame_t;

/* ---- Queues & pools ---- */

static QueueHandle_t pcm_free_queue = NULL;
static QueueHandle_t pcm_filled_queue = NULL;
static QueueHandle_t adpcm_free_queue = NULL;
static QueueHandle_t adpcm_filled_queue = NULL;

static adpcm_enc_state_t *adpcm_enc[2] = {NULL, NULL};

/* ---- Runtime audio parameters (set at stream start) ---- */

uint8_t s_channels = AUDIO_CHANNELS; /* extern in pipeline_internal.h */
static uint32_t s_sample_rate = 0;
static int s_samples_per_frame = 0;
static int s_adpcm_frame_bytes = 0;
static uint32_t s_audio_bitrate = 0;
static uint8_t s_sample_rate_enum = 0;
static uint8_t s_codec_mode = CODEC_MODE_ADPCM;
static uint8_t s_pkt_codec_id = CODEC_ID_ADPCM;
static int s_pkt_data_len = 0;     /* bytes of payload per packet */
static int s_bits_per_sample = 16; /* I2S bit depth (16 or 24) */

/* ---- Task handles & done semaphores ---- */

TaskHandle_t s_task_handles[TASK_IDX_COUNT] = {NULL}; /* extern */
static SemaphoreHandle_t s_task_done_sems[TASK_IDX_COUNT] = {NULL};
/* FIX (MEDIUM #37): see FIXES.md — mutex guarding s_task_handles[]. */
SemaphoreHandle_t s_task_handles_mutex = NULL; /* extern */


/* Boot-time WiFi state. */
bool s_wifi_initialized = false; /* extern */

/* FIX (F-C #8): see FIXES.md — tracks the WiFi channel RawTX was last
 * initialized with. AT+WCH + AT+HOTRESTART in RawTX mode needs to detect
 * a channel change and force wifi_sta_deinit()+re-init, because the channel
 * can only be applied inside wifi_init() (esp_wifi_set_channel at STA_START).
 * s_wifi_initialized alone is insufficient — it stays true across HOTRESTART. */
static uint8_t s_rawtx_wifi_channel = 0;

/* Pool sizes - computed at start_streaming from free heap & frame_ms. */
static int s_pcm_pool_size = 4;
static int s_adpcm_pool_size = 6;

/* Frame duration (ms) - computed in start_streaming from I2S params. */
uint32_t s_frame_ms = 20; /* extern */
/* FIX (GROK-21): see FIXES.md */
bool s_frame_ms_known = false; /* extern */

/* ---- Helpers ---- */

static pcm_frame_t *pcm_frame_alloc(int num_samples, int bits_per_sample, int codec_mode)
{
    /* Only PCM 24-bit mode needs int32_t (raw sign-extended 24-bit values
     * copied directly from i2s_capture_read). ADPCM 24-bit uses int16_t
     * because dither_buffer_24_to_16 reduces to 16-bit before encoding.
     * Allocating int32_t for ADPCM wastes 19 KB at 48kHz/stereo. */
    bool need_int32 = (bits_per_sample == 24) && (codec_mode == CODEC_MODE_PCM);
    size_t elem_size = need_int32 ? sizeof(int32_t) : sizeof(int16_t);
    pcm_frame_t *f = malloc(sizeof(pcm_frame_t) + (size_t)num_samples * elem_size);
    if (f)
        f->num_samples = num_samples;
    return f;
}

static adpcm_frame_t *adpcm_frame_alloc(int max_data_len)
{
    adpcm_frame_t *f = malloc(sizeof(adpcm_frame_t) + max_data_len);
    return f;
}

static void drain_and_delete_queue(QueueHandle_t *q)
{
    if (!*q)
        return;
    void *item = NULL;
    while (xQueueReceive(*q, &item, 0) == pdTRUE)
    {
        free(item);
    }
    vQueueDelete(*q);
    *q = NULL;
}

/* FIX (F-C #2): see FIXES.md — clear s_task_handles[idx] BEFORE vTaskDelete(NULL).
 *
 * Previously each task_fn gave its done semaphore then called vTaskDelete(NULL)
 * without NULLing its handle. The handle became a dangling pointer until the
 * stop path (wait_for_task_exit / teardown_pipeline) cleared it under the
 * mutex; meanwhile the supervisor's stack-HWM check or any future reader
 * could call uxTaskGetStackHighWaterMark() on a freed TCB. Calling this
 * helper right before vTaskDelete(NULL) closes the race: the handle is
 * observed NULL by any reader under the mutex, and best-effort NULL'd if
 * the mutex can't be taken. Idempotent — safe to call from any exit path. */
static void pipeline_task_mark_exiting(task_idx_t idx)
{
    if (s_task_handles_mutex &&
        xSemaphoreTake(s_task_handles_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        s_task_handles[idx] = NULL;
        xSemaphoreGive(s_task_handles_mutex);
    }
    else
    {
        s_task_handles[idx] = NULL; /* best-effort */
    }
}

/* ====================================================================
 * I2S Capture Task
 * ==================================================================== */

static void i2s_task_fn(void *arg)
{
    int idx = (int)(intptr_t)arg; /* FIX (L26): intptr_t cast */
    ESP_LOGI(TAG, "[I2S] Task started (idx=%d)", idx);

    int total = s_samples_per_frame * s_channels;
    int32_t *raw = malloc(total * sizeof(int32_t));
    if (!raw)
    {
        ESP_LOGE(TAG, "[I2S] alloc fail");
        svc_port_set_error(SVC_ERR_MEMORY);
        /* FR-PIPE / Roma #6: fatal exit — request stop so the rest of the
         * pipeline (adpcm/pcm + TX tasks) is torn down. Without this the
         * consumer tasks keep spinning on a dead producer. Mirrors the
         * F-C #1 fix already applied in stream_task_fn. */
        streaming_request_stop();
        goto task_exit;
    }

    bool is_24bit = (i2s_capture_get_bits() == 24);
    uint32_t pcm_wait_ms = (uint32_t)s_frame_ms * (s_pcm_pool_size + 2);

    /* Diagnostic counters - log every 50 frames or on timeout. */
    uint32_t ok_count = 0;
    uint32_t timeout_count = 0;
    /* FIX (GROK-2.3): see FIXES.md — partial-read underrun counter. */
    uint32_t partial_count = 0;

    while (streaming_is_active())
    {
        pcm_frame_t *pcm = NULL;
        if (xQueueReceive(pcm_free_queue, &pcm, pdMS_TO_TICKS(pcm_wait_ms)) != pdTRUE)
            continue;

        int n = 0;
        esp_err_t err = i2s_capture_read(raw, total, &n);
        if (err != ESP_OK)
        {
            timeout_count++;
            if (err != ESP_ERR_TIMEOUT)
            {
                ESP_LOGE(TAG, "[I2S] read error: %s", esp_err_to_name(err));
                svc_port_set_error(SVC_ERR_I2S);
            }
            /* Log first timeout and every 50th - shows if I2S is starved. */
            if (timeout_count == 1 || (timeout_count % 50) == 0)
            {
                ESP_LOGW(TAG, "[I2S] read timeout #%u (ok=%u) - no DMA data?",
                         (unsigned)timeout_count, (unsigned)ok_count);
            }
            xQueueSend(pcm_free_queue, &pcm, 0);
            continue;
        }

        ok_count++;
#ifdef CONFIG_STREAMER_SUPERVISOR_ENABLED
        s_supervisor_i2s_count++;
#endif
        /* Log first successful read + every 1000 - shows pipeline is alive. */
        if (ok_count == 1)
        {
            ESP_LOGI(TAG, "[I2S] first read OK: %d samples, raw[0]=%d, raw[1]=%d",
                     n, (int)raw[0], (int)raw[1]);
        }
        else if ((ok_count % 1000) == 0)
        {
            ESP_LOGI(TAG, "[I2S] %u frames read (timeouts=%u)",
                     (unsigned)ok_count, (unsigned)timeout_count);
        }

        /* FIX (GROK-2.3, B4): see FIXES.md */
        if (n < total)
        {
            partial_count++;
            if (partial_count == 1 || (partial_count % 50) == 0)
            {
                ESP_LOGW(TAG, "[I2S] partial read #%u: got %d/%d samples - "
                              "zero-padding (underrun masked as silence)",
                         (unsigned)partial_count, n, total);
            }
            if (partial_count == 5)
            {
                ESP_LOGE(TAG, "[I2S] %u consecutive partial reads - signaling "
                              "SVC_ERR_I2S",
                         (unsigned)partial_count);
                svc_port_set_error(SVC_ERR_I2S);
            }
        }
        else
        {
            if (partial_count >= 5)
            {
                ESP_LOGI(TAG, "[I2S] recovered from partial reads - clearing SVC_ERR_I2S");
                /* FIX (FR-SVC #7): per-code clear — don't clobber an unrelated
                 * upstream error (e.g. SVC_ERR_NETWORK) that may have been
                 * set in the meantime. */
                svc_port_clear_error_code(SVC_ERR_I2S);
            }
            partial_count = 0;
        }

        for (int i = n; i < total; i++)
            raw[i] = 0;

        if (s_codec_mode == CODEC_MODE_PCM && is_24bit)
        {
            /* PCM 24-bit: copy raw int32 samples directly (sign-extended
             * 24-bit values). pcm_task_fn will strip the high byte and
             * emit 3 bytes per sample. No dither - we want full 24-bit
             * precision in the stream. */
            memcpy(pcm_samples32(pcm), raw, (size_t)total * sizeof(int32_t));
        }
        else if (is_24bit)
            dither_buffer_24_to_16(raw, pcm_samples16(pcm), total);
        else
            dither_buffer_passthrough(raw, pcm_samples16(pcm), total);

        if (xQueueSend(pcm_filled_queue, &pcm, 0) != pdTRUE)
            xQueueSend(pcm_free_queue, &pcm, 0);
    }

    free(raw);

task_exit:
    ESP_LOGI(TAG, "[I2S] Task exiting");
    if (s_task_done_sems[idx])
        xSemaphoreGive(s_task_done_sems[idx]);
    /* FIX (F-C #2): NULL our handle BEFORE vTaskDelete(NULL) so the supervisor
     * and stop path never see a dangling handle. */
    pipeline_task_mark_exiting((task_idx_t)idx);
    vTaskDelete(NULL);
}

/* ====================================================================
 * ADPCM Encoding Task
 * ==================================================================== */

static void adpcm_task_fn(void *arg)
{
    int idx = (int)(intptr_t)arg; /* FIX (L26): intptr_t cast */
    ESP_LOGI(TAG, "[ADPCM] Task started (idx=%d, %d ch)", idx, s_channels);

    uint32_t frame_count = 0;
    /* FIX (FW#1): see FIXES.md — seq assigned in TX task after successful send. */
    int cap = DVI4_HEADER_SIZE + s_adpcm_frame_bytes;

    int16_t *ch_left = NULL, *ch_right = NULL;
    if (s_channels == 2)
    {
        ch_left = malloc(s_samples_per_frame * sizeof(int16_t));
        ch_right = malloc(s_samples_per_frame * sizeof(int16_t));
        if (!ch_left || !ch_right)
        {
            ESP_LOGE(TAG, "[ADPCM] deinterleave alloc fail");
            free(ch_left);
            free(ch_right);
            /* FR-PIPE / Roma #6: fatal exit — set error + request stop so the
             * rest of the pipeline is torn down. Previously this path only
             * freed + goto task_exit, leaving STREAM_EVT_ACTIVE set and the
             * I2S + TX tasks running on a dead ADPCM producer. Mirrors F-C #1
             * in stream_task_fn. */
            svc_port_set_error(SVC_ERR_MEMORY);
            streaming_request_stop();
            goto task_exit;
        }
    }

    while (streaming_is_active())
    {
        pcm_frame_t *pcm = NULL;
        adpcm_frame_t *adpcm = NULL;

        if (xQueueReceive(pcm_filled_queue, &pcm, pdMS_TO_TICKS(100)) != pdTRUE)
            continue;
        if (xQueueReceive(adpcm_free_queue, &adpcm, pdMS_TO_TICKS(100)) != pdTRUE)
        {
            xQueueSend(pcm_free_queue, &pcm, 0);
            continue;
        }

        size_t written = 0;
        esp_err_t err;

        if (s_channels == 1)
        {
            err = adpcm_enc_process(adpcm_enc[0], pcm_samples16(pcm), pcm->num_samples,
                                    adpcm->data, cap, &written);
        }
        else
        {
            /* Stereo: deinterleave L,R,L,R -> ch_left[], ch_right[] */
            int16_t *samples = pcm_samples16(pcm);
            for (int i = 0; i < s_samples_per_frame; i++)
            {
                ch_left[i] = samples[i * 2];
                ch_right[i] = samples[i * 2 + 1];
            }
            /* FIX (GROK-14): see FIXES.md
             * FIX (STEREO-BOUNDS): защитная проверка wl <= rem перед вычитанием.
             * adpcm_enc_process гарантирует wl <= out_size при ESP_OK, но явная
             * проверка исключает зависимость от внутренней реализации энкодера
             * и предотвращает underflow size_t при любом будущем рефакторинге. */
            size_t rem = (size_t)s_pkt_data_len;
            size_t wl = 0, wr = 0;
            err = adpcm_enc_process(adpcm_enc[0], ch_left, s_samples_per_frame,
                                    adpcm->data, rem, &wl);
            if (err != ESP_OK || wl > rem)
            {
                if (err == ESP_OK)
                {
                    ESP_LOGE(TAG, "[ADPCM] CRITICAL: encoder L wrote %u bytes, exceeding rem=%u", (unsigned)wl, (unsigned)rem);
                    err = ESP_ERR_INVALID_SIZE; /* wl > rem — не должно случаться */
                }
                else
                {
                    ESP_LOGW(TAG, "[ADPCM] encoder L failed: %s", esp_err_to_name(err));
                }

                svc_port_set_error(SVC_ERR_CODEC);
                xQueueSend(adpcm_free_queue, &adpcm, 0);
                xQueueSend(pcm_free_queue, &pcm, 0);
                continue;
            }
            rem -= wl;
            if (err == ESP_OK)
                err = adpcm_enc_process(adpcm_enc[1], ch_right, s_samples_per_frame,
                                        adpcm->data + wl, rem, &wr);
            written = wl + wr;
        }

        if (err != ESP_OK || written > UINT16_MAX)
        {
            if (err != ESP_OK)
                svc_port_set_error(SVC_ERR_CODEC);
            xQueueSend(adpcm_free_queue, &adpcm, 0);
            xQueueSend(pcm_free_queue, &pcm, 0);
            continue;
        }

        adpcm->data_len = (uint16_t)written;
        /* FIX (FW#1): seq_num assigned in TX task after successful send. */
        adpcm->seq_num = 0;
        /* NOTE: timestamp_ms wraps at ~49.7 days (uint32_t). This is expected —
         * the receiver should handle 32-bit wraparound. See RFC 3550 §4.1.
         * FIX (F-C #6): documented wraparound semantics. */
        adpcm->timestamp_ms = frame_count * s_frame_ms;

        if (xQueueSend(adpcm_filled_queue, &adpcm, 0) != pdTRUE)
            xQueueSend(adpcm_free_queue, &adpcm, 0);
        xQueueSend(pcm_free_queue, &pcm, 0);

        if ((++frame_count % 1000) == 0)
            ESP_LOGI(TAG, "[ADPCM] %" PRIu32 " frames encoded", frame_count);
    }

    free(ch_left);
    free(ch_right);

task_exit:
    ESP_LOGI(TAG, "[ADPCM] Task exiting");
    if (s_task_done_sems[idx])
        xSemaphoreGive(s_task_done_sems[idx]);
    /* FIX (F-C #2): NULL our handle BEFORE vTaskDelete(NULL). */
    pipeline_task_mark_exiting((task_idx_t)idx);
    vTaskDelete(NULL);
}

/* ====================================================================
 * PCM Packing Task (alternative to ADPCM when codec_mode == PCM)
 *
 * Packs raw PCM samples (already 16-bit after dither/passthrough) into
 * the packet payload WITHOUT any compression. No DVI4 header per channel.
 *
 * Layout (16-bit):
 *   mono:   [S0][S1][S2]...               (2 bytes/sample)
 *   stereo: [L0][R0][L1][R1]...           (interleaved, 4 bytes/frame)
 *
 * Layout (24-bit - sample occupies 32 bits in int32_t, top byte unused):
 *   mono:   [S0_lo][S0_mid][S0_hi]...                 (3 bytes/sample)
 *   stereo: [L0_lo][L0_mid][L0_hi][R0_lo][R0_mid][R0_hi]... (6 bytes/frame)
 *
 * 24-bit samples are stored in int32_t as sign-extended values in
 * range [-8388608, +8388607]. We emit only the low 3 bytes (little-endian),
 * stripping the redundant high byte.
 * ==================================================================== */
static void pcm_task_fn(void *arg)
{
    int idx = (int)(intptr_t)arg; /* FIX (L26): intptr_t cast */
    ESP_LOGI(TAG, "[PCM] Task started (idx=%d, %d ch, %d-bit)",
             idx, s_channels, s_bits_per_sample);

    uint32_t frame_count = 0;
    /* FIX (FW#1): removed seq_counter - assigned in TX task. */

    while (streaming_is_active())
    {
        pcm_frame_t *pcm = NULL;
        adpcm_frame_t *out = NULL;

        if (xQueueReceive(pcm_filled_queue, &pcm, pdMS_TO_TICKS(100)) != pdTRUE)
            continue;
        if (!pcm)
            continue;
        if (xQueueReceive(adpcm_free_queue, &out, pdMS_TO_TICKS(100)) != pdTRUE)
        {
            xQueueSend(pcm_free_queue, &pcm, 0);
            continue;
        }
        if (!out)
        {
            xQueueSend(pcm_free_queue, &pcm, 0);
            continue;
        }

        int n = pcm->num_samples; /* = s_samples_per_frame * s_channels */
        uint8_t *dst = out->data;
        size_t written = 0;

        if (s_bits_per_sample == 16)
        {
            /* 16-bit: samples already int16 in pcm->samples_raw[]. Just copy. */
            size_t bytes = (size_t)n * sizeof(int16_t);
            if (bytes > (size_t)s_pkt_data_len)
                bytes = s_pkt_data_len;
            memcpy(dst, pcm_samples16(pcm), bytes);
            written = bytes;
        }
        else
        {
            /* 24-bit: pcm->samples_raw[] actually holds int32_t (sign-extended
             * 24-bit values, copied raw from i2s_capture_read). Emit only
             * the low 3 bytes (LE) per sample, stripping the redundant
             * high byte. */
            int32_t *s32 = pcm_samples32(pcm);
            for (int i = 0; i < n; i++)
            {
                /* FIX (H4): see FIXES.md — bounds check BEFORE the 3-byte write. */
                if (written + 3 > (size_t)s_pkt_data_len)
                    break;
                int32_t s = s32[i];
                dst[written++] = (uint8_t)(s & 0xFF);
                dst[written++] = (uint8_t)((s >> 8) & 0xFF);
                dst[written++] = (uint8_t)((s >> 16) & 0xFF);
            }
        }

        out->data_len = (uint16_t)written;
        /* FIX (FW#1): seq_num assigned in TX task after successful send. */
        out->seq_num = 0;
        /* NOTE: timestamp_ms wraps at ~49.7 days (uint32_t). This is expected —
         * the receiver should handle 32-bit wraparound. See RFC 3550 §4.1.
         * FIX (F-C #6): documented wraparound semantics. */
        out->timestamp_ms = frame_count * s_frame_ms;

        if (xQueueSend(adpcm_filled_queue, &out, 0) != pdTRUE)
            xQueueSend(adpcm_free_queue, &out, 0);
        xQueueSend(pcm_free_queue, &pcm, 0);

        if ((++frame_count % 1000) == 0)
            ESP_LOGI(TAG, "[PCM] %" PRIu32 " frames packed", frame_count);
    }

    ESP_LOGI(TAG, "[PCM] Task exiting");
    if (s_task_done_sems[idx])
        xSemaphoreGive(s_task_done_sems[idx]);
    /* FIX (F-C #2): NULL our handle BEFORE vTaskDelete(NULL). */
    pipeline_task_mark_exiting((task_idx_t)idx);
    vTaskDelete(NULL);
}

/* ====================================================================
 * Stream TX Task - sends encoded audio packets via the active transport
 * (UDP socket or Raw 802.11 TX). Mode-agnostic: all mode differences
 * are handled by the stream_mode_ops table.
 * ==================================================================== */

static void stream_task_fn(void *arg)
{
    int idx = (int)(intptr_t)arg; /* FIX (L26): intptr_t cast */
    const stream_mode_ops_t *ops = stream_mode_ops();
    ESP_LOGI(TAG, "[%s] Task started (idx=%d)", ops->name, idx);

    /* Wait for transport ready (+ WiFi association in UDP mode).
     * In Raw TX mode, wifi_sta_is_connected() is not meaningful (no AP),
     * so we only check transport readiness.
     *
     * In UDP mode, without WiFi association, esp_wifi_tx returns ENOMEM
     * (errno=12), causing startup drops. Waiting here eliminates them. */
    int wait_count = 0;
    while (streaming_is_active())
    {
        bool transport_ready = transport_is_ready();
        bool wifi_ready = ops->needs_wifi_association
                              ? wifi_sta_is_connected()
                              : true;
        if (transport_ready && wifi_ready)
            break;

        vTaskDelay(pdMS_TO_TICKS(STOP_POLL_DELAY_MS));
        if (++wait_count > STOP_POLL_ITERATIONS)
        {
            ESP_LOGE(TAG, "[%s] Stream/WiFi not ready after 30s - giving up",
                     ops->name);
            svc_port_set_error(SVC_ERR_NETWORK);
            /* FIX (F-C #1): fatal exit — request stop so the main loop tears
             * down the rest of the pipeline. Without this, STREAM_EVT_ACTIVE
             * stays set and i2s/adpcm/pcm tasks keep running on a dead TX. */
            streaming_request_stop();
            goto task_exit;
        }
    }

    if (!streaming_is_active())
        goto task_exit; /* normal exit — stop was requested, do NOT call streaming_request_stop() */

    ESP_LOGI(TAG, "[%s] Streaming started (WiFi %s)",
             ops->name,
             wifi_sta_is_connected() ? "connected" : "WARN: not connected");

    uint8_t *pkt = malloc(PKT_HDR_SIZE + s_pkt_data_len);
    if (!pkt)
    {
        ESP_LOGE(TAG, "[%s] alloc fail", ops->name);
        svc_port_set_error(SVC_ERR_MEMORY);
        /* FIX (F-C #1): fatal exit — request stop so the rest of the pipeline
         * is torn down. */
        streaming_request_stop();
        goto task_exit;
    }

    uint32_t sent = 0, dropped = 0;
    /* FIX (supervisor-drops): see FIXES.md */
    uint32_t consecutive_drops = 0;

    /* FIX (FW#1): see FIXES.md */
    uint16_t seq_counter = 0;

    while (streaming_is_active())
    {
        adpcm_frame_t *adpcm = NULL;
        if (xQueueReceive(adpcm_filled_queue, &adpcm, pdMS_TO_TICKS(100)) != pdTRUE)
            continue;

        if (!transport_is_ready() || !adpcm->data_len)
        {
            dropped++;
            /* FIX (F-C #3): previously this drop path only incremented the
             * local `dropped` counter; the supervisor's drop-based heuristics
             * (s_supervisor_tx_consecutive_drops) missed these drops entirely,
             * so a persistently broken transport could spin here forever
             * without triggering a reboot. Count it as a consecutive drop. */
            consecutive_drops++;
#ifdef CONFIG_STREAMER_SUPERVISOR_ENABLED
            s_supervisor_tx_consecutive_drops = consecutive_drops;
#endif
            if (dropped == 1 || (dropped % 100) == 0)
            {
                ESP_LOGW(TAG, "[%s] transport not ready, drops=%u",
                         ops->name, (unsigned)dropped);
            }
            xQueueSend(adpcm_free_queue, &adpcm, 0);
            continue;
        }

        pkt_header_t hdr;
        /* FIX (FW#1): see FIXES.md */
        adpcm->seq_num = seq_counter;
        /* FIX (HIGH #3): see FIXES.md */
        uint16_t bits_field = (uint16_t)s_bits_per_sample;
        /* FIX (F-C #5): explicit (uint8_t) cast — pkt_header_init takes
         * frame_ms as uint8_t (packet_format.h:51) but s_frame_ms is uint32_t.
         * The narrowing is intentional (frame_ms is clamped to [4,255] by
         * i2s_capture_compute_frame_ms) but the implicit cast was fragile. */
        pkt_header_init(&hdr, adpcm->seq_num, adpcm->timestamp_ms,
                        s_pkt_codec_id, s_sample_rate_enum, s_channels,
                        (uint8_t)s_frame_ms, s_audio_bitrate,
                        bits_field);

        memcpy(pkt, &hdr, PKT_HDR_SIZE);
        memcpy(pkt + PKT_HDR_SIZE, adpcm->data, adpcm->data_len);

        if (transport_send(pkt, PKT_HDR_SIZE + adpcm->data_len) == ESP_OK)
        {
            sent++;
            consecutive_drops = 0; /* reset on success */
#ifdef CONFIG_STREAMER_SUPERVISOR_ENABLED
            s_supervisor_tx_count++;
            s_supervisor_tx_consecutive_drops = 0;
#endif
            /* FIX (FW#1): see FIXES.md — commit seq only on successful send. */
            seq_counter++;
            /* Clear stale NETWORK error after successful send - the initial
             * send may fail (errno=12 ENOMEM while WiFi still associating),
             * but once streaming works the error flag should be cleared so
             * the server status shows "OK" instead of stale "Error".
             * FIX (FR-SVC #7): per-code clear — only clears SVC_ERR_NETWORK,
             * leaving any concurrent SVC_ERR_I2S / SVC_ERR_CODEC intact. */
            if (dropped > 0 && (sent % 100) == 0)
            {
                svc_port_clear_error_code(SVC_ERR_NETWORK);
            }
        }
        else
        {
            ++dropped;
            consecutive_drops++;
#ifdef CONFIG_STREAMER_SUPERVISOR_ENABLED
            s_supervisor_tx_consecutive_drops = consecutive_drops;
#endif
            if (dropped == 1 || (dropped % 100) == 0)
            {
                ESP_LOGW(TAG, "[%s] send fail (drops: %" PRIu32 ")",
                         ops->name, dropped);
            }
            /* Only report network error if stream is still active.
             * During stop, transport_close_client() closes the socket →
             * send() fails with errno=128 (ENOTCONN) — this is expected,
             * not a real error. Reporting it causes STATUS_ERROR in INFO
             * packets, showing "Error" in the receiver UI after stop.
             * FIX (F2-SVC #8): use svc_port_set_error_if_none() so a
             * transient NETWORK error doesn't clobber a higher-priority
             * error (I2S, CODEC, MEMORY) already set by an upstream
             * task. The previous unconditional svc_port_set_error() would
             * overwrite e.g. SVC_ERR_I2S with SVC_ERR_NETWORK, masking
             * the real upstream failure in the next INFO packet. */
            if (streaming_is_active())
                svc_port_set_error_if_none(SVC_ERR_NETWORK);
        }

        svc_port_update_stats(sent);
        xQueueSend(adpcm_free_queue, &adpcm, 0);

        if (sent && (sent % 1000) == 0)
            ESP_LOGI(TAG, "[%s] %" PRIu32 " sent, %" PRIu32 " dropped",
                     ops->name, sent, dropped);
    }

    free(pkt);

task_exit:
    ESP_LOGI(TAG, "[%s] Task exiting", ops->name);
    if (s_task_done_sems[idx])
    {
        xSemaphoreGive(s_task_done_sems[idx]);
    }
    /* FIX (F-C #2): NULL our handle BEFORE vTaskDelete(NULL) so the supervisor
     * and stop path never see a dangling handle. */
    pipeline_task_mark_exiting((task_idx_t)idx);
    vTaskDelete(NULL);
}

/* ====================================================================
 * Task exit helper
 * ==================================================================== */

/* Pass4 M3: distinguish "no semaphore" from "force-deleted" so teardown
 * doesn't spuriously reboot when sems weren't created (early fail path). */
typedef enum
{
    TASK_EXIT_CLEAN,
    TASK_EXIT_FORCED,
    TASK_EXIT_NO_SEM
} task_exit_t;

static task_exit_t wait_for_task_exit(int idx, uint32_t timeout_ms)
{
    if (!s_task_done_sems[idx])
    {
        return TASK_EXIT_NO_SEM;
    }

    if (xSemaphoreTake(s_task_done_sems[idx], pdMS_TO_TICKS(timeout_ms)) == pdTRUE)
    {
        /* FIX (MEDIUM #37): see FIXES.md */
        if (s_task_handles_mutex &&
            xSemaphoreTake(s_task_handles_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            s_task_handles[idx] = NULL;
            xSemaphoreGive(s_task_handles_mutex);
        }
        else
        {
            s_task_handles[idx] = NULL; /* best-effort */
        }
        return TASK_EXIT_CLEAN;
    }

    /* FIX (MEDIUM #37): see FIXES.md */
    TaskHandle_t h_to_delete = NULL;
    bool need_force_delete = false;
    if (s_task_handles_mutex &&
        xSemaphoreTake(s_task_handles_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        h_to_delete = s_task_handles[idx];
        s_task_handles[idx] = NULL;
        need_force_delete = (h_to_delete != NULL);
        xSemaphoreGive(s_task_handles_mutex);
    }
    else
    {
        h_to_delete = s_task_handles[idx];
        s_task_handles[idx] = NULL;
        need_force_delete = (h_to_delete != NULL);
    }

    if (need_force_delete)
    {
        /* FIX (GROK-18): see FIXES.md */
        ESP_LOGE(TAG, "Task %d did not exit in %u ms - force deleting. "
                      "WARNING: this may orphan lwIP/svc_port mutexes and "
                      "deadlock the next stream start. REBOOT RECOMMENDED (AT+RST).",
                 idx, (unsigned)timeout_ms);
        /* FIX (LOW #14): see FIXES.md — NOTE: force-deleting a task that
         * holds a frame buffer (taken from a queue but not yet returned)
         * leaks that buffer. This is an acceptable tradeoff: force-delete
         * only happens after STREAM_STOP_TIMEOUT (3s), the buffer is small
         * (~1-4KB), and the device reboots on next stream start if heap is
         * exhausted. A full fix would require task-local cleanup callbacks,
         * which FreeRTOS vTaskDelete doesn't support. */
        vTaskDelete(h_to_delete);
    }
    return TASK_EXIT_FORCED;
}

/* ====================================================================
 * Stream start / stop
 * ==================================================================== */

/* Common teardown for stop_streaming (partial: transport_close_client) and
 * start_streaming failure cleanup (full: transport_deinit). Shares: clear
 * ACTIVE bit, wait for tasks, close transport, NULL+delete sems, drain
 * queues, destroy encoders, i2s_capture_deinit + 50ms, on_stream_stopped. */
static void teardown_pipeline(bool full_transport_teardown)
{
    /* Signal pipeline tasks to exit (idempotent if already cleared). */
    xEventGroupClearBits(s_stream_evt_grp, STREAM_EVT_ACTIVE);

    /* FR-PIPE / Roma #8: clear s_frame_ms_known as early as possible so that
     * any INFO packet issued after stop (via streaming_frame_ms_known()
     * returning false -> streaming_get_frame_ms() returns 0) reports
     * frame_ms=0 instead of the stale value from the just-stopped stream.
     * Set false here, re-published true only after the next successful
     * start_streaming (line ~1345, F-C #4). */
    s_frame_ms_known = false;

    /* F2-TCP (#1.2): abort any in-flight blocking send() BEFORE waiting for
     * the TX task to exit. TCP send() can block for up to TCP_SEND_TIMEOUT_MS
     * (default 2s) holding s_client_mutex; without abort, stop_streaming()
     * would wait the full STREAM_STOP_TIMEOUT_TCP_MS (3s) and then force-
     * delete the TX task while it still holds the mutex — orphaning the
     * mutex and corrupting lwIP state. shutdown(SHUT_RDWR) makes the
     * blocking send() return immediately (ENOTCONN/EPIPE) so the TX task
     * breaks out of its loop and self-exits cleanly within the stop
     * timeout. No-op for UDP/RAWTX (their send paths don't block). */
    transport_abort_send();

    /* FIX (F-C #7): wait for tasks to notice ACTIVE cleared and exit BEFORE
     * closing the transport. Previously the transport (socket / rawtx state)
     * was closed FIRST, then we waited for tasks; the TX task could still be
     * inside sendto()/send() on a now-closed fd, producing spurious ENOTCONN
     * errors and (worse) racing the fd-recycle path. Waiting first ensures
     * the TX task has either exited cleanly or been force-deleted before the
     * socket goes away. */

    /* Wait for each task to exit cleanly. Use a SHARED deadline across all
     * tasks (not stop_to per task) so total wait is bounded by stop_to. */
    uint32_t stop_to = (stream_mode_current_transport() == TRANSPORT_MODE_TCP)
                           ? STREAM_STOP_TIMEOUT_TCP_MS
                           : STREAM_STOP_TIMEOUT_UDP_MS;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(stop_to);
    bool forced_delete = false; /* F2-TCP (#1.3): track if any task was force-deleted */
    for (int i = 0; i < TASK_IDX_COUNT; i++)
    {
        /* FIX (MEDIUM #37, B10): see FIXES.md */
        TaskHandle_t h = NULL;
        if (s_task_handles_mutex &&
            xSemaphoreTake(s_task_handles_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            h = s_task_handles[i];
            xSemaphoreGive(s_task_handles_mutex);
        }
        else
        {
            h = s_task_handles[i]; /* best-effort */
        }
        if (h)
        {
            /* Calculate remaining time from the shared deadline. */
            TickType_t now_tick = xTaskGetTickCount();
            uint32_t remaining_ms;
            /* Signed comparison handles wrap correctly:
             * if deadline is in the past, (deadline - now) is a huge unsigned,
             * but casting to signed gives a negative -> clamp to 0. */
            int32_t diff = (int32_t)(deadline - now_tick);

            if (diff <= 0)
                remaining_ms = 0;
            else
                remaining_ms = (uint32_t)diff * portTICK_PERIOD_MS;

            if (remaining_ms == 0)
            {
                /* No time left — force-delete immediately. */
                vTaskDelete(h);
                /* Clear handle under mutex (supervisor reads it under mutex). */
                if (s_task_handles_mutex &&
                    xSemaphoreTake(s_task_handles_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
                {
                    s_task_handles[i] = NULL;
                    xSemaphoreGive(s_task_handles_mutex);
                }
                else
                {
                    s_task_handles[i] = NULL; /* best-effort (reboot follows) */
                }
                forced_delete = true;
                continue;
            }
            task_exit_t r = wait_for_task_exit(i, remaining_ms);
            if (r == TASK_EXIT_FORCED)
            {
                /* F2-TCP (#1.3): task did not exit in time and was force-
                 * deleted. It may still hold s_client_mutex (TCP) or other
                 * lwIP/svc_port mutexes — closing the transport under those
                 * conditions would either block forever (mutex held) or
                 * corrupt state. Reboot instead. */
                forced_delete = true;
            }
            /* TASK_EXIT_NO_SEM: semaphore not created (early fail path) —
             * don't set forced_delete, no task was actually force-deleted. */
        }
        /* wait_for_task_exit already cleared s_task_handles[i] under mutex. */
    }

    /* F2-TCP (#1.3): if any task was force-deleted, it may hold a mutex
     * (s_client_mutex for TCP, or lwIP-internal mutexes). Calling
     * transport_deinit()/transport_close_client() in that state can:
     *   - block forever on xSemaphoreTake(s_client_mutex, portMAX_DELAY) in
     *     tcp_stream_deinit/close_client, OR
     *   - corrupt lwIP state by closing a socket whose send() is still
     *     in flight on the deleted task.
     * Reboot is the only safe recovery: it releases all mutexes via the
     * kernel and re-initializes lwIP cleanly. The 500ms delay gives the
     * log + ESP_LOGE time to flush before the reset. */
    if (forced_delete)
    {
        ESP_LOGE(TAG, "Pipeline task force-deleted (mutex may be held) - REBOOT");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return; /* unreachable */
    }

    /* Now safe to close transport — tasks are done. */
    /* FIX (C1): see FIXES.md */
    if (full_transport_teardown)
        transport_deinit();
    else
        transport_close_client();

    /* FIX (C4): see FIXES.md */
    for (int i = 0; i < TASK_IDX_COUNT; i++)
    {
        SemaphoreHandle_t tmp = s_task_done_sems[i];
        s_task_done_sems[i] = NULL;
        if (tmp)
            vSemaphoreDelete(tmp);
    }

    drain_and_delete_queue(&pcm_free_queue);
    drain_and_delete_queue(&pcm_filled_queue);
    drain_and_delete_queue(&adpcm_free_queue);
    drain_and_delete_queue(&adpcm_filled_queue);

    for (int i = 0; i < 2; i++)
    {
        if (adpcm_enc[i])
        {
            adpcm_enc_destroy(adpcm_enc[i]);
            adpcm_enc[i] = NULL;
        }
    }

    {
        esp_err_t derr = i2s_capture_deinit();
        if (derr != ESP_OK)
            ESP_LOGW(TAG, "i2s_capture_deinit: %s", esp_err_to_name(derr));
    }
    /* I2S hardware needs time to power down before a potential restart
     * (rapid CONFIGURE / AT+HOTRESTART) — without this, the next
     * i2s_driver_install() can crash with LoadStoreAlignment. */
    vTaskDelay(pdMS_TO_TICKS(50));

    stream_mode_ops()->on_stream_stopped();
}

esp_err_t start_streaming(void)
{
    if (streaming_is_active())
    {
        /* Duplicate CONFIGURE from server (it sends 3 with 200ms gaps).
         * Not an error - just ignore. */
        return ESP_ERR_INVALID_STATE;
    }

    /* FIX (A1, MEDIUM #37): see FIXES.md */
    for (int i = 0; i < TASK_IDX_COUNT; i++)
    {
        TaskHandle_t h = NULL;
        if (s_task_handles_mutex &&
            xSemaphoreTake(s_task_handles_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            h = s_task_handles[i];
            xSemaphoreGive(s_task_handles_mutex);
        }
        else
        {
            h = s_task_handles[i]; /* best-effort */
        }
        if (h != NULL)
        {
            ESP_LOGE(TAG, "start_streaming: task %d handle non-NULL - stop not complete", i);
            return ESP_ERR_INVALID_STATE;
        }
    }

    device_config_t cfg;
    config_get_copy(&cfg);

    uint8_t old_transport = stream_mode_current_transport();
    const stream_mode_ops_t *old_ops = stream_mode_ops();
    const stream_mode_ops_t *ops = old_ops; /* default: no change */
    bool transport_changed_in_nvs = (cfg.transport_mode != old_transport);

    if (transport_changed_in_nvs)
    {
        /* Transport change ALWAYS requires AT+RST (full reboot).
          * Neither HOTRESTART nor server-initiated stop+start applies it.
          * NVS already holds the new value; it takes effect after reboot. */
        ESP_LOGW(TAG, "Transport changed in NVS (%s -> %s) - "
                       "AT+RST required to apply. Keeping old transport (%s) "
                       "for this stream.",
                  old_ops->name,
                  (cfg.transport_mode == TRANSPORT_MODE_UDP)    ? "UDP" :
                  (cfg.transport_mode == TRANSPORT_MODE_TCP)    ? "TCP"
                                                                : "Raw 802.11 TX",
                  old_ops->name);
        cfg.transport_mode = old_transport;
        /* ops stays == old_ops — stream continues on the OLD transport. */
    }

    /* Resolve stream destination (UDP: from server CONFIGURE; RAWTX: none). */
    uint32_t stream_host = 0;
    uint16_t stream_port = 0;
    esp_err_t dest_err = ops->get_stream_dest(&stream_host, &stream_port);
    if (dest_err != ESP_OK)
    {
        ESP_LOGE(TAG, "Cannot resolve stream destination: %s",
                 esp_err_to_name(dest_err));
        return dest_err;
    }

    /* Resolve channel count and notify mode-specific subsystems. */
    s_channels = channel_format_to_count(cfg.channel_format);
    ops->set_channels(s_channels);

    /* Compute runtime audio parameters. frame_ms is computed adaptively
     * from sample_rate and channels (even samples, DMA min 32, UDP MTU).
     * FIX (4-C): see FIXES.md — RawTX uses the air-time constrained variant. */
    s_sample_rate = cfg.sample_rate;
    if (cfg.transport_mode == TRANSPORT_MODE_RAWTX)
    {
        s_frame_ms = i2s_capture_compute_frame_ms_rawtx(s_sample_rate, s_channels,
                                                        cfg.codec_mode, cfg.bits_per_sample);
    }
    else
    {
        s_frame_ms = i2s_capture_compute_frame_ms(s_sample_rate, s_channels,
                                                  cfg.codec_mode, cfg.bits_per_sample);
    }
    s_samples_per_frame = (int)(s_sample_rate * s_frame_ms / 1000);

    /* Align samples_per_frame: 16-bit needs multiple of 8 (SLC word-align
     * + rw_pos drift); 24-bit needs multiple of 4. */
    if (cfg.bits_per_sample == 16)
        s_samples_per_frame &= ~7; /* кратно 8: 661->656, 882->880, 220->216 */
    else
        s_samples_per_frame &= ~3; /* кратно 4 (24-bit) */

    /* FIX (FW#2, GROK-21): see FIXES.md */
    s_frame_ms = (uint32_t)((uint64_t)s_samples_per_frame * 1000 / s_sample_rate);
    /* FIX (F-C #4): s_frame_ms_known is intentionally NOT set here. It must
     * only be set true after ALL subsequent validations succeed (samples<8
     * check, MTU guard, WiFi/I2S init, pool alloc, task creation). Previously
     * it was set on this line, so every early-return failure path (lines
     * 1016, 1016, 1094, 1110, 1119, 1128, 1142, 1171, 1190, ...) left
     * s_frame_ms_known=true; callers like streaming_get_frame_ms() /
     * build_info_payload() then read a stale or placeholder frame_ms. The
     * true assignment is now at the very end of start_streaming, right
     * before `return ESP_OK;`. */

    /* FIX (M14): see FIXES.md */
    if (s_samples_per_frame < 8)
    {
        ESP_LOGE(TAG, "samples_per_frame=%d too small (frame_ms=%u, rate=%u) - aborting",
                 s_samples_per_frame, (unsigned)s_frame_ms, (unsigned)s_sample_rate);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Computed frame_ms=%u (aligned) -> samples_per_frame=%d (rate=%u, ch=%u)",
             (unsigned)s_frame_ms, s_samples_per_frame,
             (unsigned)s_sample_rate, (unsigned)s_channels);

    s_adpcm_frame_bytes = s_samples_per_frame / 2;
    /* Update codec mode + bit depth from config BEFORE computing bitrate.
     * Previously these were assigned AFTER the bitrate calc, so on the first
     * stream after boot (or after AT+CODEC / AT+BITS change + HOTRESTART) the
     * bitrate used stale s_codec_mode/s_bits_per_sample values — e.g. an ADPCM
     * bitrate (4 bits/sample) was reported in the packet header for a PCM
     * stream. The receiver then displays the wrong kbps and may mis-size
     * buffers. */
    s_codec_mode = cfg.codec_mode;
    s_bits_per_sample = cfg.bits_per_sample;
    s_sample_rate_enum = sample_rate_to_enum(s_sample_rate);
    /* Bitrate depends on codec: ADPCM = 4 bits/sample, PCM = bits_per_sample
     * bits/sample. Now uses the just-updated s_codec_mode/s_bits_per_sample. */
    {
        int bits_per_codec = (s_codec_mode == CODEC_MODE_PCM)
                                 ? s_bits_per_sample
                                 : 4;
        s_audio_bitrate = s_sample_rate * (uint32_t)bits_per_codec * s_channels;
    }

    /* Codec-dependent packet payload size:
     * ADPCM: [DVI4 hdr 4B][adpcm nibbles] per channel -> s_channels x (4 + samples/2)
     * PCM 16-bit: [int16 samples]                       -> samples_per_frame x channels x 2
     * PCM 24-bit: [3 bytes/sample, low bytes of int32]  -> samples_per_frame x channels x 3
     * (24-bit packs to 3 bytes: int32_t sign-extended sample -> strip high byte) */
    int adpcm_data_len, pcm_data_len;
    if (s_codec_mode == CODEC_MODE_PCM)
    {
        int bytes_per_sample = (cfg.bits_per_sample == 24) ? 3 : 2;
        pcm_data_len = s_samples_per_frame * s_channels * bytes_per_sample;
        adpcm_data_len = 0;
        s_pkt_codec_id = CODEC_ID_PCM;
        s_pkt_data_len = pcm_data_len;
    }
    else
    {
        adpcm_data_len = s_channels * (DVI4_HEADER_SIZE + s_adpcm_frame_bytes);
        pcm_data_len = 0;
        s_pkt_codec_id = CODEC_ID_ADPCM;
        s_pkt_data_len = adpcm_data_len;
    }

    /* UDP MTU guard. For PCM at high sample rates, frame_ms is auto-reduced
     * by i2s_capture_compute_frame_ms, but we re-check here. */
    int max_pkt_len = PKT_HDR_SIZE + s_pkt_data_len;
    if (max_pkt_len > UDP_MTU_BYTES)
    {
        ESP_LOGE(TAG, "Packet size %d bytes exceeds UDP MTU (%d). "
                      "Reduce sample rate, frame duration, channels, or use ADPCM. "
                      "(rate=%u, frame=%ums, ch=%u, codec=%s)",
                 max_pkt_len, UDP_MTU_BYTES, (unsigned)s_sample_rate, (unsigned)s_frame_ms, (unsigned)s_channels,
                 s_codec_mode == CODEC_MODE_PCM ? "PCM" : "ADPCM");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Audio: %u Hz, %u ms, %u ch, %d samples/frame, codec=%s, %d bytes/pkt, %u bps",
             (unsigned)s_sample_rate, (unsigned)s_frame_ms, (unsigned)s_channels,
             s_samples_per_frame,
             s_codec_mode == CODEC_MODE_PCM ? "PCM" : "ADPCM",
             s_pkt_data_len, (unsigned)s_audio_bitrate);

    /* 1. WiFi - initialize (if not done at boot) and wait for readiness. */
    /* FIX (F-C #8): For RawTX, the WiFi channel can only be applied inside
     * wifi_init() (esp_wifi_set_channel is called from the STA_START handler
     * in wifi_sta.c). s_wifi_initialized stays true across AT+HOTRESTART, so
     * a plain `if (!s_wifi_initialized)` skip would leave an AT+WCH channel
     * change unapplied. Detect the change via s_rawtx_wifi_channel and force
     * a deinit+re-init so the new channel takes effect. */
    if (cfg.transport_mode == TRANSPORT_MODE_RAWTX && s_wifi_initialized &&
        s_rawtx_wifi_channel != cfg.wifi_channel)
    {
        if (s_rawtx_wifi_channel == 0)
        {
            ESP_LOGI(TAG, "RawTX: recording initial WiFi channel %u (skip reinit)",
                     (unsigned)cfg.wifi_channel);
            s_rawtx_wifi_channel = cfg.wifi_channel;
        }
        else
        {
            /* Реальная смена канала (AT+WCH + HOTRESTART) */
            ESP_LOGI(TAG, "RawTX channel changed %u -> %u, re-initializing WiFi",
                     (unsigned)s_rawtx_wifi_channel, (unsigned)cfg.wifi_channel);
            ops->deinit();
            wifi_sta_deinit();
            s_wifi_initialized = false;
        }
    }

    if (!s_wifi_initialized)
    {
        esp_err_t err = ops->wifi_init(&cfg);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
            return err;
        }
        s_wifi_initialized = true;
        /* FIX (F-C #8): remember the channel we just initialized RawTX WiFi
         * with, so a later AT+WCH + AT+HOTRESTART can detect the change. */
        if (cfg.transport_mode == TRANSPORT_MODE_RAWTX)
            s_rawtx_wifi_channel = cfg.wifi_channel;
    }

    esp_err_t err = ops->wifi_wait_ready(&cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "WiFi not ready: %s", esp_err_to_name(err));
        return err;
    }

    /* 2. Service port (UDP: init EASSP listener; RAWTX: no-op). */
    err = ops->svc_port_init(s_stream_evt_grp, cfg.svc_port);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Service port init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 3. I2S capture. DMA-размеры и таймауты выводятся из samples_per_frame. */
    err = i2s_capture_init(s_sample_rate, cfg.bits_per_sample,
                           cfg.comm_format, cfg.channel_format,
                           s_samples_per_frame, s_frame_ms,
                           cfg.gain, cfg.agc_mode,
                           cfg.i2s_timing_sd_delay,
                           cfg.i2s_timing_ws_delay,
                           cfg.i2s_timing_bck_delay);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "I2S init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 4. TPDF dither. */
    tpdf_init();
    tpdf_seed(esp_random());

    /* 5. ADPCM encoders (one per channel). Skipped in PCM mode. */
    int num_enc = 0;
    if (s_codec_mode == CODEC_MODE_ADPCM)
    {
        num_enc = (s_channels == 2) ? 2 : 1;
    }
    for (int i = 0; i < num_enc; i++)
    {
        adpcm_enc[i] = adpcm_enc_create();
        if (!adpcm_enc[i])
        {
            ESP_LOGE(TAG, "ADPCM encoder %d init failed", i);
            for (int j = 0; j < i; j++)
            {
                adpcm_enc_destroy(adpcm_enc[j]);
                adpcm_enc[j] = NULL;
            }
            {
                esp_err_t derr = i2s_capture_deinit();
                if (derr != ESP_OK)
                    ESP_LOGW(TAG, "i2s_capture_deinit: %s", esp_err_to_name(derr));
            }
            return ESP_FAIL;
        }
    }

    /* 6. Transport init (UDP: create socket; RAWTX: build 802.11 header). */
    err = ops->transport_init(stream_host, stream_port);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Transport init failed: %s", esp_err_to_name(err));
        for (int i = 0; i < num_enc; i++)
        {
            adpcm_enc_destroy(adpcm_enc[i]);
            adpcm_enc[i] = NULL;
        }
        {
            esp_err_t derr = i2s_capture_deinit();
            if (derr != ESP_OK)
                ESP_LOGW(TAG, "i2s_capture_deinit: %s", esp_err_to_name(derr));
        }
        return err;
    }

    /* 7. Queues & pools — sizes computed adaptively from free heap and
     * frame_ms. Target: 100ms PCM (I2S->ADPCM), 200ms ADPCM (encoder->UDP,
     * protects against WiFi jitter). pool_size = buffer_ms / frame_ms,
     * clamped to [min, max]. Memory budget: max 2/5 of free heap combined. */
    int samples_per_pcm = s_samples_per_frame * s_channels;
    bool need_int32 = (s_bits_per_sample == 24) && (s_codec_mode == CODEC_MODE_PCM);
    size_t pcm_elem_size = need_int32 ? sizeof(int32_t) : sizeof(int16_t);
    int pcm_frame_bytes = (int)sizeof(pcm_frame_t) + samples_per_pcm * (int)pcm_elem_size;
    int adpcm_frame_bytes = (int)sizeof(adpcm_frame_t) + s_pkt_data_len;

    /* 7. Queues & pools — sizes computed adaptively from free heap,
     * frame_ms, codec and transport.
     *
     * pcm_pool  — buffer between I2S and the encoder/packer task:
     *   ADPCM:    encoder is CPU-heavy        -> 100 ms
     *   PCM 24b:  4B->3B conversion           ->  60 ms
     *   PCM 16b:  plain memcpy                ->  30 ms
     *
     * adpcm_pool — buffer between encoder/packer and TX (network jitter):
     *   TCP:      blocking send (SO_SNDTIMEO) -> 200 ms (do NOT shrink)
     *   UDP/RAWTX: non-blocking send          -> 100 ms
     *
     * pool_size = buffer_ms / frame_ms, clamped to [min, max].
     * Memory budget: max 2/5 of free heap combined (unchanged). */
    int pcm_target_ms, adpcm_target_ms;

    if (s_codec_mode == CODEC_MODE_ADPCM)
    {
        pcm_target_ms = 100;
        adpcm_target_ms = (cfg.transport_mode == TRANSPORT_MODE_TCP) ? 200 : 150;
    }
    else if (s_bits_per_sample == 24)
    {
        pcm_target_ms = 60;
        adpcm_target_ms = (cfg.transport_mode == TRANSPORT_MODE_TCP) ? 200 : 100;
    }
    else /* PCM 16-bit */
    {
        pcm_target_ms = 30;
        adpcm_target_ms = (cfg.transport_mode == TRANSPORT_MODE_TCP) ? 200 : 100;
    }

    s_pcm_pool_size = pcm_target_ms / s_frame_ms;
    s_adpcm_pool_size = adpcm_target_ms / s_frame_ms;

    /* Clamps: PCM packer is fast -> smaller pcm_pool cap is safe.
     * TCP needs a deeper adpcm_pool (blocking send backpressure). */
    int pcm_max = (s_codec_mode == CODEC_MODE_PCM) ? 6 : PCM_POOL_MAX;
    int adpcm_max = (cfg.transport_mode == TRANSPORT_MODE_TCP)
                        ? ADPCM_POOL_MAX /* 16 — TCP blocking */
                        : 10;            /* UDP/RAWTX non-blocking */

    if (s_pcm_pool_size < PCM_POOL_MIN)
        s_pcm_pool_size = PCM_POOL_MIN;
    if (s_pcm_pool_size > pcm_max)
        s_pcm_pool_size = pcm_max;
    if (s_adpcm_pool_size < ADPCM_POOL_MIN)
        s_adpcm_pool_size = ADPCM_POOL_MIN;
    if (s_adpcm_pool_size > adpcm_max)
        s_adpcm_pool_size = adpcm_max;
    // оригинальн
    /*     s_pcm_pool_size = 100 / s_frame_ms;
        s_adpcm_pool_size = 200 / s_frame_ms;
        if (s_pcm_pool_size < PCM_POOL_MIN)
            s_pcm_pool_size = PCM_POOL_MIN;
        if (s_pcm_pool_size > PCM_POOL_MAX)
            s_pcm_pool_size = PCM_POOL_MAX;
        if (s_adpcm_pool_size < ADPCM_POOL_MIN)
            s_adpcm_pool_size = ADPCM_POOL_MIN;
        if (s_adpcm_pool_size > ADPCM_POOL_MAX)
            s_adpcm_pool_size = ADPCM_POOL_MAX;  */

    /* Memory budget: max 2/5 of free heap for both pools combined. */
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t pool_mem = (uint32_t)(s_pcm_pool_size * pcm_frame_bytes +
                                   s_adpcm_pool_size * adpcm_frame_bytes);
    uint32_t mem_budget = free_heap * POOL_BUDGET_FRACTION / POOL_BUDGET_DIVISOR;
    if (pool_mem > mem_budget && pool_mem > 0)
    {
        int scale = (int)(mem_budget * 100U / pool_mem);
        s_pcm_pool_size = (s_pcm_pool_size * scale) / 100;
        s_adpcm_pool_size = (s_adpcm_pool_size * scale) / 100;
        if (s_pcm_pool_size < 2)
            s_pcm_pool_size = 2;
        if (s_adpcm_pool_size < 2)
            s_adpcm_pool_size = 2;
        ESP_LOGW(TAG, "Pool sizes reduced to fit memory: pcm=%d adpcm=%d (heap=%u)",
                 s_pcm_pool_size, s_adpcm_pool_size, (unsigned)free_heap);
    }

    ESP_LOGI(TAG, "Pools: pcm=%dx%d=%uB, adpcm=%dx%d=%uB (heap=%u, budget=%u)",
             s_pcm_pool_size, pcm_frame_bytes,
             (unsigned)(s_pcm_pool_size * pcm_frame_bytes),
             s_adpcm_pool_size, adpcm_frame_bytes,
             (unsigned)(s_adpcm_pool_size * adpcm_frame_bytes),
             (unsigned)free_heap, (unsigned)mem_budget);

    pcm_free_queue = xQueueCreate(s_pcm_pool_size, sizeof(pcm_frame_t *));
    pcm_filled_queue = xQueueCreate(s_pcm_pool_size, sizeof(pcm_frame_t *));
    adpcm_free_queue = xQueueCreate(s_adpcm_pool_size, sizeof(adpcm_frame_t *));
    adpcm_filled_queue = xQueueCreate(s_adpcm_pool_size, sizeof(adpcm_frame_t *));
    if (!pcm_free_queue || !pcm_filled_queue ||
        !adpcm_free_queue || !adpcm_filled_queue)
    {
        ESP_LOGE(TAG, "Failed to create queues");
        goto cleanup_on_fail;
    }

    for (int i = 0; i < s_pcm_pool_size; i++)
    {
        pcm_frame_t *f = pcm_frame_alloc(samples_per_pcm, s_bits_per_sample, s_codec_mode);
        if (!f)
        {
            ESP_LOGE(TAG, "PCM alloc fail");
            goto cleanup_on_fail;
        }
        xQueueSend(pcm_free_queue, &f, 0);
    }

    for (int i = 0; i < s_adpcm_pool_size; i++)
    {
        adpcm_frame_t *f = adpcm_frame_alloc(s_pkt_data_len);
        if (!f)
        {
            ESP_LOGE(TAG, "ADPCM alloc fail");
            goto cleanup_on_fail;
        }
        xQueueSend(adpcm_free_queue, &f, 0);
    }

    /* 8. Per-task done semaphores. */
    for (int i = 0; i < TASK_IDX_COUNT; i++)
    {
        s_task_done_sems[i] = xSemaphoreCreateBinary();
        if (!s_task_done_sems[i])
        {
            ESP_LOGE(TAG, "Failed to create task done semaphore %d", i);
            goto cleanup_on_fail;
        }
    }

    /* 9. Create pipeline tasks. */
    xEventGroupSetBits(s_stream_evt_grp, STREAM_EVT_ACTIVE);
#ifdef CONFIG_STREAMER_SUPERVISOR_ENABLED
    s_supervisor_stream_start_tick = xTaskGetTickCount();
#endif

    /* Choose encoder task: adpcm_task_fn for ADPCM, pcm_task_fn for PCM. */
    TaskFunction_t enc_task_fn = (s_codec_mode == CODEC_MODE_PCM)
                                     ? pcm_task_fn
                                     : adpcm_task_fn;
    const char *enc_task_name = (s_codec_mode == CODEC_MODE_PCM)
                                    ? "pcm"
                                    : "adpcm";

    /* FIX (MEDIUM #4, MEDIUM #37): see FIXES.md */
    bool tasks_created = false;
    if (s_task_handles_mutex &&
        xSemaphoreTake(s_task_handles_mutex, portMAX_DELAY) == pdTRUE)
    {
        if (xTaskCreate(i2s_task_fn, "i2s", TASK_STACK_I2S,
                        (void *)TASK_IDX_I2S, TASK_PRIO_I2S,
                        &s_task_handles[TASK_IDX_I2S]) != pdPASS ||
            xTaskCreate(enc_task_fn, enc_task_name, TASK_STACK_ADPCM,
                        (void *)TASK_IDX_ADPCM, TASK_PRIO_ADPCM,
                        &s_task_handles[TASK_IDX_ADPCM]) != pdPASS ||
            xTaskCreate(stream_task_fn, "tx", TASK_STACK_UDP,
                        (void *)TASK_IDX_UDP, TASK_PRIO_UDP,
                        &s_task_handles[TASK_IDX_UDP]) != pdPASS)
        {
            tasks_created = false;
        }
        else
        {
            tasks_created = true;
        }
        xSemaphoreGive(s_task_handles_mutex);
    }
    if (!tasks_created)
    {
        ESP_LOGE(TAG, "Failed to create pipeline tasks (out of memory?)");
        goto cleanup_on_fail;
    }

    /* Notify mode-specific subsystems that streaming has started. */
    ops->on_stream_started();

    ESP_LOGI(TAG, "Streaming started - %u Hz, %u ms frames, %u ch",
             (unsigned)s_sample_rate, (unsigned)s_frame_ms, (unsigned)s_channels);
    /* FIX (F-C #4): only now — after ALL validations passed and the pipeline
     * tasks are actually running — is s_frame_ms_known safe to publish.
     * streaming_frame_ms_known() / streaming_get_frame_ms() callers will
     * now read the real value, never a stale one from a half-started stream. */
    s_frame_ms_known = true;
    return ESP_OK;

cleanup_on_fail:
    teardown_pipeline(true);
    return ESP_FAIL;
}

void stop_streaming(void)
{
    if (!streaming_is_active())
    {
        ESP_LOGW(TAG, "Not streaming");
        return;
    }

    ESP_LOGI(TAG, "Stopping streaming...");
    teardown_pipeline(false);
    ESP_LOGI(TAG, "Streaming stopped");
}
