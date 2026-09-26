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
// =============================================================================
// ESP8266 I2S driver — concurrency-safe rework
// =============================================================================
//
// Locking model (deliberately simple):
//
//   I2S_ENTER_CRITICAL()  protects drv_state, tx/rx_users, tx/rx_waiters and
//                         life_refs. Only plain loads/stores inside — never a
//                         blocking call.
//   api_lock (mutex)      one control operation at a time (start / stop /
//                         set_clk / zero / uninstall / tail of install). Held
//                         for the WHOLE operation, including the wait for
//                         readers/writers to drain.
//   tx->mux / rx->mux     data path. A writer/reader holds it for the whole
//                         copy loop. The control path touches it only inside
//                         start/stop_internal, i.e. after all users drained.
//
//   Lock order: api_lock -> mux -> critical section.
//   The data path (i2s_write / i2s_read) never takes api_lock, so there is no
//   AB-BA between the data path and the control operations.
//
// States: UNINIT / INSTALLING / STOPPED / RUNNING / RECONFIGURING / STOPPING /
//         UNINSTALLING
//   * i2s_write()/i2s_read() are admitted only in RUNNING (users++ under
//     critical section).
//   * A control operation first moves the state out of RUNNING (no new users
//     can enter), then pushes NULL sentinels into the DMA queues to wake
//     blocked users and waits on idle_evt until users == 0, and only then
//     touches hardware / queues.
//   * NULL in a DMA queue is a sentinel, never a buffer. Everybody that pops
//     from a DMA queue (ISR, write, read) tolerates it.
//
// Behavioural differences from the stock ESP8266_RTOS_SDK driver are listed in
// include/driver/i2s.h.
// =============================================================================

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include <float.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "esp8266/eagle_soc.h"
#include "esp8266/pin_mux_register.h"
#include "esp8266/i2s_register.h"
#include "esp8266/i2s_struct.h"
#include "esp8266/slc_register.h"
#include "esp8266/slc_struct.h"

#include "rom/ets_sys.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_libc.h"
#include "esp_heap_caps.h"

#include "driver/i2s.h"
#include "esp_wifi_types.h"
#include "esp_wifi.h"

static const char *I2S_TAG = "i2s";

extern void rom_i2c_writeReg_Mask(uint8_t block,
                                  uint8_t host_id,
                                  uint8_t reg_add,
                                  uint8_t msb,
                                  uint8_t lsb,
                                  uint8_t indata);

/* Memory barrier: on Xtensa also waits for outstanding register writes. On
 * other targets (host unit tests) it degrades to a compiler barrier. */
#if defined(__XTENSA__) || defined(__xtensa__)
#define I2S_MEMW() __asm__ __volatile__("memw" ::: "memory")
#else
#define I2S_MEMW() __asm__ __volatile__("" ::: "memory")
#endif

/* Only use before any lock is taken — it returns early. */
#define I2S_CHECK(a, str, ret_val)                                        \
    do                                                                    \
    {                                                                     \
        if (!(a))                                                         \
        {                                                                 \
            ESP_LOGE(I2S_TAG, "%s(%d): %s", __FUNCTION__, __LINE__, str); \
            return (ret_val);                                             \
        }                                                                 \
    } while (0)

#define dma_intr_enable() _xt_isr_unmask(1 << ETS_SLC_INUM)
#define dma_intr_disable() _xt_isr_mask(1 << ETS_SLC_INUM)
#define dma_intr_register(a, b) _xt_isr_attach(ETS_SLC_INUM, (a), (b))

#define I2S_MAX_BUFFER_SIZE (4 * 1024 * 1024)
#define I2S_BASE_CLK (2 * APB_CLK_FREQ)
#define I2S_MAX_DMA_BUF_BYTES 4092

#define I2S_ENTER_CRITICAL() portENTER_CRITICAL()
#define I2S_EXIT_CRITICAL() portEXIT_CRITICAL()

/* Timeout for acquiring api_lock. */
#define I2S_API_LOCK_TIMEOUT_MS 2000
/* Timeout for waiting active read/write callers to drain. */
#define I2S_DRAIN_TIMEOUT_MS 2000
/* How often the drain loop re-checks / re-sends sentinels. */
#define I2S_DRAIN_POLL_MS 10
/* Waiting for in-flight public API calls to release the object. Must be longer
 * than I2S_API_LOCK_TIMEOUT_MS: a caller blocked on api_lock holds a ref. */
#define I2S_REF_DRAIN_TIMEOUT_MS (I2S_API_LOCK_TIMEOUT_MS + 500)

/* Event group bits set when tx_users / rx_users drop to zero. */
#define I2S_TX_IDLE_BIT (1 << 0)
#define I2S_RX_IDLE_BIT (1 << 1)

/* SLC interrupt bits, translated to I2S direction. The SLC *RX* link reads
 * memory and feeds the I2S TX FIFO; the SLC *TX* link fills memory from the
 * I2S RX FIFO. So SLC rx_eof == "I2S TX buffer sent", SLC tx_suc_eof ==
 * "I2S RX buffer filled". */
#define I2S_SLC_INT_TX_DONE SLC_RX_EOF_INT_ST
#define I2S_SLC_INT_RX_DONE SLC_TX_EOF_INT_ST
#define I2S_SLC_INT_DSCR_ERR (SLC_RX_DSCR_ERR_INT_ST | SLC_TX_DSCR_ERR_INT_ST)

typedef struct lldesc
{
    uint32_t blocksize : 12;
    uint32_t datalen : 12;
    uint32_t unused : 5;
    uint32_t sub_sof : 1;
    uint32_t eof : 1;
    volatile uint32_t owner : 1; /* DMA can change this value */
    uint32_t *buf_ptr;
    struct lldesc *next_link_ptr;
} lldesc_t;

typedef struct
{
    char **buf;
    int buf_size;
    int buf_count;         /* number of buffers in this queue */
    int rw_pos;
    void *curr_ptr;
    SemaphoreHandle_t mux; /* data path lock: curr_ptr / rw_pos / buffer contents */
    QueueHandle_t queue;   /* holds buf pointers (or NULL sentinels) */
    lldesc_t **desc;
} i2s_dma_t;

typedef struct
{
    i2s_port_t i2s_num;

    volatile i2s_drv_state_t drv_state; /* critical section */

    SemaphoreHandle_t api_lock;  /* serialises control operations */
    EventGroupHandle_t idle_evt; /* TX/RX idle signalling */

    volatile int tx_users;   /* tasks inside i2s_write()      (critical) */
    volatile int rx_users;   /* tasks inside i2s_read()       (critical) */
    volatile int tx_waiters; /* tasks blocked on tx->queue    (critical) */
    volatile int rx_waiters; /* tasks blocked on rx->queue    (critical) */
    volatile int life_refs;  /* public API calls holding this object alive (critical) */

    int queue_size;
    QueueHandle_t i2s_queue; /* event queue — app-owned after uninstall */

    int dma_buf_count;
    int dma_buf_len;      /* effective, after the 4092-byte clamp */
    int dma_buf_len_orig; /* as requested at install */

    i2s_dma_t *rx;
    i2s_dma_t *tx;

    int channel_num;
    int bytes_per_sample;
    int bits_per_sample;
    i2s_mode_t mode;
    uint32_t sample_rate;
    bool tx_desc_auto_clear;
    i2s_channel_fmt_t channel_format; /* current */
    i2s_channel_fmt_t stereo_format;  /* stereo format to return to after a mono excursion */

    /* NOTE: must stay volatile. slc_struct_t itself is not declared volatile
     * in the SDK headers, so accessing SLC0 through a plain pointer lets the
     * compiler merge the `x_rst = 1; x_rst = 0;` pulses into a single store. */
    volatile slc_struct_t *dma;
} i2s_obj_t;

static i2s_obj_t *p_i2s_obj[I2S_NUM_MAX] = {0};
static i2s_struct_t *I2S[I2S_NUM_MAX] = {&I2S0};

/* Forward declarations. */
static i2s_dma_t *i2s_dma_queue_create(int dma_buf_count, int dma_buf_len, int sample_size);
static void i2s_destroy_dma_queue(i2s_dma_t *dma);
static esp_err_t i2s_start_internal(i2s_obj_t *obj);
static esp_err_t i2s_stop_internal(i2s_obj_t *obj);
static esp_err_t i2s_param_config(i2s_obj_t *obj, const i2s_config_t *i2s_config);
static esp_err_t i2s_set_rate(i2s_obj_t *obj, uint32_t rate, int bits_per_sample);
static esp_err_t i2s_set_clk_locked(i2s_obj_t *obj, uint32_t rate, i2s_bits_per_sample_t bits, i2s_channel_t ch);

/* ===========================================================================
 *  State / lifetime / user accounting helpers
 * ======================================================================== */

static inline i2s_drv_state_t i2s_state_get(i2s_obj_t *obj)
{
    i2s_drv_state_t st;
    I2S_ENTER_CRITICAL();
    st = obj->drv_state;
    I2S_EXIT_CRITICAL();
    return st;
}

static inline void i2s_state_set(i2s_obj_t *obj, i2s_drv_state_t st)
{
    I2S_ENTER_CRITICAL();
    obj->drv_state = st;
    I2S_EXIT_CRITICAL();
}

/* Take a lifetime reference. Fails when not installed or being uninstalled. */
static i2s_obj_t *i2s_obj_acquire(i2s_port_t i2s_num)
{
    i2s_obj_t *obj = NULL;

    if (i2s_num >= I2S_NUM_MAX)
        return NULL;

    I2S_ENTER_CRITICAL();
    if (p_i2s_obj[i2s_num] != NULL &&
        p_i2s_obj[i2s_num]->drv_state != I2S_DRV_UNINSTALLING)
    {
        obj = p_i2s_obj[i2s_num];
        obj->life_refs++;
    }
    I2S_EXIT_CRITICAL();

    return obj;
}

