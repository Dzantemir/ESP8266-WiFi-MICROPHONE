#ifndef BOARD_BATTERY_H
#define BOARD_BATTERY_H

/* Battery monitoring constants.
 * Extracted from board_config.h (R3-C). */

#include "sdkconfig.h"
#include <stdint.h>
#include <stdbool.h>

/* ====================================================================
 *  Battery Monitoring (optional, ported from ESP8285-WEBSERVER project)
 * ====================================================================
 *
 * ESP8266 has a single ADC channel (TOUT pin, ADC_READ_TOUT_MODE) measuring
 * 0-1V (10-bit, 0-1023). A voltage divider on the battery side scales V_batt
 * down to fit this range.
 *
 * Example divider for Li-Ion (3.0-4.2V):
 *   R1=100k (to V_batt), R2=33k (to GND), TOUT at junction
 *   V_tout = V_batt * 33/133 = V_batt * 0.2481
 *   At V_batt=4.2V -> V_tout=1.042V (slightly clipped, OK)
 *   At V_batt=3.0V -> V_tout=0.744V
 *   RATIO = 1024 / 0.2481 ~ 5711 (calibration constant)
 *
 * Formula: V_batt_mV = (ADC_raw × RATIO) / 1024
 *
 * IMPORTANT: ESP8266 ADC is SHARED. If I2S INMP441 uses GPIO12 (data), the
 * ADC TOUT pin (separate from GPIOs) remains available. Verify that your
 * hardware routes V_batt divider to the TOUT pin, not a GPIO used by I2S.
 */

#ifdef CONFIG_STREAMER_BATTERY_ENABLED
#define BATTERY_ENABLED 1
#else
#define BATTERY_ENABLED 0
#endif

#ifdef CONFIG_STREAMER_BATT_CRITICAL_MV
#define BATT_CRITICAL_MV  CONFIG_STREAMER_BATT_CRITICAL_MV
#else
#define BATT_CRITICAL_MV  3700   /* below -> deep sleep */
#endif

#ifdef CONFIG_STREAMER_BATT_START_MV
#define BATT_START_MV     CONFIG_STREAMER_BATT_START_MV
#else
#define BATT_START_MV     3900   /* below on boot -> don't start */
#endif

#ifdef CONFIG_STREAMER_BATT_BAD_MV
#define BATT_BAD_MV       CONFIG_STREAMER_BATT_BAD_MV
#else
#define BATT_BAD_MV       2500   /* below -> reading is invalid */
#endif

#ifdef CONFIG_STREAMER_BATT_DIVIDER_RATIO
#define BATT_DIVIDER_RATIO CONFIG_STREAMER_BATT_DIVIDER_RATIO
#else
#define BATT_DIVIDER_RATIO 5711  /* R1=100k, R2=33k -> 5711 */
#endif

#ifdef CONFIG_STREAMER_BATT_ADC_SAMPLES
#define BATT_ADC_SAMPLES  CONFIG_STREAMER_BATT_ADC_SAMPLES
#else
#define BATT_ADC_SAMPLES  15
#endif

#ifdef CONFIG_STREAMER_BATT_ADC_DELAY_MS
#define BATT_ADC_DELAY_MS CONFIG_STREAMER_BATT_ADC_DELAY_MS
#else
#define BATT_ADC_DELAY_MS 50
#endif

#ifdef CONFIG_STREAMER_BATT_CHECK_MIN
#define BATT_CHECK_MIN    CONFIG_STREAMER_BATT_CHECK_MIN
#else
#define BATT_CHECK_MIN    1      /* check every N minutes during operation */
#endif

#ifdef CONFIG_STREAMER_BATT_SLEEP_MIN
#define BATT_SLEEP_MIN    CONFIG_STREAMER_BATT_SLEEP_MIN
#else
#define BATT_SLEEP_MIN    30     /* deep sleep duration on critical battery */
#endif

#ifdef CONFIG_STREAMER_BATT_TASK_STACK
#define BATT_TASK_STACK   CONFIG_STREAMER_BATT_TASK_STACK
#else
#define BATT_TASK_STACK   1024
#endif

#ifdef CONFIG_STREAMER_BATT_TASK_PRIO
#define BATT_TASK_PRIO    CONFIG_STREAMER_BATT_TASK_PRIO
#else
#define BATT_TASK_PRIO    3
#endif

#endif /* BOARD_BATTERY_H */
