/*
 * ESP8266 WiFi Microphone - Main Application
 *
 * Architecture:
 *   Service Port (always active) for discovery/control
 *   I2S Task -> PCM Queue -> ADPCM Task -> ADPCM Queue -> UDP Task
 *
 * Stream control via FreeRTOS EventGroup:
 *   STREAM_EVT_START_REQ  - set by svc_port on CONFIGURE
 *   STREAM_EVT_STOP_REQ   - set by svc_port on watchdog expiry / re-CONFIGURE
 *   STREAM_EVT_ACTIVE     - set by start_streaming, cleared by stop_streaming
 *
 * Clean shutdown: pipeline tasks exit their loops when STREAM_EVT_ACTIVE is
 * cleared, give per-task done semaphores, and self-delete. The I2S task uses
 * a short i2s_read timeout (computed from DMA buffer capacity) so it re-checks
 * the active flag frequently - enables fast clean stop without force-deletion
 * (which would leave the I2S driver mutex locked and deadlock i2s_driver_uninstall).
 *
 * ---------------------------------------------------------------------------
 * R3-A structural refactor: the pipeline lifecycle, pipeline tasks, pool
 * management and supervisor task have been split out of this file into
 * pipeline.c, supervisor.c and stream_control.c. main.c now keeps only:
 *   - app_main()         (boot sequence + main event loop)
 *   - wifi_boot_retry_or_sleep()  (boot-time WiFi retry-or-sleep helper)
 * Cross-file state is declared extern in include/pipeline_internal.h.
 */

/* ---- System / SDK includes ---- */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h" /* xSemaphoreCreateMutex (s_task_handles_mutex) */
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_sleep.h" /* FIX (wifi-boot-retry): esp_deep_sleep for WiFi boot retry */
#include "nvs_flash.h"

/* ---- Project includes ---- */
#include "board_config.h"
#include "config_mgr.h"
#include "wifi_sta.h"
#include "svc_port.h"
#include "svc_protocol.h"
#include "at_cmd.h"
#include "battery.h"
#include "stream_mode.h"
#include "stream_control.h"
#include "pipeline_internal.h" /* start_streaming/stop_streaming/supervisor_task_fn/etc. */

static const char *TAG = "main";

/* ====================================================================
 * WiFi Boot Retry — connect or deep sleep
 * ====================================================================
 *
 * On boot, tries to connect to the AP WIFI_BOOT_RETRY_ATTEMPTS times.
 * If all attempts fail, enters deep sleep for WIFI_BOOT_SLEEP_MINUTES,
 * then reboots (deep sleep wake = reboot on ESP8266) and retries.
 *
 * Only applies to UDP (transport=0) and TCP (transport=1). RawTX (2)
 * doesn't use AP association — skipped.
 *
 * This prevents the ESP from hanging in a zombie state when the AP is
 * unreachable (power outage, AP reboot, out of range). Without this, the
 * WiFi reconnect task retries forever with exponential backoff, but the
 * ESP never reboots — it just sits there, invisible to the server. */
#if WIFI_BOOT_RETRY_ENABLED
void wifi_boot_retry_or_sleep(uint8_t transport_mode)
{
    /* RawTX doesn't use AP association — nothing to retry. */
    if (transport_mode != TRANSPORT_MODE_UDP &&
        transport_mode != TRANSPORT_MODE_TCP)
    {
        return;
    }

    ESP_LOGI(TAG, "WiFi boot retry: %d attempts, %ds timeout each, %d min sleep on failure",
             WIFI_BOOT_RETRY_ATTEMPTS,
             (int)(WIFI_CONNECT_TIMEOUT_MS / 1000),
             WIFI_BOOT_SLEEP_MINUTES);

    for (int attempt = 1; attempt <= WIFI_BOOT_RETRY_ATTEMPTS; attempt++)
    {
        if (wifi_sta_is_connected())
        {
            ESP_LOGI(TAG, "WiFi connected on attempt %d/%d",
                     attempt, WIFI_BOOT_RETRY_ATTEMPTS);
            return;
        }

        ESP_LOGW(TAG, "WiFi connect attempt %d/%d (waiting %d ms)...",
                 attempt, WIFI_BOOT_RETRY_ATTEMPTS,
                 (int)WIFI_CONNECT_TIMEOUT_MS);

        esp_err_t err = wifi_sta_wait_connected(WIFI_CONNECT_TIMEOUT_MS);
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "WiFi connected on attempt %d/%d",
                     attempt, WIFI_BOOT_RETRY_ATTEMPTS);
            return;
        }

        ESP_LOGW(TAG, "WiFi connect attempt %d/%d failed (timeout)",
                 attempt, WIFI_BOOT_RETRY_ATTEMPTS);
    }

    /* All attempts failed — enter sleep. */
    ESP_LOGE(TAG, "WiFi connect failed after %d attempts — entering %s for %d minutes",
             WIFI_BOOT_RETRY_ATTEMPTS,
             (WIFI_BOOT_SLEEP_MODE == 0) ? "deep sleep" : "soft sleep",
             WIFI_BOOT_SLEEP_MINUTES);