static void i2s_obj_release(i2s_obj_t *obj)
{
    if (obj == NULL)
        return;

    I2S_ENTER_CRITICAL();
    if (obj->life_refs > 0)
        obj->life_refs--;
    I2S_EXIT_CRITICAL();
}

/* Register a reader/writer. Only allowed while RUNNING. */
static bool i2s_user_begin(i2s_obj_t *obj, bool is_tx)
{
    bool ok = false;

    I2S_ENTER_CRITICAL();
    if (obj->drv_state == I2S_DRV_RUNNING)
    {
        if (is_tx)
            obj->tx_users++;
        else
            obj->rx_users++;
        ok = true;
    }
    I2S_EXIT_CRITICAL();

    return ok;
}

static void i2s_user_end(i2s_obj_t *obj, bool is_tx)
{
    bool last;

    I2S_ENTER_CRITICAL();
    if (is_tx)
    {
        if (obj->tx_users > 0)
            obj->tx_users--;
        last = (obj->tx_users == 0);
    }
    else
    {
        if (obj->rx_users > 0)
            obj->rx_users--;
        last = (obj->rx_users == 0);
    }
    I2S_EXIT_CRITICAL();

    /* Outside the critical section: may unblock a higher priority task. */
    if (last)
        xEventGroupSetBits(obj->idle_evt, is_tx ? I2S_TX_IDLE_BIT : I2S_RX_IDLE_BIT);
}

/* Wake every task blocked inside i2s_write()/i2s_read() for one direction and
 * wait until all of them have left.
 *
 * Preconditions: api_lock held and drv_state already != RUNNING, so no new
 * user can enter while we wait.
 *
 * A blocked user sits in xQueueReceive() on the DMA queue; a NULL sentinel
 * wakes it, it re-checks drv_state and leaves. We re-send sentinels every
 * I2S_DRAIN_POLL_MS in case a task incremented *_waiters after our snapshot.
 * A stale idle bit only costs one extra loop iteration. */
static esp_err_t i2s_wake_and_wait_idle(i2s_obj_t *obj, bool is_tx)
{
    const EventBits_t bit = is_tx ? I2S_TX_IDLE_BIT : I2S_RX_IDLE_BIT;
    i2s_dma_t *d = is_tx ? obj->tx : obj->rx;
    QueueHandle_t q = d ? d->queue : NULL;

    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(I2S_DRAIN_TIMEOUT_MS);
    TickType_t slice = pdMS_TO_TICKS(I2S_DRAIN_POLL_MS);

    if (slice == 0)
        slice = 1; /* never spin: pdMS_TO_TICKS() rounds down to 0 at low tick rates */

    for (;;)
    {
        int users, waiters;

        I2S_ENTER_CRITICAL();
        users = is_tx ? obj->tx_users : obj->rx_users;
        waiters = is_tx ? obj->tx_waiters : obj->rx_waiters;
        I2S_EXIT_CRITICAL();

        if (users == 0)
            return ESP_OK;

        /* One sentinel per blocked task, never more. A full queue means nobody
         * can be blocked on it, so a dropped sentinel is harmless. */
        for (int i = 0; q != NULL && i < waiters; i++)
        {
            void *sentinel = NULL;
            (void)xQueueSend(q, &sentinel, 0);
        }

        if ((TickType_t)(xTaskGetTickCount() - start) >= timeout)
        {
            ESP_LOGE(I2S_TAG, "timeout waiting for %s users to drain (%d left)",
                     is_tx ? "tx" : "rx", users);
            return ESP_ERR_TIMEOUT;
        }

        (void)xEventGroupWaitBits(obj->idle_evt, bit, pdTRUE, pdTRUE, slice);
    }
}

static esp_err_t i2s_drain_users(i2s_obj_t *obj)
{
    esp_err_t e;

    if (obj->mode & I2S_MODE_TX)
    {
        e = i2s_wake_and_wait_idle(obj, true);
        if (e != ESP_OK)
            return e;
    }

    if (obj->mode & I2S_MODE_RX)
    {
        e = i2s_wake_and_wait_idle(obj, false);
        if (e != ESP_OK)
            return e;
    }

    return ESP_OK;
}

/* Wait until no other public API call holds a reference to the object.
 * `own_refs` is how many references the caller itself holds. */
static esp_err_t i2s_wait_refs(i2s_obj_t *obj, int own_refs)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(I2S_REF_DRAIN_TIMEOUT_MS);

    for (;;)
    {
        int refs;

        I2S_ENTER_CRITICAL();
        refs = obj->life_refs;
        I2S_EXIT_CRITICAL();

        if (refs <= own_refs)
            return ESP_OK;

        if ((TickType_t)(xTaskGetTickCount() - start) >= timeout)
        {
            ESP_LOGE(I2S_TAG, "timeout waiting for API references to drain (%d left)",
                     refs - own_refs);
            return ESP_ERR_TIMEOUT;
        }

        vTaskDelay(1);
    }
}

/* ===========================================================================
 *  Register-level helpers (no locking inside — callers decide)
 * ======================================================================== */

static inline void i2s_hw_reset_fifo(i2s_port_t i2s_num)
{
    I2S[i2s_num]->conf.rx_fifo_reset = 1;
    I2S[i2s_num]->conf.rx_fifo_reset = 0;
    I2S[i2s_num]->conf.tx_fifo_reset = 1;
    I2S[i2s_num]->conf.tx_fifo_reset = 0;
}

static inline void i2s_hw_reset_i2s(i2s_port_t i2s_num)
{
    I2S[i2s_num]->conf.tx_reset = 1;
    I2S[i2s_num]->conf.tx_reset = 0;
    I2S[i2s_num]->conf.rx_reset = 1;
    I2S[i2s_num]->conf.rx_reset = 0;
}

static inline void i2s_hw_reset_dma(volatile slc_struct_t *dma)
{
    dma->conf0.rx_rst = 1;
    dma->conf0.rx_rst = 0;
    dma->conf0.tx_rst = 1;
    dma->conf0.tx_rst = 0;
}

/* I2S TX data travels over the SLC RX link. */
static inline void i2s_hw_tx_intr(volatile slc_struct_t *dma, bool en)
{
    dma->int_ena.rx_eof = en ? 1 : 0;
    dma->int_ena.rx_dscr_err = en ? 1 : 0;
}

/* I2S RX data travels over the SLC TX link. */
static inline void i2s_hw_rx_intr(volatile slc_struct_t *dma, bool en)
{
    dma->int_ena.tx_suc_eof = en ? 1 : 0;
    dma->int_ena.tx_dscr_err = en ? 1 : 0;
}

/* 0 = two channels, 1 = right, 2 = left (same table as the SDK). */
static inline uint32_t i2s_chan_mod_from_fmt(i2s_channel_fmt_t cf)
{
    return (cf < I2S_CHANNEL_FMT_ONLY_RIGHT) ? (uint32_t)cf : (uint32_t)(cf >> 1);
}

/* ===========================================================================
 *  ISR
 * ======================================================================== */

static void IRAM_ATTR i2s_zero_buffer_iram(void *buf, size_t len)
{
    uint32_t *p = (uint32_t *)buf;
    size_t words = len / 4;

    for (size_t i = 0; i < words; i++)
        p[i] = 0;
}

/* Post an event, dropping the oldest one if the app is not keeping up. */
static inline void IRAM_ATTR i2s_post_event_isr(QueueHandle_t q,
                                                const i2s_event_t *evt,
                                                BaseType_t *woken)
{
    i2s_event_t discarded;

    if (xQueueIsQueueFullFromISR(q))
        (void)xQueueReceiveFromISR(q, &discarded, woken);

    (void)xQueueSendFromISR(q, evt, woken);
}

static void IRAM_ATTR i2s_intr_handler_default(void *arg)
{
    i2s_obj_t *p_i2s = (i2s_obj_t *)arg;
    volatile slc_struct_t *dma_reg = p_i2s->dma;

    BaseType_t high_priority_task_awoken = pdFALSE;
    i2s_event_t i2s_event = {0};
    void *discarded_buf = NULL;
    lldesc_t *finish_desc;

    /* Snapshot once; clear exactly these bits at the end so nothing that
     * arrives while we run is lost. */
    const uint32_t int_st = dma_reg->int_st.val;
    const uint32_t tx_done_desc = dma_reg->rx_eof_des_addr; /* I2S TX finished */
    const uint32_t rx_done_desc = dma_reg->tx_eof_des_addr; /* I2S RX finished */

    if (int_st & I2S_SLC_INT_DSCR_ERR)
    {
        if (p_i2s->i2s_queue)
        {
            i2s_event.type = I2S_EVENT_DMA_ERROR;
            i2s_event.size = 0;
            i2s_post_event_isr(p_i2s->i2s_queue, &i2s_event, &high_priority_task_awoken);
        }
        /* Deliberately fall through: the EOF handling below re-arms RX
         * descriptors (owner = 1). Skipping it would turn a transient error
         * into a permanent one. */
    }

    /* I2S TX buffer finished — return it to the free queue. */
    if ((int_st & I2S_SLC_INT_TX_DONE) && p_i2s->tx)
    {
        finish_desc = (lldesc_t *)tx_done_desc;

        if (finish_desc != NULL && finish_desc->buf_ptr != NULL)
        {
            if (xQueueIsQueueFullFromISR(p_i2s->tx->queue))
            {
                /* Underflow: nobody is consuming. Drop the oldest entry and,
                 * if requested, silence it so stale audio is not replayed. */
                BaseType_t recv_ok = xQueueReceiveFromISR(p_i2s->tx->queue,
                                                          &discarded_buf,
                                                          &high_priority_task_awoken);

                if (recv_ok == pdPASS &&
                    p_i2s->tx_desc_auto_clear &&
                    discarded_buf != NULL) /* NULL == sentinel */
                {
                    i2s_zero_buffer_iram(discarded_buf, (size_t)p_i2s->tx->buf_size);
                }
            }

            (void)xQueueSendFromISR(p_i2s->tx->queue,
                                    (void *)(&finish_desc->buf_ptr),
                                    &high_priority_task_awoken);

            if (p_i2s->i2s_queue)
            {
                i2s_event.type = I2S_EVENT_TX_DONE;
                i2s_event.size = (size_t)p_i2s->tx->buf_size;
                i2s_post_event_isr(p_i2s->i2s_queue, &i2s_event, &high_priority_task_awoken);
            }
        }
    }

    /* I2S RX buffer filled — hand it to the reader, re-arm the descriptor. */
    if ((int_st & I2S_SLC_INT_RX_DONE) && p_i2s->rx)
    {
        finish_desc = (lldesc_t *)rx_done_desc;

        if (finish_desc != NULL && finish_desc->buf_ptr != NULL)
        {
            finish_desc->owner = 1;

            if (xQueueIsQueueFullFromISR(p_i2s->rx->queue))
            {
                /* Overflow: drop the oldest filled buffer. */
                (void)xQueueReceiveFromISR(p_i2s->rx->queue,
                                           &discarded_buf,
                                           &high_priority_task_awoken);
            }

            (void)xQueueSendFromISR(p_i2s->rx->queue,
                                    (void *)(&finish_desc->buf_ptr),
                                    &high_priority_task_awoken);

            if (p_i2s->i2s_queue)
            {
                i2s_event.type = I2S_EVENT_RX_DONE;
                i2s_event.size = (size_t)p_i2s->rx->buf_size;
                i2s_post_event_isr(p_i2s->i2s_queue, &i2s_event, &high_priority_task_awoken);
            }
        }
    }

    if (high_priority_task_awoken == pdTRUE)
        portYIELD_FROM_ISR();

    dma_reg->int_clr.val = int_st;
}

