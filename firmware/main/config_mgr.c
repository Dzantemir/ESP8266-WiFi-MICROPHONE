/*
 * Configuration manager with NVS persistence.
 *
 * Stores WiFi, service port, and audio I2S settings in flash via NVS.
 * A mutex protects the in-memory config struct; setters persist to NVS
 * immediately so settings survive reset.
 */

/* ---- System / SDK includes ---- */
#include <string.h>
#include <stddef.h> /* offsetof — R2-B table-driven setters */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "esp_log.h"

/* ---- Project includes ---- */
#include "board_config.h"
#include "config_mgr.h"
/* stream_control.h: streaming_get_frame_ms() / streaming_frame_ms_known()
 * prototypes (2-E LOW — previously declared as fragile `extern` in init()). */
#include "stream_control.h"

/* ---- Board config data tables (defined once here, shared via extern
 *      in board_audio.h). FIX (F-E LOW): previously these were `static const`
 *      in the header — every translation unit that included board_audio.h
 *      got its own private copy, wasting flash. Now defined once here and
 *      declared `extern` in board_audio.h. Initializer lists copied
 *      byte-for-byte from the previous header definition. ---- */
const uint32_t VALID_SAMPLE_RATES[SAMPLE_RATE_COUNT] = {
    8000, 11025, 16000, 22050, 32000, 44100, 48000};

const agc_preset_t AGC_PRESETS[AGC_MODE_COUNT] = {
    /* 0: OFF — bypass, use fixed gain (AT+GAIN) */
    {"OFF", 0, 0, 0, 0, AGC_MIN_GAIN_BOOST_ONLY},
    /* 1: Studio Soft — smooth, minimal pumping. target=-18dBFS, gate=-48dBFS */
    {"Studio Soft", 30, 5, 8248, 261, AGC_MIN_GAIN_BOOST_ONLY},
    /* 2: Podcast — smooth voice control. target=-18dBFS, gate=-42dBFS */
    {"Podcast", 50, 15, 8248, 521, AGC_MIN_GAIN_BOOST_ONLY},
    /* 3: Voice Balanced — default for speech. target=-18dBFS, gate=-42dBFS */
    {"Voice Balanced", 75, 20, 8248, 521, AGC_MIN_GAIN_BOOST_ONLY},
    /* 4: Voice Fast — fast reaction. target=-18dBFS, gate=-36dBFS */
    {"Voice Fast", 90, 40, 8248, 1039, AGC_MIN_GAIN_BOOST_ONLY},
    /* 5: Noisy Room — high gate cuts background. target=-15dBFS, gate=-30dBFS */
    {"Noisy Room", 60, 25, 11658, 2073, AGC_MIN_GAIN_BOOST_ONLY},
    /* 6: Music — slow attack preserves transients. target=-12dBFS, gate=-60dBFS */
    {"Music", 15, 60, 16463, 66, AGC_MIN_GAIN_BOOST_ONLY},
    /* 7: Limiter — limits peaks (can attenuate to -36dB). target=-6dBFS, gate=-60dBFS */
    {"Limiter", 100, 5, 32846, 66, AGC_MIN_GAIN_LIMITER},
    /* 8: Surveillance — aggressive, constant level. target=-12dBFS, gate=-60dBFS */
    {"Surveillance", 95, 80, 16463, 66, AGC_MIN_GAIN_LIMITER},
};

static const char *TAG = "config_mgr";
static const char *NVS_NAMESPACE = "streamer";

static SemaphoreHandle_t s_mutex = NULL;
static device_config_t s_config;
static bool s_initialized = false;

/* ---- Defaults from board_config.h ---- */

static void set_defaults(device_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->wifi_ssid, WIFI_SSID_DEFAULT, sizeof(cfg->wifi_ssid) - 1);
    /* Validate Kconfig default password: empty is allowed (open network),
     * non-empty must be 8-63 chars. If invalid, use empty (open). */
    {
        size_t plen = strlen(WIFI_PASSWORD_DEFAULT);
        if (plen == 0 || (plen >= 8 && plen <= 63))
        {
            strncpy(cfg->wifi_password, WIFI_PASSWORD_DEFAULT, sizeof(cfg->wifi_password) - 1);
        }
        else
        {
            ESP_LOGW(TAG, "Kconfig password '%s' has invalid length %u - using open AP",
                     WIFI_PASSWORD_DEFAULT, (unsigned)plen);
            cfg->wifi_password[0] = '\0';
        }
    }
    strncpy(cfg->hostname, WIFI_HOSTNAME_DEFAULT, sizeof(cfg->hostname) - 1);
    cfg->tx_power = WIFI_TX_POWER_DEFAULT;
    cfg->svc_port = SVC_PORT_DEFAULT;
    cfg->sample_rate = AUDIO_SAMPLE_RATE_DEFAULT;
    cfg->bits_per_sample = I2S_BITS_PER_SAMPLE;
    cfg->comm_format = I2S_COMM_FORMAT_CFG;
    cfg->channel_format = I2S_CHANNEL_FORMAT;
    cfg->gain = AUDIO_GAIN_DEFAULT;
    cfg->agc_mode = AUDIO_AGC_DEFAULT;
    cfg->codec_mode = AUDIO_CODEC_DEFAULT;
    cfg->wifi_channel = RAWTX_CHANNEL_DEFAULT;
    cfg->transport_mode = TRANSPORT_MODE_DEFAULT;
    cfg->i2s_timing_sd_delay = I2S_TIMING_SD_DELAY_DEFAULT;
    cfg->i2s_timing_ws_delay = I2S_TIMING_WS_DELAY_DEFAULT;
    cfg->i2s_timing_bck_delay = I2S_TIMING_BCK_DELAY_DEFAULT;
}

