/*
 * ESP8266 RTOS SDK I2S capture driver wrapper.
 *
 * Wraps the patched i2s.c driver (BBPLL audio clock + /48 divider for
 * 24-bit). Uses i2s_read() with timeout — proven to work (same approach
 * as esp-i2s-debugger).
 *
 * I2S_EVENT_RX_DONE is NOT used for reads: on ESP8266 RTOS SDK the event
 * fires before data lands in the rx queue, so i2s_read after RX_DONE
 * returns 0 bytes. i2s_read() blocks the task and wakes only when data
 * is guaranteed ready — the standard reliable approach.
 *
 * 16-bit (rx_fifo_mod=1): 32-bit word = [S_N hi16 | S_N+1 lo16],
 *   pairs swapped on little-endian -> swap back, sign-extend to int32.
 * 24-bit (rx_fifo_mod=3): 32-bit word = [S24 in bits 31:8 | 8 padding 7:0].
 *   LEFT-justified. Arithmetic >>8 extracts the 24-bit sample and sign-
 *   extends it into int32_t. Verified by AT+DUMP hex output (low 8 bits
 *   always 0x00 = padding).
 */

/* ---- System / SDK includes ---- */
#include <string.h>   /* memcpy for 16-bit swap/sign-extend (AUDIT-C3) */
/* PRIu32 for portable uint32_t printf format (2-E LOW). */
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "driver/i2s.h"
#include "esp_log.h"
#include "esp8266/i2s_struct.h"

/* ---- Project includes ---- */
#include "board_config.h"
#include "i2s_capture.h"
#include "config_mgr.h"  /* channel_format_to_count() */
#include "agc.h"

/* ---- Named magic numbers ---- */
#define I2S_MTU_BYTES               1400
#define DMA_MIN_SAMPLES             128
#define DMA_POOL_TARGET_MS          80      /* current target (was 256ms) */
#define MAX_FRAME_MS                60
#define MIN_FRAME_MS                5
#define SAMPLE_MAX_16BIT            32767
#define SAMPLE_MIN_16BIT           -32768
#define SAMPLE_MAX_24BIT            8388607
#define SAMPLE_MIN_24BIT           -8388608

static const char *TAG = "i2s_cap";
#define I2S_PORT I2S_NUM_0

static bool s_initialized = false;
static int s_bits = 24;
static int s_channels = 1;
static uint32_t s_sample_rate = 16000;
static uint32_t s_frame_ms = 20;
static uint8_t s_gain = 32;        /* 0=bypass, 1..64 = multiplier */
static uint8_t s_timing_sd_delay = 0;
static uint8_t s_timing_ws_delay = 0;
static uint8_t s_timing_bck_delay = 0;

/* Preferred frame durations (ms), largest first. Shared by compute_frame_ms
 * and compute_frame_ms_rawtx — single source of truth. */
static const uint32_t FRAME_MS_PREFERRED[] = {MAX_FRAME_MS, 50, 40, 30, 25, 20, 15, 10, MIN_FRAME_MS};
#define FRAME_MS_PREFERRED_COUNT (sizeof(FRAME_MS_PREFERRED) / sizeof(FRAME_MS_PREFERRED[0]))

/* ---- Frame duration computation ----
 *
 * Pick the largest frame_ms (from the preferred set) such that the UDP
 * packet fits I2S_MTU_BYTES and the DMA minimum (DMA_MIN_SAMPLES /
 * quarter-frame) is satisfied. Larger frames = less overhead, less CPU,
 * better ADPCM quality.
 *
 * Two hard limits on samples_per_frame:
 *   1. MTU:      pkt = 16 + samples * ch * bytes_per_sample  <= I2S_MTU_BYTES
 *                -> samples <= (I2S_MTU_BYTES - 16) / (ch * bytes_per_sample)
 *   2. DMA min:  dma_buf_len = samples / 4 >= 32  ->  samples >= DMA_MIN_SAMPLES
 * MTU is the upper bound; DMA the lower. We pick the largest preferred ms
 * whose sample count is <= MTU upper bound (and >= DMA_MIN_SAMPLES).
 *
 * ADPCM note: packs 2 samples per byte, so samples must be even. We round
 * the MTU upper bound down to even before checking.
 */
