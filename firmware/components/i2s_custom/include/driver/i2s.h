// Copyright 2018-2025 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// REFACTORED:
// Proper state machine + reference-counted concurrency model.
// api_lock, tx_users/rx_users, tx_waiters/rx_waiters, idle event group.
// Public API remains backward compatible.

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h" // Required for TickType_t used in i2s_read/i2s_write

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief I2S bit width per sample.
     */
    typedef enum
    {
        I2S_BITS_PER_SAMPLE_8BIT = 8,   /*!< Not supported by this driver (rejected by i2s_set_clk) */
        I2S_BITS_PER_SAMPLE_16BIT = 16, /*!< I2S bits per sample: 16-bits */
        I2S_BITS_PER_SAMPLE_24BIT = 24, /*!< I2S bits per sample: 24-bits (left-justified in 32-bit DMA word) */
    } i2s_bits_per_sample_t;

    /**
     * @brief I2S channel.
     */
    typedef enum
    {
        I2S_CHANNEL_MONO = 1,  /*!< I2S 1 channel (mono)*/
        I2S_CHANNEL_STEREO = 2 /*!< I2S 2 channel (stereo)*/
    } i2s_channel_t;

    /**
     * @brief I2S communication standard format
     */
    typedef enum
    {
        I2S_COMM_FORMAT_I2S = 0x01,     /*!< I2S communication format I2S*/
        I2S_COMM_FORMAT_I2S_MSB = 0x02, /*!< I2S format MSB*/
        I2S_COMM_FORMAT_I2S_LSB = 0x04, /*!< I2S format LSB*/
    } i2s_comm_format_t;

    /**
     * @brief I2S channel format type
     */
    typedef enum
    {
        I2S_CHANNEL_FMT_RIGHT_LEFT = 0x00,
        I2S_CHANNEL_FMT_ALL_RIGHT,
        I2S_CHANNEL_FMT_ALL_LEFT,
        I2S_CHANNEL_FMT_ONLY_RIGHT,
        I2S_CHANNEL_FMT_ONLY_LEFT,
    } i2s_channel_fmt_t;

    /**
     * @brief I2S Peripheral, 0
     */
    typedef enum
    {
        I2S_NUM_0 = 0x0, /*!< I2S 0*/
        I2S_NUM_MAX,
    } i2s_port_t;

    /**
     * @brief I2S Mode, default is I2S_MODE_MASTER | I2S_MODE_TX
     */
    typedef enum
    {
        I2S_MODE_MASTER = 1,
        I2S_MODE_SLAVE = 2,
        I2S_MODE_TX = 4,
        I2S_MODE_RX = 8,
    } i2s_mode_t;

    /**
     * @brief I2S configuration parameters for i2s_param_config function
     */
    typedef struct
    {
        i2s_mode_t mode;                        /*!< I2S work mode (must include TX and/or RX)*/
        int sample_rate;                        /*!< I2S sample rate (must be > 0)*/
        i2s_bits_per_sample_t bits_per_sample;  /*!< I2S bits per sample (16 or 24)*/
        i2s_channel_fmt_t channel_format;       /*!< I2S channel format */
        i2s_comm_format_t communication_format; /*!< I2S communication format */
        int dma_buf_count;                      /*!< I2S DMA Buffer Count */
        int dma_buf_len;                        /*!< I2S DMA Buffer Length */
        bool tx_desc_auto_clear;                /*!< I2S auto clear tx descriptor if there is underflow condition */
    } i2s_config_t;

    /**
     * @brief I2S event types
     */
    typedef enum
    {
        I2S_EVENT_DMA_ERROR,
        I2S_EVENT_TX_DONE, /*!< I2S DMA finish sent 1 buffer*/
        I2S_EVENT_RX_DONE, /*!< I2S DMA finish received 1 buffer*/
        I2S_EVENT_MAX,     /*!< I2S event max index*/
    } i2s_event_type_t;

    /**
     * @brief Event structure used in I2S event queue
     */
    typedef struct
    {
        i2s_event_type_t type; /*!< I2S event type */
        size_t size;           /*!< DMA buffer size in bytes for TX_DONE/RX_DONE; 0 for DMA_ERROR */
    } i2s_event_t;

    /**
     * @brief I2S pin enable for i2s_set_pin
     *
     * NOTE: fields are `int` (not `bool`) to support the `1/-1` sentinel pattern
     * used by other SDK drivers (e.g. ir_tx.c uses `1` to enable a pin and `-1`
     * to skip it). i2s_set_pin uses a `> 0` check: positive enables the pin;
     * 0 and -1 skip it.
     */
    typedef struct
    {
        int bck_o_en;    /*!< BCK out pin (>0 = enable, 0 or -1 = skip)*/
        int ws_o_en;     /*!< WS out pin*/
        int bck_i_en;    /*!< BCK in pin*/
        int ws_i_en;     /*!< WS in pin*/
        int data_out_en; /*!< DATA out pin*/
        int data_in_en;  /*!< DATA in pin*/
    } i2s_pin_config_t;

    /**
     * @brief Driver lifecycle state.
     *
     * Only I2S_DRV_RUNNING allows i2s_write()/i2s_read() to proceed.
     * Control operations move the driver through transitional states so that
     * active read/write callers can drain safely.
     */
    typedef enum
    {
        I2S_DRV_UNINIT = 0,    /*!< Not allocated */
        I2S_DRV_INSTALLING,    /*!< Allocated, hardware being configured */
        I2S_DRV_STOPPED,       /*!< Installed, DMA halted, safe to reconfigure */
        I2S_DRV_RUNNING,       /*!< DMA active, write/read allowed */
        I2S_DRV_RECONFIGURING, /*!< set_clk/zero in progress — write/read blocked */
        I2S_DRV_STOPPING,      /*!< stop in progress — waiting for users to drain */
        I2S_DRV_UNINSTALLING,  /*!< uninstall in progress — waiting for users to drain */
    } i2s_drv_state_t;

    /**
     * @brief Set I2S pin number
     */
    esp_err_t i2s_set_pin(i2s_port_t i2s_num, const i2s_pin_config_t *pin);

    /**
     * @brief Install and start I2S driver.
     *
     * @return
     *     - ESP_OK on success
     *     - ESP_ERR_INVALID_ARG on bad config
     *     - ESP_ERR_NO_MEM on allocation failure
     *     - ESP_ERR_INVALID_STATE if already installed
     */
    esp_err_t i2s_driver_install(i2s_port_t i2s_num,
                                 const i2s_config_t *i2s_config,
                                 int queue_size,
                                 void *i2s_queue);

    /**
     * @brief Uninstall I2S driver.
     *
     * NOTE:
     * The driver no longer deletes the event queue internally.
     * If a queue was created by the driver during install, it is detached but left
     * valid. The application must call vQueueDelete() on the queue after it has
     * stopped using it.
     */
    esp_err_t i2s_driver_uninstall(i2s_port_t i2s_num);

    /**
     * @brief Write data to I2S DMA transmit buffer.
     *
     * On timeout (ticks_to_wait expired before all @p size bytes could be queued),
     * returns ESP_OK and *bytes_written < size. Always check *bytes_written.
     */
    esp_err_t i2s_write(i2s_port_t i2s_num,
                        const void *src,
                        size_t size,
                        size_t *bytes_written,
                        TickType_t ticks_to_wait);

    /**
     * @brief Read data from I2S DMA receive buffer.
     *
     * On timeout returns ESP_OK and *bytes_read < size. Always check *bytes_read.
     */
    esp_err_t i2s_read(i2s_port_t i2s_num,
                       void *dest,
                       size_t size,
                       size_t *bytes_read,
                       TickType_t ticks_to_wait);

    /**
     * @brief Set sample rate used for I2S RX and TX.
     */
    esp_err_t i2s_set_sample_rates(i2s_port_t i2s_num, uint32_t rate);

    /**
     * @brief Stop I2S driver.
     */
    esp_err_t i2s_stop(i2s_port_t i2s_num);

    /**
     * @brief Start I2S driver.
     */
    esp_err_t i2s_start(i2s_port_t i2s_num);

    /**
     * @brief Zero the contents of the TX/RX DMA buffers.
     *
     * If the driver was RUNNING, it is restarted after zeroing.
     * If the driver was STOPPED, it remains STOPPED.
     */
    esp_err_t i2s_zero_dma_buffer(i2s_port_t i2s_num);

    /**
     * @brief Set clock & bit width used for I2S RX and TX.
     *
     * If the driver was RUNNING before the call, it is restarted after applying
     * the new clock/bit width. If it was STOPPED, it remains STOPPED.
     */
    esp_err_t i2s_set_clk(i2s_port_t i2s_num,
                          uint32_t rate,
                          i2s_bits_per_sample_t bits,
                          i2s_channel_t ch);

    /**
     * @brief Query the current driver lifecycle state.
     *
     * Returns I2S_DRV_UNINIT if the driver is not installed.
     */
    i2s_drv_state_t i2s_get_driver_state(i2s_port_t i2s_num);

    /**
     * @brief Write data to I2S DMA transmit buffer while expanding the number
     *        of bits per sample. For example, expanding 16-bit PCM to 32-bit PCM.
     *
     * @note For this ESP8266 driver, aim_bits must be 16 or 32 (not 24),
     *       because the 24-bit DMA mode uses 32-bit left-justified words.
     *
     * @param i2s_num             I2S_NUM_0
     * @param src                 Source address to write from
     * @param size                Size of data in bytes
     * @param src_bits            Source audio bit (8, 16 or 24)
     * @param aim_bits            Target bits (16 or 32, must be > src_bits)
     * @param[out] bytes_written  Number of source bytes written
     * @param ticks_to_wait       TX buffer wait timeout in RTOS ticks
     * @return
     *     - ESP_OK              Success
     *     - ESP_ERR_INVALID_ARG Parameter error
     *     - ESP_ERR_NO_MEM      Out of memory
     */
    esp_err_t i2s_write_expand(i2s_port_t i2s_num,
                               const void *src,
                               size_t size,
                               size_t src_bits,
                               size_t aim_bits,
                               size_t *bytes_written,
                               TickType_t ticks_to_wait);

#ifdef __cplusplus
}
#endif