/* ---- NVS load/save ---- */

static esp_err_t load_from_nvs(device_config_t *cfg)
{
    nvs_handle h;
    /* Обнуляем структуру перед загрузкой, чтобы отсутствующие в NVS ключи
     * не оставили мусор от предыдущего состояния. */
    memset(cfg, 0, sizeof(*cfg));

    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK)
    {
        /* Log the specific error code (L31): distinguish "namespace absent"
         * (NOT_FOUND, first boot) from "NVS corrupt" (NOT_INITIALIZED). */
        ESP_LOGW(TAG, "load_from_nvs: nvs_open failed: %s "
                      "(NOT_FOUND=first boot, NOT_INITIALIZED=NVS corrupt)",
                 esp_err_to_name(err));
        return err;
    }

    size_t len;
    esp_err_t e;

    /* Fall back to compile-time defaults when a string key is missing (C7). */
    len = sizeof(cfg->wifi_ssid);
    e = nvs_get_str(h, "ssid", cfg->wifi_ssid, &len);
    if (e != ESP_OK || !cfg->wifi_ssid[0])
    {
        strncpy(cfg->wifi_ssid, WIFI_SSID_DEFAULT, sizeof(cfg->wifi_ssid) - 1);
        cfg->wifi_ssid[sizeof(cfg->wifi_ssid) - 1] = '\0';
    }
    len = sizeof(cfg->wifi_password);
    e = nvs_get_str(h, "pass", cfg->wifi_password, &len);
    if (e != ESP_OK)
    {
        strncpy(cfg->wifi_password, WIFI_PASSWORD_DEFAULT, sizeof(cfg->wifi_password) - 1);
        cfg->wifi_password[sizeof(cfg->wifi_password) - 1] = '\0';
    }
    /* Validate: empty is allowed (open network), non-empty must be 8-63. */
    if (cfg->wifi_password[0])
    {
        size_t plen = strlen(cfg->wifi_password);
        if (plen < 8 || plen > 63)
        {
            ESP_LOGW(TAG, "load_from_nvs: password length %u invalid (8-63) - clearing",
                     (unsigned)plen);
            cfg->wifi_password[0] = '\0';
        }
    }
    else
    {
        ESP_LOGI(TAG, "load_from_nvs: empty password - open AP mode");
    }
    len = sizeof(cfg->hostname);
    e = nvs_get_str(h, "host", cfg->hostname, &len);
    if (e != ESP_OK || !cfg->hostname[0])
    {
        strncpy(cfg->hostname, WIFI_HOSTNAME_DEFAULT, sizeof(cfg->hostname) - 1);
        cfg->hostname[sizeof(cfg->hostname) - 1] = '\0';
    }

    /* Apply compile-time defaults when each scalar key is missing (GROK-6):
     * the OLD code called nvs_get_* without checking the return — on partial
     * NVS, missing keys left fields at 0 (gain=0 bypass, agc=0 OFF, etc.). */
    if (nvs_get_u8(h, "txpwr", &cfg->tx_power) != ESP_OK)
        cfg->tx_power = WIFI_TX_POWER_DEFAULT;
    if (nvs_get_u16(h, "svcport", &cfg->svc_port) != ESP_OK)
        cfg->svc_port = SVC_PORT_DEFAULT;
    if (nvs_get_u32(h, "rate", &cfg->sample_rate) != ESP_OK)
        cfg->sample_rate = AUDIO_SAMPLE_RATE_DEFAULT;
    if (nvs_get_u8(h, "bits", &cfg->bits_per_sample) != ESP_OK)
        cfg->bits_per_sample = I2S_BITS_PER_SAMPLE;
    if (nvs_get_u8(h, "fmt", &cfg->comm_format) != ESP_OK)
        cfg->comm_format = I2S_COMM_FORMAT_CFG;
    if (nvs_get_u8(h, "ch", &cfg->channel_format) != ESP_OK)
        cfg->channel_format = I2S_CHANNEL_FORMAT;
    if (nvs_get_u8(h, "gain", &cfg->gain) != ESP_OK)
        cfg->gain = AUDIO_GAIN_DEFAULT;
    if (nvs_get_u8(h, "agc", &cfg->agc_mode) != ESP_OK)
        cfg->agc_mode = AUDIO_AGC_DEFAULT;
    if (nvs_get_u8(h, "codec", &cfg->codec_mode) != ESP_OK)
        cfg->codec_mode = AUDIO_CODEC_DEFAULT;
    if (nvs_get_u8(h, "wch", &cfg->wifi_channel) != ESP_OK)
        cfg->wifi_channel = RAWTX_CHANNEL_DEFAULT;
    if (nvs_get_u8(h, "xport", &cfg->transport_mode) != ESP_OK)
        cfg->transport_mode = TRANSPORT_MODE_DEFAULT;
    if (nvs_get_u8(h, "tmsd", &cfg->i2s_timing_sd_delay) != ESP_OK)
        cfg->i2s_timing_sd_delay = I2S_TIMING_SD_DELAY_DEFAULT;
    if (nvs_get_u8(h, "tmws", &cfg->i2s_timing_ws_delay) != ESP_OK)
        cfg->i2s_timing_ws_delay = I2S_TIMING_WS_DELAY_DEFAULT;
    if (nvs_get_u8(h, "tmbck", &cfg->i2s_timing_bck_delay) != ESP_OK)
        cfg->i2s_timing_bck_delay = I2S_TIMING_BCK_DELAY_DEFAULT;

    nvs_close(h);
    return ESP_OK;
}