uint32_t i2s_capture_compute_frame_ms(uint32_t sample_rate, int channels,
                                      int codec_mode, int bits_per_sample)
{

    if (sample_rate == 0 || (channels != 1 && channels != 2))
        return 20;

    /* bytes per sample in the packet payload */
    int bps;
    if (codec_mode == 1 /* CODEC_MODE_PCM */)
    {
        bps = (bits_per_sample == 24) ? 3 : 2;
    }
    else
    {
        /* ADPCM: 4 bits/sample = 0.5 bytes/sample, + 4-byte DVI4 header per channel */
        bps = 0; /* handled separately below */
    }

    /* Max samples that fit in MTU (I2S_MTU_BYTES total, 16-byte header). */
    int max_samples;
    if (codec_mode == 1 /* CODEC_MODE_PCM */)
    {
        max_samples = (I2S_MTU_BYTES - 16) / (channels * bps);
    }
    else
    {
        /* ADPCM: 16 + ch*(4 + samples/2) <= I2S_MTU_BYTES
         *      -> samples <= (I2S_MTU_BYTES - 16 - ch*4) * 2 / ch */
        max_samples = ((I2S_MTU_BYTES - 16) - channels * 4) * 2 / channels;
        if (max_samples & 1)
            max_samples--; /* ADPCM needs even */
        /* Server buffer limit: WAVE_BUF_SZ = 1920 bytes.
         * ADPCM decode produces 4 bytes per input byte (2 samples x 2 bytes).
         * Server clips adpcmLen to WAVE_BUF_SZ/4 = 480 bytes per channel.
         * -> samples <= 480 * 2 = 960 (adpcmLen = samples/2 <= 480). Without
         * this limit, large frames (e.g. 50ms@48kHz -> adpcmLen=1200) get
         * truncated by the server -> 60% data loss -> distortion. */
        if (max_samples > 960)
            max_samples = 960;
    }
    if (max_samples < DMA_MIN_SAMPLES)
        max_samples = DMA_MIN_SAMPLES; /* DMA minimum */
    /* DMA_MIN_SAMPLES floor can violate MTU for high-bitrate configs
     * (2-E MEDIUM #34). Save the MTU-derived max before the DMA-min clamp,
     * then warn (don't re-clamp down — DMA minimum is a harder requirement
     * than MTU; IP fragmentation is recoverable, DMA underrun is not). */
    {
        int mtu_max_samples = (codec_mode == 1 /* PCM */)
            ? (I2S_MTU_BYTES - 16) / (channels * bps)
            : (((I2S_MTU_BYTES - 16) - channels * 4) * 2 / channels);
        if (mtu_max_samples > 960 && codec_mode != 1)
            mtu_max_samples = 960;
        if (mtu_max_samples < DMA_MIN_SAMPLES && max_samples > mtu_max_samples)
        {
            ESP_LOGW(TAG, "max_samples=%u exceeds MTU-derived max=%u "
                     "(DMA min %d enforced; expect IP fragmentation)",
                     (unsigned)max_samples, (unsigned)mtu_max_samples, DMA_MIN_SAMPLES);
        }
    }

    /* Use file-scope FRAME_MS_PREFERRED[] (single source of truth). */
    for (int i = 0; i < (int)FRAME_MS_PREFERRED_COUNT; i++)
    {
        uint32_t ms = FRAME_MS_PREFERRED[i];
        int samples = (int)(sample_rate * ms / 1000);
        if (samples < DMA_MIN_SAMPLES)
            continue; /* DMA minimum */
        if (samples > max_samples)
            continue; /* MTU limit */
        /* ADPCM even-sample constraint (PCM: alignment done in main.c). */
        if (codec_mode != 1 && (samples & 1))
            continue;
        return ms;
    }

    /* Fallback: scan from max_ms down to 1ms (only reached when MIN_FRAME_MS
     * doesn't fit, e.g. 48kHz stereo PCM-24bit where max_ms=4). Capped at
     * MAX_FRAME_MS. */
    int max_ms = max_samples * 1000 / (int)sample_rate;
    if (max_ms > MAX_FRAME_MS)
        max_ms = MAX_FRAME_MS;
    for (int ms = max_ms; ms >= 1; ms--)
    {
        int samples = (int)(sample_rate * ms / 1000);
        if (samples < DMA_MIN_SAMPLES)
            continue;
        if (samples > max_samples)
            continue;
        /* Same even-sample constraint as the preferred loop (ADPCM only). */
        if (codec_mode != 1 && (samples & 1))
            continue;
        return (uint32_t)ms;
    }
    return 20;
}