/* ===========================================================================
 *  DMA queue create / destroy
 * ======================================================================== */

/* Tolerates partially built objects (used from the create failure path). */
static void i2s_destroy_dma_queue(i2s_dma_t *dma)
{
    if (dma == NULL)
        return;

    for (int i = 0; i < dma->buf_count; i++)
    {
        if (dma->desc && dma->desc[i])
            heap_caps_free(dma->desc[i]);

        if (dma->buf && dma->buf[i])
            heap_caps_free(dma->buf[i]);
    }

    if (dma->buf)
        heap_caps_free(dma->buf);

    if (dma->desc)
        heap_caps_free(dma->desc);

    if (dma->queue)
        vQueueDelete(dma->queue);

    if (dma->mux)
        vSemaphoreDelete(dma->mux);

    heap_caps_free(dma);
}

/* Builds a complete DMA ring for the given geometry. Does not touch
 * p_i2s_obj, so it is safe to call before switching state. */
static i2s_dma_t *i2s_dma_queue_create(int dma_buf_count, int dma_buf_len, int sample_size)
{
    int buf_bytes = dma_buf_len * sample_size;

    if (sample_size <= 0 || dma_buf_count < 2 || dma_buf_len <= 0 ||
        buf_bytes <= 0 || buf_bytes > I2S_MAX_DMA_BUF_BYTES || (buf_bytes % 4) != 0)
    {
        ESP_LOGE(I2S_TAG,
                 "Invalid DMA geometry: sample_size=%d, dma_buf_count=%d, dma_buf_len=%d, buf_bytes=%d",
                 sample_size, dma_buf_count, dma_buf_len, buf_bytes);
        return NULL;
    }

    i2s_dma_t *dma = (i2s_dma_t *)heap_caps_zalloc(sizeof(i2s_dma_t), MALLOC_CAP_8BIT);
    if (dma == NULL)
    {
        ESP_LOGE(I2S_TAG, "Error malloc i2s_dma_t");
        return NULL;
    }

    /* Set early so i2s_destroy_dma_queue() can clean up a partial build. */
    dma->buf_count = dma_buf_count;
    dma->buf_size = buf_bytes;

    dma->buf = (char **)heap_caps_zalloc(sizeof(char *) * dma_buf_count, MALLOC_CAP_8BIT);
    dma->desc = (lldesc_t **)heap_caps_zalloc(sizeof(lldesc_t *) * dma_buf_count, MALLOC_CAP_8BIT);

    if (dma->buf == NULL || dma->desc == NULL)
    {
        ESP_LOGE(I2S_TAG, "Error malloc dma pointer tables");
        goto fail;
    }

    for (int i = 0; i < dma_buf_count; i++)
    {
        /* MALLOC_CAP_8BIT == DRAM; the SLC cannot reach IRAM. */
        dma->buf[i] = (char *)heap_caps_calloc(1, buf_bytes, MALLOC_CAP_8BIT);
        dma->desc[i] = (lldesc_t *)heap_caps_malloc(sizeof(lldesc_t), MALLOC_CAP_8BIT);

        if (dma->buf[i] == NULL || dma->desc[i] == NULL)
        {
            ESP_LOGE(I2S_TAG, "Error malloc dma buffer/descriptor %d", i);
            goto fail;
        }

        if ((((uintptr_t)dma->buf[i]) & 3) != 0 || (((uintptr_t)dma->desc[i]) & 3) != 0)
        {
            ESP_LOGE(I2S_TAG, "DMA buffer/descriptor %d is not 4-byte aligned", i);
            goto fail;
        }

        ESP_LOGD(I2S_TAG, "Addr[%d] = %p", i, dma->buf[i]);
    }

    for (int i = 0; i < dma_buf_count; i++)
    {
        dma->desc[i]->owner = 1;
        dma->desc[i]->eof = 1; /* one interrupt per buffer */
        dma->desc[i]->sub_sof = 0;
        dma->desc[i]->datalen = buf_bytes;
        dma->desc[i]->blocksize = buf_bytes;
        dma->desc[i]->buf_ptr = (uint32_t *)dma->buf[i];
        dma->desc[i]->unused = 0;
        dma->desc[i]->next_link_ptr = (i < dma_buf_count - 1) ? dma->desc[i + 1] : dma->desc[0];
    }

    dma->queue = xQueueCreate(dma_buf_count - 1, sizeof(char *));
    dma->mux = xSemaphoreCreateMutex();

    if (dma->queue == NULL || dma->mux == NULL)
    {
        ESP_LOGE(I2S_TAG, "Error creating dma queue/mutex");
        goto fail;
    }

    dma->rw_pos = 0;
    dma->curr_ptr = NULL;

    ESP_LOGI(I2S_TAG, "DMA Malloc info, datalen=blocksize=%d, dma_buf_count=%d",
             buf_bytes, dma_buf_count);

    return dma;

fail:
    i2s_destroy_dma_queue(dma);
    return NULL;
}

/* Reset the read/write cursor (and optionally the queue) of one direction.
 * Callers guarantee no reader/writer is active, so the mux is uncontended. */
static void i2s_reset_cursor(i2s_dma_t *d, bool reset_queue)
{
    if (d == NULL)
        return;

    xSemaphoreTake(d->mux, portMAX_DELAY);

    if (reset_queue && d->queue)
        xQueueReset(d->queue);

    d->curr_ptr = NULL;
    d->rw_pos = 0;

    xSemaphoreGive(d->mux);
}

/* ===========================================================================
 *  start / stop internal (api_lock held, users drained)
 * ======================================================================== */

static esp_err_t i2s_start_internal(i2s_obj_t *obj)
{
    i2s_port_t i2s_num = obj->i2s_num;
    volatile slc_struct_t *dma = obj->dma;

    /* Cold start: forget everything the previous run left in the queues. */
    i2s_reset_cursor(obj->tx, true);
    i2s_reset_cursor(obj->rx, true);

    I2S_ENTER_CRITICAL();

    dma_intr_disable();

    i2s_hw_reset_fifo(i2s_num);
    i2s_hw_reset_dma(dma);
    i2s_hw_reset_i2s(i2s_num);
    I2S_MEMW();

    if (obj->tx)
    {
        for (int i = 0; i < obj->tx->buf_count; i++)
            obj->tx->desc[i]->owner = 1;
    }

    if (obj->rx)
    {
        for (int i = 0; i < obj->rx->buf_count; i++)
            obj->rx->desc[i]->owner = 1;
    }

    /* Drop stale flags *before* the links start so a genuine first EOF can
     * never be cleared by accident. */
    dma->int_clr.val = 0xFFFFFFFF;
    I2S_MEMW();

    if ((obj->mode & I2S_MODE_TX) && obj->tx)
    {
        i2s_hw_tx_intr(dma, true);
        dma->rx_link.start = 1;
    }

    if ((obj->mode & I2S_MODE_RX) && obj->rx)
    {
        i2s_hw_rx_intr(dma, true);
        dma->tx_link.start = 1;
    }

    I2S_MEMW();

    /* Both TX and RX are started to ensure clock generation (same as SDK);
     * DMA direction is controlled by the SLC links above. Reset both
     * simultaneously so they share the same phase. */
    I2S[i2s_num]->conf.val |= I2S_I2S_TX_START | I2S_I2S_RX_START;
    I2S_MEMW();
    I2S[i2s_num]->conf.val |= I2S_I2S_RESET_MASK;
    I2S_MEMW();
    I2S[i2s_num]->conf.val &= ~I2S_I2S_RESET_MASK;
    I2S_MEMW();

    dma_intr_enable();

    I2S_EXIT_CRITICAL();

    return ESP_OK;
}