static esp_err_t save_to_nvs(const device_config_t *cfg)
{
    nvs_handle h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        return err;
    }

    /* Check each nvs_set_* return (M19): if the namespace is full, individual
     * sets fail silently; without this check the commit succeeds and the user
     * thinks the value was saved but it wasn't. */
    esp_err_t e;
    if ((e = nvs_set_str(h, "ssid", cfg->wifi_ssid)) != ESP_OK)
        goto save_fail;
    /* SECURITY NOTE: WiFi password is stored in plaintext in NVS.
     * Any attacker with flash access can read it. NVS encryption
     * (if supported by the SDK) should be enabled for production. */
    if ((e = nvs_set_str(h, "pass", cfg->wifi_password)) != ESP_OK)
        goto save_fail;
    if ((e = nvs_set_str(h, "host", cfg->hostname)) != ESP_OK)
        goto save_fail;
    if ((e = nvs_set_u8(h, "txpwr", cfg->tx_power)) != ESP_OK)
        goto save_fail;
    if ((e = nvs_set_u16(h, "svcport", cfg->svc_port)) != ESP_OK)
        goto save_fail;
    if ((e = nvs_set_u32(h, "rate", cfg->sample_rate)) != ESP_OK)
        goto save_fail;
    if ((e = nvs_set_u8(h, "bits", cfg->bits_per_sample)) != ESP_OK)
        goto save_fail;
    if ((e = nvs_set_u8(h, "fmt", cfg->comm_format)) != ESP_OK)
        goto save_fail;
    if ((e = nvs_set_u8(h, "ch", cfg->channel_format)) != ESP_OK)
        goto save_fail;
    if ((e = nvs_set_u8(h, "gain", cfg->gain)) != ESP_OK)
        goto save_fail;
    if ((e = nvs_set_u8(h, "agc", cfg->agc_mode)) != ESP_OK)
        goto save_fail;
    if ((e = nvs_set_u8(h, "codec", cfg->codec_mode)) != ESP_OK)
        goto save_fail;
    if ((e = nvs_set_u8(h, "wch", cfg->wifi_channel)) != ESP_OK)
        goto save_fail;
    if ((e = nvs_set_u8(h, "xport", cfg->transport_mode)) != ESP_OK)
        goto save_fail;
    if ((e = nvs_set_u8(h, "tmsd", cfg->i2s_timing_sd_delay)) != ESP_OK)
        goto save_fail;
    if ((e = nvs_set_u8(h, "tmws", cfg->i2s_timing_ws_delay)) != ESP_OK)
        goto save_fail;
    if ((e = nvs_set_u8(h, "tmbck", cfg->i2s_timing_bck_delay)) != ESP_OK)
        goto save_fail;

    err = nvs_commit(h);
    nvs_close(h);
    return err;

save_fail:
    ESP_LOGE(TAG, "save_to_nvs: nvs_set_* failed: %s", esp_err_to_name(e));
    nvs_close(h);
    return e;
}

/* ---- Field descriptor table (R2-B) ----
 *
 * The 14 numeric config_set_* setters each repeat the same ~25-line shape
 * (validate -> lock -> snapshot -> modify -> save -> unlock -> log). The
 * FIELDS[] table captures name / type / offset / validate-fn so all 11
 * single-field setters and the 3-field i2s_timing setter can share one
 * generic implementation (config_set_fields). The two string setters
 * (config_set_wifi, config_set_hostname) keep their bespoke bodies below
 * — they need length + character-class validation that doesn't fit the
 * numeric table.
 */
typedef enum
{
    FIELD_TX_POWER = 0,
    FIELD_SVC_PORT,
    FIELD_SAMPLE_RATE,
    FIELD_BITS_PER_SAMPLE,
    FIELD_COMM_FORMAT,
    FIELD_CHANNEL_FORMAT,
    FIELD_GAIN,
    FIELD_AGC_MODE,
    FIELD_CODEC_MODE,
    FIELD_WIFI_CHANNEL,
    FIELD_TRANSPORT_MODE,
    FIELD_I2S_TIMING_SD,
    FIELD_I2S_TIMING_WS,
    FIELD_I2S_TIMING_BCK,
    FIELD_COUNT,
} field_id_t;