/* RawTX air-time constrained variant (2-E HIGH #14). The base function
 * doesn't know transport mode; main.c calls this when transport_mode ==
 * TRANSPORT_MODE_RAWTX. Enforces the 802.11 air-time budget:
 *   audio_bitrate/1000 + 3000/frame_ms <= 950
 * derived from per-packet on-air time = packet_size_bits/1e6 + 0.003 s
 * (4-C HIGH #3 corrected the dimensional bug in the prior formula). Also
 * enforces MTU/DMA-min/even-sample so the _rawtx choice never violates
 * them (4-C MEDIUM #11). */
uint32_t i2s_capture_compute_frame_ms_rawtx(uint32_t sample_rate, int channels,
                                             int codec_mode, int bits_per_sample)
{
    /* Audio bitrate in bps. ADPCM = 4 bits/sample, PCM = bits_per_sample. */
    uint32_t bits_per_sample_eff = (codec_mode == CODEC_MODE_PCM)
        ? (uint32_t)bits_per_sample : 4;
    uint32_t audio_bitrate = sample_rate * bits_per_sample_eff * (uint32_t)channels;

    /* Use file-scope FRAME_MS_PREFERRED[] (single source of truth). */
    for (int i = 0; i < (int)FRAME_MS_PREFERRED_COUNT; i++)
    {
        uint32_t ms = FRAME_MS_PREFERRED[i];

        /* Air-time constraint: audio_bitrate (bps) -> kbps + per-packet
         * CSMA/CA + driver overhead in ms. */
        uint32_t kbps = audio_bitrate / 1000U;
        uint32_t overhead_ms = 3000U / ms;
        if (kbps + overhead_ms > 950U)
        {
            continue;  /* exceeds 95% air-time budget */
        }

        /* Mirror base function's MTU/DMA-min/even-sample guards. */
        uint32_t samples = sample_rate * ms / 1000U;
        /* Server buffer limit: WAVE_BUF_SZ = 1920 bytes.
         * ADPCM: server clips to 480 bytes/channel = 960 samples max. */
        if (codec_mode != CODEC_MODE_PCM && samples > 960U)
            continue;
        if (samples < (uint32_t)DMA_MIN_SAMPLES)
        {
            continue;
        }
        if (codec_mode != 1 /* not PCM -> ADPCM */ && (samples & 1U))
        {
            continue;
        }
        uint32_t pkt_size = PKT_HDR_SIZE;  /* 16 bytes */
        if (codec_mode != CODEC_MODE_PCM)
        {
            pkt_size += (uint32_t)channels * (DVI4_HEADER_SIZE + samples / 2U);
        }
        else
        {
            uint32_t bytes_per_sample_pkt = (bits_per_sample == 24) ? 3U : 2U;
            pkt_size += samples * (uint32_t)channels * bytes_per_sample_pkt;
        }
        if (pkt_size > (uint32_t)I2S_MTU_BYTES)
        {
            continue;
        }

        return ms;
    }

    /* No preferred frame_ms satisfies the air-time constraint. This happens
     * when audio_bitrate > ~950 kbps (e.g. 48 kHz stereo 24-bit PCM =
     * 2.304 Mbps). The air-time budget is a SOFT constraint — exceeding it
     * causes packet drops (recoverable). The DMA minimum (DMA_MIN_SAMPLES)
     * enforced by the base function is a HARD constraint — violating it makes
     * i2s_capture_init() reject the config with ESP_ERR_INVALID_ARG and the
     * stream never starts. The previous code returned the smallest preferred
     * (5ms), which at e.g. 8 kHz mono = 40 samples < DMA_MIN_SAMPLES (128)
     * and would fail init outright. Fall back to the base function instead:
     * its choice always satisfies DMA/MTU/even-sample; air-time overruns
     * surface as drops, not a refused start. (FR-PIPE / Roma #2) */
    ESP_LOGW(TAG, "RawTX: audio_bitrate=%" PRIu32 " bps exceeds air-time budget, "
                  "falling back to DMA/MTU-safe frame_ms (expect packet drops)",
             audio_bitrate);
    return i2s_capture_compute_frame_ms(sample_rate, channels, codec_mode, bits_per_sample);
}

