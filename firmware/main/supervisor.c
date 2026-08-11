/*
 * supervisor.c
 * ============
 *
 * Software watchdog task — split out of main.c in the R3-A structural
 * refactor.
 *
 * Checks every SUPERVISOR_CHECK_INTERVAL_MS: free heap, pipeline counter
 * progress (I2S/TX), task stack high-water. Calls esp_restart() on failure.
 * The HW WDT only fires on CPU-hog; this catches deadlocks where tasks
 * yield but make no progress. Low priority (runs only when idle).
 *
 * This file owns the supervisor liveness counters (declared extern in
 * pipeline_internal.h). The pipeline tasks in pipeline.c increment them
 * as frames flow through; supervisor_task_fn() reads them here.
 */


/* ---- System / SDK includes ---- */
#include <stdio.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_system.h"
#include "esp_log.h"

/* ---- Project includes ---- */
#include "board_config.h"      /* SUPERVISOR_* thresholds, TASK_* */
#include "pipeline_internal.h"
#include "stream_control.h"    /* streaming_is_active() */



#ifdef CONFIG_STREAMER_SUPERVISOR_ENABLED

static const char *TAG = "supervisor";

/* ---- Supervisor liveness counters -------------------------------- */
/* Defined here; declared extern in pipeline_internal.h. Written from
 * the pipeline tasks (pipeline.c) and read here in supervisor_task_fn. */

volatile uint32_t s_supervisor_i2s_count = 0;
volatile uint32_t s_supervisor_tx_count = 0;
/* FIX (supervisor-drops): see FIXES.md — consecutive TX drops. */
volatile uint32_t s_supervisor_tx_consecutive_drops = 0;
volatile TickType_t s_supervisor_stream_start_tick = 0;

/* ====================================================================
 * Supervisor task — software watchdog
 * ==================================================================== */
