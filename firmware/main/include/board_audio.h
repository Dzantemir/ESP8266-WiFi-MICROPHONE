#ifndef BOARD_AUDIO_H
#define BOARD_AUDIO_H

/* Audio parameters, AGC presets, codec modes, I2S config.
 * Extracted from board_config.h (R3-C). */

#include "sdkconfig.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * I2S PINS (fixed in silicon, ESP8266 TRM):
 *   I2SI_DATA = GPIO12 (MTDI), I2SI_BCK = GPIO13 (MTCK), I2SI_WS = GPIO14 (MTMS)
 *
 * INMP441 wiring:
 *   SCK -> GPIO13, SD -> GPIO12 (+ 100k pulldown), WS -> GPIO14
 *   L/R -> GND (left channel), VDD -> 3.3V + 0.1uF, CHIPEN -> 3.3V
 */

/* ====================================================================
 *  Audio parameters — configured via Kconfig (menuconfig)
 * ==================================================================== */

#ifdef CONFIG_STREAMER_AUDIO_SAMPLE_RATE
#define AUDIO_SAMPLE_RATE   CONFIG_STREAMER_AUDIO_SAMPLE_RATE
#else
#define AUDIO_SAMPLE_RATE   16000
#endif

#define AUDIO_SAMPLE_RATE_DEFAULT  AUDIO_SAMPLE_RATE

/* Valid sample rates (for AT+RATE validation) */
#define SAMPLE_RATE_COUNT  7
extern const uint32_t VALID_SAMPLE_RATES[SAMPLE_RATE_COUNT];
/* Definition lives in config_mgr.c (single shared copy).
 * FIX (F-E LOW): was `static const` in header — every translation unit
 * that included board_audio.h got its own private copy. */

/* Sample rate enum for packet header (RTP payload format).
 *   0=8000, 1=11025, 2=16000, 3=22050, 4=32000, 5=44100, 6=48000 */
static inline uint8_t sample_rate_to_enum(uint32_t rate)
{
    for (int i = 0; i < SAMPLE_RATE_COUNT; i++) {
        if (VALID_SAMPLE_RATES[i] == rate) return (uint8_t)i;
    }
    return 2;  /* fallback: 16000 */
}

static inline bool sample_rate_is_valid(uint32_t rate)
{
    for (int i = 0; i < SAMPLE_RATE_COUNT; i++) {
        if (VALID_SAMPLE_RATES[i] == rate) return true;
    }
    return false;
}

/* I2S bits per sample — 16 or 24 */
#ifdef CONFIG_STREAMER_I2S_BITS_PER_SAMPLE
#define I2S_BITS_PER_SAMPLE CONFIG_STREAMER_I2S_BITS_PER_SAMPLE
#else
#define I2S_BITS_PER_SAMPLE 24
#endif

/* Digital gain multiplier (0-64), applied before TPDF dither in BOTH 16-bit
 * and 24-bit modes (with clamping). Compensates for INMP441's low sensitivity
 * (-26 dBFS @ 94 dB SPL). 0=bypass, 32=+30dB (default). Runtime: AT+GAIN.
 * Used only when AGC is off. */
#ifdef CONFIG_STREAMER_AUDIO_GAIN
#define AUDIO_GAIN_DEFAULT CONFIG_STREAMER_AUDIO_GAIN
#else
#define AUDIO_GAIN_DEFAULT 32
#endif

/* FIX (4-E LOW #15): see FIXES.md */
#ifdef CONFIG_STREAMER_AGC_MODE
#define AUDIO_AGC_DEFAULT CONFIG_STREAMER_AGC_MODE
#else
#define AUDIO_AGC_DEFAULT 3   /* Voice Balanced */
#endif