/* Forward decl — defined after i2s_capture_init (called from it). */
static void apply_timing(int sd_delay, int ws_delay, int bck_delay);

esp_err_t i2s_capture_init(uint32_t sample_rate, int bits, int comm_format,
                           int channel_format, int samples_per_frame,
                           uint32_t frame_ms, uint8_t gain, uint8_t agc_mode,
                           uint8_t timing_sd_delay, uint8_t timing_ws_delay,
                           uint8_t timing_bck_delay)
{
    if (s_initialized)
        return ESP_ERR_INVALID_STATE;
    if (sample_rate == 0 || (bits != 16 && bits != 24) ||
        (channel_format != I2S_CAP_CHFMT_LEFT &&
         channel_format != I2S_CAP_CHFMT_RIGHT &&
         channel_format != I2S_CAP_CHFMT_STEREO) ||
        /* Validate comm_format (L7): reject anything != PHILIPS/LSB. */
        (comm_format != I2S_CAP_CFMT_PHILIPS &&
         comm_format != I2S_CAP_CFMT_LSB) ||
        /* Enforce documented DMA minimum (L4): samples_per_frame/4 >= 32. */
        samples_per_frame < DMA_MIN_SAMPLES || frame_ms == 0)
        return ESP_ERR_INVALID_ARG;
    if (gain > 64)
    {
        /* Explicit clamp + log instead of silent reset to default (2-E LOW). */
        ESP_LOGW(TAG, "gain %u > 64, clamping to 64", (unsigned)gain);
        gain = 64;
    }
    if (agc_mode >= AGC_MODE_COUNT)
        agc_mode = AGC_MODE_VOICE_BALANCED;
    if (timing_sd_delay > I2S_TIMING_DELAY_MAX)
        timing_sd_delay = 0;
    if (timing_ws_delay > I2S_TIMING_DELAY_MAX)
        timing_ws_delay = 0;
    if (timing_bck_delay > I2S_TIMING_DELAY_MAX)
        timing_bck_delay = 0;

    s_bits = bits;
    s_channels = channel_format_to_count((uint8_t)channel_format);
    s_sample_rate = sample_rate;
    s_frame_ms = frame_ms;
    s_gain = gain;
    s_timing_sd_delay  = timing_sd_delay;
    s_timing_ws_delay  = timing_ws_delay;
    s_timing_bck_delay = timing_bck_delay;

    /* Initialize AGC (loads preset parameters + resets envelope/gain state).
     * Mode/bits are validated inside agc_init; AGC state is file-scope in
     * agc.c. */
    agc_init(agc_mode, bits);

    /* DMA-буфер = 1/4 PCM-кадра -> 4 события RX_DONE на кадр. Это улучшает
     * stop-респонсивность. Драйвер может срезать dma_buf_len если буфер > 4092.
     * samples_per_frame уже выровнено в main.c: 16-bit кратно 8, 24-bit кратно 4
     * (см. align comment в main.c) -> SLC word-alignment + rw_pos drift-free. */
    int dma_buf_len = samples_per_frame / 4;
    if (dma_buf_len < 8)
        dma_buf_len = 8;
    if (dma_buf_len > 1024)
        dma_buf_len = 1024;

    /* Целевая DMA-буферизация: ~DMA_POOL_TARGET_MS мс. I2S - real-time источник,
     * не jitter, поэтому DMA_POOL_TARGET_MS мс более чем достаточно. Экономит
     * ~5 кБ heap на 16kHz/mono. Запас против WiFi-джиттера даёт ADPCM pool.
     *
     * MEMORY CAP: total DMA memory <= 8 KB. At 48kHz, dma_buf_len=240 ->
     * 16 bufs = 15.4 KB (too much). Cap reduces to 8 bufs = 7.5 KB (40ms).
     * Keeps free heap >30 KB even at 48kHz/24-bit/stereo.
     *
     * bytes_per_dma_word depends on bit depth + channels (stereo-mem + GROK-2.2):
     *   16-bit (any channels): 2 bytes/sample on DMA wire for mono, 4 for stereo
     *   24-bit mono:   one 32-bit word per sample -> 4 bytes/word
     *   24-bit stereo: L and R in SEPARATE 32-bit words -> 8 bytes/word
     * Matches the driver's actual sample_size (previously '* 4' was wrong for
     * 24-bit stereo — wasted 7 KB of heap -> lwIP send() EAGAIN). */
    uint32_t bytes_per_dma_word =
        (s_bits == 24 && s_channels == 2) ? 8U :
        (s_bits == 24 && s_channels == 1) ? 4U :
        (s_bits == 16 && s_channels == 2) ? 4U :
        2U;  /* 16-bit mono */

    int dma_buf_count = (int)((uint32_t)DMA_POOL_TARGET_MS * sample_rate / (1000U * (uint32_t)dma_buf_len)) + 4;
    if (dma_buf_count < 6)
        dma_buf_count = 6;
    if (dma_buf_count > 16)
        dma_buf_count = 16;
    while (dma_buf_count > 4 &&
           (uint32_t)dma_buf_count * (uint32_t)dma_buf_len * bytes_per_dma_word > 8192U)
    {
        dma_buf_count--;
    }
    /* If dma_buf_len is large (samples_per_frame > 1024), the count floor of 4
     * may still exceed the 8 KB cap (M4). Also reduce dma_buf_len in that case
     * so the total stays under 8 KB. */
    while (dma_buf_len > 8 &&
           (uint32_t)dma_buf_count * (uint32_t)dma_buf_len * bytes_per_dma_word > 8192U)
    {
        dma_buf_len--;
    }

    static const i2s_channel_fmt_t ch_map[5] = {
        [0] = I2S_CHANNEL_FMT_RIGHT_LEFT, // STEREO
        [1] = I2S_CHANNEL_FMT_ALL_RIGHT,  // unused (defensive)
        [2] = I2S_CHANNEL_FMT_ALL_LEFT,   // unused (defensive)
        [3] = I2S_CHANNEL_FMT_ONLY_RIGHT, // RIGHT
        [4] = I2S_CHANNEL_FMT_ONLY_LEFT,  // LEFT
    };

    i2s_config_t cfg = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = sample_rate,
        .bits_per_sample = (bits == 16) ? I2S_BITS_PER_SAMPLE_16BIT
                                        : I2S_BITS_PER_SAMPLE_24BIT,
        .channel_format = ch_map[channel_format],
        .communication_format = (comm_format == I2S_CAP_CFMT_LSB)
                                    ? (I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_LSB)
                                    : I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB,
        .dma_buf_count = dma_buf_count,
        .dma_buf_len = dma_buf_len,
        .tx_desc_auto_clear = false,
    };

    i2s_pin_config_t pins = {
        .bck_i_en = 1,
        .ws_i_en = 1,
        .data_in_en = 1,
    };

    ESP_LOGI(TAG, "I2S init: %u Hz, %d-bit, %s, %d ch, dma=%dx%d, gain=%u, agc=%u",
             (unsigned)sample_rate, bits,
             comm_format == I2S_CAP_CFMT_LSB ? "LSB" : "Philips", s_channels,
             dma_buf_count, dma_buf_len,
             (unsigned)s_gain, (unsigned)agc_mode);

    /* set_pin before driver_install (patched driver configures GPIO matrix). */
    esp_err_t err = i2s_set_pin(I2S_PORT, &pins);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "i2s_set_pin failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Stereo L/R order: we rely on the SDK default (msb_right=0 for Philips).
     * If the SDK changes this default, stereo channels may swap. For production,
     * consider calling i2s_set_clk with explicit msb_right parameter. The
     * ESP8266 RTOS SDK v3.4 i2s_set_clk signature is
     *   i2s_set_clk(i2s_port_t port, uint32_t rate, uint8_t bits, uint8_t ch)
     * — it does NOT expose msb_right, so we cannot set it explicitly here.
     * See the matching note in i2s_capture_read() (16-bit path) for the
     * downstream impact. */

    /* i2s_driver_install: передаём NULL для очереди событий - не нужна.
     * i2s_read сама блокирует таску и просыпается когда данные готовы. */
    err = i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "i2s_driver_install failed: %s", esp_err_to_name(err));
        /* Cleanup on failure (2-E MEDIUM #35): i2s_set_pin already configured
         * the GPIO matrix above; there is no i2s_pin_unconfig() API in ESP8266
         * RTOS SDK v3.4 to revert it. The leftover GPIO input-enable bits are
         * harmless. No driver was installed (install failed), so no
         * i2s_driver_uninstall is needed here. Log the leftover state so the
         * user knows a reboot may be needed if re-init fails. */
        ESP_LOGW(TAG, "GPIO matrix config from i2s_set_pin remains after "
                 "driver_install failure - reboot if re-init fails");
        return err;
    }

    /* Set s_initialized NOW (right after install success), BEFORE the DMA
     * flush and apply_timing (FW#3b). Previously apply_timing() checked
     * s_initialized and returned early -> timing delays were NEVER applied. */
    s_initialized = true;

    /* Flush stale DMA buffers after i2s_driver_install (FW#3). The DMA
     * pipeline contains 2-3 buffers of stale data; the first reads would
     * return this stale data, causing startup clicks. The INMP441 MEMS mic
     * also has a startup transient (~10-15ms). Draining 6 small buffers
     * clears the DMA pipeline so only the mic transient remains for the
     * server-side skip to handle. If a read times out (I2S clock not fully
     * stable), we stop flushing; the server's skip will handle whatever
     * stale data remains. */
    {
        uint8_t flush_buf[256];
        size_t flush_got = 0;
        for (int flush_i = 0; flush_i < 6; flush_i++)
        {
            esp_err_t flush_err = i2s_read(I2S_PORT, flush_buf, sizeof(flush_buf),
                                           &flush_got, pdMS_TO_TICKS(50));
            if (flush_err != ESP_OK || flush_got == 0)
                break;
        }
        ESP_LOGI(TAG, "I2S DMA flush complete (up to 6 buffers drained)");
    }

    /* Apply RX input timing delays (TRM §10.2.1.6, I2S.timing register).
     * Драйвер установлен → регистры I2S доступны. Делаем до старта захвата. */
    apply_timing(s_timing_sd_delay, s_timing_ws_delay,
                 s_timing_bck_delay);
    if (s_timing_sd_delay || s_timing_ws_delay || s_timing_bck_delay)
    {
        ESP_LOGI(TAG, "I2S RX timing: sd=%u ws=%u bck=%u",
                 (unsigned)s_timing_sd_delay, (unsigned)s_timing_ws_delay,
                 (unsigned)s_timing_bck_delay);
    }

    return ESP_OK;
}