typedef enum
{
    FIELD_U8,
    FIELD_U16,
    FIELD_U32,
} field_type_t;

typedef struct
{
    const char *name; /* for logging */
    field_type_t type;
    size_t offset; /* offset into device_config_t */
    bool (*validate)(uint32_t value);
} field_desc_t;

/* Per-field validation. Extracted verbatim from the original setters + the
 * NVS-load path in config_mgr_init so the two paths can never drift. */
static bool validate_gain(uint32_t v) { return v <= 64; }
static bool validate_agc_mode(uint32_t v) { return v < AGC_MODE_COUNT; }
static bool validate_bits(uint32_t v) { return v == 16 || v == 24; }
static bool validate_codec(uint32_t v) { return v <= CODEC_MODE_PCM; }
static bool validate_transport(uint32_t v) { return v <= TRANSPORT_MODE_RAWTX; }
/* channel_format: only {LEFT=4, RIGHT=3, STEREO=0} are valid — NOT every
 * value <= 4 (the in-between enum slots are reserved/unusable on the
 * ESP8266 I2S peripheral, see I2S_CHFMT_* in config_mgr.h). */
static bool validate_channel_fmt(uint32_t v) { return v == I2S_CHFMT_LEFT || v == I2S_CHFMT_RIGHT || v == I2S_CHFMT_STEREO; }
static bool validate_comm_fmt(uint32_t v) { return v == I2S_CFMT_PHILIPS || v == I2S_CFMT_LSB; }
static bool validate_wifi_channel(uint32_t v) { return v >= 1 && v <= 14; }
static bool validate_tx_power(uint32_t v) { return v <= WIFI_TX_POWER_MAX; }
static bool validate_svc_port(uint32_t v) { return v > 0 && v <= 65535; }
static bool validate_sample_rate(uint32_t v) { return sample_rate_is_valid(v); }
static bool validate_timing(uint32_t v) { return v <= I2S_TIMING_DELAY_MAX; }

static const field_desc_t FIELDS[FIELD_COUNT] = {
    [FIELD_TX_POWER] = {"tx_power", FIELD_U8, offsetof(device_config_t, tx_power), validate_tx_power},
    [FIELD_SVC_PORT] = {"svc_port", FIELD_U16, offsetof(device_config_t, svc_port), validate_svc_port},
    [FIELD_SAMPLE_RATE] = {"sample_rate", FIELD_U32, offsetof(device_config_t, sample_rate), validate_sample_rate},
    [FIELD_BITS_PER_SAMPLE] = {"bits_per_sample", FIELD_U8, offsetof(device_config_t, bits_per_sample), validate_bits},
    [FIELD_COMM_FORMAT] = {"comm_format", FIELD_U8, offsetof(device_config_t, comm_format), validate_comm_fmt},
    [FIELD_CHANNEL_FORMAT] = {"channel_format", FIELD_U8, offsetof(device_config_t, channel_format), validate_channel_fmt},
    [FIELD_GAIN] = {"gain", FIELD_U8, offsetof(device_config_t, gain), validate_gain},
    [FIELD_AGC_MODE] = {"agc_mode", FIELD_U8, offsetof(device_config_t, agc_mode), validate_agc_mode},
    [FIELD_CODEC_MODE] = {"codec_mode", FIELD_U8, offsetof(device_config_t, codec_mode), validate_codec},
    [FIELD_WIFI_CHANNEL] = {"wifi_channel", FIELD_U8, offsetof(device_config_t, wifi_channel), validate_wifi_channel},
    [FIELD_TRANSPORT_MODE] = {"transport_mode", FIELD_U8, offsetof(device_config_t, transport_mode), validate_transport},
    [FIELD_I2S_TIMING_SD] = {"i2s_timing_sd", FIELD_U8, offsetof(device_config_t, i2s_timing_sd_delay), validate_timing},
    [FIELD_I2S_TIMING_WS] = {"i2s_timing_ws", FIELD_U8, offsetof(device_config_t, i2s_timing_ws_delay), validate_timing},
    [FIELD_I2S_TIMING_BCK] = {"i2s_timing_bck", FIELD_U8, offsetof(device_config_t, i2s_timing_bck_delay), validate_timing},
};

/*
 * Apply N numeric field updates atomically:
 *   1. validate ALL fields up-front (no partial apply on a bad batch),
 *   2. take mutex, snapshot the whole struct (M21),
 *   3. modify each field via its offset + type tag,
 *   4. save_to_nvs; on failure restore the snapshot (GROK-G11-18 rollback),
 *   5. give mutex, log.
 * Used by config_set_field (n=1) and config_set_i2s_timing (n=3). The
 * whole-struct snapshot is ~120 B on the stack — cheaper than 3 separate
 * take/save/give cycles and the only way to keep the 3-field i2s_timing
 * update atomic (calling config_set_field 3× would triple the NVS writes
 * and break rollback on a mid-batch save failure).
 */