/* AGC (Automatic Gain Control) — 9 presets.
 *
 * Per-preset parameters:
 *   attack        — gain-drop speed (%/frame, 1-100)
 *   release       — gain-rise speed (%/frame, 1-100)
 *   target_q16    — target output level in Q16.16 (0 = bit-depth default)
 *   noise_gate_q16 — below this, gain=1x (bypass). 0 = use default.
 *
 * Q16.16 = level * 65536. Examples:
 *   -18 dBFS: q16 =  8248   (bit-depth default)
 *   -15 dBFS: q16 = 11658
 *   -12 dBFS: q16 = 16463
 *    -6 dBFS: q16 = 32846
 *   -48 dBFS: q16 =   261
 *   -42 dBFS: q16 =   521
 *   -36 dBFS: q16 =  1039
 *   -30 dBFS: q16 =  2073
 *   -60 dBFS: q16 =    66
 *
 * i2s_capture.c converts q16 → raw sample value:
 *   raw_target = (full_scale * target_q16) >> 16
 * (full_scale = 32768 for 16-bit, 8388608 for 24-bit). */
#define AGC_MODE_COUNT      9

#define AGC_MODE_OFF            0
#define AGC_MODE_STUDIO_SOFT    1
#define AGC_MODE_PODCAST        2
#define AGC_MODE_VOICE_BALANCED 3
#define AGC_MODE_VOICE_FAST     4
#define AGC_MODE_NOISY_ROOM     5
#define AGC_MODE_MUSIC          6
#define AGC_MODE_LIMITER        7
#define AGC_MODE_SURVEILLANCE   8

typedef struct {
    const char *name;
    uint8_t  attack;        /* %/frame, gain dropping (1-100) */
    uint8_t  release;       /* %/frame, gain rising (1-100) */
    int32_t  target_q16;    /* target level in Q16.16 (0 = bit-depth default) */
    int32_t  noise_gate_q16;/* noise gate in Q16.16 (0 = use default) */
    /* FIX (GROK-5): see FIXES.md — min-gain floor enables true limiter behavior. */
    int32_t  min_gain_q16;  /* floor; (1<<16) = 1.0x for backward compat */
} agc_preset_t;

/* FIX (GROK-5): see FIXES.md */
#define AGC_MIN_GAIN_BOOST_ONLY  (1 << 16)       /* 1.0x = 0 dB  */
/* Limiter/Surveillance can attenuate to 1/64x (-36 dB) so peaks above target
 * are reduced, not hard-clipped. */
#define AGC_MIN_GAIN_LIMITER     (1 << 10)       /* 1/64x = -36 dB */

extern const agc_preset_t AGC_PRESETS[AGC_MODE_COUNT];
/* Definition lives in config_mgr.c (single shared copy).
 * FIX (F-E LOW): was `static const` in header — every translation unit
 * that included board_audio.h got its own private copy. */

/* I2S RX input timing delays (ESP8266 TRM §10.2.1.6, I2S.timing register).
 * Each 0..3 (2 bits) = input sample delay in APB clock cycles (12.5 ns @ 80 MHz).
 * Compensates skew between INMP441 and BCK/WS on long wires or odd PCB layouts.
 * Default 0 (no delay) suits INMP441 on short wires. Runtime: AT+TIMING. */
#define I2S_TIMING_DELAY_MAX 3

#ifdef CONFIG_STREAMER_I2S_TIMING_SD_DELAY
#define I2S_TIMING_SD_DELAY_DEFAULT CONFIG_STREAMER_I2S_TIMING_SD_DELAY
#else
#define I2S_TIMING_SD_DELAY_DEFAULT 0
#endif

#ifdef CONFIG_STREAMER_I2S_TIMING_WS_DELAY
#define I2S_TIMING_WS_DELAY_DEFAULT CONFIG_STREAMER_I2S_TIMING_WS_DELAY
#else
#define I2S_TIMING_WS_DELAY_DEFAULT 0
#endif

#ifdef CONFIG_STREAMER_I2S_TIMING_BCK_DELAY
#define I2S_TIMING_BCK_DELAY_DEFAULT CONFIG_STREAMER_I2S_TIMING_BCK_DELAY
#else
#define I2S_TIMING_BCK_DELAY_DEFAULT 0
#endif

