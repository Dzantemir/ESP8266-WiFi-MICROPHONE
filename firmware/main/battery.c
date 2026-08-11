/*
 * Battery monitoring - ported from ESP8285-WEBSERVER project
 * (https://github.com/Dzantemir/ESP8285-WEBSERVER).
 *
 * Uses ESP8266's ADC (TOUT pin, 0-1V, 10-bit) with an external voltage
 * divider to measure battery voltage. A background task periodically
 * checks the voltage and puts the device into deep sleep if it drops
 * below the critical threshold.
 *
 * When CONFIG_STREAMER_BATTERY_ENABLED is NOT set, all functions compile
 * to no-ops / return 0, so callers need no #ifdefs around battery_* calls.
 */

/* ---- System / SDK includes ---- */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "driver/adc.h"

/* ---- Project includes ---- */
#include "board_config.h"
/* Include battery.h unconditionally so prototypes are visible in the
 * disabled-stubs branch too (MEDIUM #26). */
#include "battery.h"


/* Needed for streaming_is_active() / streaming_request_stop() in
 * battery_enter_deep_sleep() — graceful stream stop before deep sleep (GROK-3.7). */
#include "stream_control.h"



#if BATTERY_ENABLED

static const char *TAG = "battery";

/* Last measured voltage - written by monitor task, read by anyone.
 * 32-bit aligned write is atomic on Xtensa LX106, so no mutex needed. */
static volatile uint32_t s_last_mv = 0;

/* The ESP8266 ADC driver is NOT reentrant (M26): battery_get_voltage_mv was
 * previously callable from the AT task while the monitor task was inside
 * adc_read(), producing garbage readings or driver corruption. Serialize all
 * adc_read() calls with this mutex.
 *
 * The mutex is created once in battery_init() before any caller can touch it
 * (AUDIT-C2: the previous lazy-init in ensure_adc_mutex() had a race — two
 * concurrent callers could both create a mutex and use different handles).
 * battery_get_voltage_mv() logs+returns the cached value if battery_init()
 * was not called. */
static SemaphoreHandle_t s_adc_mutex = NULL;

