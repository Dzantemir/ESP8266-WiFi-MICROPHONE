/*
 * AGC (Automatic Gain Control) — Q16.16 fixed-point gain control loop.
 *
 * Extracted from i2s_capture.c (R2-A). Called after sample extraction
 * (>>8 for 24-bit, sign-extend for 16-bit), before TPDF dither/passthrough.
 * Operates in-place on buf[].
 *
 * 9 presets (AGC_PRESETS[] in board_config.h), each with
 * attack/release/target/noise_gate/min_gain parameters. Parameters loaded
 * in agc_init() based on the selected mode. State is file-scope statics —
 * persists across frames within a stream, reset on every agc_init() call.
 */

/* ---- System / SDK includes ---- */
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* ---- Project includes ---- */
#include "board_config.h" /* AGC_PRESETS, agc_preset_t, AGC_MODE_* */
#include "agc.h"

/* ---- Named magic numbers ---- */
#define SAMPLE_MAX_16BIT 32767
#define SAMPLE_MIN_16BIT -32768
#define SAMPLE_MAX_24BIT 8388607
#define SAMPLE_MIN_24BIT -8388608
#define AGC_MAX_GAIN_Q16 (64 << 16)        /* 64.0 = +36 dB */
#define AGC_TARGET_16BIT_DEFAULT (1 << 12) /* -18 dBFS, 16-bit */
#define AGC_TARGET_24BIT_DEFAULT (1 << 20) /* -18 dBFS, 24-bit */

/* ---- AGC state (file-scope, reset by agc_init) ---- */
static uint8_t s_agc_mode = AGC_MODE_OFF; /* 0=OFF, 1..8 = preset index */
static int s_agc_bits = 24;               /* 16 or 24, set by agc_init */
static uint8_t s_agc_attack = 75;         /* from preset, % per frame */
static uint8_t s_agc_release = 20;        /* from preset, % per frame */
static int32_t s_agc_target = 0;          /* target level (raw, bit-depth dependent) */
static int32_t s_agc_noise_gate = 0;      /* noise gate threshold (raw) */
/* Per-preset min-gain floor in Q16.16 (GROK-5). Boost-only presets use
 * (1<<16)=1.0x; Limiter/Surveillance use (1<<10)=1/64x so they can attenuate. */
static int32_t s_agc_min_gain_q16 = (1 << 16);
/* Envelope/gain state — persists across frames within a stream, reset on init. */
static int32_t s_agc_envelope = 0;         /* peak envelope follower */
static int32_t s_agc_gain_q16 = (1 << 16); /* Q16.16 gain (65536 = 1.0x) */

void agc_init(uint8_t mode, int bits_per_sample)
{
    /* Defensive input clamping (callers in i2s_capture.c already validate). */
    if (mode >= AGC_MODE_COUNT)
        mode = AGC_MODE_VOICE_BALANCED;
    if (bits_per_sample != 16 && bits_per_sample != 24)
        bits_per_sample = 24;

    s_agc_mode = mode;
    s_agc_bits = bits_per_sample;

    /* Load AGC preset parameters (attack, release, target, noise_gate). */
    const agc_preset_t *p = &AGC_PRESETS[mode];
    s_agc_attack = p->attack;
    s_agc_release = p->release;
    /* Per-preset min-gain floor (GROK-5). */
    s_agc_min_gain_q16 = p->min_gain_q16;
    if (s_agc_min_gain_q16 < 1)
        s_agc_min_gain_q16 = 1; /* defensive: never multiply by 0 */

    /* Compute target and noise_gate for current bit depth.
     * Presets use 0 for target -> default (-18 dBFS).
     * Noise gate = target / 64 (if preset has 0). */
    if (s_agc_bits == 24)
    {
        s_agc_target = AGC_TARGET_24BIT_DEFAULT;
        s_agc_noise_gate = s_agc_target / 64;
    }
    else
    {
        s_agc_target = AGC_TARGET_16BIT_DEFAULT;
        s_agc_noise_gate = s_agc_target / 64;
    }

    /* Override with preset-specific target/noise_gate if nonzero.
     * target_q16 is in Q16.16: raw_target = (full_scale * target_q16) >> 16.
     * Works for both 16-bit (full_scale=32768) and 24-bit (8388608). */
    if (p->target_q16 != 0)
    {
        int32_t full_scale = (s_agc_bits == 24) ? 8388608 : 32768;
        s_agc_target = (int32_t)(((int64_t)full_scale * p->target_q16) >> 16);
    }
    if (p->noise_gate_q16 != 0)
    {
        int32_t full_scale = (s_agc_bits == 24) ? 8388608 : 32768;
        s_agc_noise_gate = (int32_t)(((int64_t)full_scale * p->noise_gate_q16) >> 16);
    }

    /* Reset envelope/gain state for fresh stream. */
    s_agc_envelope = 0;
    s_agc_gain_q16 = (1 << 16);
}