void supervisor_task_fn(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Supervisor task started (interval=%dms, min_heap=%u, stall=%dms)",
             SUPERVISOR_CHECK_INTERVAL_MS,
             (unsigned)SUPERVISOR_MIN_HEAP_BYTES,
             SUPERVISOR_STALL_TIMEOUT_MS);

    uint32_t last_i2s_count = 0;
    uint32_t last_tx_count = 0;
    /* FIX (GROK-3.3): see FIXES.md */
    TickType_t last_i2s_progress_tick = xTaskGetTickCount();
    TickType_t last_tx_progress_tick = xTaskGetTickCount();
    /* FIX (LOW #13): see FIXES.md — track when we first noticed TX handle
     * is NULL while streaming, to implement a grace period before rebooting.
     * Function-local static: only supervisor_task_fn reads/writes it. */
    static TickType_t s_tx_dead_since = 0;

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(SUPERVISOR_CHECK_INTERVAL_MS));

        TickType_t now = xTaskGetTickCount();

        /* ---- Check 1: Heap ---- */
        uint32_t free_heap = esp_get_free_heap_size();
        if (free_heap < SUPERVISOR_MIN_HEAP_BYTES)
        {
            ESP_LOGE(TAG, "SUPERVISOR: free heap %u < %u — REBOOT",
                     (unsigned)free_heap, (unsigned)SUPERVISOR_MIN_HEAP_BYTES);
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }

        /* ---- Check 2: Pipeline liveness ---- */
        if (streaming_is_active())
        {
            TickType_t stream_elapsed = now - s_supervisor_stream_start_tick;

            /* Only check after grace period has passed since stream start.
             * During startup, counters may not move for legitimate reasons
             * (I2S DMA fill, WiFi connect, etc.). */
            if (stream_elapsed >= pdMS_TO_TICKS(SUPERVISOR_STALL_TIMEOUT_MS))
            {
                uint32_t cur_i2s = s_supervisor_i2s_count;
                uint32_t cur_tx = s_supervisor_tx_count;

                bool i2s_advanced = (cur_i2s != last_i2s_count);
                bool tx_advanced = (cur_tx != last_tx_count);

                /* FIX (F-C #9): see FIXES.md — definitive TX-death detection.
                 * A NULL s_task_handles[TASK_IDX_UDP] while streaming_is_active()
                 * means the TX task has exited (clean exit via task_exit label,
                 * or force-delete by wait_for_task_exit). With the TX task gone
                 * the pipeline cannot make progress — reboot immediately. This
                 * catches the case the counter-based heuristics miss: TX died
                 * without ever incrementing s_supervisor_tx_count (so cur_tx==0
                 * and s_supervisor_tx_consecutive_drops==0 — the existing
                 * "TX idle (no client yet?)" warning would just wait forever). */
                TaskHandle_t tx_handle = NULL;
                if (s_task_handles_mutex &&
                    xSemaphoreTake(s_task_handles_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
                {
                    tx_handle = s_task_handles[TASK_IDX_UDP];
                    xSemaphoreGive(s_task_handles_mutex);
                }
                else
                {
                    /* Mutex busy (pipeline task holding it during start/stop) —
                     * can't check TX handle, skip this iteration like the
                     * stack HWM check does. Without this, tx_handle stays NULL
                     * and we'd spuriously reboot after 3s grace. */
                    last_i2s_count = cur_i2s;
                    last_tx_count = cur_tx;
                    continue;
                }

                if (tx_handle == NULL)
                {
                    /* FIX (LOW #13): see FIXES.md — grace period after TX exit.
                     * The TX task may have just called streaming_request_stop()
                     * and exited (handle NULL) before the main loop cleared
                     * streaming_is_active(). Give the main loop time to process
                     * the pending STOP_REQ before rebooting. Without this grace
                     * period, a normal clean stop (TX exits first, main loop
                     * clears ACTIVE a few ms later) would spuriously reboot. */
                    if (s_tx_dead_since == 0)
                    {
                        s_tx_dead_since = now;
                        ESP_LOGW(TAG, "SUPERVISOR: TX task handle NULL while "
                                      "streaming (I2S=%u, TX=%u) - grace period",
                                 (unsigned)cur_i2s, (unsigned)cur_tx);
                    }
                    /* Give main loop time to process the pending STOP_REQ */
                    if ((now - s_tx_dead_since) > pdMS_TO_TICKS(3000))
                    {
                        ESP_LOGE(TAG, "SUPERVISOR: TX task dead for 3s, streaming "
                                      "still active (I2S=%u, TX=%u) — REBOOT",
                                 (unsigned)cur_i2s, (unsigned)cur_tx);
                        vTaskDelay(pdMS_TO_TICKS(500));
                        esp_restart();
                    }
                    /* In grace period: skip the counter-based liveness checks
                     * below — they would fire spuriously since TX is gone (TX
                     * counter can't advance without a TX task). Also skips
                     * this iteration's stack high-water check, which is an
                     * acceptable tradeoff for the 1-2 iterations (≤3s) we're
                     * in grace. Update baselines so the next iteration sees
                     * clean state if TX comes back (new stream start). */
                    last_i2s_count = cur_i2s;
                    last_tx_count = cur_tx;
                    continue;
                }
                else
                {
                    /* TX is alive — reset the grace-period timer. */
                    s_tx_dead_since = 0;
                }


                /* FIX (GROK-3.3): see FIXES.md */
                if (i2s_advanced)
                    last_i2s_progress_tick = now;
                if (tx_advanced)
                    last_tx_progress_tick = now;

                if (!i2s_advanced && !tx_advanced)
                {
                    /* Neither counter moved since last check.
                     * Pipeline is fully deadlocked. */
                    ESP_LOGE(TAG, "SUPERVISOR: pipeline stalled — "
                                  "I2S=%u (was %u), TX=%u (was %u), "
                                  "stream_elapsed=%ums — REBOOT",
                             (unsigned)cur_i2s, (unsigned)last_i2s_count,
                             (unsigned)cur_tx, (unsigned)last_tx_count,
                             (unsigned)(stream_elapsed * portTICK_PERIOD_MS));
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_restart();
                }

                /* Partial deadlock: I2S producing frames but TX not sending.
                 * FIX (GROK-3.3, B6): see FIXES.md — use last_tx_progress_tick
                 * age; only fire if TX has EVER progressed (cur_tx > 0) so a
                 * TCP client that hasn't connected yet isn't a false stall. */
                TickType_t tx_stall_age = now - last_tx_progress_tick;
                if (i2s_advanced && !tx_advanced &&
                    cur_tx > 0 &&
                    tx_stall_age >= pdMS_TO_TICKS(SUPERVISOR_STALL_TIMEOUT_MS))
                {
                    ESP_LOGE(TAG, "SUPERVISOR: TX stalled but I2S active — "
                                  "I2S=%u (was %u), TX=%u (was %u), "
                                  "tx_stall_age=%ums — REBOOT",
                             (unsigned)cur_i2s, (unsigned)last_i2s_count,
                             (unsigned)cur_tx, (unsigned)last_tx_count,
                             (unsigned)(tx_stall_age * portTICK_PERIOD_MS));
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_restart();
                }
                /* FIX (B6, supervisor-drops): see FIXES.md */
                if (i2s_advanced && !tx_advanced && cur_tx == 0 &&
                    tx_stall_age >= pdMS_TO_TICKS(SUPERVISOR_STALL_TIMEOUT_MS))
                {
                    uint32_t cdrops = s_supervisor_tx_consecutive_drops;
                    if (cdrops > 200)
                    {
                        /* TX task is alive (dropping frames) but transport is
                         * persistently broken. 200 drops × ~500ms backoff =
                         * ~100s of continuous failure — reboot. */
                        ESP_LOGE(TAG, "SUPERVISOR: TX dropping continuously "
                                      "(%u consecutive drops, 0 success) — transport "
                                      "dead — REBOOT",
                                 (unsigned)cdrops);
                        vTaskDelay(pdMS_TO_TICKS(500));
                        esp_restart();
                    }
                    ESP_LOGW(TAG, "SUPERVISOR: TX idle (no client yet?), "
                                  "I2S=%u active, TX=%u, drops=%u — waiting",
                             (unsigned)cur_i2s, (unsigned)cur_tx,
                             (unsigned)cdrops);
                    /* Reset last_tx_progress_tick so we don't log this every 2s */
                    last_tx_progress_tick = now;
                }

                /* FIX (supervisor-drops): see FIXES.md */
                if (i2s_advanced && !tx_advanced && cur_tx > 0)
                {
                    uint32_t cdrops = s_supervisor_tx_consecutive_drops;
                    if (cdrops > 500)
                    {
                        ESP_LOGE(TAG, "SUPERVISOR: TX stalled with %u consecutive "
                                      "drops (was working, now failing) — REBOOT",
                                 (unsigned)cdrops);
                        vTaskDelay(pdMS_TO_TICKS(500));
                        esp_restart();
                    }
                }

                /* Symmetric: I2S stalled but TX still sending (rare — TX task
                 * recycling old buffers?). Also detect via last_i2s_progress_tick. */
                TickType_t i2s_stall_age = now - last_i2s_progress_tick;
                if (!i2s_advanced && tx_advanced &&
                    i2s_stall_age >= pdMS_TO_TICKS(SUPERVISOR_STALL_TIMEOUT_MS))
                {
                    ESP_LOGE(TAG, "SUPERVISOR: I2S stalled but TX active — "
                                  "I2S=%u (was %u), TX=%u (was %u), "
                                  "i2s_stall_age=%ums — REBOOT",
                             (unsigned)cur_i2s, (unsigned)last_i2s_count,
                             (unsigned)cur_tx, (unsigned)last_tx_count,
                             (unsigned)(i2s_stall_age * portTICK_PERIOD_MS));
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_restart();
                }

                last_i2s_count = cur_i2s;
                last_tx_count = cur_tx;
            }
        }
        else
        {
            /* Not streaming — reset baselines so next stream start is clean */
            last_i2s_count = s_supervisor_i2s_count;
            last_tx_count = s_supervisor_tx_count;
            last_i2s_progress_tick = xTaskGetTickCount();
            last_tx_progress_tick = xTaskGetTickCount();
            s_supervisor_tx_consecutive_drops = 0;
            /* FIX (LOW #13): see FIXES.md — reset TX-dead grace timer when
             * stream stops, so next stream start isn't immediately in grace. */
            s_tx_dead_since = 0;
        }

        /* ---- Check 3: Stack high-water mark ---- */
        /* FIX (MEDIUM #37, MEDIUM #4): see FIXES.md */
        for (int i = 0; i < TASK_IDX_COUNT; i++)
        {
            TaskHandle_t h = NULL;
            UBaseType_t hwm = 0;
            bool have_hwm = false;
            if (s_task_handles_mutex &&
                xSemaphoreTake(s_task_handles_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
            {
                h = s_task_handles[i];
                if (h != NULL)
                {
                    hwm = uxTaskGetStackHighWaterMark(h);
                    have_hwm = true;
                }
                xSemaphoreGive(s_task_handles_mutex);
            }
            else
            {
                /* FIX (Task 6-B): see FIXES.md */
                continue;
            }
            if (have_hwm && hwm < (SUPERVISOR_MIN_STACK_BYTES / sizeof(StackType_t)))
            {
                ESP_LOGE(TAG, "SUPERVISOR: task %d stack low (%u words) — REBOOT",
                         i, (unsigned)hwm);
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
        }
    }
}

#endif /* CONFIG_STREAMER_SUPERVISOR_ENABLED */
