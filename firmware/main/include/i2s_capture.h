#ifndef I2S_CAPTURE_H
#define I2S_CAPTURE_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * ESP8266 I2S capture driver wrapper.
 *
 * Wraps the patched ESP8266 RTOS SDK i2s.c driver (which enables BBPLL
 * audio clock and uses /48 for 24-bit frame divider). Handles:
 *   - Driver install / pin config / DMA buffer setup
 *   - 16-bit and 24-bit capture modes
 *   - Mono (left/right) and stereo channel selection
 *   - Sample extraction from DMA 32-bit words into int32_t buffer
 *
 * 16-bit mode (rx_fifo_mod=1, ONLY_LEFT/RIGHT):
 *   Each 32-bit DMA word contains TWO 16-bit samples packed as
 *   [Sample_N (hi16) | Sample_N+1 (lo16)]. Both halves carry real audio
 *   (in ONLY_LEFT mode both I2S slots capture the left channel). On
 *   little-endian Xtensa, reading as int16_t* gives pairs in swapped
 *   order; we swap them back before sign-extending into int32_t.
 *
 * 24-bit mode (rx_fifo_mod=3, ONLY_LEFT/RIGHT):
 *   Each 32-bit DMA word contains ONE 24-bit sample LEFT-justified in
 *   bits [31:8], with 8 bits of zero padding in [7:0]. i2s_capture_read()
 *   right-shifts by 8 to extract and sign-extend the 24-bit sample into
 *   int32_t, which is what tpdf_dither.c expects (it then does the
 *   24->16 reduction with its own >>8).
 */

/* Channel format constants (match config_mgr.h) */
#define I2S_CAP_CHFMT_LEFT    4
#define I2S_CAP_CHFMT_RIGHT   3
#define I2S_CAP_CHFMT_STEREO  0

/* Comm format constants */
#define I2S_CAP_CFMT_PHILIPS  0
#define I2S_CAP_CFMT_LSB      1

/* Compute optimal frame_ms from sample_rate and channels.
 * Returns a value that satisfies ALL constraints:
 *   - samples_per_frame = rate x frame_ms / 1000 is even (ADPCM requirement)
 *   - samples_per_frame / 4 >= 32 (SDK DMA buffer minimum)
 *   - UDP packet = 16 + channels x (4 + samples/2) <= 1400 bytes (MTU safe)
 *   - If transport_mode == RawTX: air_time/sec <= 0.95 (Raw TX throughput limit)
 * Tries preferred durations largest-first: 60, 50, 40, 30, 25, 20, 15, 10, 5 ms.
 * Picks the largest that fits MTU + DMA minimum + ADPCM even-sample constraint.
 *
 * RawTX transport: enforces an additional constraint for Raw 802.11 TX.
 *   esp_wifi_80211_tx is limited to ~1 Mbps effective throughput (ppTxPkt
 *   driver overhead + CSMA/CA). PCM at high bitrates exceeds this, causing
 *   50%+ packet drops. The constraint is:
 *     (1000/frame_ms) * (packet_size_bits/1e6 + 0.003) <= 0.95
 *   where packet_size_bits = audio_bitrate * frame_ms / 1000. This
 *   simplifies (multiply both sides by ms, divide by 1000) to:
 *     audio_bitrate/1000 + 3000/frame_ms <= 950
 *   with audio_bitrate in bps and frame_ms in ms. If audio_bitrate is so
 *   high that no preferred frame_ms satisfies this (e.g. 48 kHz/24-bit/
 *   stereo PCM at 1536 kbps), the function returns the largest frame_ms
 *   that fits MTU (stream will have drops). ADPCM is unaffected (bitrate
 *   <= 384 kbps, always fits). */
uint32_t i2s_capture_compute_frame_ms(uint32_t sample_rate, int channels,
                                       int codec_mode, int bits_per_sample);

/* RawTX-aware variant of i2s_capture_compute_frame_ms() (2-E/#14).
 * Enforces the additional air-time constraint documented above:
 *   audio_bitrate/1000 + 3000/frame_ms <= 950
 * (kbps + per-packet-overhead-in-ms <= 950), derived from the per-packet
 * on-air time = packet_size_bits/1e6 + 0.003 s. Falls back to the base
 * function's choice (with a warning log) when no frame_ms can satisfy
 * the constraint. Also enforces the MTU/DMA-minimum/ADPCM-even-sample
 * guards (same as the base function) so the _rawtx choice never
 * violates them. Call this instead of i2s_capture_compute_frame_ms()
 * when transport_mode == TRANSPORT_MODE_RAWTX. */
uint32_t i2s_capture_compute_frame_ms_rawtx(uint32_t sample_rate, int channels,
                                             int codec_mode, int bits_per_sample);

/* Initialize I2S capture with the given parameters.
 * DMA buffer dimensions are derived from samples_per_frame:
 *   dma_buf_len  = samples_per_frame / 4  (4 events per PCM frame)
 *   dma_buf_count = ~DMA_POOL_TARGET_MS of total buffering + safety margin
 *   event_queue  = dma_buf_count x 2
 * Timeout is computed internally from frame_ms (no caller input).
 *
 * gain: fixed gain multiplier for 24-bit AND 16-bit mode (0-64). 0 = bypass,
 *       32 = +30 dB. Applied after sample extraction, before TPDF dither.
 *       Used only when agc_mode=0 (OFF).
 * agc_mode: 0=OFF (use fixed gain), 1..8 = presets.
 *           See AGC_PRESETS[] table in board_config.h for per-preset
 *           attack/release/target/noise_gate/min_gain values.
 * timing_sd_delay / timing_ws_delay / timing_bck_delay:
 *           RX input sample delays (0..3) — ESP8266 TRM §10.2.1.6,
 *           I2S.timing register. Задержка в тактах APB (12.5 нс @ 80 МГц).
 *           Применяются после i2s_driver_install внутри i2s_capture_init.
 *           0 = без задержки. */
esp_err_t i2s_capture_init(uint32_t sample_rate,
                            int bits_per_sample,
                            int comm_format,
                            int channel_format,
                            int samples_per_frame,
                            uint32_t frame_ms,
                            uint8_t gain,
                            uint8_t agc_mode,
                            uint8_t timing_sd_delay,
                            uint8_t timing_ws_delay,
                            uint8_t timing_bck_delay);

/* Deinitialize I2S capture and free resources. */
esp_err_t i2s_capture_deinit(void);

/*
 * Read samples from I2S DMA into buf.
 *
 * For 16-bit mono: buf receives buf_len samples (int32_t each, sign-extended).
 * For 24-bit mono: buf receives buf_len samples (int32_t each). Each sample
 *                  is a sign-extended 24-bit value (top 8 bits all-0 or all-1)
 *                  produced by the explicit >>=8 in i2s_capture_read() that
 *                  discards the low-8 padding of the left-justified DMA word
 *                  (rx_fifo_mod=3). The dither stage (tpdf_dither.c) consumes
 *                  this format and does the 24->16 reduction with its own >>8.
 * For stereo: buf receives buf_len * channels samples (interleaved L,R,L,R).
 *
 * Timeout is computed internally from frame_ms - no caller input.
 * Returns ESP_OK with *samples_read set to the actual number of samples read.
 * Returns ESP_ERR_TIMEOUT if DMA didn't produce data in time (normal on stop).
 */
esp_err_t i2s_capture_read(int32_t *buf, int buf_len, int *samples_read);

/* Query functions */
int  i2s_capture_get_bits(void);

#endif /* I2S_CAPTURE_H */