static esp_err_t config_set_fields(const field_id_t *ids, const uint32_t *values, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        const field_desc_t *f = &FIELDS[ids[i]];
        if (f->validate && !f->validate(values[i]))
        {
            ESP_LOGW(TAG, "%s %u out of range", f->name, (unsigned)values[i]);
            return ESP_ERR_INVALID_ARG;
        }
    }
    /* Bail out if mutex not initialized (AUDIT-H17). */
    if (!s_initialized || !s_mutex)
    {
        ESP_LOGE(TAG, "config mutex not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    /* Snapshot whole struct so rollback is one assignment on save failure. */
    device_config_t old = s_config;
    for (size_t i = 0; i < n; i++)
    {
        const field_desc_t *f = &FIELDS[ids[i]];
        char *dst = (char *)&s_config + f->offset;
        /* FIX FR-AT #18: see FIXES.md — use memcpy instead of pointer casts.
         * The previous pointer casts *(uint8_t*)dst / *(uint16_t*)dst /
         * *(uint32_t*)dst are technically UB on architectures with strict
         * alignment (uint16_t/uint32_t require 2/4-byte alignment). On the
         * ESP8266 (Xtensa LX106) this happens to work for the field offsets
         * in FIELDS[] (all are naturally aligned in device_config_t), but
         * memcpy is the portable C idiom and lets the compiler emit the
         * same code on this target. */
        switch (f->type)
        {
        case FIELD_U8:
        {
            uint8_t v8 = (uint8_t)values[i];
            memcpy(dst, &v8, sizeof(v8));
            break;
        }
        case FIELD_U16:
        {
            uint16_t v16 = (uint16_t)values[i];
            memcpy(dst, &v16, sizeof(v16));
            break;
        }
        case FIELD_U32:
        {
            uint32_t v32 = values[i];
            memcpy(dst, &v32, sizeof(v32));
            break;
        }
        default:
        {
            ESP_LOGE(TAG, "config_set_fields: unknown field type %d", (int)f->type);
            xSemaphoreGive(s_mutex);
            return ESP_ERR_INVALID_STATE;
        }
        }
    }
    esp_err_t err = save_to_nvs(&s_config);
    if (err != ESP_OK)
        s_config = old; /* rollback to pre-call state */
    xSemaphoreGive(s_mutex);
    if (err == ESP_OK)
    {
        if (n == 1)
            ESP_LOGI(TAG, "Config saved to NVS (%s=%u)",
                     FIELDS[ids[0]].name, (unsigned)values[0]);
        else
            ESP_LOGI(TAG, "Config saved to NVS (%u fields)", (unsigned)n);
    }
    else
    {
        if (n == 1)
            ESP_LOGW(TAG, "Config save FAILED (%s) - rolled back",
                     FIELDS[ids[0]].name);
        else
            ESP_LOGW(TAG, "Config save FAILED (%u fields) - rolled back",
                     (unsigned)n);
    }
    return err;
}

/* Generic single-field setter. The 11 numeric wrappers below delegate here. */
static esp_err_t config_set_field(field_id_t id, uint32_t value)
{
    return config_set_fields(&id, &value, 1);
}

/* ---- Public API ---- */

esp_err_t config_mgr_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex)
    {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    set_defaults(&s_config);

    /* Try to load from NVS; if it fails, keep defaults and save them. */
    esp_err_t err = load_from_nvs(&s_config);
    if (err != ESP_OK)
    {
        ESP_LOGI(TAG, "No NVS config - saving defaults");
        set_defaults(&s_config); /* Восстанавливаем - load_from_nvs мог обнулить */
        /* Check save_to_nvs return on first boot (2-E LOW): a silent NVS write
         * failure would leave the device running on in-RAM defaults with no
         * persistence — every reboot would re-trigger "No NVS config". Don't
         * fail init: the device can still operate with in-RAM defaults. */
        esp_err_t save_err = save_to_nvs(&s_config);
        if (save_err != ESP_OK)
        {
            ESP_LOGE(TAG, "save_to_nvs of defaults failed on first boot: %s "
                          "(device will use in-RAM defaults, no persistence)",
                     esp_err_to_name(save_err));
        }
    }
    else
    {
        ESP_LOGI(TAG, "Config loaded from NVS");
    }

    /* Validate loaded values, fall back to defaults if invalid.
     * The per-field rules mirror the validate_* functions above (R2-B); kept
     * inline here so a corrupt NVS value still gets clamped on load (where
     * there is no caller-supplied value to reject). */

    bool corrected = false;

    if (!sample_rate_is_valid(s_config.sample_rate))
    {
        s_config.sample_rate = AUDIO_SAMPLE_RATE_DEFAULT;
        corrected = true;
    }
    if (s_config.bits_per_sample != 16 && s_config.bits_per_sample != 24)
    {
        s_config.bits_per_sample = I2S_BITS_PER_SAMPLE;
        corrected = true;
    }
    if (s_config.comm_format != I2S_CFMT_PHILIPS && s_config.comm_format != I2S_CFMT_LSB)
    {
        s_config.comm_format = I2S_COMM_FORMAT_CFG;
        corrected = true;
    }
    if (s_config.channel_format != I2S_CHFMT_LEFT &&
        s_config.channel_format != I2S_CHFMT_RIGHT &&
        s_config.channel_format != I2S_CHFMT_STEREO)
    {
        s_config.channel_format = I2S_CHANNEL_FORMAT;
        corrected = true;
    }
    if (s_config.tx_power > WIFI_TX_POWER_MAX)
    {
        s_config.tx_power = WIFI_TX_POWER_DEFAULT;
        corrected = true;
    }
    if (s_config.gain > 64)
    {
        s_config.gain = AUDIO_GAIN_DEFAULT;
        corrected = true;
    }
    if (s_config.agc_mode >= AGC_MODE_COUNT)
    {
        s_config.agc_mode = AUDIO_AGC_DEFAULT;
        corrected = true;
    }
    if (s_config.codec_mode > CODEC_MODE_PCM)
    {
        s_config.codec_mode = AUDIO_CODEC_DEFAULT;
        corrected = true;
    }
    if (s_config.wifi_channel < 1 || s_config.wifi_channel > 14)
    {
        s_config.wifi_channel = 1;
        corrected = true;
    }
    /* svc_port=0 lets the OS pick a random ephemeral port on bind() (AUDIT-H18)
     * -> the receiver can't discover the device on the expected port. */
    if (s_config.svc_port == 0)
    {
        s_config.svc_port = SVC_PORT_DEFAULT;
        corrected = true;
    }
    if (s_config.transport_mode > TRANSPORT_MODE_RAWTX)
    {
        s_config.transport_mode = TRANSPORT_MODE_DEFAULT;
        corrected = true;
    }
    if (s_config.i2s_timing_sd_delay > I2S_TIMING_DELAY_MAX)
    {
        s_config.i2s_timing_sd_delay = I2S_TIMING_SD_DELAY_DEFAULT;
        corrected = true;
    }
    if (s_config.i2s_timing_ws_delay > I2S_TIMING_DELAY_MAX)
    {
        s_config.i2s_timing_ws_delay = I2S_TIMING_WS_DELAY_DEFAULT;
        corrected = true;
    }
    if (s_config.i2s_timing_bck_delay > I2S_TIMING_DELAY_MAX)
    {
        s_config.i2s_timing_bck_delay = I2S_TIMING_BCK_DELAY_DEFAULT;
        corrected = true;
    }
    /* Validate loaded WiFi password: empty is allowed (open network),
     * non-empty must be 8-63 chars (WPA2 requirement). */
    if (s_config.wifi_password[0])
    {
        size_t plen = strlen(s_config.wifi_password);
        if (plen < 8 || plen > 63)
        {
            ESP_LOGW(TAG, "NVS password length %u invalid (must be 0 or 8-63) - clearing",
                     (unsigned)plen);
            s_config.wifi_password[0] = '\0';
            corrected = true;
        }
    }

    if (corrected)
    {
        ESP_LOGW(TAG, "Some NVS values were invalid and corrected - saving fixed config");
        esp_err_t save_err = save_to_nvs(&s_config);
        if (save_err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to save corrected config: %s", esp_err_to_name(save_err));
        }
    }
    /* Print last-known runtime frame_ms (GROK-G11-8): previously hardcoded
     * `20` lied about the actual runtime frame_ms (5..60ms). The
     * streaming_frame_ms_known() predicate distinguishes "real computed
     * value" from "init". */
    {
        const char *frame_ms_str = streaming_frame_ms_known()
                                       ? ""
                                       : " (init, not yet computed)";
        ESP_LOGI(TAG, "Runtime audio: %u Hz, %u ms%s, %d-bit, fmt=%d, ch=%d, gain=%u, agc=%u, codec=%u",
                 (unsigned)s_config.sample_rate,
                 (unsigned)streaming_get_frame_ms(),
                 frame_ms_str,
                 s_config.bits_per_sample, s_config.comm_format,
                 s_config.channel_format, (unsigned)s_config.gain,
                 (unsigned)s_config.agc_mode, (unsigned)s_config.codec_mode);
    }

    s_initialized = true;
    return ESP_OK;
}