esp_err_t battery_init(void)
{
    adc_config_t adc_cfg = {
        .mode = ADC_READ_TOUT_MODE,
        .clk_div = 8,
    };
    esp_err_t err = adc_init(&adc_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "ADC init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_adc_mutex = xSemaphoreCreateMutex();
    if (!s_adc_mutex)
    {
        ESP_LOGE(TAG, "Failed to create ADC mutex");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Battery monitoring enabled (divider ratio=%u, critical=%u mV)",
             (unsigned)BATT_DIVIDER_RATIO, (unsigned)BATT_CRITICAL_MV);
    return ESP_OK;
}

uint32_t battery_get_voltage_mv(void)
{
    /* NOTE: This function holds s_adc_mutex for ~750ms (15 samples x 50ms).
     * For non-blocking reads, use battery_get_last_mv() instead. AT commands
     * should NEVER call this directly -- use battery_get_last_mv().
     *
     * Serialize ADC access (M26): AT task and monitor task can't both call
     * adc_read() at the same time. */
    if (!s_adc_mutex ||
        xSemaphoreTake(s_adc_mutex, pdMS_TO_TICKS(2000)) != pdTRUE)
    {
        ESP_LOGW(TAG, "battery_get_voltage_mv: ADC mutex unavailable "
                      "(battery_init not called or mutex timeout)");
        return s_last_mv; /* return last cached value */
    }

    uint32_t adc_sum = 0;
    int valid_samples = 0;
    const int samples = BATT_ADC_SAMPLES;

    for (int i = 0; i < samples; i++)
    {
        uint16_t val = 0;
        if (adc_read(&val) != ESP_OK)
        {
            continue;
        }
        adc_sum += val;
        valid_samples++;
        vTaskDelay(pdMS_TO_TICKS(BATT_ADC_DELAY_MS));
    }

    xSemaphoreGive(s_adc_mutex);

    if (valid_samples == 0)
    {
        ESP_LOGW(TAG, "ADC read failed for all %d samples", samples);
        return 0;
    }

    uint32_t adc_avg = adc_sum / valid_samples;
    uint32_t v_mv = (uint32_t)((adc_avg * (uint32_t)BATT_DIVIDER_RATIO) / 1024);
    return v_mv;
}

void battery_enter_deep_sleep(uint32_t minutes)
{
    ESP_LOGW(TAG, "Entering deep sleep for %u minutes", (unsigned)minutes);

    /* Stop the stream gracefully before deep sleep (GROK-3.7). Without this,
     * TCP/UDP sockets disappear without FIN/close -> server has to time out
     * (cosmetic), and no final INFO packet is sent. A short delay lets the
     * pipeline flush, the TX task send a final frame, and sockets close
     * cleanly. streaming_request_stop() is async (sets a bit); the 500ms
     * delay gives the main loop time to process it. If streaming is not
     * active, this is a no-op. */
    if (streaming_is_active())
    {
        ESP_LOGI(TAG, "Stopping stream before deep sleep...");
        streaming_request_stop();
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    /* RF calibration on wake: 0=no cal, 1=cal, 2=cal but don't write to NVS.
     * Option 2 is safe and faster than 1, doesn't wear out NVS. */
    esp_deep_sleep_set_rf_option(2);
    esp_deep_sleep((uint64_t)minutes * 60ULL * 1000000ULL);

    /* Should never reach here - esp_deep_sleep() doesn't return. */
    ESP_LOGE(TAG, "esp_deep_sleep returned unexpectedly! Restarting...");
    esp_restart();
}

void battery_monitor_task(void *arg)
{
    (void)arg; /* suppress unused-parameter warning (LOW). */

    /* First measurement immediately. */
    uint32_t v_batt = battery_get_voltage_mv();
    s_last_mv = v_batt;

    if (v_batt == 0)
    {
        /* ADC returning 0 mV almost certainly means the battery divider is
         * disconnected or the ADC is not wired up (GROK-9). Continuing to
         * boot means the runtime loop will also see 0 and skip every check
         * (no battery protection at all). We still continue (so a bench
         * device without a battery can run off USB), but log prominently. */
        ESP_LOGW(TAG, "Battery reading invalid (0 mV) - ADC not connected? "
                      "Battery protection DISABLED (running without gauge).");
    }
    else if (v_batt < BATT_BAD_MV)
    {
        /* A reading below BATT_BAD_MV is suspicious (likely a disconnected
         * divider), BUT it could also be a genuine critically-low battery
         * (MEDIUM #27). Retry once to distinguish: if the retry is still
         * < BATT_BAD_MV, assume ADC fault and continue (bench testing on USB).
         * If the retry succeeds and lands between BATT_BAD_MV and
         * BATT_CRITICAL_MV, the battery really is critical — trigger deep
         * sleep. Previously a 2400 mV battery would be silently ignored at boot. */
        ESP_LOGW(TAG, "Battery reading suspiciously low (%u mV < %u) - retrying",
                 (unsigned)v_batt, (unsigned)BATT_BAD_MV);
        vTaskDelay(pdMS_TO_TICKS(BATT_ADC_DELAY_MS));
        v_batt = battery_get_voltage_mv();
        s_last_mv = v_batt;
        /* The first retry check was previously `if (v_batt != 0 && v_batt <
         * BATT_BAD_MV)` — so a retry that returned 0 (ADC fault on every
         * sample) FELL THROUGH to the `else if (v_batt < BATT_CRITICAL_MV)`
         * branch, where `0 < BATT_CRITICAL_MV` is TRUE -> the device entered
         * deep sleep on every boot with a flaky ADC (4-C MEDIUM #8). The outer
         * if at line above already treats v_batt==0 as "ADC not connected,
         * continue (battery protection DISABLED)"; the retry must do the same.
         * Treat v_batt==0 (ADC fault) AND v_batt<BATT_BAD_MV (still suspicious)
         * the same way: skip the critical check and fall through to the
         * monitoring loop (matches the runtime path below which uses
         * `if (v_batt < BATT_BAD_MV)` without the `!= 0` guard). */
        if (v_batt == 0 || v_batt < BATT_BAD_MV)
        {
            ESP_LOGW(TAG, "ADC reading still invalid (%u mV) — skipping",
                     (unsigned)v_batt);
            /* Fall through to monitoring loop (allow bench testing on USB). */
        }
        else if (v_batt < BATT_CRITICAL_MV)
        {
            ESP_LOGW(TAG, "Battery CRITICAL (%u mV < %u) - deep sleeping",
                     (unsigned)v_batt, (unsigned)BATT_CRITICAL_MV);
            battery_enter_deep_sleep(BATT_SLEEP_MIN);
            return; /* never reached */
        }
        else if (v_batt < BATT_START_MV)
        {
            ESP_LOGW(TAG, "Battery LOW (%u mV < %u) - deep sleeping",
                     (unsigned)v_batt, (unsigned)BATT_START_MV);
            battery_enter_deep_sleep(BATT_SLEEP_MIN);
            return; /* never reached */
        }
        else
        {
            ESP_LOGI(TAG, "Battery OK after retry: %u mV (%u%%)",
                     (unsigned)v_batt, (unsigned)battery_get_percent());
        }
    }
    else if (v_batt < BATT_CRITICAL_MV)
    {
        ESP_LOGW(TAG, "Battery CRITICAL (%u mV < %u) - deep sleeping",
                 (unsigned)v_batt, (unsigned)BATT_CRITICAL_MV);
        battery_enter_deep_sleep(BATT_SLEEP_MIN);
        return; /* never reached */
    }
    else if (v_batt < BATT_START_MV)
    {
        /* Enforce BATT_START_MV at boot (H14): board_config.h documents this
         * as "below on boot -> don't start" but the previous code only checked
         * CRITICAL_MV, allowing the device to boot and stream with a nearly-
         * dead battery (between CRITICAL=3700 and START=3900), draining it
         * further into deep-sleep territory.
         *
         * NOTE (GROK-9): this boot-only START check creates an asymmetry with
         * the runtime loop, which used to check only CRITICAL_MV. The runtime
         * loop below now intentionally keeps CRITICAL_MV-only — see the
         * asymmetry note in the runtime section. */
        ESP_LOGW(TAG, "Battery LOW (%u mV < %u) - deep sleeping",
                 (unsigned)v_batt, (unsigned)BATT_START_MV);
        battery_enter_deep_sleep(BATT_SLEEP_MIN);
        return; /* never reached */
    }
    else
    {
        ESP_LOGI(TAG, "Battery OK: %u mV (%u%%)",
                 (unsigned)v_batt, (unsigned)battery_get_percent());
    }

    /* Periodic monitoring loop. */
    const TickType_t check_period = pdMS_TO_TICKS(BATT_CHECK_MIN * 60U * 1000U);
    /* Use vTaskDelayUntil for accurate periodic timing (M27): vTaskDelay adds
     * the measurement time (~750 ms) to the period, drifting ~40% over time
     * at BATT_CHECK_MIN=1. */
    TickType_t last_wake = xTaskGetTickCount();
    while (1)
    {
        vTaskDelayUntil(&last_wake, check_period);

        v_batt = battery_get_voltage_mv();
        s_last_mv = v_batt;

        if (v_batt == 0)
        {
            ESP_LOGW(TAG, "ADC read failed - skipping check");
            continue;
        }
        /* A reading below BATT_BAD_MV was previously treated as "invalid,
         * skip" — but a genuinely critical battery (e.g. 2400 mV) is REAL
         * and should trigger deep sleep (MEDIUM #27). Now: retry once; if
         * the retry is still < BATT_BAD_MV, assume ADC fault and skip this
         * cycle (genuinely disconnected divider). If the retry succeeds and
         * lands between BATT_BAD_MV and BATT_CRITICAL_MV, the battery is
         * genuinely critical — trigger deep sleep. */
        if (v_batt < BATT_BAD_MV)
        {
            ESP_LOGW(TAG, "Battery reading suspiciously low (%u mV < %u) - retrying",
                     (unsigned)v_batt, (unsigned)BATT_BAD_MV);
            vTaskDelay(pdMS_TO_TICKS(BATT_ADC_DELAY_MS));
            v_batt = battery_get_voltage_mv();
            s_last_mv = v_batt;
            if (v_batt < BATT_BAD_MV)
            {
                ESP_LOGW(TAG, "ADC reading still invalid (%u mV) — skipping", (unsigned)v_batt);
                continue;  /* skip this cycle */
            }
        }
        if (v_batt < BATT_CRITICAL_MV)
        {
            ESP_LOGW(TAG, "Battery critical (%u mV < %u) — deep sleep",
                     (unsigned)v_batt, (unsigned)BATT_CRITICAL_MV);
            battery_enter_deep_sleep(BATT_SLEEP_MIN);
            return;  /* won't reach here */
        }
        /* ASYMMETRIC policy (B1 REGRESSION): GROK-9 made the runtime check
         * symmetric with the boot check (both checked BATT_START_MV). This
         * was WRONG: a Li-ion cell under WiFi-TX + I2S load sags to 3.7-3.85V
         * even at 50% SoC. With START=3900mV, the symmetric policy caused the
         * device to deep-sleep every BATT_CHECK_MIN minutes on a healthy
         * battery, making it nearly unusable below ~75% SoC.
         *
         * The CORRECT policy is ASYMMETRIC:
         *   - Boot: sleep at < START (conservative — don't bring up the full
         *     pipeline if the cell is already weak)
         *   - Runtime: sleep ONLY at < CRITICAL (transient sags under load
         *     are normal; only true critical level should trigger sleep)
         * The boot START check (above) is kept. The runtime START check is
         * intentionally removed here — this is the original design intent;
         * the GROK-9 "fix" broke it by misinterpreting the asymmetry as a bug. */

        ESP_LOGI(TAG, "Battery: %u mV (%u%%)",
                 (unsigned)v_batt, (unsigned)battery_get_percent());
    }
}

/* battery.h promises "all functions are available (stubs when disabled)"
 * (MEDIUM #26) — provide them here so callers need no #ifdefs. */
uint32_t battery_get_last_mv(void)
{
    return s_last_mv;
}

uint8_t battery_get_percent(void)
{
    uint32_t v = s_last_mv;
    if (v == 0)
        return 0;
    if (v <= BATT_CRITICAL_MV)
        return 0;
    if (v >= 4200)
        return 100;
    /* Linear interpolation between critical and 4.2V full. */
    return (uint8_t)((v - BATT_CRITICAL_MV) * 100U / (4200U - BATT_CRITICAL_MV));
}

#else  /* !BATTERY_ENABLED */

/* Disabled-stubs (MEDIUM #26): battery.h promises "all functions are
 * available (stubs when disabled)" — without these, any caller (e.g. main.c,
 * at_cmd.c) that calls battery_* without an #ifdef guard would fail to link
 * when CONFIG_STREAMER_BATTERY_ENABLED is unset. */

esp_err_t battery_init(void) { return ESP_OK; }
uint32_t battery_get_voltage_mv(void) { return 0; }
uint32_t battery_get_last_mv(void) { return 0; }
uint8_t battery_get_percent(void) { return 0; }
void battery_monitor_task(void *arg) { (void)arg; vTaskDelete(NULL); }
void battery_enter_deep_sleep(uint32_t minutes) { (void)minutes; }

#endif /* BATTERY_ENABLED */