#if WIFI_BOOT_SLEEP_MODE == 0
    /* Deep sleep: requires GPIO16 (XPD_DCDC) connected to RST.
     * On wake, ESP reboots → app_main runs → WiFi retry loop repeats. */
    ESP_LOGW(TAG, "Entering deep sleep for %d minutes...", WIFI_BOOT_SLEEP_MINUTES);
    vTaskDelay(pdMS_TO_TICKS(500));  /* let log flush */
    esp_deep_sleep_set_rf_option(2); /* RF cal on wake, don't write NVS */
    esp_deep_sleep((uint64_t)WIFI_BOOT_SLEEP_MINUTES * 60ULL * 1000000ULL);
    /* Should never reach here. */
    ESP_LOGE(TAG, "esp_deep_sleep returned unexpectedly! Restarting...");
    esp_restart();
#else
    /* Soft sleep: works without GPIO16-RST wiring, but uses more power.
     * After the delay, retry WiFi in-place (no reboot). */
    ESP_LOGW(TAG, "Soft sleep (vTaskDelay) for %d minutes, then retry...",
             WIFI_BOOT_SLEEP_MINUTES);
    vTaskDelay(pdMS_TO_TICKS(WIFI_BOOT_SLEEP_MINUTES * 60 * 1000));

    /* After soft sleep, force a WiFi reconnection cycle and reboot
     * to restart cleanly (the WiFi state may be corrupted by now). */
    ESP_LOGW(TAG, "Soft sleep complete — rebooting for clean retry");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
#endif
}
#endif /* WIFI_BOOT_RETRY_ENABLED */

/* ====================================================================
 * app_main
 * ==================================================================== */

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP8266 WiFi Microphone " FIRMWARE_VERSION " (DVI4/RFC 3551) ===");
    ESP_LOGI(TAG, "AT command interface on UART0 (%d 8N1)", UART_BAUD_RATE);

    /* 0. NVS (must be first - battery and config depend on it). */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS partition needs erase - erasing...");
        nvs_flash_erase();
        err = nvs_flash_init();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "NVS init failed after erase: %s", esp_err_to_name(err));
            return;
        }
    }
    if (err != ESP_OK)
    {
        /* FIX (LOW #18): see FIXES.md — reboot instead of returning so the
         * device retries the boot (NVS may recover on a power cycle). Without
         * this, app_main would return and leave the device idle with no WiFi,
         * no AT, no supervisor — silent bricked state. Consistent with the
         * config_mgr_init / stream_control_init / mutex-create failure paths
         * below. */
        ESP_LOGE(TAG, "NVS init failed: %s - rebooting in 5s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
        return; /* unreachable, but keeps compiler happy */
    }

    /* 1. Battery monitoring (optional, ported from ESP8285-WEBSERVER).
     * Initializes ADC and starts a background task that checks V_batt
     * every BATT_CHECK_MIN minutes. If V_batt < BATT_CRITICAL_MV, device
     * enters deep sleep to preserve battery.
     * Fully excluded from build when BATTERY_ENABLED=0 in menuconfig. */
#if BATTERY_ENABLED
    err = battery_init();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Battery init failed (continuing anyway)");
    }
    if (xTaskCreate(battery_monitor_task, "bat", BATT_TASK_STACK,
                    NULL, BATT_TASK_PRIO, NULL) != pdPASS)
    {
        ESP_LOGW(TAG, "Failed to create battery_monitor_task (continuing)");
    }