static esp_err_t i2s_stop_internal(i2s_obj_t *obj)
{
    i2s_port_t i2s_num = obj->i2s_num;
    volatile slc_struct_t *dma = obj->dma;

    I2S_ENTER_CRITICAL();

    dma_intr_disable();

    if (obj->mode & I2S_MODE_TX)
        dma->rx_link.stop = 1;

    if (obj->mode & I2S_MODE_RX)
        dma->tx_link.stop = 1;

    I2S[i2s_num]->conf.val &= ~(I2S_I2S_TX_START | I2S_I2S_RX_START);
    I2S_MEMW();

    i2s_hw_tx_intr(dma, false);
    i2s_hw_rx_intr(dma, false);

    /* Whatever completed while we were stopping is irrelevant: every caller
     * either resets the queues on the next start or destroys them. */
    dma->int_clr.val = 0xFFFFFFFF;
    I2S_MEMW();

    I2S_EXIT_CRITICAL();

    i2s_reset_cursor(obj->tx, false);
    i2s_reset_cursor(obj->rx, false);

    return ESP_OK;
}

/* ===========================================================================
 *  Public start / stop
 * ======================================================================== */

esp_err_t i2s_start(i2s_port_t i2s_num)
{
    I2S_CHECK((i2s_num < I2S_NUM_MAX), "i2s_num error", ESP_ERR_INVALID_ARG);

    i2s_obj_t *obj = i2s_obj_acquire(i2s_num);
    I2S_CHECK(obj, "i2s not installed or being uninstalled", ESP_ERR_INVALID_STATE);

    if (xSemaphoreTake(obj->api_lock, pdMS_TO_TICKS(I2S_API_LOCK_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGE(I2S_TAG, "i2s_start: api_lock busy");
        i2s_obj_release(obj);
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    i2s_drv_state_t st = i2s_state_get(obj);

    if (st == I2S_DRV_STOPPED)
    {
        ret = i2s_start_internal(obj);
        i2s_state_set(obj, (ret == ESP_OK) ? I2S_DRV_RUNNING : I2S_DRV_STOPPED);
    }
    else if (st != I2S_DRV_RUNNING)
    {
        /* Cannot happen while api_lock is held, kept as a guard. */
        ret = ESP_ERR_INVALID_STATE;
    }
    /* Already running: not an error (idempotent, like i2s_stop()). Apps that
     * call i2s_start() right after i2s_driver_install() keep working. */

    xSemaphoreGive(obj->api_lock);
    i2s_obj_release(obj);
    return ret;
}

esp_err_t i2s_stop(i2s_port_t i2s_num)
{
    I2S_CHECK((i2s_num < I2S_NUM_MAX), "i2s_num error", ESP_ERR_INVALID_ARG);

    i2s_obj_t *obj = i2s_obj_acquire(i2s_num);
    I2S_CHECK(obj, "i2s not installed or being uninstalled", ESP_ERR_INVALID_STATE);

    if (xSemaphoreTake(obj->api_lock, pdMS_TO_TICKS(I2S_API_LOCK_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGE(I2S_TAG, "i2s_stop: api_lock busy");
        i2s_obj_release(obj);
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;

    if (i2s_state_get(obj) == I2S_DRV_RUNNING)
    {
        /* 1. no new readers/writers  2. wake + wait for the current ones
         * 3. only then touch the hardware. */
        i2s_state_set(obj, I2S_DRV_STOPPING);

        ret = i2s_drain_users(obj);
        if (ret != ESP_OK)
        {
            ESP_LOGE(I2S_TAG, "i2s_stop: drain timeout, driver left running");
            i2s_state_set(obj, I2S_DRV_RUNNING);
        }
        else
        {
            ret = i2s_stop_internal(obj);
            i2s_state_set(obj, I2S_DRV_STOPPED);
        }
    }
    /* Already stopped: not an error. */

    xSemaphoreGive(obj->api_lock);
    i2s_obj_release(obj);
    return ret;
}

/* ===========================================================================
 *  Pin config
 * ======================================================================== */

esp_err_t i2s_set_pin(i2s_port_t i2s_num, const i2s_pin_config_t *pin)
{
    I2S_CHECK((i2s_num < I2S_NUM_MAX), "i2s_num error", ESP_ERR_INVALID_ARG);
    I2S_CHECK(pin, "param null", ESP_ERR_INVALID_ARG);

    if (pin->bck_o_en > 0)
        PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDO_U, FUNC_I2SO_BCK);

    if (pin->ws_o_en > 0)
        PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U, FUNC_I2SO_WS);

    if (pin->data_out_en > 0)
        PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0RXD_U, FUNC_I2SO_DATA);

    if (pin->bck_i_en > 0)
        PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTCK_U, FUNC_I2SI_BCK);

    if (pin->ws_i_en > 0)
        PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTMS_U, FUNC_I2SI_WS);

    if (pin->data_in_en > 0)
        PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDI_U, FUNC_I2SI_DATA);

    return ESP_OK;
}

/* ===========================================================================
 *  Clock divider calculation
 * ======================================================================== */

static esp_err_t i2s_set_rate(i2s_obj_t *obj, uint32_t rate, int bits_per_sample)
{
    i2s_port_t i2s_num = obj->i2s_num;

    if (rate == 0)
        return ESP_ERR_INVALID_ARG;

    uint8_t bck_div = 1;
    uint8_t mclk_div = 1;

    /* BCK cycles per frame: 2 x 16 for 16-bit, 2 x 24 for 24-bit (bits_mod = 8). */
    float scaled_base_freq =
        (float)I2S_BASE_CLK / (bits_per_sample == I2S_BITS_PER_SAMPLE_16BIT ? 32.0f : 48.0f);

    float delta_best = FLT_MAX;

    for (uint8_t i = 1; i < 64; i++)
    {
        for (uint8_t j = i; j < 64; j++)
        {
            float new_delta = fabsf((scaled_base_freq / i / j) - (float)rate);

            if (new_delta < delta_best)
            {
                delta_best = new_delta;
                bck_div = i;
                mclk_div = j;

                if (new_delta == 0.0f)
                    goto done;
            }
        }
    }

done:
    I2S_ENTER_CRITICAL();
    I2S[i2s_num]->conf.bck_div_num = bck_div & 0x3F;
    I2S[i2s_num]->conf.clkm_div_num = mclk_div & 0x3F;
    I2S_MEMW();
    I2S_EXIT_CRITICAL();

    obj->sample_rate = rate;

    return ESP_OK;
}

/* ===========================================================================
 *  i2s_set_clk
 * ======================================================================== */

/* Core of set_clk. Preconditions: api_lock held, state is RUNNING or STOPPED.
 * Leaves the state as it found it (RUNNING is restarted), or STOPPED if the
 * restart failed. Used by i2s_set_clk(), i2s_set_sample_rates() and
 * i2s_driver_install(). */