void config_get_copy(device_config_t *cfg)
{
    if (!s_initialized || !cfg)
    {
        if (cfg)
            set_defaults(cfg);
        return;
    }
    /* Bail out if config_mgr_init() failed to create the mutex (AUDIT-H17):
     * xSemaphoreTake(NULL, ...) crashes. */
    if (!s_mutex)
    {
        ESP_LOGE(TAG, "config mutex not initialized");
        set_defaults(cfg);
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *cfg = s_config;
    xSemaphoreGive(s_mutex);
}

static esp_err_t save_locked(void)
{
    return save_to_nvs(&s_config);
}

/* ---- String setters (bespoke: dual-field / RFC-952 validation) ---- */

esp_err_t config_set_wifi(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0])
    {
        return ESP_ERR_INVALID_ARG;
    }
    /* Password is optional: NULL or empty = open network.
     * If present, WPA2 mandates 8-63 characters. */
    if (!password)
        password = "";
    if (password[0])
    {
        size_t plen = strlen(password);
        if (plen < 8 || plen > 63)
        {
            ESP_LOGW(TAG, "config_set_wifi: password length %u invalid (must be 8-63)",
                     (unsigned)plen);
            return ESP_ERR_INVALID_ARG;
        }
    }
    /* Reject whitespace-only SSID (L30): 802.11 allows it but it's almost
     * certainly a user typo. */
    bool ssid_has_nonws = false;
    for (const char *p = ssid; *p; p++)
        if (*p != ' ' && *p != '\t')
        {
            ssid_has_nonws = true;
            break;
        }
    if (!ssid_has_nonws)
        return ESP_ERR_INVALID_ARG;
    if (strlen(ssid) >= sizeof(s_config.wifi_ssid) ||
        strlen(password) >= sizeof(s_config.wifi_password))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    /* Bail out if mutex not initialized (AUDIT-H17). */
    if (!s_mutex)
    {
        ESP_LOGE(TAG, "config mutex not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    /* Capture previous values so we can roll back the in-memory state if
     * save_to_nvs fails (M21): without this, runtime != persisted state after
     * a save failure -> user confusion after reboot. */
    char old_ssid[sizeof(s_config.wifi_ssid)];
    char old_pass[sizeof(s_config.wifi_password)];
    memcpy(old_ssid, s_config.wifi_ssid, sizeof(old_ssid));
    memcpy(old_pass, s_config.wifi_password, sizeof(old_pass));

    strncpy(s_config.wifi_ssid, ssid, sizeof(s_config.wifi_ssid) - 1);
    s_config.wifi_ssid[sizeof(s_config.wifi_ssid) - 1] = '\0';
    strncpy(s_config.wifi_password, password, sizeof(s_config.wifi_password) - 1);
    s_config.wifi_password[sizeof(s_config.wifi_password) - 1] = '\0';
    esp_err_t err = save_locked();
    if (err != ESP_OK)
    {
        /* Roll back in-memory state to match NVS. */
        memcpy(s_config.wifi_ssid, old_ssid, sizeof(s_config.wifi_ssid));
        memcpy(s_config.wifi_password, old_pass, sizeof(s_config.wifi_password));
    }
    xSemaphoreGive(s_mutex);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "Config saved to NVS");
    else
        ESP_LOGE(TAG, "Config save failed, in-memory state rolled back: %s", esp_err_to_name(err));
    /* Wipe the captured plaintext password (L29). */
    memset(old_pass, 0, sizeof(old_pass));
    return err;
}