/* I2S communication format.
 *   0 = Philips I2S (msb_shift=1) — standard, INMP441
 *   1 = LSB / Left-justified (msb_shift=0) */
#ifdef CONFIG_STREAMER_I2S_COMM_FORMAT
#define I2S_COMM_FORMAT_CFG CONFIG_STREAMER_I2S_COMM_FORMAT
#else
#define I2S_COMM_FORMAT_CFG 0
#endif

/* Frame duration (ms) — COMPUTED at runtime from sample_rate, channels, bits.
 * See i2s_capture_compute_frame_ms() in i2s_capture.h. Guarantees:
 *   - samples_per_frame is even (ADPCM packs 2 samples/byte)
 *   - samples_per_frame/4 >= 32 (SDK DMA minimum)
 *   - UDP packet <= 1400 bytes (MTU safe, codec & bits aware)
 *   - If transport_mode == RawTX: air_time/sec <= 0.95 (throughput limit) */

/* Number of audio channels — configured via Kconfig. 1=mono, 2=stereo. */
#ifdef CONFIG_STREAMER_AUDIO_CHANNELS
#define AUDIO_CHANNELS      CONFIG_STREAMER_AUDIO_CHANNELS
#else
#define AUDIO_CHANNELS      1
#endif

/* I2S channel format (matches ESP8266 RTOS SDK enum):
 *   0=RIGHT_LEFT (stereo), 1=ALL_RIGHT, 2=ALL_LEFT,
 *   3=ONLY_RIGHT (mono right), 4=ONLY_LEFT (mono left) */
#ifdef CONFIG_STREAMER_I2S_CHANNEL_FORMAT
#define I2S_CHANNEL_FORMAT  CONFIG_STREAMER_I2S_CHANNEL_FORMAT
#else
#define I2S_CHANNEL_FORMAT  4   /* ONLY_LEFT */
#endif

/* ADPCM bitrate = 4 bits/sample * channels * sample_rate.
 * NOTE: ADPCM-only. For PCM, compute bitrate at runtime:
 *   bitrate = sample_rate * bits_per_sample * channels. */
#define AUDIO_BITRATE         (AUDIO_SAMPLE_RATE * 4 * AUDIO_CHANNELS)
#define CODEC_ID_ADPCM      5       /* DVI4 IMA ADPCM */
#define CODEC_ID_PCM        6       /* Raw 16/24-bit signed PCM */

/* Audio codec selection (0 = ADPCM, 1 = PCM).
 * ADPCM: 4 bits/sample, 32 kbps @ 16kHz, ~10% CPU.
 * PCM:   raw 16/24-bit signed, no compression, 1536 kbps @ 48kHz/16-bit/stereo.
 *        Frame_ms auto-reduced to fit UDP MTU at high rates.
 * Runtime: AT+CODEC=0|1 (saved, applies on next stream). */
#define CODEC_MODE_ADPCM    0
#define CODEC_MODE_PCM      1

#ifdef CONFIG_STREAMER_AUDIO_CODEC
#define AUDIO_CODEC_DEFAULT CONFIG_STREAMER_AUDIO_CODEC
#else
#define AUDIO_CODEC_DEFAULT CODEC_MODE_ADPCM
#endif

/* WiFi channel for Raw 802.11 TX mode (transport_mode == TRANSPORT_MODE_RAWTX).
 * Configurable via menuconfig (STREAMER_RAWTX_CHANNEL) or AT+WCH=1..14. */
#ifdef CONFIG_STREAMER_RAWTX_CHANNEL
#define RAWTX_CHANNEL_DEFAULT CONFIG_STREAMER_RAWTX_CHANNEL
#else
#define RAWTX_CHANNEL_DEFAULT 1
#endif

/* samples_per_frame and adpcm_frame_bytes are computed at runtime in
 * start_streaming() from frame_ms — no compile-time constants. */

#endif /* BOARD_AUDIO_H */