static esp_err_t i2s_set_clk_locked(i2s_obj_t *obj,
                                    uint32_t rate,
                                    i2s_bits_per_sample_t bits,
                                    i2s_channel_t ch)
{
    i2s_port_t i2s_num = obj->i2s_num;
    i2s_drv_state_t prev_state = i2s_state_get(obj);

    /* --- Compute the new geometry without side effects. ------------------ */
    int new_bytes_per_sample = (bits == I2S_BITS_PER_SAMPLE_16BIT) ? 2 : 4;
    int new_channel_num = (ch == I2S_CHANNEL_STEREO) ? 2 : 1;
    int new_sample_size = new_bytes_per_sample * new_channel_num;

    int new_dma_buf_len = obj->dma_buf_len_orig;

    if (new_dma_buf_len * new_sample_size > I2S_MAX_DMA_BUF_BYTES)
        new_dma_buf_len = I2S_MAX_DMA_BUF_BYTES / new_sample_size;

    /* Buffer size must be a positive multiple of 4 bytes. */
    while ((new_dma_buf_len * new_sample_size) % 4 != 0 && new_dma_buf_len > 1)
        new_dma_buf_len--;

    if (new_dma_buf_len <= 0 || (new_dma_buf_len * new_sample_size) % 4 != 0)
    {
        ESP_LOGE(I2S_TAG, "cannot find a DMA buffer size that is a multiple of 4");
        return ESP_ERR_INVALID_ARG;
    }

    bool need_rebuild = ((int)bits != obj->bits_per_sample) || (new_channel_num != obj->channel_num);

    /* --- Allocate new DMA rings BEFORE stopping. OOM leaves old state intact. */
    i2s_dma_t *new_tx = NULL;
    i2s_dma_t *new_rx = NULL;

    if (need_rebuild)
    {
        if (obj->mode & I2S_MODE_TX)
        {
            new_tx = i2s_dma_queue_create(obj->dma_buf_count, new_dma_buf_len, new_sample_size);
            if (new_tx == NULL)
            {
                ESP_LOGE(I2S_TAG, "set_clk: no memory for new tx dma ring — old state preserved");
                return ESP_ERR_NO_MEM;
            }
        }

        if (obj->mode & I2S_MODE_RX)
        {
            new_rx = i2s_dma_queue_create(obj->dma_buf_count, new_dma_buf_len, new_sample_size);
            if (new_rx == NULL)
            {
                ESP_LOGE(I2S_TAG, "set_clk: no memory for new rx dma ring — old state preserved");
                i2s_destroy_dma_queue(new_tx);
                return ESP_ERR_NO_MEM;
            }
        }
    }

    /* --- Block new readers/writers, drain the current ones. --------------- */
    i2s_state_set(obj, I2S_DRV_RECONFIGURING);

    esp_err_t err = i2s_drain_users(obj);
    if (err != ESP_OK)
    {
        ESP_LOGE(I2S_TAG, "set_clk: drain timeout, aborting reconfiguration");
        i2s_destroy_dma_queue(new_tx);
        i2s_destroy_dma_queue(new_rx);
        i2s_state_set(obj, prev_state);
        return err;
    }

    /* --- Engine stopped from here on. ------------------------------------- */
    i2s_stop_internal(obj);

    err = i2s_set_rate(obj, rate, bits);
    if (err != ESP_OK)
    {
        i2s_destroy_dma_queue(new_tx);
        i2s_destroy_dma_queue(new_rx);

        if (prev_state == I2S_DRV_RUNNING)
            i2s_start_internal(obj);

        i2s_state_set(obj, prev_state);
        return err;
    }

    /* Channel count change: mono keeps its side (L/R), stereo goes back to
     * the stereo format chosen at install (RIGHT_LEFT unless the app asked
     * for ALL_LEFT/ALL_RIGHT), so mono -> stereo -> mono round-trips. */
    if (new_channel_num != obj->channel_num)
    {
        i2s_channel_fmt_t cf = obj->channel_format;

        if (new_channel_num == 2)
        {
            cf = obj->stereo_format;
        }
        else if (cf < I2S_CHANNEL_FMT_ONLY_RIGHT)
        {
            cf = (cf == I2S_CHANNEL_FMT_ALL_LEFT) ? I2S_CHANNEL_FMT_ONLY_LEFT
                                                  : I2S_CHANNEL_FMT_ONLY_RIGHT;
        }

        obj->channel_format = cf;
        obj->channel_num = new_channel_num;

        uint32_t chan_mod = i2s_chan_mod_from_fmt(cf);
        I2S[i2s_num]->conf_chan.tx_chan_mod = chan_mod;
        I2S[i2s_num]->conf_chan.rx_chan_mod = chan_mod;
        I2S_MEMW();
    }

    /* Swap DMA rings. The ISR is masked (stop_internal), so this is safe. */
    if (need_rebuild)
    {
        i2s_dma_t *old_tx = obj->tx;
        i2s_dma_t *old_rx = obj->rx;

        obj->bits_per_sample = bits;
        obj->bytes_per_sample = new_bytes_per_sample;
        obj->dma_buf_len = new_dma_buf_len;

        if (new_tx)
        {
            obj->tx = new_tx;
            obj->dma->rx_link.addr = (uint32_t)obj->tx->desc[0];
        }

        if (new_rx)
        {
            obj->rx = new_rx;
            I2S[i2s_num]->rx_eof_num = (uint32_t)obj->rx->buf_size / 4;
            obj->dma->tx_link.addr = (uint32_t)obj->rx->desc[0];
        }

        I2S_MEMW();

        if (new_tx)
            i2s_destroy_dma_queue(old_tx);
        if (new_rx)
            i2s_destroy_dma_queue(old_rx);
    }

    /* FIFO mode: bit0 = mono, bit1 = 24-bit. Derived from the final
     * (bits, channels), never from the live register. */
    {
        uint32_t fifo_mod = 0;
        if (bits >= I2S_BITS_PER_SAMPLE_24BIT)
            fifo_mod |= 2u;
        if (new_channel_num == 1)
            fifo_mod |= 1u;

        I2S[i2s_num]->fifo_conf.tx_fifo_mod = fifo_mod;
        I2S[i2s_num]->fifo_conf.rx_fifo_mod = fifo_mod;

        /* bits_mod = extra BCK cycles per channel slot on top of 16. */
        I2S[i2s_num]->conf.bits_mod = (bits == I2S_BITS_PER_SAMPLE_16BIT) ? 0 : 8;
        I2S_MEMW();
    }

    /* --- Restart only if we were running before. -------------------------- */
    err = ESP_OK;

    if (prev_state == I2S_DRV_RUNNING)
        err = i2s_start_internal(obj);

    i2s_state_set(obj, (prev_state == I2S_DRV_RUNNING && err == ESP_OK) ? I2S_DRV_RUNNING
                                                                        : I2S_DRV_STOPPED);
    return err;
}