/* Применяет RX input timing delays к I2S-периферии (TRM §10.2.1.6).
 * Маскирует каждое поле до 2 бит (0..3) и пишет в I2S0.timing.{rx_sd,rx_ws,
 * rx_bck}_in_delay. Используется внутри i2s_capture_init (also safe to call
 * at runtime after init, with I2S stopped). */
static void apply_timing(int sd_delay, int ws_delay, int bck_delay)
{
    /* Bail out if i2s_capture_init() has not been called (or deinit was just
     * called) — AUDIT-H20. Writing I2S0.timing.* before i2s_driver_install()
     * is undefined (peripheral clock may be off). */
    if (!s_initialized)
    {
        ESP_LOGW(TAG, "apply_timing ignored - not initialized");
        return;
    }
    I2S0.timing.rx_sd_in_delay  = sd_delay  & 0x3;
    I2S0.timing.rx_ws_in_delay  = ws_delay  & 0x3;
    I2S0.timing.rx_bck_in_delay = bck_delay & 0x3;
    /* Memory barrier — гарантируем, что запись завершится до возврата. */
    asm volatile("" ::: "memory");
}

esp_err_t i2s_capture_deinit(void)
{
    if (!s_initialized)
        return ESP_OK;
    /* Propagate the uninstall return (L2) so the caller knows if the driver
     * was in a bad state. */
    esp_err_t err = i2s_driver_uninstall(I2S_PORT);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "i2s_driver_uninstall: %s - clearing s_initialized anyway "
                 "(caller should reboot if re-init fails)", esp_err_to_name(err));
        /* Clear s_initialized even on uninstall failure (GROK-7): otherwise
         * the next init sees s_initialized==true and returns ESP_OK without
         * re-installing, leaving the device stuck half-torn-down. */
        s_initialized = false;
        return err;
    }
    s_initialized = false;
    return ESP_OK;
}