bool agc_is_active(void)
{
    return s_agc_mode != AGC_MODE_OFF;
}

void agc_process(int32_t *buf, int n)
{
    if (n <= 0)
        return;

    int32_t target_level = s_agc_target;
    int32_t noise_gate = s_agc_noise_gate;
    int32_t max_sample = (s_agc_bits == 24) ? SAMPLE_MAX_24BIT : SAMPLE_MAX_16BIT;
    int32_t min_sample = (s_agc_bits == 24) ? SAMPLE_MIN_24BIT : SAMPLE_MIN_16BIT;

    /* 1. Find frame peak (absolute max). */
    int64_t frame_peak = 0;
    for (int i = 0; i < n; i++)
    {
        int64_t v = buf[i];
        int64_t a = (v < 0) ? -v : v;
        if (a > frame_peak)
            frame_peak = a;
    }

    /* 2. Update envelope (asymmetric attack/release). */
    if (frame_peak > (int64_t)s_agc_envelope)
    {
        s_agc_envelope = (int32_t)(((100 - (int)s_agc_attack) * (int64_t)s_agc_envelope +
                                    (int)s_agc_attack * frame_peak) /
                                   100);
    }
    else
    {
        s_agc_envelope = (int32_t)(((100 - (int)s_agc_release) * (int64_t)s_agc_envelope +
                                    (int)s_agc_release * frame_peak) /
                                   100);
    }

    /* 3. Compute target gain. Use <= so envelope==0 is caught here (M2)
     * instead of falling through to division by zero. */
    int32_t target_gain_q16;
    if (s_agc_envelope <= noise_gate)
    {
        target_gain_q16 = s_agc_min_gain_q16;
    }
    else
    {
        int64_t num = (int64_t)target_level << 16;
        /* Clamp in 64-bit BEFORE the narrowing cast (M3). */
        int64_t raw = num / s_agc_envelope;
        if (raw > AGC_MAX_GAIN_Q16)
            raw = AGC_MAX_GAIN_Q16;
        /* Use per-preset min-gain floor (GROK-5) so Limiter/Surveillance can
         * actually attenuate loud signals instead of clamping to 1.0x and
         * then hard-clipping at the integer range. */
        if (raw < (int64_t)s_agc_min_gain_q16)
            raw = (int64_t)s_agc_min_gain_q16;
        target_gain_q16 = (int32_t)raw;
    }

    /* 4. Smooth gain (prevents zipper noise). */
    if (target_gain_q16 < s_agc_gain_q16)
    {
        s_agc_gain_q16 = ((100 - (int)s_agc_attack) * s_agc_gain_q16 +
                          (int)s_agc_attack * target_gain_q16) /
                         100;
    }
    else
    {
        s_agc_gain_q16 = ((100 - (int)s_agc_release) * s_agc_gain_q16 +
                          (int)s_agc_release * target_gain_q16) /
                         100;
    }

    /* 5. Apply gain per-sample + hard limiter. */
    for (int i = 0; i < n; i++)
    {
        int64_t v = (int64_t)buf[i] * (int64_t)s_agc_gain_q16;
        v >>= 16;
        if (v > max_sample)
            v = max_sample;
        if (v < min_sample)
            v = min_sample;
        buf[i] = (int32_t)v;
    }
}