esp_err_t config_set_hostname(const char *hostname)
{
    if (!hostname || !hostname[0])
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(hostname) >= sizeof(s_config.hostname))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    /* RFC 952: alphanumeric + hyphens, must start with letter.
     * We're lenient here - just reject clearly invalid chars. */
    for (const char *p = hostname; *p; p++)
    {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-'))
        {
            ESP_LOGE(TAG, "Invalid hostname char '%c' (0x%02X)", c, (unsigned)c);
            return ESP_ERR_INVALID_ARG;
        }
    }
    /* Reject leading/trailing hyphen and pure-numeric (GROK-39): RFC 952/1123
     * forbid the former; the latter breaks mDNS on some platforms. */
    if (hostname[0] == '-' || hostname[strlen(hostname) - 1] == '-')
    {
        ESP_LOGE(TAG, "Invalid hostname: must not start or end with '-'");
        return ESP_ERR_INVALID_ARG;
    }
    {
        bool has_alpha = false;
        for (const char *p = hostname; *p; p++)
        {
            char c = *p;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
            {
                has_alpha = true;
                break;
            }
        }
        if (!has_alpha)
        {
            ESP_LOGE(TAG, "Invalid hostname: must contain at least one letter "
                          "(pure-numeric hostnames break mDNS on some platforms)");
            return ESP_ERR_INVALID_ARG;
        }
    }
    /* Bail out if mutex not initialized (AUDIT-H17). */
    if (!s_mutex)
    {
        ESP_LOGE(TAG, "config mutex not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    /* Rollback on NVS save failure (GROK-G11-18). */
    char old_hostname[sizeof(s_config.hostname)];
    memcpy(old_hostname, s_config.hostname, sizeof(old_hostname));
    strncpy(s_config.hostname, hostname, sizeof(s_config.hostname) - 1);
    s_config.hostname[sizeof(s_config.hostname) - 1] = '\0';
    esp_err_t err = save_locked();
    if (err != ESP_OK)
        memcpy(s_config.hostname, old_hostname, sizeof(s_config.hostname));
    xSemaphoreGive(s_mutex);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "Config saved to NVS");
    else
        ESP_LOGW(TAG, "Config save FAILED (hostname) - rolled back");
    return err;
}

/* ---- Numeric setters: thin wrappers over config_set_field (R2-B) ----
 *
 * Each wrapper preserves the public signature from config_mgr.h and delegates
 * to the table-driven config_set_field, which performs validate -> lock ->
 * snapshot -> modify -> save -> rollback -> log. */

esp_err_t config_set_tx_power(uint8_t tx_power)
{
    return config_set_field(FIELD_TX_POWER, (uint32_t)tx_power);
}

esp_err_t config_set_svc_port(uint16_t port)
{
    return config_set_field(FIELD_SVC_PORT, port);
}

esp_err_t config_set_sample_rate(uint32_t rate)
{
    return config_set_field(FIELD_SAMPLE_RATE, rate);
}