#else
    ESP_LOGI(TAG, "Battery monitoring disabled (menuconfig)");
#endif

    /* 2. Config manager (requires NVS). */
    err = config_mgr_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Config manager init failed - rebooting in 5s");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }

    /* 3. Stream control EventGroup. */
    if (stream_control_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create stream event group - rebooting in 5s");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }

    /* FIX (MEDIUM #37): see FIXES.md */
    s_task_handles_mutex = xSemaphoreCreateMutex();
    if (!s_task_handles_mutex)
    {
        ESP_LOGE(TAG, "Failed to create task-handles mutex - rebooting in 5s");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }

    /* 4. AT command interface — started EARLY (before WiFi init) so the
     * user can issue commands during WiFi boot retry.
     * FIX (AT-DURING-WIFI-RETRY): see FIXES.md — previously AT was started
     * AFTER the blocking wifi_boot_retry_or_sleep(), stranding the user
     * during AP-unreachable windows. */
#if AT_CMD_ENABLED
    at_cmd_init();
#endif

    /* 5. WiFi init - mode-specific (UDP: connect to AP; RAWTX: radio+channel). */
    device_config_t cfg;
    config_get_copy(&cfg);

    /* Boot log: bitrate computed from cfg.codec_mode (ADPCM: 4 bits/sample,
     * PCM: cfg.bits_per_sample bits/sample). Moved here from the top of
     * app_main() because cfg isn't loaded until config_get_copy(). */
    uint32_t boot_bitrate;
    if (cfg.codec_mode == CODEC_MODE_ADPCM)
        boot_bitrate = cfg.sample_rate * 4 * channel_format_to_count(cfg.channel_format);
    else
        boot_bitrate = cfg.sample_rate * cfg.bits_per_sample * channel_format_to_count(cfg.channel_format);
    ESP_LOGI(TAG, "Audio: %u Hz, %u ch, %u bps, codec=%u, bits=%u",
             (unsigned)cfg.sample_rate, (unsigned)channel_format_to_count(cfg.channel_format),
             (unsigned)boot_bitrate, (unsigned)cfg.codec_mode, (unsigned)cfg.bits_per_sample);

    /* Select stream mode (UDP or Raw TX) based on config.
     * Must be called before any ops->xxx() usage. */
    stream_mode_init(&cfg);

    err = stream_mode_ops()->wifi_init(&cfg);
    if (err == ESP_OK)
    {
        s_wifi_initialized = true;
        
#if WIFI_BOOT_RETRY_ENABLED
        /* FIX (wifi-boot-retry): see FIXES.md */
        wifi_boot_retry_or_sleep(cfg.transport_mode);
#else
        /* Block until WiFi is ready (UDP: wait for AP association;
         * RAWTX: no-op). Ignoring errors - start_streaming will retry. */
        stream_mode_ops()->wifi_wait_ready(&cfg);
#endif
    }
    else
    {
        ESP_LOGW(TAG, "WiFi init failed, will retry at stream start");
    }

    /* 6. Service port (UDP/TCP: starts EASSP listener; RAWTX: no-op). */
    err = stream_mode_ops()->svc_port_init(s_stream_evt_grp, cfg.svc_port);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Service port init failed: %s", esp_err_to_name(err));
    }
    else if (stream_mode_ops()->uses_svc_port)
    {
        ESP_LOGI(TAG, "Service port started on UDP:%u - waiting for server...",
                 (unsigned)cfg.svc_port);
    }

    /* Initialize s_channels from NVS config.
     * For UDP mode, svc_port must be initialized first - the ops table
     * handles this correctly (udp_set_channels calls svc_port_set_channels,
     * rawtx_set_channels is a no-op). */
    s_channels = channel_format_to_count(cfg.channel_format);
    stream_mode_ops()->set_channels(s_channels);
    ESP_LOGI(TAG, "Config from NVS: ch=%u (fmt=%u)", s_channels, cfg.channel_format);

    /* 7.5. Supervisor task — software watchdog. Started before the main
     * loop so it's always running. Catches heap leaks and stack overflows. */