/* ---- Fixed gain implementation ----
 * Applied when AGC is off and s_gain > 0. s_gain=0 means bypass.
 *
 * Fixed gain is applied in BOTH 16-bit and 24-bit modes with proper
 * clamping (AUDIT-GAIN-CONSISTENCY; previously M1 bypassed 16-bit).
 *
 * 24-bit PCM sounds louder than 16-bit PCM at the same gain because the
 * signal path differs by codec:
 *   - ADPCM (any bit depth): dither normalizes to 16-bit domain, so gain=N
 *     produces roughly the same loudness in both bit depths.
 *   - PCM 24-bit: gain=32 lifts a -30 dBFS signal to 0 dBFS (full 24-bit
 *     scale = ±SAMPLE_MAX_24BIT) without clipping — server reproduces at
 *     FULL 24-bit amplitude -> very loud.
 *   - PCM 16-bit: gain=32 clips anything >= 1024 (-30 dBFS) to ±SAMPLE_MAX_16BIT
 *     -> heavily distorted AND loud, less dynamic range than 24-bit.
 *
 * Recommendation for consistent loudness across bit depths / codecs:
 *   - Use AGC (AT+AGC=3+). AGC targets -18 dBFS in BOTH 16-bit and 24-bit
 *     domains, so loudness is automatically matched.
 *   - For fixed gain: 24-bit PCM use gain=32 (typical); 16-bit PCM use
 *     gain=4-8 (higher causes heavy clipping). ADPCM either is fine. */