esp_err_t i2s_set_clk(i2s_port_t i2s_num,
                      uint32_t rate,
                      i2s_bits_per_sample_t bits,
                      i2s_channel_t ch)
{
    I2S_CHECK((i2s_num < I2S_NUM_MAX), "i2s_num error", ESP_ERR_INVALID_ARG);
    I2S_CHECK((ch == I2S_CHANNEL_MONO || ch == I2S_CHANNEL_STEREO),
              "channel must be 1 (mono) or 2 (stereo)", ESP_ERR_INVALID_ARG);
    I2S_CHECK((bits == I2S_BITS_PER_SAMPLE_16BIT || bits == I2S_BITS_PER_SAMPLE_24BIT),
              "bits per sample must be 16 or 24", ESP_ERR_INVALID_ARG);
    I2S_CHECK((rate > 0), "rate must be > 0", ESP_ERR_INVALID_ARG);

    i2s_obj_t *obj = i2s_obj_acquire(i2s_num);
    I2S_CHECK(obj, "i2s not installed or being uninstalled", ESP_ERR_INVALID_STATE);

    if (xSemaphoreTake(obj->api_lock, pdMS_TO_TICKS(I2S_API_LOCK_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGE(I2S_TAG, "i2s_set_clk: api_lock busy");
        i2s_obj_release(obj);
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret;
    i2s_drv_state_t st = i2s_state_get(obj);

    if (st != I2S_DRV_RUNNING && st != I2S_DRV_STOPPED)
        ret = ESP_ERR_INVALID_STATE;
    else
        ret = i2s_set_clk_locked(obj, rate, bits, ch);

    xSemaphoreGive(obj->api_lock);
    i2s_obj_release(obj);
    return ret;
}

esp_err_t i2s_set_sample_rates(i2s_port_t i2s_num, uint32_t rate)
{
    I2S_CHECK((i2s_num < I2S_NUM_MAX), "i2s_num error", ESP_ERR_INVALID_ARG);
    I2S_CHECK((rate > 0), "rate must be > 0", ESP_ERR_INVALID_ARG);

    i2s_obj_t *obj = i2s_obj_acquire(i2s_num);
    I2S_CHECK(obj, "i2s not installed or being uninstalled", ESP_ERR_INVALID_STATE);

    if (xSemaphoreTake(obj->api_lock, pdMS_TO_TICKS(I2S_API_LOCK_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGE(I2S_TAG, "i2s_set_sample_rates: api_lock busy");
        i2s_obj_release(obj);
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret;
    i2s_drv_state_t st = i2s_state_get(obj);

    if (st != I2S_DRV_RUNNING && st != I2S_DRV_STOPPED)
    {
        ret = ESP_ERR_INVALID_STATE;
    }
    else if (obj->bytes_per_sample <= 0)
    {
        ESP_LOGE(I2S_TAG, "bits_per_sample not set");
        ret = ESP_ERR_INVALID_STATE;
    }
    else
    {
        /* bits/channels are read under the same api_lock that set_clk uses
         * to change them — no torn snapshot possible. */
        ret = i2s_set_clk_locked(obj, rate,
                                 (i2s_bits_per_sample_t)obj->bits_per_sample,
                                 (i2s_channel_t)obj->channel_num);
    }

    xSemaphoreGive(obj->api_lock);
    i2s_obj_release(obj);
    return ret;
}

/* ===========================================================================
 *  param_config (install time, api_lock held, DMA stopped)
 * ======================================================================== */

static esp_err_t i2s_param_config(i2s_obj_t *obj, const i2s_config_t *i2s_config)
{
    i2s_port_t i2s_num = obj->i2s_num;
    volatile slc_struct_t *dma = obj->dma;

    i2s_hw_reset_fifo(i2s_num);
    i2s_hw_reset_i2s(i2s_num);
    I2S_MEMW();

    /* The I2S peripheral's own interrupts are never used — only the SLC ones. */
    I2S[i2s_num]->int_ena.val = 0;

    i2s_hw_reset_dma(dma);
    I2S_MEMW();

    dma->conf0.txdata_burst_en = 0;
    dma->conf0.txdscr_burst_en = 1;

    dma->rx_dscr_conf.rx_fill_mode = 0;
    dma->rx_dscr_conf.rx_eof_mode = 0;
    dma->rx_dscr_conf.rx_fill_en = 0;
    dma->rx_dscr_conf.token_no_replace = 1;
    dma->rx_dscr_conf.infor_no_replace = 1;
    I2S_MEMW();

    I2S[i2s_num]->fifo_conf.dscr_en = 0;
    I2S_MEMW();

    /* Channel routing. fifo_mod / bits_mod are finalised by set_clk. */
    uint32_t chan_mod = i2s_chan_mod_from_fmt(i2s_config->channel_format);
    uint32_t mono = (i2s_config->channel_format < I2S_CHANNEL_FMT_ONLY_RIGHT) ? 0 : 1;

    I2S[i2s_num]->conf_chan.tx_chan_mod = chan_mod;
    I2S[i2s_num]->conf_chan.rx_chan_mod = chan_mod;
    I2S[i2s_num]->fifo_conf.tx_fifo_mod = mono;
    I2S[i2s_num]->fifo_conf.rx_fifo_mod = mono;
    I2S_MEMW();

    I2S[i2s_num]->fifo_conf.dscr_en = 1; /* connect DMA to FIFO */
    I2S_MEMW();

    I2S[i2s_num]->conf.tx_start = 0;
    I2S[i2s_num]->conf.rx_start = 0;
    I2S[i2s_num]->conf.msb_right = 1;
    I2S[i2s_num]->conf.right_first = 1;

    /* Master / slave. Both directions share the clock, so full-duplex
     * follows the MASTER/SLAVE flag for both. */
    uint32_t slave = (i2s_config->mode & I2S_MODE_SLAVE) ? 1 : 0;
    I2S[i2s_num]->conf.tx_slave_mod = slave;
    I2S[i2s_num]->conf.rx_slave_mod = slave;

    /* Data alignment. Written explicitly in both cases: the register keeps
     * its value across uninstall/install, so relying on the reset default
     * (as the SDK does) would leak the previous configuration.
     *   I2S_COMM_FORMAT_I2S                     -> 1 BCK delay (Philips)
     *   I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_LSB -> no delay (left-justified)
     *   no I2S_COMM_FORMAT_I2S flag              -> no delay (left-justified) */
    uint32_t msb_shift =
        ((i2s_config->communication_format & I2S_COMM_FORMAT_I2S) &&
         !(i2s_config->communication_format & I2S_COMM_FORMAT_I2S_LSB)) ? 1 : 0;

    I2S[i2s_num]->conf.tx_msb_shift = msb_shift;
    I2S[i2s_num]->conf.rx_msb_shift = msb_shift;
    I2S_MEMW();

    obj->tx_desc_auto_clear = i2s_config->tx_desc_auto_clear;

    return ESP_OK;
}

/* ===========================================================================
 *  zero_dma_buffer
 * ======================================================================== */

static void i2s_zero_ring(i2s_dma_t *d)
{
    if (d == NULL || d->buf == NULL || d->buf_size == 0)
        return;

    xSemaphoreTake(d->mux, portMAX_DELAY);

    for (int i = 0; i < d->buf_count; i++)
    {
        if (d->buf[i])
            memset(d->buf[i], 0, d->buf_size);
    }

    d->rw_pos = 0;
    d->curr_ptr = NULL;

    if (d->queue)
        xQueueReset(d->queue);

    xSemaphoreGive(d->mux);
}

esp_err_t i2s_zero_dma_buffer(i2s_port_t i2s_num)
{
    I2S_CHECK((i2s_num < I2S_NUM_MAX), "i2s_num error", ESP_ERR_INVALID_ARG);

    i2s_obj_t *obj = i2s_obj_acquire(i2s_num);
    I2S_CHECK(obj, "i2s not installed or being uninstalled", ESP_ERR_INVALID_STATE);

    if (xSemaphoreTake(obj->api_lock, pdMS_TO_TICKS(I2S_API_LOCK_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGE(I2S_TAG, "zero_dma_buffer: api_lock busy");
        i2s_obj_release(obj);
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    i2s_drv_state_t prev_state = i2s_state_get(obj);

    if (prev_state != I2S_DRV_RUNNING && prev_state != I2S_DRV_STOPPED)
    {
        ret = ESP_ERR_INVALID_STATE;
        goto out;
    }

    i2s_state_set(obj, I2S_DRV_RECONFIGURING);

    ret = i2s_drain_users(obj);
    if (ret != ESP_OK)
    {
        ESP_LOGE(I2S_TAG, "zero_dma_buffer: drain timeout, aborting");
        i2s_state_set(obj, prev_state);
        goto out;
    }

    i2s_stop_internal(obj);

    i2s_zero_ring(obj->rx);
    i2s_zero_ring(obj->tx);

    if (prev_state == I2S_DRV_RUNNING)
        ret = i2s_start_internal(obj);

    i2s_state_set(obj, (prev_state == I2S_DRV_RUNNING && ret == ESP_OK) ? I2S_DRV_RUNNING
                                                                        : I2S_DRV_STOPPED);

out:
    xSemaphoreGive(obj->api_lock);
    i2s_obj_release(obj);
    return ret;
}

/* ===========================================================================
 *  i2s_write / i2s_read
 * ======================================================================== */

/* A buffer popped from the DMA queue right when the driver left RUNNING:
 * put it back so the ring notification is not lost if the control
 * operation aborts (drain timeout). Same bounded-queue policy as the ISR. */
static void i2s_return_buffer(i2s_dma_t *d, void *buf, bool zero_discarded)
{
    if (uxQueueSpacesAvailable(d->queue) == 0)
    {
        void *discarded = NULL;

        if (xQueueReceive(d->queue, &discarded, 0) == pdPASS &&
            zero_discarded && discarded != NULL)
        {
            memset(discarded, 0, d->buf_size);
        }
    }

    (void)xQueueSend(d->queue, &buf, 0);
}

/* Wait for the next DMA buffer of one direction. Returns:
 *   ESP_OK             -> *out is a valid buffer
 *   ESP_ERR_TIMEOUT    -> ticks_to_wait expired
 *   ESP_ERR_INVALID_STATE -> driver left RUNNING (stop / set_clk / uninstall) */
static esp_err_t i2s_wait_buffer(i2s_obj_t *obj, bool is_tx, TickType_t ticks_to_wait, void **out)
{
    i2s_dma_t *d = is_tx ? obj->tx : obj->rx;
    volatile int *waiters = is_tx ? &obj->tx_waiters : &obj->rx_waiters;

    for (;;)
    {
        void *buf = NULL;
        bool running;

        /* State check and waiter registration are one atomic step, so the
         * control path either sees us as a waiter (and sends a sentinel) or
         * we see that it already left RUNNING. */
        I2S_ENTER_CRITICAL();
        running = (obj->drv_state == I2S_DRV_RUNNING);
        if (running)
            (*waiters)++;
        I2S_EXIT_CRITICAL();

        if (!running)
            return ESP_ERR_INVALID_STATE;

        BaseType_t ok = xQueueReceive(d->queue, &buf, ticks_to_wait);

        I2S_ENTER_CRITICAL();
        (*waiters)--;
        running = (obj->drv_state == I2S_DRV_RUNNING);
        I2S_EXIT_CRITICAL();

        if (ok != pdTRUE)
            return ESP_ERR_TIMEOUT;

        if (buf == NULL)
        {
            /* Sentinel. If the control operation that sent it is still in
             * progress we are done; if it was aborted the sentinel is stale
             * and we simply wait again. */
            if (!running)
                return ESP_ERR_INVALID_STATE;
            continue;
        }

        if (!running)
        {
            i2s_return_buffer(d, buf, is_tx && obj->tx_desc_auto_clear);
            return ESP_ERR_INVALID_STATE;
        }

        *out = buf;
        return ESP_OK;
    }
}

esp_err_t i2s_write(i2s_port_t i2s_num,
                    const void *src,
                    size_t size,
                    size_t *bytes_written,
                    TickType_t ticks_to_wait)
{
    I2S_CHECK(bytes_written, "bytes_written is NULL", ESP_ERR_INVALID_ARG);
    *bytes_written = 0;

    I2S_CHECK((i2s_num < I2S_NUM_MAX), "i2s_num error", ESP_ERR_INVALID_ARG);
    I2S_CHECK((size < I2S_MAX_BUFFER_SIZE), "size is too large", ESP_ERR_INVALID_ARG);
    I2S_CHECK((src != NULL || size == 0), "src is NULL", ESP_ERR_INVALID_ARG);

    if (size == 0)
        return ESP_OK;

    i2s_obj_t *obj = i2s_obj_acquire(i2s_num);
    I2S_CHECK(obj, "i2s not installed or being uninstalled", ESP_ERR_INVALID_STATE);

    if (!i2s_user_begin(obj, true))
    {
        i2s_obj_release(obj);
        return ESP_ERR_INVALID_STATE;
    }

    /* obj->tx cannot change while we are registered as a user. */
    i2s_dma_t *tx = obj->tx;
    if (tx == NULL)
    {
        i2s_user_end(obj, true);
        i2s_obj_release(obj);
        ESP_LOGE(I2S_TAG, "tx NULL (driver not configured for TX)");
        return ESP_ERR_INVALID_ARG;
    }

    /* Serialises concurrent writers. Bounded by the caller's timeout so a
     * second writer does not wait forever behind a blocked first one. */
    if (xSemaphoreTake(tx->mux, ticks_to_wait) != pdTRUE)
    {
        i2s_user_end(obj, true);
        i2s_obj_release(obj);
        return ESP_OK; /* timeout: 0 bytes written */
    }

    const char *src_byte = (const char *)src;

    while (size > 0)
    {
        if (tx->curr_ptr == NULL || tx->rw_pos >= tx->buf_size)
        {
            void *buf = NULL;

            if (i2s_wait_buffer(obj, true, ticks_to_wait, &buf) != ESP_OK)
                break; /* timeout or driver stopped: return what we managed */

            tx->curr_ptr = buf;
            tx->rw_pos = 0;
        }

        size_t bytes_can_write = (size_t)(tx->buf_size - tx->rw_pos);
        if (bytes_can_write > size)
            bytes_can_write = size;

        memcpy((char *)tx->curr_ptr + tx->rw_pos, src_byte, bytes_can_write);

        size -= bytes_can_write;
        src_byte += bytes_can_write;
        tx->rw_pos += (int)bytes_can_write;
        *bytes_written += bytes_can_write;
    }

    xSemaphoreGive(tx->mux);
    i2s_user_end(obj, true);
    i2s_obj_release(obj);

    return ESP_OK;
}

esp_err_t i2s_read(i2s_port_t i2s_num,
                   void *dest,
                   size_t size,
                   size_t *bytes_read,
                   TickType_t ticks_to_wait)
{
    I2S_CHECK(bytes_read, "bytes_read is NULL", ESP_ERR_INVALID_ARG);
    *bytes_read = 0;

    I2S_CHECK((i2s_num < I2S_NUM_MAX), "i2s_num error", ESP_ERR_INVALID_ARG);
    I2S_CHECK((size < I2S_MAX_BUFFER_SIZE), "size is too large", ESP_ERR_INVALID_ARG);
    I2S_CHECK((dest != NULL || size == 0), "dest is NULL", ESP_ERR_INVALID_ARG);

    if (size == 0)
        return ESP_OK;

    i2s_obj_t *obj = i2s_obj_acquire(i2s_num);
    I2S_CHECK(obj, "i2s not installed or being uninstalled", ESP_ERR_INVALID_STATE);

    if (!i2s_user_begin(obj, false))
    {
        i2s_obj_release(obj);
        return ESP_ERR_INVALID_STATE;
    }

    i2s_dma_t *rx = obj->rx;
    if (rx == NULL)
    {
        i2s_user_end(obj, false);
        i2s_obj_release(obj);
        ESP_LOGE(I2S_TAG, "rx NULL (driver not configured for RX)");
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(rx->mux, ticks_to_wait) != pdTRUE)
    {
        i2s_user_end(obj, false);
        i2s_obj_release(obj);
        return ESP_OK; /* timeout: 0 bytes read */
    }

    char *dest_byte = (char *)dest;

    while (size > 0)
    {
        if (rx->curr_ptr == NULL || rx->rw_pos >= rx->buf_size)
        {
            void *buf = NULL;

            if (i2s_wait_buffer(obj, false, ticks_to_wait, &buf) != ESP_OK)
                break;

            rx->curr_ptr = buf;
            rx->rw_pos = 0;
        }

        size_t bytes_can_read = (size_t)(rx->buf_size - rx->rw_pos);
        if (bytes_can_read > size)
            bytes_can_read = size;

        memcpy(dest_byte, (char *)rx->curr_ptr + rx->rw_pos, bytes_can_read);

        size -= bytes_can_read;
        dest_byte += bytes_can_read;
        rx->rw_pos += (int)bytes_can_read;
        *bytes_read += bytes_can_read;
    }

    xSemaphoreGive(rx->mux);
    i2s_user_end(obj, false);
    i2s_obj_release(obj);

    return ESP_OK;
}

/* ===========================================================================
 *  i2s_write_expand — chunked, no heap allocation
 * ======================================================================== */

/* 240 is a multiple of 2, 3 and 4 bytes, so any src/aim combination fills
 * whole samples. Lives on the caller's stack. */
#define I2S_EXPAND_CHUNK_BYTES 240

esp_err_t i2s_write_expand(i2s_port_t i2s_num,
                           const void *src,
                           size_t size,
                           size_t src_bits,
                           size_t aim_bits,
                           size_t *bytes_written,
                           TickType_t ticks_to_wait)
{
    I2S_CHECK(bytes_written, "bytes_written is NULL", ESP_ERR_INVALID_ARG);
    *bytes_written = 0;

    I2S_CHECK((i2s_num < I2S_NUM_MAX), "i2s_num error", ESP_ERR_INVALID_ARG);
    I2S_CHECK((size < I2S_MAX_BUFFER_SIZE), "size is too large", ESP_ERR_INVALID_ARG);
    I2S_CHECK((src != NULL || size == 0), "src is NULL", ESP_ERR_INVALID_ARG);
    I2S_CHECK((src_bits == 8 || src_bits == 16 || src_bits == 24),
              "src_bits must be 8, 16 or 24", ESP_ERR_INVALID_ARG);
    I2S_CHECK((aim_bits == 16 || aim_bits == 32),
              "aim_bits must be 16 or 32 (24-bit DMA words are 32-bit on ESP8266)",
              ESP_ERR_INVALID_ARG);
    I2S_CHECK((src_bits < aim_bits), "src_bits must be < aim_bits", ESP_ERR_INVALID_ARG);

    const size_t src_bytes = src_bits / 8;
    const size_t aim_bytes = aim_bits / 8;

    I2S_CHECK((size % src_bytes) == 0, "size must be a multiple of the source sample size",
              ESP_ERR_INVALID_ARG);

    if (size == 0)
        return ESP_OK;

    /* aim_bits must match the configured sample width (16-bit driver -> 16,
     * 24-bit driver -> 32-bit DMA words). */
    i2s_obj_t *obj = i2s_obj_acquire(i2s_num);
    I2S_CHECK(obj, "i2s not installed or being uninstalled", ESP_ERR_INVALID_STATE);

    I2S_ENTER_CRITICAL();
    int drv_bits = obj->bits_per_sample;
    I2S_EXIT_CRITICAL();
    i2s_obj_release(obj);

    if (!((drv_bits == I2S_BITS_PER_SAMPLE_16BIT && aim_bits == 16) ||
          (drv_bits == I2S_BITS_PER_SAMPLE_24BIT && aim_bits == 32)))
    {
        ESP_LOGE(I2S_TAG,
                 "i2s_write_expand: aim_bits=%u does not match driver bits=%d "
                 "(16-bit driver needs aim_bits=16, 24-bit driver needs aim_bits=32)",
                 (unsigned)aim_bits, drv_bits);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t chunk[I2S_EXPAND_CHUNK_BYTES];
    const size_t samples_per_chunk = sizeof(chunk) / aim_bytes;
    const unsigned shift = (unsigned)(aim_bits - src_bits);
    const uint8_t *src_byte = (const uint8_t *)src;
    size_t n_samples = size / src_bytes;
    esp_err_t err = ESP_OK;

    while (n_samples > 0)
    {
        size_t n = (n_samples < samples_per_chunk) ? n_samples : samples_per_chunk;
        uint8_t *dst = chunk;

        for (size_t i = 0; i < n; i++)
        {
            /* Read little-endian source sample and sign-extend it. */
            int32_t sample = 0;
            for (size_t b = 0; b < src_bytes; b++)
                sample |= (int32_t)src_byte[b] << (b * 8);

            if (sample & (1 << (src_bits - 1)))
                sample |= ~((1 << src_bits) - 1);

            /* Shift into the high bits (unsigned: no UB on negative values). */
            uint32_t expanded = (uint32_t)sample << shift;

            for (size_t b = 0; b < aim_bytes; b++)
                dst[b] = (uint8_t)(expanded >> (b * 8));

            src_byte += src_bytes;
            dst += aim_bytes;
        }

        size_t written = 0;
        err = i2s_write(i2s_num, chunk, n * aim_bytes, &written, ticks_to_wait);

        *bytes_written += (written / aim_bytes) * src_bytes;

        if (err != ESP_OK || written != n * aim_bytes)
            break; /* error, timeout or driver stopped */

        n_samples -= n;
    }

    return err;
}

/* ===========================================================================
 *  install / uninstall
 * ======================================================================== */

static void i2s_hw_shutdown(i2s_obj_t *obj)
{
    i2s_port_t i2s_num = obj->i2s_num;

    (void)i2s_stop_internal(obj); /* masks the SLC interrupt */
    dma_intr_register(NULL, NULL);

    I2S_ENTER_CRITICAL();
    i2s_hw_reset_i2s(i2s_num);
    I2S_MEMW();
    i2s_hw_reset_dma(obj->dma);
    I2S_MEMW();
    I2S_EXIT_CRITICAL();
}

/* Free everything and unpublish. Preconditions: api_lock held, object
 * published, drv_state == UNINSTALLING, users drained, no foreign refs.
 * Releases (and deletes) api_lock. */
static void i2s_obj_teardown(i2s_obj_t *obj, bool delete_event_queue, void *user_queue)
{
    i2s_port_t i2s_num = obj->i2s_num;

    i2s_hw_shutdown(obj);

    i2s_destroy_dma_queue(obj->tx);
    i2s_destroy_dma_queue(obj->rx);
    obj->tx = NULL;
    obj->rx = NULL;

    if (obj->i2s_queue != NULL)
    {
        if (delete_event_queue)
        {
            vQueueDelete(obj->i2s_queue);
            if (user_queue)
                *((QueueHandle_t *)user_queue) = NULL;
        }
        obj->i2s_queue = NULL;
    }

    SemaphoreHandle_t api_lock = obj->api_lock;
    EventGroupHandle_t idle_evt = obj->idle_evt;

    I2S_ENTER_CRITICAL();
    p_i2s_obj[i2s_num] = NULL;
    I2S_EXIT_CRITICAL();

    /* Nobody can reach the object any more (unpublished + UNINSTALLING),
     * so releasing the lock before deleting it is safe. */
    xSemaphoreGive(api_lock);

    heap_caps_free(obj);
    vEventGroupDelete(idle_evt);
    vSemaphoreDelete(api_lock);
}

/* Install failed after the object was published. api_lock is held.
 * On return api_lock has been released (and deleted, unless we had to leave
 * the object alive). */
static void i2s_cleanup_on_install_fail(i2s_obj_t *obj, void *user_queue)
{
    i2s_state_set(obj, I2S_DRV_UNINSTALLING);

    /* No user can exist (the state never was RUNNING) — this returns at once. */
    (void)i2s_drain_users(obj);

    /* Install holds no lifetime reference itself. Callers blocked on api_lock
     * hold one; they give up after I2S_API_LOCK_TIMEOUT_MS. */
    if (i2s_wait_refs(obj, 0) != ESP_OK)
    {
        /* Practically unreachable. Leave the object alive in STOPPED instead of
         * freeing it under an in-flight caller; i2s_driver_uninstall() can
         * recover it later (documented in i2s.h). */
        ESP_LOGE(I2S_TAG, "install cleanup: references did not drain, object left installed (STOPPED)");
        (void)i2s_stop_internal(obj);
        i2s_state_set(obj, I2S_DRV_STOPPED);
        xSemaphoreGive(obj->api_lock);
        return;
    }

    i2s_obj_teardown(obj, true, user_queue);
}

esp_err_t i2s_driver_uninstall(i2s_port_t i2s_num)
{
    I2S_CHECK((i2s_num < I2S_NUM_MAX), "i2s_num error", ESP_ERR_INVALID_ARG);

    i2s_obj_t *obj = i2s_obj_acquire(i2s_num);
    if (obj == NULL)
    {
        ESP_LOGW(I2S_TAG, "uninstall: not installed (or already being uninstalled)");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(obj->api_lock, pdMS_TO_TICKS(I2S_API_LOCK_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGE(I2S_TAG, "uninstall: api_lock busy");
        i2s_obj_release(obj);
        return ESP_ERR_TIMEOUT;
    }

    i2s_drv_state_t prev_state = i2s_state_get(obj);

    if (prev_state != I2S_DRV_RUNNING && prev_state != I2S_DRV_STOPPED)
    {
        /* Cannot happen while api_lock is held, kept as a guard. */
        ESP_LOGE(I2S_TAG, "uninstall: unexpected state %d", (int)prev_state);
        xSemaphoreGive(obj->api_lock);
        i2s_obj_release(obj);
        return ESP_ERR_INVALID_STATE;
    }

    /* From here on i2s_obj_acquire() fails: no new API call can start. */
    i2s_state_set(obj, I2S_DRV_UNINSTALLING);

    esp_err_t err = i2s_drain_users(obj);
    if (err == ESP_OK)
        err = i2s_wait_refs(obj, 1); /* we hold exactly one reference */

    if (err != ESP_OK)
    {
        ESP_LOGE(I2S_TAG, "uninstall: aborted, driver left in previous state");
        i2s_state_set(obj, prev_state);
        xSemaphoreGive(obj->api_lock);
        i2s_obj_release(obj);
        return err;
    }

    /* The event queue stays alive: the application owns it (see i2s.h). */
    i2s_obj_teardown(obj, false, NULL);

    return ESP_OK;
}

esp_err_t i2s_driver_install(i2s_port_t i2s_num,
                             const i2s_config_t *i2s_config,
                             int queue_size,
                             void *i2s_queue)
{
    I2S_CHECK((i2s_num < I2S_NUM_MAX), "i2s_num error", ESP_ERR_INVALID_ARG);
    I2S_CHECK((i2s_config != NULL), "I2S configuration must not be NULL", ESP_ERR_INVALID_ARG);

    I2S_CHECK((i2s_config->dma_buf_count >= 2 && i2s_config->dma_buf_count <= 128),
              "dma_buf_count must be 2..128", ESP_ERR_INVALID_ARG);
    I2S_CHECK((i2s_config->dma_buf_len >= 8 && i2s_config->dma_buf_len <= 1024),
              "dma_buf_len must be 8..1024", ESP_ERR_INVALID_ARG);
    I2S_CHECK((i2s_config->sample_rate > 0), "sample_rate must be > 0", ESP_ERR_INVALID_ARG);
    I2S_CHECK((i2s_config->mode & (I2S_MODE_TX | I2S_MODE_RX)),
              "mode must include TX and/or RX", ESP_ERR_INVALID_ARG);
    I2S_CHECK(((unsigned)i2s_config->channel_format <= (unsigned)I2S_CHANNEL_FMT_ONLY_LEFT),
              "channel_format invalid", ESP_ERR_INVALID_ARG);
    I2S_CHECK((i2s_config->bits_per_sample == I2S_BITS_PER_SAMPLE_16BIT ||
               i2s_config->bits_per_sample == I2S_BITS_PER_SAMPLE_24BIT),
              "bits_per_sample must be 16 or 24", ESP_ERR_INVALID_ARG);
    I2S_CHECK(((i2s_config->mode & I2S_MODE_MASTER) || (i2s_config->mode & I2S_MODE_SLAVE)),
              "mode must include MASTER or SLAVE", ESP_ERR_INVALID_ARG);
    I2S_CHECK(!((i2s_config->mode & I2S_MODE_MASTER) && (i2s_config->mode & I2S_MODE_SLAVE)),
              "mode must not include both MASTER and SLAVE", ESP_ERR_INVALID_ARG);
    I2S_CHECK((i2s_config->communication_format &
               (I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB | I2S_COMM_FORMAT_I2S_LSB)) != 0,
              "communication_format invalid", ESP_ERR_INVALID_ARG);
    I2S_CHECK((i2s_config->communication_format &
               ~(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB | I2S_COMM_FORMAT_I2S_LSB)) == 0,
              "communication_format has unknown bits", ESP_ERR_INVALID_ARG);

    if (i2s_queue != NULL)
        I2S_CHECK((queue_size > 0), "queue_size must be > 0 when i2s_queue is provided",
                  ESP_ERR_INVALID_ARG);

    /* Early, non-authoritative check (the authoritative one is at publish). */
    I2S_ENTER_CRITICAL();
    bool already_installed = (p_i2s_obj[i2s_num] != NULL);
    I2S_EXIT_CRITICAL();

    if (already_installed)
    {
        ESP_LOGW(I2S_TAG, "I2S driver already installed");
        return ESP_ERR_INVALID_STATE;
    }

    i2s_obj_t *obj = (i2s_obj_t *)heap_caps_zalloc(sizeof(i2s_obj_t), MALLOC_CAP_8BIT);
    I2S_CHECK(obj, "Malloc I2S driver error", ESP_ERR_NO_MEM);

    obj->i2s_num = i2s_num;
    obj->drv_state = I2S_DRV_INSTALLING;
    obj->dma = &SLC0;

    obj->api_lock = xSemaphoreCreateMutex();
    obj->idle_evt = xEventGroupCreate();

    if (obj->api_lock == NULL || obj->idle_evt == NULL)
    {
        ESP_LOGE(I2S_TAG, "Failed to create api_lock / idle_evt");
        if (obj->api_lock)
            vSemaphoreDelete(obj->api_lock);
        if (obj->idle_evt)
            vEventGroupDelete(obj->idle_evt);
        heap_caps_free(obj);
        return ESP_ERR_NO_MEM;
    }

    obj->queue_size = queue_size;
    obj->dma_buf_count = i2s_config->dma_buf_count;
    obj->dma_buf_len = i2s_config->dma_buf_len;
    obj->dma_buf_len_orig = i2s_config->dma_buf_len;
    obj->mode = i2s_config->mode;
    obj->bits_per_sample = 0; /* forces set_clk to build the DMA rings */
    obj->bytes_per_sample = 0;
    obj->channel_format = i2s_config->channel_format;
    obj->channel_num = (i2s_config->channel_format < I2S_CHANNEL_FMT_ONLY_RIGHT) ? 2 : 1;
    obj->stereo_format = (i2s_config->channel_format < I2S_CHANNEL_FMT_ONLY_RIGHT)
                             ? i2s_config->channel_format
                             : I2S_CHANNEL_FMT_RIGHT_LEFT;

    /* Hold the control lock for the whole install: a concurrent
     * i2s_start()/i2s_stop()/... simply waits and then sees the final state. */
    xSemaphoreTake(obj->api_lock, portMAX_DELAY);

    /* Publish. Authoritative "already installed" check. */
    I2S_ENTER_CRITICAL();
    bool raced = (p_i2s_obj[i2s_num] != NULL);
    if (!raced)
        p_i2s_obj[i2s_num] = obj;
    I2S_EXIT_CRITICAL();

    if (raced)
    {
        ESP_LOGW(I2S_TAG, "I2S driver already installed");
        xSemaphoreGive(obj->api_lock);
        vSemaphoreDelete(obj->api_lock);
        vEventGroupDelete(obj->idle_evt);
        heap_caps_free(obj);
        return ESP_ERR_INVALID_STATE;
    }

    dma_intr_register(i2s_intr_handler_default, obj);

    /* Enable the audio clock out of the BBPLL (i2c_bbpll_en_audio_clock_out).
     * The Wi-Fi PHY does this itself; only touch the PLL when it is down. */
    if (esp_wifi_get_state() == WIFI_STATE_DEINIT)
    {
        rom_i2c_writeReg_Mask(0x67, 4, 4, 7, 7, 1);
        I2S_MEMW();
    }

    (void)i2s_stop_internal(obj);

    esp_err_t err = i2s_param_config(obj, i2s_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(I2S_TAG, "I2S param configure error");
        i2s_cleanup_on_install_fail(obj, i2s_queue);
        return err;
    }

    if (i2s_queue)
    {
        obj->i2s_queue = xQueueCreate(queue_size, sizeof(i2s_event_t));
        if (obj->i2s_queue == NULL)
        {
            ESP_LOGE(I2S_TAG, "Failed to create i2s event queue");
            i2s_cleanup_on_install_fail(obj, i2s_queue);
            return ESP_ERR_NO_MEM;
        }

        *((QueueHandle_t *)i2s_queue) = obj->i2s_queue;
        ESP_LOGI(I2S_TAG, "queue free spaces: %d", (int)uxQueueSpacesAvailable(obj->i2s_queue));
    }

    /* set_clk needs RUNNING or STOPPED; STOPPED means "do not auto-start". */
    i2s_state_set(obj, I2S_DRV_STOPPED);

    err = i2s_set_clk_locked(obj,
                             (uint32_t)i2s_config->sample_rate,
                             i2s_config->bits_per_sample,
                             (i2s_channel_t)obj->channel_num);
    if (err != ESP_OK)
    {
        ESP_LOGE(I2S_TAG, "I2S set_clk failed during install: 0x%x", (int)err);
        i2s_cleanup_on_install_fail(obj, i2s_queue);
        return err;
    }

    err = i2s_start_internal(obj);
    if (err != ESP_OK)
    {
        ESP_LOGE(I2S_TAG, "install: i2s_start_internal failed: 0x%x", (int)err);
        i2s_cleanup_on_install_fail(obj, i2s_queue);
        return err;
    }

    i2s_state_set(obj, I2S_DRV_RUNNING);
    xSemaphoreGive(obj->api_lock);

    return ESP_OK;
}

/* ===========================================================================
 *  State introspection
 * ======================================================================== */

i2s_drv_state_t i2s_get_driver_state(i2s_port_t i2s_num)
{
    if (i2s_num >= I2S_NUM_MAX)
        return I2S_DRV_UNINIT;

    i2s_obj_t *obj = i2s_obj_acquire(i2s_num);
    if (obj == NULL)
        return I2S_DRV_UNINIT;

    i2s_drv_state_t state = i2s_state_get(obj);
    i2s_obj_release(obj);
    return state;
}