esp_err_t config_set_bits_per_sample(uint8_t bits)
{
    return config_set_field(FIELD_BITS_PER_SAMPLE, bits);
}

esp_err_t config_set_comm_format(uint8_t fmt)
{
    return config_set_field(FIELD_COMM_FORMAT, fmt);
}

esp_err_t config_set_channel_format(uint8_t fmt)
{
    return config_set_field(FIELD_CHANNEL_FORMAT, fmt);
}

esp_err_t config_set_gain(uint8_t gain)
{
    return config_set_field(FIELD_GAIN, gain);
}

esp_err_t config_set_agc_mode(uint8_t mode)
{
    return config_set_field(FIELD_AGC_MODE, mode);
}

esp_err_t config_set_codec_mode(uint8_t mode)
{
    return config_set_field(FIELD_CODEC_MODE, mode);
}

esp_err_t config_set_wifi_channel(uint8_t ch)
{
    return config_set_field(FIELD_WIFI_CHANNEL, ch);
}

esp_err_t config_set_transport_mode(uint8_t mode)
{
    return config_set_field(FIELD_TRANSPORT_MODE, mode);
}

/* i2s_timing: 3 fields applied atomically with one mutex take / one NVS save
 * / one rollback. Calling config_set_field 3× would triple the NVS writes and
 * break atomicity (a mid-batch save failure would leave sd_delay already
 * persisted but ws_delay/bck_delay rolled back). config_set_fields validates
 * all 3 up-front, snapshots the whole struct, and rolls back as a unit. */
esp_err_t config_set_i2s_timing(uint8_t sd_delay, uint8_t ws_delay, uint8_t bck_delay)
{
    const field_id_t ids[3] = {FIELD_I2S_TIMING_SD, FIELD_I2S_TIMING_WS, FIELD_I2S_TIMING_BCK};
    const uint32_t vals[3] = {sd_delay, ws_delay, bck_delay};
    return config_set_fields(ids, vals, 3);
}

esp_err_t config_factory_reset(void)
{
    /* Bail out if mutex not initialized (AUDIT-H17). */
    if (!s_mutex)
    {
        ESP_LOGE(TAG, "config mutex not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    set_defaults(&s_config);

    /* Erase ALL keys in the namespace before saving defaults (2-E MEDIUM #33):
     * without nvs_erase_all, keys from older firmware versions remain as
     * orphans in NVS — they consume flash pages and can confuse a future
     * firmware downgrade that expects the old key. nvs_erase_all wipes every
     * key in the namespace atomically; the subsequent save_locked() writes a
     * clean set of defaults. */
    nvs_handle h;
    esp_err_t open_err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (open_err == ESP_OK)
    {
        /* On nvs_erase_all failure, do NOT return early (4-C MEDIUM #12):
         * set_defaults(&s_config) already ran, so the in-memory config IS the
         * defaults. Returning here would leave in-memory = defaults but NVS =
         * old values (desync). Instead, fall through to save_locked() which
         * writes the in-memory defaults to NVS, achieving the same end state. */
        esp_err_t erase_err = nvs_erase_all(h);
        if (erase_err != ESP_OK)
        {
            ESP_LOGE(TAG, "nvs_erase_all failed: %s — will save defaults explicitly",
                     esp_err_to_name(erase_err));
            nvs_close(h);
            /* Fall through to save_locked() to persist the in-memory defaults. */
        }
        else
        {
            esp_err_t commit_err = nvs_commit(h);
            if (commit_err != ESP_OK)
            {
                /* Log but fall through to save_locked (6-C MED #1): flash may
                 * still be writable, and skipping it would leave in-memory =
                 * defaults but NVS = old values (the desync 4-C MEDIUM #12 was
                 * supposed to prevent). save_locked() opens its own handle, so
                 * closing here is safe. The mutex is given once after
                 * save_locked() returns. */
                ESP_LOGE(TAG, "nvs_commit after erase failed: %s — continuing with save_locked",
                         esp_err_to_name(commit_err));
                nvs_close(h);
                /* Don't return — fall through to save_locked to persist defaults */
            }
            else
            {
                nvs_close(h);
                ESP_LOGI(TAG, "Factory reset - NVS namespace erased (orphan keys removed)");
            }
        }
    }
    else
    {
        /* If we can't open the namespace to erase, log but continue with
         * save_locked() — it will open its own handle and overwrite the known
         * keys. Orphan keys remain, but the reset still succeeds for the
         * known fields. */
        ESP_LOGW(TAG, "nvs_open for erase failed (%s) - continuing with save_locked",
                 esp_err_to_name(open_err));
    }

    /* Gate the success log on save_locked()'s return value (4-C LOW):
     * previously "Factory reset - defaults restored" was logged even when
     * save_locked() failed (NVS NOT actually reset), misleading the log. */
    esp_err_t err = save_locked();
    xSemaphoreGive(s_mutex);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Factory reset - defaults restored");
    }
    else
    {
        ESP_LOGE(TAG, "Factory reset FAILED to persist defaults: %s",
                 esp_err_to_name(err));
    }
    return err;
}