static void apply_fixed_gain(int32_t *buf, int n)
{
    if (s_gain == 0)
        return;

    int32_t max_sample = (s_bits == 24) ? SAMPLE_MAX_24BIT : SAMPLE_MAX_16BIT;
    int32_t min_sample = (s_bits == 24) ? SAMPLE_MIN_24BIT : SAMPLE_MIN_16BIT;

    for (int i = 0; i < n; i++)
    {
        int64_t g = (int64_t)buf[i] * (int64_t)s_gain;
        if (g > max_sample)
            g = max_sample;
        if (g < min_sample)
            g = min_sample;
        buf[i] = (int32_t)g;
    }
}

esp_err_t i2s_capture_read(int32_t *buf, int buf_len, int *samples_read)
{
    if (!s_initialized || !buf || !samples_read || buf_len <= 0)
        return ESP_ERR_INVALID_ARG;
    *samples_read = 0;

    /* Прямой вызов i2s_read - как в debugger. i2s_read внутри ждёт на rx->queue.
     * Таймаут: время кадра x 3 + запас.
     *
     * bytes_per_sample is the DMA-wire byte width per audio sample, NOT
     * sizeof(int32_t) (2-E LOW). 16-bit: 2 bytes/sample (two 16-bit samples
     * packed into each 32-bit DMA word). 24-bit: 4 bytes/sample (one
     * left-justified 24-bit sample per 32-bit DMA word, 8 bits padding).
     * The post-processing loops below unpack into int32_t slots. */
    int bytes_per_sample = (s_bits == 16) ? 2 : 4;
    size_t want = (size_t)buf_len * bytes_per_sample;
    size_t got = 0;
    uint32_t read_timeout = s_frame_ms * 3 + 50;

    esp_err_t err = i2s_read(I2S_PORT, buf, want, &got, pdMS_TO_TICKS(read_timeout));
    if (err != ESP_OK)
        return err;
    if (got == 0)
        return ESP_ERR_TIMEOUT;

    int n = (int)(got / bytes_per_sample);

    /* Пост-обработка: extract samples. */
    if (s_bits == 16)
    {
        /* 16-bit mode: 32-bit DMA word = [S_N hi16 | S_N+1 lo16].
         * Both halves carry real samples (in ONLY_LEFT mode both slots
         * capture the left channel; in RIGHT_LEFT stereo they carry L/R).
         * Mono: swap pairs (little-endian). Stereo: already interleaved.
         * Then sign-extend int16 -> int32.
         *
         * Use memcpy + uint32_t scratch (AUDIT-C3): aliasing int32_t[] through
         * int16_t* is a strict-aliasing violation (UB at -O2).
         *
         * Swap loop iterates while i+1<n_swap (n_swap = n & ~1); for odd n
         * the last sample is not swapped but is still sign-extended (from
         * the wrong DMA half). Round n down to even for the swap; for odd
         * trailing sample, copy the hi16 of the last DMA word into the lo16
         * slot before sign-extend (AUDIT-H21).
         *
         * SDK default msb_right/right_first determines whether the LEFT-channel
         * sample lands in hi16 or lo16 in stereo mode (GROK-16 doc-only): we
         * don't call i2s_set_clk(...,msb_right=...) here, so we rely on the
         * SDK default. If a future SDK version flips that default, the
         * stereo L/R ordering would silently swap on the wire. */
        if (s_channels == 1)
        {
            int n_swap = n & ~1;
            for (int i = 0; i + 1 < n_swap; i += 2)
            {
                /* Read both 16-bit halves of buf[i/2] via memcpy. */
                uint32_t dw;
                memcpy(&dw, &buf[i / 2], sizeof(uint32_t));
                int16_t lo = (int16_t)(dw & 0xFFFFu);
                int16_t hi = (int16_t)((dw >> 16) & 0xFFFFu);
                /* Write back swapped. */
                uint32_t ndw = ((uint32_t)(uint16_t)hi) |
                               ((uint32_t)(uint16_t)lo << 16);
                memcpy(&buf[i / 2], &ndw, sizeof(uint32_t));
            }
            /* For odd n, the last DMA word's hi16 holds the last real
             * sample; lo16 is stale. Move hi16 -> lo16 so the sign-
             * extend loop below picks the right half. */
            if (n & 1)
            {
                uint32_t dw;
                memcpy(&dw, &buf[(n - 1) / 2], sizeof(uint32_t));
                int16_t hi = (int16_t)((dw >> 16) & 0xFFFFu);
                dw = (dw & 0xFFFF0000u) | (uint32_t)(uint16_t)hi;
                memcpy(&buf[(n - 1) / 2], &dw, sizeof(uint32_t));
            }
        }
        /* Sign-extend int16 -> int32, walking from the end so we don't
         * clobber DMA words we still need to read. Use memcpy to read
         * the lo16 of each 32-bit slot without aliasing. */
        for (int i = n - 1; i >= 0; i--)
        {
            uint32_t dw;
            memcpy(&dw, &buf[i / 2], sizeof(uint32_t));
            int16_t v = (i & 1) ? (int16_t)((dw >> 16) & 0xFFFFu)
                                : (int16_t)(dw & 0xFFFFu);
            buf[i] = (int32_t)v;
        }
        *samples_read = n;
    }
    else
    {
        /* 24-bit mode: 32-bit DMA word is LEFT-justified -
         * sample in bits [31:8], padding (0x00) in bits [7:0].
         * Arithmetic >>8 extracts the 24-bit sample and sign-extends it
         * into int32_t. Verified by AT+DUMP hex output: low 8 bits
         * are ALWAYS 0x00 (padding), confirming left-justified.
         *
         * After extraction, AGC/gain operates in 24-bit domain
         * (±SAMPLE_MAX_24BIT), before TPDF dither does the 24->16 reduction. */
        for (int i = 0; i < n; i++)
            buf[i] >>= 8;
        *samples_read = n;
    }

    /* Apply AGC (if enabled) or fixed gain (if gain > 0) — hoisted out of
     * the 16/24-bit branches since the dispatch is identical (AUDIT-GAIN-
     * CONSISTENCY: fixed gain is applied in both modes with clamping).
     * AGC state lives in agc.c; agc_is_active() reads the mode set by
     * agc_init() in i2s_capture_init(). */
    if (agc_is_active())
    {
        agc_process(buf, n);
    }
    else
    {
        apply_fixed_gain(buf, n);
    }
    return ESP_OK;
}

int i2s_capture_get_bits(void) { return s_bits; }