#ifdef CONFIG_STREAMER_SUPERVISOR_ENABLED
    {
        if (xTaskCreate(supervisor_task_fn, "supervisor",
                        TASK_STACK_SUPERVISOR, NULL,
                        TASK_PRIO_SUPERVISOR, NULL) != pdPASS)
        {
            ESP_LOGE(TAG, "Failed to create supervisor task — continuing (no soft-WDT)");
        }
    }
#else
    ESP_LOGI(TAG, "Supervisor task disabled (menuconfig)");
#endif

    /* 8. Auto-start streaming if the mode requires it
     *    (Raw TX: yes - no server to send CONFIGURE; UDP: no - waits
     *    for server to discover and configure us). */
    if (stream_mode_ops()->auto_start)
    {
        ESP_LOGI(TAG, "Auto-starting stream (%s mode)...",
                 stream_mode_ops()->name);
        xEventGroupSetBits(s_stream_evt_grp, STREAM_EVT_START_REQ);
    }

    /* 9. Main loop - wait for START_REQ / STOP_REQ from svc_port. */
    ESP_LOGI(TAG, "Main loop running");
    /* FIX (B10): see FIXES.md */
    int auto_start_attempts = 0;
    const int AUTO_START_MAX_ATTEMPTS = 3;
    /* FIX (LOW): see FIXES.md — uint32_t ms (not TickType_t) for explicit units. */
    const uint32_t auto_start_backoff_ms[3] = {0, 1000, 5000};

    while (1)
    {
        EventBits_t bits = xEventGroupWaitBits(s_stream_evt_grp,
                                               STREAM_EVT_START_REQ | STREAM_EVT_STOP_REQ,
                                               pdTRUE, pdFALSE,
                                               portMAX_DELAY);

        if (bits & STREAM_EVT_STOP_REQ)
        {
            stop_streaming();
            /* Critical: yield between stop and start. When HOTRESTART sets
             * both STOP_REQ + START_REQ, this delay gives exited pipeline
             * tasks time to fully die (vTaskDelete is async), WiFi/lwIP to
             * settle, and heap to stabilize. 200ms chosen empirically. */
            vTaskDelay(pdMS_TO_TICKS(200));
            /* Stop cancels any pending auto-start retry sequence. */
            auto_start_attempts = 0;
        }
        if (bits & STREAM_EVT_START_REQ)
        {
            esp_err_t start_err = start_streaming();
            /* FIX (B10): see FIXES.md */
            if (start_err != ESP_OK && stream_mode_ops()->auto_start &&
                auto_start_attempts < AUTO_START_MAX_ATTEMPTS)
            {
                auto_start_attempts++;
                uint32_t delay = auto_start_backoff_ms[auto_start_attempts - 1];
                ESP_LOGW(TAG, "Auto-start failed (attempt %d/%d) - retrying in %u ms",
                         auto_start_attempts, AUTO_START_MAX_ATTEMPTS,
                         (unsigned)delay);
                if (delay > 0)
                    vTaskDelay(pdMS_TO_TICKS(delay));
                xEventGroupSetBits(s_stream_evt_grp, STREAM_EVT_START_REQ);
            }
            else if (start_err != ESP_OK)
            {
                if (stream_mode_ops()->auto_start)
                    ESP_LOGE(TAG, "Auto-start exhausted %d attempts - manual AT+RST required",
                             AUTO_START_MAX_ATTEMPTS);
                else
                    ESP_LOGE(TAG, "Stream start failed: %s", esp_err_to_name(start_err));
                auto_start_attempts = 0;
            }
            else
            {
                /* Start succeeded — reset retry counter. */
                auto_start_attempts = 0;
            }
        }
    }
}
