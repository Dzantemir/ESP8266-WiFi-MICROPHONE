#ifndef AGC_H
#define AGC_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/*
 * AGC (Automatic Gain Control) — Q16.16 fixed-point gain control loop.
 *
 * Extracted from i2s_capture.c (R2-A). Operates in-place on int32_t sample
 * buffers, after I2S sample extraction (>>8 for 24-bit, sign-extend for
 * 16-bit) and before TPDF dither/passthrough.
 *
 * 9 presets (AGC_PRESETS[] in board_config.h), each with
 * attack/release/target/noise_gate/min_gain parameters. State is file-scope
 * statics in agc.c — initialized via agc_init(), updated by agc_process()
 * every frame. Reset on every agc_init() call (fresh stream).
 */

/* Initialize AGC with the given preset mode and bit depth.
 * mode: 0=OFF, 1..8 = presets (see AGC_PRESETS[] in board_config.h)
 * bits_per_sample: 16 or 24 (determines target/noise_gate defaults) */
void agc_init(uint8_t mode, int bits_per_sample);

/* Returns true if AGC is active (mode != OFF). */
bool agc_is_active(void);

/* Process a buffer of samples in-place. Applies gain based on envelope.
 * buf: sample buffer (int32_t, sign-extended)
 * n: number of samples */
void agc_process(int32_t *buf, int n);

#endif /* AGC_H */
