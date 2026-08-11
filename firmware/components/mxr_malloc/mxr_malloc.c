#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#else
#include "sdkconfig.h"
#endif

#include "esp_attr.h"

#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

#ifdef CONFIG_MXR_IRAM_HOT_PATH_DISABLED
#define MXR_IRAM_ATTR
#define MXR_IRAM_INLINE_ATTR
#define MXR_IRAM_ALLOC_ATTR
#else
#define MXR_IRAM_ATTR IRAM_ATTR
#define MXR_IRAM_INLINE_ATTR IRAM_ATTR
#ifdef CONFIG_MXR_IRAM_PATH_ALLOC_FAMILY
#define MXR_IRAM_ALLOC_ATTR IRAM_ATTR
#else
#define MXR_IRAM_ALLOC_ATTR
#endif
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "mxr_malloc.h"

static const char *TAG = "mxr_malloc";

extern void vPortETSIntrLock(void);
extern void vPortETSIntrUnlock(void);

static inline void MXR_IRAM_INLINE_ATTR mxr_lock(void)
{
    vPortETSIntrLock();
}

static inline void MXR_IRAM_INLINE_ATTR mxr_unlock(void)
{
    vPortETSIntrUnlock();
}

/* ================================================================
 *  Allocator state — split descriptor arrays
 *
 *  ВАЖНО: MXR_IRAM_DATA_ATTR применяется ТОЛЬКО к массивам
 *  дескрипторов (mxr_desc_t = 2×uint32_t, доступ всегда 32-битный).
 *
 *  Все скаляры (bool, uint8_t, uint16_t, uint32_t, указатели)
 *  размещаются в DRAM (.bss), потому что:
 *    1) IRAM ESP8266 не поддерживает 8/16-битные операции —
 *       обращение из IRAM-кода к bool/uint8/uint16 в IRAM
 *       вызовет LoadStoreError;
 *    2) startup обнуляет .iram0.bss словами по 4 байта, что может
 *       затирать соседние невыровненные поля.
 * ================================================================ */
static mxr_desc_t s_dram_desc[CONFIG_MXR_MAX_DESC] MXR_IRAM_DATA_ATTR;
#ifdef CONFIG_MXR_USE_IRAM
static mxr_desc_t s_iram_desc[CONFIG_MXR_IRAM_MAX_DESC] MXR_IRAM_DATA_ATTR;
#endif

static volatile bool s_dump_in_progress;

/* Все скаляры — только DRAM (без MXR_IRAM_DATA_ATTR) */
static uint16_t s_dram_desc_count;
static uint8_t s_region_count;
static uint8_t *s_arena_base;
static uint32_t s_arena_total_bytes MXR_IRAM_DATA_ATTR;
static uint32_t s_dram_free_bytes MXR_IRAM_DATA_ATTR;
static uint32_t s_dram_min_free_bytes MXR_IRAM_DATA_ATTR;
static bool s_initialized;

static mxr_status_t s_stats MXR_IRAM_DATA_ATTR;

#ifdef CONFIG_MXR_USE_IRAM
/* ---- EXEC zone accounting (зона [0, reserve) только для EXEC) ---- */
static uint32_t s_iram_exec_free_bytes MXR_IRAM_DATA_ATTR;
static uint32_t s_iram_exec_min_free_bytes MXR_IRAM_DATA_ATTR;
static uint16_t s_iram_desc_count;
static bool s_iram_enabled;
static uint8_t *s_iram_base;
static uint32_t s_iram_total_bytes MXR_IRAM_DATA_ATTR;
static uint32_t s_iram_free_bytes MXR_IRAM_DATA_ATTR;
static uint32_t s_iram_min_free_bytes MXR_IRAM_DATA_ATTR;
static uint32_t s_iram_exec_allocs MXR_IRAM_DATA_ATTR;
static uint32_t s_iram_fallback_allocs MXR_IRAM_DATA_ATTR;

/* ---- IRAM fallback zone + regions ---- */
static uint32_t s_iram_fb_zone_start MXR_IRAM_DATA_ATTR;
static uint32_t s_iram_fb_zone_total MXR_IRAM_DATA_ATTR;
static uint8_t s_iram_fb_region_count;
static mxr_region_t s_iram_fb_region[MXR_IRAM_FB_REGION_COUNT] MXR_IRAM_DATA_ATTR;
#endif

#define MXR_ACTIVE_TOTAL_REGIONS MXR_USER_REGIONS

/* Массив регионов — только DRAM (содержит uint16_t alloc_count
 * при COMPACT_TYPES, в IRAM это LoadStoreError) */
static mxr_region_t s_region[MXR_ACTIVE_TOTAL_REGIONS] MXR_IRAM_DATA_ATTR;

static uint8_t mxr_parse_region_config(const char *s, mxr_region_cfg_t *out, uint8_t max_count);

/* ================================================================
 *  Word-aligned memory helpers (IRAM-safe, no libc)
 * ================================================================ */
static inline void MXR_IRAM_ALLOC_ATTR mxr_memset4(void *ptr, size_t bytes)
{
    uint32_t *p = (uint32_t *)ptr;
    size_t words = bytes >> 2;
    for (size_t i = 0; i < words; i++)
        p[i] = 0;
}

static inline void MXR_IRAM_ALLOC_ATTR mxr_memcpy4(void *dst, const void *src, size_t bytes)
{
    uint32_t *d = (uint32_t *)dst;
    const uint32_t *s = (const uint32_t *)src;
    size_t words = bytes >> 2;
    for (size_t i = 0; i < words; i++)
        d[i] = s[i];
}

/* ================================================================
 *  Basic conversions
 * ================================================================ */
static inline void *MXR_IRAM_INLINE_ATTR mxr_off_to_ptr(uint32_t off_bytes)
{
    return (void *)(s_arena_base + off_bytes);
}

static inline uint32_t MXR_IRAM_INLINE_ATTR mxr_ptr_to_off(const void *ptr)
{
    return (uint32_t)((const uint8_t *)ptr - s_arena_base);
}

#ifdef CONFIG_MXR_USE_IRAM
static inline void *MXR_IRAM_INLINE_ATTR mxr_iram_off_to_ptr(uint32_t off_bytes)
{
    return (void *)(s_iram_base + off_bytes);
}

static inline uint32_t MXR_IRAM_INLINE_ATTR mxr_iram_ptr_to_off(const void *ptr)
{
    return (uint32_t)((const uint8_t *)ptr - s_iram_base);
}
#endif

typedef enum
{
    MXR_ARENA_NONE = 0,
    MXR_ARENA_DRAM,
    MXR_ARENA_IRAM,
} mxr_arena_id_t;

static mxr_arena_id_t MXR_IRAM_ATTR mxr_ptr_to_arena(const void *ptr)
{
    uintptr_t p = (uintptr_t)ptr;
    uintptr_t dram_start = (uintptr_t)s_arena_base;
    uintptr_t dram_end = dram_start + (uintptr_t)s_arena_total_bytes;

    if (p >= dram_start && p < dram_end)
    {
        if ((p & MXR_ALIGN_MASK) != 0)
            return MXR_ARENA_NONE;
        return MXR_ARENA_DRAM;
    }

#ifdef CONFIG_MXR_USE_IRAM
    if (s_iram_enabled)
    {
        uintptr_t iram_start = (uintptr_t)s_iram_base;
        uintptr_t iram_end = iram_start + (uintptr_t)s_iram_total_bytes;
        if (p >= iram_start && p < iram_end)
        {
            if ((p & MXR_ALIGN_MASK) != 0)
                return MXR_ARENA_NONE;
            return MXR_ARENA_IRAM;
        }
    }
#endif
    return MXR_ARENA_NONE;
}

/* ================================================================
 *  DRAM descriptor array operations
 * ================================================================ */
static void MXR_IRAM_ATTR mxr_dram_desc_shift_right(uint16_t pos)
{
    for (uint16_t i = s_dram_desc_count; i > pos; --i)
        s_dram_desc[i] = s_dram_desc[i - 1];
}

static void MXR_IRAM_ATTR mxr_dram_desc_shift_left(int pos)
{
    for (int i = pos; i + 1 < (int)s_dram_desc_count; ++i)
        s_dram_desc[i] = s_dram_desc[i + 1];
}

static int MXR_IRAM_ATTR mxr_dram_desc_find_key(uint32_t key)
{
    int left = 0;
    int right = (int)s_dram_desc_count;
    while (left < right)
    {
        int mid = (left + right) / 2;
        uint32_t cur = s_dram_desc[mid].off_flags;
        if (cur == key)
            return mid;
        if (cur < key)
            left = mid + 1;
        else
            right = mid;
    }
    return -1;
}

static bool MXR_IRAM_ATTR mxr_dram_desc_insert(
    uint32_t off_bytes,
    uint32_t len_bytes,
    uint32_t len_flags)
{
    if (!s_initialized)
        return false;
    if (off_bytes > MXR_MAX_OFFSET_BYTES)
    {
        s_stats.desc_insert_fail_bounds++;
        return false;
    }

    if (len_bytes == 0 || len_bytes > MXR_MAX_LEN_BYTES)
    {
        s_stats.desc_insert_fail_bounds++;
        return false;
    }

    if ((uint32_t)off_bytes + len_bytes > s_arena_total_bytes)
    {
        s_stats.desc_insert_fail_bounds++;
        return false;
    }
    if (s_dram_desc_count >= CONFIG_MXR_MAX_DESC)
    {
        s_stats.alloc_fail_table_full++;
        return false;
    }

    uint32_t key = off_bytes;
    uint16_t pos;
    {
        int left = 0, right = (int)s_dram_desc_count;
        while (left < right)
        {
            int mid = (left + right) / 2;
            if (s_dram_desc[mid].off_flags < key)
                left = mid + 1;
            else
                right = mid;
        }
        pos = (uint16_t)left;
    }

    if (pos < s_dram_desc_count && s_dram_desc[pos].off_flags == key)
    {
        s_stats.desc_insert_fail_duplicate++;
        return false;
    }

    if (pos > 0)
    {
        uint32_t prev_off = mxr_desc_off(&s_dram_desc[pos - 1]);
        uint32_t prev_len = mxr_desc_len(&s_dram_desc[pos - 1]);
        if ((uint32_t)prev_off + prev_len > (uint32_t)off_bytes)
        {
            s_stats.desc_insert_fail_overlap++;
            return false;
        }
    }

    if (pos < s_dram_desc_count)
    {
        uint32_t next_off = mxr_desc_off(&s_dram_desc[pos]);
        if ((uint32_t)off_bytes + len_bytes > (uint32_t)next_off)
        {
            s_stats.desc_insert_fail_overlap++;
            return false;
        }
    }

    if (pos < s_dram_desc_count)
        mxr_dram_desc_shift_right(pos);

    mxr_desc_set(&s_dram_desc[pos], off_bytes, len_bytes, len_flags);
    s_dram_desc_count++;

    uint16_t total_active = s_dram_desc_count;
#ifdef CONFIG_MXR_USE_IRAM
    total_active += s_iram_desc_count;
#endif
    if (total_active > s_stats.max_active_allocs)
        s_stats.max_active_allocs = total_active;

    return true;
}

static void MXR_IRAM_ATTR mxr_dram_desc_remove(int index)
{
    if (index < 0 || index >= s_dram_desc_count)
        return;
    if (index < (int)s_dram_desc_count - 1)
        mxr_dram_desc_shift_left(index);
    s_dram_desc_count--;
    mxr_desc_clear(&s_dram_desc[s_dram_desc_count]);
}

/* ================================================================
 *  IRAM descriptor array operations
 * ================================================================ */
#ifdef CONFIG_MXR_USE_IRAM

static bool MXR_IRAM_ATTR mxr_iram_fb_find_free_in_region(
    int reg,
    uint32_t bytes,
    uint32_t *out_off,
    uint32_t *out_alloc_bytes);

static uint32_t MXR_IRAM_ATTR mxr_iram_fb_region_largest_free(int reg);

#if defined(CONFIG_MXR_CROSS_REGION_FALLBACK) && \
    defined(CONFIG_MXR_IRAM_CROSS_ENABLED)

static bool MXR_IRAM_ATTR mxr_iram_fb_find_free_and_largest(
    int reg,
    uint32_t bytes,
    uint32_t *out_off,
    uint32_t *out_largest,
    uint32_t *out_alloc_bytes);

static bool MXR_IRAM_ATTR mxr_iram_fb_try_cross_region(
    uint32_t bytes,
    int skip_fb_reg,
    uint32_t *out_off,
    uint32_t *out_alloc_bytes)
{
    uint8_t n = s_iram_fb_region_count;
    if (n == 0)
        return false;
    uint8_t order[MXR_IRAM_FB_REGIONS_MAX];
    uint8_t order_count = 0;
    if (skip_fb_reg < 0 || skip_fb_reg >= (int)n)
    {
        for (uint8_t i = 0; i < n; i++)
            order[order_count++] = i;
    }
    else if (skip_fb_reg < (int)(n / 2))
    {
        for (int i = skip_fb_reg + 1; i < (int)n; i++)
            order[order_count++] = (uint8_t)i;
        for (int i = skip_fb_reg - 1; i >= 0; i--)
            order[order_count++] = (uint8_t)i;
    }
    else
    {
        for (int i = skip_fb_reg - 1; i >= 0; i--)
            order[order_count++] = (uint8_t)i;
        for (int i = skip_fb_reg + 1; i < (int)n; i++)
            order[order_count++] = (uint8_t)i;
    }
    for (uint8_t k = 0; k < order_count; k++)
    {
        uint8_t i = order[k];

        /* FIX(3.2): учитываем причины пропуска */
        if (s_iram_fb_region[i].free_bytes < bytes)
        {
            s_stats.cross_free_skips++;
            continue;
        }

        if (s_iram_fb_region[i].largest_cache_valid &&
            s_iram_fb_region[i].largest_free_cache < bytes)
        {
            s_stats.cross_cache_skips++;
            continue;
        }
/* Правило 1: IRAM GUARD */
#if defined(MXR_IRAM_GUARD_NUM) && defined(MXR_IRAM_GUARD_DEN)
        if (s_iram_fb_region[i].max_bytes != MXR_REGION_MAX_UNLIMITED &&
            bytes > ((uint32_t)s_iram_fb_region[i].max_bytes * MXR_IRAM_GUARD_NUM) /
                        MXR_IRAM_GUARD_DEN)
        {
            s_stats.cross_region_guard_rejects++;
            continue;
        }
#endif
        /* Правило 2: IRAM min_bytes guard */
#if defined(MXR_IRAM_MIN_BYTES_DIVISOR)
        if (bytes < ((uint32_t)s_iram_fb_region[i].min_bytes) /
                        MXR_IRAM_MIN_BYTES_DIVISOR)
        {
            s_stats.cross_region_guard_rejects++;
            continue;
        }
#endif

        uint32_t off_bytes = 0;
        uint32_t largest = 0;
        uint32_t alloc_bytes = bytes;
        bool found = mxr_iram_fb_find_free_and_largest(
            (int)i, bytes, &off_bytes, &largest, &alloc_bytes);
        s_iram_fb_region[i].largest_free_cache = largest;
        s_iram_fb_region[i].largest_cache_valid = 1;
        if (!found)
        {
            s_stats.cross_region_skip_fragmented++;
            continue;
        }
        *out_off = off_bytes;
        *out_alloc_bytes = alloc_bytes;
        return true;
    }
    return false;
}

#endif

static uint32_t MXR_IRAM_INLINE_ATTR mxr_iram_fb_region_end(int reg)
{
    return s_iram_fb_region[reg].start_byte +
           (uint32_t)s_iram_fb_region[reg].total_bytes;
}

static inline void MXR_IRAM_INLINE_ATTR mxr_iram_fb_region_invalidate_cache(int reg)
{
    if (reg >= 0 && reg < (int)s_iram_fb_region_count)
        s_iram_fb_region[reg].largest_cache_valid = 0;
}

static void MXR_IRAM_ATTR mxr_iram_desc_shift_right(uint16_t pos)
{
    for (uint16_t i = s_iram_desc_count; i > pos; --i)
        s_iram_desc[i] = s_iram_desc[i - 1];
}

static void MXR_IRAM_ATTR mxr_iram_desc_shift_left(int pos)
{
    for (int i = pos; i + 1 < (int)s_iram_desc_count; ++i)
        s_iram_desc[i] = s_iram_desc[i + 1];
}

static int MXR_IRAM_ATTR mxr_iram_desc_find_key(uint32_t key)
{
    int left = 0;
    int right = (int)s_iram_desc_count;
    while (left < right)
    {
        int mid = (left + right) / 2;
        uint32_t cur = s_iram_desc[mid].off_flags;
        if (cur == key)
            return mid;
        if (cur < key)
            left = mid + 1;
        else
            right = mid;
    }
    return -1;
}

static bool MXR_IRAM_ATTR mxr_iram_desc_insert(
    uint32_t off_bytes,
    uint32_t len_bytes,
    uint32_t len_flags)
{
    if (!s_initialized)
        return false;
    if (!s_iram_enabled)
        return false;
    if (off_bytes > MXR_MAX_OFFSET_BYTES)
    {
        s_stats.desc_insert_fail_bounds++;
        return false;
    }
    if (len_bytes == 0 || len_bytes > MXR_MAX_LEN_BYTES)
    {
        s_stats.desc_insert_fail_bounds++;
        return false;
    }
    if ((uint32_t)off_bytes + len_bytes > s_iram_total_bytes)
    {
        s_stats.desc_insert_fail_bounds++;
        return false;
    }
    if (s_iram_desc_count >= CONFIG_MXR_IRAM_MAX_DESC)
    {
        s_stats.alloc_fail_table_full++;
        return false;
    }

    uint32_t key = off_bytes;
    uint16_t pos;
    {
        int left = 0, right = (int)s_iram_desc_count;
        while (left < right)
        {
            int mid = (left + right) / 2;
            if (s_iram_desc[mid].off_flags < key)
                left = mid + 1;
            else
                right = mid;
        }
        pos = (uint16_t)left;
    }

    if (pos < s_iram_desc_count && s_iram_desc[pos].off_flags == key)
    {
        s_stats.desc_insert_fail_duplicate++;
        return false;
    }

    if (pos > 0)
    {
        uint32_t prev_off = mxr_desc_off(&s_iram_desc[pos - 1]);
        uint32_t prev_len = mxr_desc_len(&s_iram_desc[pos - 1]);
        if ((uint32_t)prev_off + prev_len > (uint32_t)off_bytes)
        {
            s_stats.desc_insert_fail_overlap++;
            return false;
        }
    }

    if (pos < s_iram_desc_count)
    {
        uint32_t next_off = mxr_desc_off(&s_iram_desc[pos]);
        if ((uint32_t)off_bytes + len_bytes > (uint32_t)next_off)
        {
            s_stats.desc_insert_fail_overlap++;
            return false;
        }
    }

    if (pos < s_iram_desc_count)
        mxr_iram_desc_shift_right(pos);

    mxr_desc_set(&s_iram_desc[pos], off_bytes, len_bytes, len_flags);
    s_iram_desc_count++;

    uint16_t total_active = s_iram_desc_count + s_dram_desc_count;
    if (total_active > s_stats.max_active_allocs)
        s_stats.max_active_allocs = total_active;

    return true;
}

static void MXR_IRAM_ATTR mxr_iram_desc_remove(int index)
{
    if (index < 0 || index >= s_iram_desc_count)
        return;
    if (index < (int)s_iram_desc_count - 1)
        mxr_iram_desc_shift_left(index);
    s_iram_desc_count--;
    mxr_desc_clear(&s_iram_desc[s_iram_desc_count]);
}
#endif /* CONFIG_MXR_USE_IRAM */

/* ================================================================
 *  DRAM region helpers
 * ================================================================ */
static int MXR_IRAM_ATTR mxr_region_by_off(uint32_t off_bytes)
{
    for (uint8_t i = 0; i < s_region_count; i++)
    {
        uint32_t start = s_region[i].start_byte;
        uint32_t end = start + (uint32_t)s_region[i].total_bytes;
        if (off_bytes >= start && off_bytes < end)
            return i;
    }
    return -1;
}

static int MXR_IRAM_ATTR mxr_region_for_size(uint32_t len_bytes, uint32_t caps)
{
    for (uint8_t i = 0; i < s_region_count; i++)
    {
        if (((uint32_t)s_region[i].caps & caps) != caps)
            continue;
        if (len_bytes < (uint32_t)s_region[i].min_bytes)
            continue;
        if (s_region[i].max_bytes != MXR_REGION_MAX_UNLIMITED)
        {
            if (len_bytes > (uint32_t)s_region[i].max_bytes)
                continue;
        }
        return i;
    }

    return -1;
}

static bool MXR_IRAM_ATTR mxr_region_caps_ok(int region_index, uint32_t caps)
{
    if (region_index < 0 || region_index >= s_region_count)
        return false;
    return ((uint32_t)s_region[region_index].caps & caps) == caps;
}

static bool MXR_IRAM_ATTR mxr_region_size_ok(int region_index, uint32_t bytes)
{
    if (region_index < 0 || region_index >= s_region_count)
        return false;
    if (bytes == 0)
        return false;
    if (bytes < (uint32_t)s_region[region_index].min_bytes)
        return false;
    if (s_region[region_index].max_bytes != MXR_REGION_MAX_UNLIMITED)
    {
        if (bytes > (uint32_t)s_region[region_index].max_bytes)
            return false;
    }
    return true;
}

static uint32_t MXR_IRAM_ATTR mxr_region_largest_free_bytes(uint8_t region_index)
{
    if (region_index >= s_region_count)
        return 0;

    uint32_t region_start = s_region[region_index].start_byte;
    uint32_t region_end = region_start + (uint32_t)s_region[region_index].total_bytes;
    uint32_t cur = region_start;
    uint32_t largest = 0;

    for (uint16_t i = 0; i < s_dram_desc_count; i++)
    {
        uint32_t off = mxr_desc_off(&s_dram_desc[i]);
        uint32_t len = mxr_desc_len(&s_dram_desc[i]);
        uint32_t block_end = off + (uint32_t)len;

        if (block_end <= region_start)
            continue;
        if (off >= region_end)
            break;

        if (off > cur)
        {
            uint32_t gap = (uint32_t)(off - cur);
            if (gap > largest)
                largest = gap;
        }
        if (block_end > cur)
            cur = block_end;
        if (cur >= region_end)
            break;
    }

    if (region_end > cur)
    {
        uint32_t gap = (uint32_t)(region_end - cur);
        if (gap > largest)
            largest = gap;
    }
    return largest;
}

static inline void MXR_IRAM_INLINE_ATTR mxr_region_invalidate_cache(int region_index)
{
    if (region_index >= 0 && region_index < s_region_count)
        s_region[region_index].largest_cache_valid = 0;
}

static void MXR_IRAM_ATTR mxr_region_allocated(int region_index, uint32_t bytes)
{
    if (region_index >= 0 && region_index < s_region_count)
    {
        if (s_region[region_index].free_bytes >= bytes)
            s_region[region_index].free_bytes -= bytes;
        else
            s_region[region_index].free_bytes = 0;
        if (s_region[region_index].free_bytes < s_region[region_index].min_free_bytes)
            s_region[region_index].min_free_bytes = s_region[region_index].free_bytes;
    }

    if (s_dram_free_bytes >= bytes)
        s_dram_free_bytes -= bytes;
    else
        s_dram_free_bytes = 0;
    if (s_dram_free_bytes < s_dram_min_free_bytes)
        s_dram_min_free_bytes = s_dram_free_bytes;

    if (s_stats.free_bytes >= (size_t)bytes)
        s_stats.free_bytes -= (size_t)bytes;
    else
        s_stats.free_bytes = 0;
    if (s_stats.free_bytes < s_stats.min_free_bytes)
        s_stats.min_free_bytes = s_stats.free_bytes;

    mxr_region_invalidate_cache(region_index);
}

static void MXR_IRAM_ATTR mxr_region_released(int region_index, uint32_t bytes)
{
    if (region_index >= 0 && region_index < s_region_count)
    {
        uint32_t new_free = s_region[region_index].free_bytes + bytes;
        if (new_free > s_region[region_index].total_bytes)
            new_free = s_region[region_index].total_bytes;
        s_region[region_index].free_bytes = new_free;
    }

    uint32_t new_dram_free = s_dram_free_bytes + bytes;
    if (new_dram_free > s_arena_total_bytes)
        new_dram_free = s_arena_total_bytes;
    s_dram_free_bytes = new_dram_free;

    s_stats.free_bytes += (size_t)bytes;
    if (s_stats.free_bytes > s_stats.total_bytes)
        s_stats.free_bytes = s_stats.total_bytes;

    mxr_region_invalidate_cache(region_index);
}

/* ================================================================
 *  DRAM free-block search — BEST-FIT с early-exit
 *
 *  Ищет gap >= bytes. Если найден gap с waste <= bytes >> WASTE_SHIFT,
 *  возвращает его немедленно (early-exit). Иначе запоминает лучший
 *  (наименьший подходящий) gap и продолжает поиск.
 *
 *  Anti-sliver: если лучший gap имеет waste < MXR_MIN_SLICE_BYTES,
 *  выходной размер *out_alloc_bytes расширяется до полного gap,
 *  чтобы не оставлять неиспользуемый осколок.
 * ================================================================ */
static bool MXR_IRAM_ATTR mxr_find_best_free(
    int region_index,
    uint32_t bytes,
    uint32_t *out_off,
    uint32_t *out_alloc_bytes,
    uint32_t *out_largest,
    bool *out_largest_exact)
{
    uint32_t region_start = s_region[region_index].start_byte;
    uint32_t region_end = region_start + (uint32_t)s_region[region_index].total_bytes;

    /* ===== ДОБАВЛЕНО: лимит расширения ===== */
    uint32_t max_allowed = s_region[region_index].max_bytes;
    /* ======================================= */

    uint32_t cur = region_start;
    uint32_t best_off = 0;
    uint32_t best_gap = UINT32_MAX;
    bool found = false;
    uint32_t largest = 0;
#if MXR_EARLY_EXIT_ACTIVE
    uint32_t waste_limit = bytes >> MXR_BEST_FIT_WASTE_SHIFT;
    if (waste_limit < MXR_ALIGN_SIZE)
        waste_limit = MXR_ALIGN_SIZE;
#else
    /* Строгий best-fit: ранний выход только при точном совпадении
     * (waste == 0) — лучше найти невозможно. */
    uint32_t waste_limit = 0;
#endif

    for (uint16_t i = 0; i < s_dram_desc_count; i++)
    {
        uint32_t off = mxr_desc_off(&s_dram_desc[i]);
        uint32_t len = mxr_desc_len(&s_dram_desc[i]);
        uint32_t block_end = off + (uint32_t)len;
        if (block_end <= region_start)
            continue;
        if (off >= region_end)
            break;
        if (off > cur)
        {
            uint32_t gap = (uint32_t)(off - cur);
            if (gap > largest)
                largest = gap;
            if (gap >= bytes)
            {
                uint32_t waste = gap - bytes;
                if (waste <= waste_limit)
                {
                    *out_off = cur;
                    /* ===== ИСПРАВЛЕНО: ограничение max_bytes ===== */
                    if (MXR_IS_SLIVER(waste) &&
                        (max_allowed == MXR_REGION_MAX_UNLIMITED || gap <= max_allowed))
                    {
                        *out_alloc_bytes = gap;
                        s_stats.anti_sliver_expansions++;
                    }
                    else
                    {
                        *out_alloc_bytes = bytes;
                    }
                    /* ================================================ */
                    s_stats.best_fit_early_exits++;
                    if (out_largest)
                        *out_largest = largest;
                    if (out_largest_exact)
                        *out_largest_exact = false;
                    return true;
                }
                if (gap < best_gap || (gap == best_gap && cur < best_off))
                {
                    best_gap = gap;
                    best_off = cur;
                    found = true;
                }
            }
        }
        if (block_end > cur)
            cur = block_end;
        if (cur >= region_end)
            break;
    }

    /* Хвост региона */
    if (region_end > cur)
    {
        uint32_t gap = (uint32_t)(region_end - cur);
        if (gap > largest)
            largest = gap;
        if (gap >= bytes)
        {
            uint32_t waste = gap - bytes;
            if (waste <= waste_limit)
            {
                *out_off = cur;
                /* ===== ИСПРАВЛЕНО: ограничение max_bytes ===== */
                if (MXR_IS_SLIVER(waste) &&
                    (max_allowed == MXR_REGION_MAX_UNLIMITED || gap <= max_allowed))
                {
                    *out_alloc_bytes = gap;
                    s_stats.anti_sliver_expansions++;
                }
                else
                {
                    *out_alloc_bytes = bytes;
                }
                /* ================================================ */
                s_stats.best_fit_early_exits++;
                if (out_largest)
                    *out_largest = largest;
                if (out_largest_exact)
                    *out_largest_exact = false;
                return true;
            }
            if (gap < best_gap || (gap == best_gap && cur < best_off))
            {
                best_gap = gap;
                best_off = cur;
                found = true;
            }
        }
    }

    if (out_largest)
        *out_largest = largest;
    if (out_largest_exact)
        *out_largest_exact = true;
    if (found)
    {
        *out_off = best_off;
        uint32_t waste = best_gap - bytes;
        /* ===== ИСПРАВЛЕНО: ограничение max_bytes ===== */
        if (MXR_IS_SLIVER(waste) &&
            (max_allowed == MXR_REGION_MAX_UNLIMITED || best_gap <= max_allowed))
        {
            *out_alloc_bytes = best_gap;
            s_stats.anti_sliver_expansions++;
        }
        else
        {
            *out_alloc_bytes = bytes;
        }
        /* ================================================ */
        return true;
    }
    return false;
}

#if defined(CONFIG_MXR_CROSS_REGION_FALLBACK) && \
    defined(CONFIG_MXR_DRAM_CROSS_ENABLED)
static bool MXR_IRAM_ATTR mxr_find_free_and_largest(
    int region_index,
    uint32_t bytes,
    uint32_t *out_off,
    uint32_t *out_largest,
    uint32_t *out_alloc_bytes)
{
    uint32_t region_start = s_region[region_index].start_byte;
    uint32_t region_end = region_start + (uint32_t)s_region[region_index].total_bytes;
    uint32_t cur = region_start;
    uint32_t largest = 0;
    uint32_t best_off = 0;
    uint32_t best_gap = UINT32_MAX;
    bool found = false;

#if MXR_EARLY_EXIT_ACTIVE
    uint32_t waste_limit = bytes >> MXR_BEST_FIT_WASTE_SHIFT;
    if (waste_limit < MXR_ALIGN_SIZE)
        waste_limit = MXR_ALIGN_SIZE;
#else
    /* Строгий best-fit: ранний выход только при точном совпадении
     * (waste == 0) — лучше найти невозможно. */
    uint32_t waste_limit = 0;
#endif

    for (uint16_t i = 0; i < s_dram_desc_count; i++)
    {
        uint32_t off = mxr_desc_off(&s_dram_desc[i]);
        uint32_t len = mxr_desc_len(&s_dram_desc[i]);
        uint32_t block_end = off + (uint32_t)len;

        if (block_end <= region_start)
            continue;
        if (off >= region_end)
            break;

        if (off > cur)
        {
            uint32_t gap = (uint32_t)(off - cur);
            if (gap > largest)
                largest = gap;
            if (gap >= bytes)
            {
                uint32_t waste = gap - bytes;
                if (!found && waste <= waste_limit)
                {
                    /* Early-exit */
                    best_off = cur;
                    best_gap = gap;
                    found = true;
                    /* Не прерываем — нужно досчитать largest */
                }
                else if (!found || gap < best_gap || (gap == best_gap && cur < best_off))
                {
                    best_off = cur;
                    best_gap = gap;
                    found = true;
                }
            }
        }
        if (block_end > cur)
            cur = block_end;
        if (cur >= region_end)
            break;
    }

    if (region_end > cur)
    {
        uint32_t gap = (uint32_t)(region_end - cur);
        if (gap > largest)
            largest = gap;
        if (gap >= bytes)
        {
            uint32_t waste = gap - bytes;
            if (!found && waste <= waste_limit)
            {
                best_off = cur;
                best_gap = gap;
                found = true;
            }
            else if (!found || gap < best_gap || (gap == best_gap && cur < best_off))
            {
                best_off = cur;
                best_gap = gap;
                found = true;
            }
        }
    }

    if (out_largest)
        *out_largest = largest;

    if (found)
    {
        *out_off = best_off;
        uint32_t waste = best_gap - bytes;
        /* ===== ИСПРАВЛЕНО: ограничение max_bytes + счётчик ===== */
        uint32_t max_allowed = s_region[region_index].max_bytes;
        if (MXR_IS_SLIVER(waste) &&
            (max_allowed == MXR_REGION_MAX_UNLIMITED || best_gap <= max_allowed))
        {
            *out_alloc_bytes = best_gap;
            s_stats.anti_sliver_expansions++; /* ДОБАВЛЕНО */
        }
        else
        {
            *out_alloc_bytes = bytes;
        }
        /* ========================================================= */
    }
    return found;
}

#endif

static bool MXR_IRAM_ATTR mxr_try_alloc_region(
    int region_index,
    uint32_t bytes,
    uint32_t *out_off,
    uint32_t *out_alloc_bytes)
{
    if (region_index < 0 || region_index >= s_region_count)
        return false;

    if (bytes == 0 || bytes > MXR_MAX_LEN_BYTES)
        return false;
    /* FIX: запрет размещения блока меньше min_bytes региона */
    if (bytes < (uint32_t)s_region[region_index].min_bytes)
        return false;
    /* FIX(2.1): запрет размещения блока выше max_bytes региона */
    if (s_region[region_index].max_bytes != MXR_REGION_MAX_UNLIMITED &&
        bytes > (uint32_t)s_region[region_index].max_bytes)
    {
        return false;
    }

    if (s_region[region_index].free_bytes < bytes)
        return false;

    /* Быстрая проверка кэша */
    if (s_region[region_index].largest_cache_valid &&
        s_region[region_index].largest_free_cache < bytes)
    {
        return false;
    }

    uint32_t largest = 0;
    bool largest_exact = false;

    bool ok = mxr_find_best_free(
        region_index,
        bytes,
        out_off,
        out_alloc_bytes,
        &largest,
        &largest_exact);

    /*
     * Если largest был посчитан полным проходом,
     * сохраняем кэш.
     */
    if (largest_exact)
    {
        s_region[region_index].largest_free_cache = largest;
        s_region[region_index].largest_cache_valid = 1;
    }
    else
    {
        /*
         * После early-exit точный largest неизвестен.
         * Лучше оставить кэш невалидным, чем рисковать.
         */
        s_region[region_index].largest_cache_valid = 0;
    }

    return ok;
}
/* ================================================================
 *  IRAM helpers
 * ================================================================ */
#ifdef CONFIG_MXR_USE_IRAM

/*
 * Last-fit search inside one fallback region.
 * Keeps fallback blocks away from the EXEC zone at the start of IRAM.
 */
static bool MXR_IRAM_ATTR mxr_iram_fb_find_free_in_region(
    int reg,
    uint32_t bytes,
    uint32_t *out_off,
    uint32_t *out_alloc_bytes)
{
    if (reg < 0 || reg >= (int)s_iram_fb_region_count)
        return false;
    uint32_t reg_start = s_iram_fb_region[reg].start_byte;
    uint32_t reg_end = mxr_iram_fb_region_end(reg);

    /* ===== ДОБАВЛЕНО ===== */
    uint32_t max_allowed = s_iram_fb_region[reg].max_bytes;
    /* ====================== */

    if (s_iram_fb_region[reg].free_bytes < bytes)
        return false;
    uint32_t candidate_end = reg_end;
    for (int i = (int)s_iram_desc_count - 1; i >= 0; i--)
    {
        uint32_t off = mxr_desc_off(&s_iram_desc[i]);
        uint32_t len = mxr_desc_len(&s_iram_desc[i]);
        uint32_t block_end = off + (uint32_t)len;
        if (block_end <= reg_start)
            break;
        if (off >= reg_end)
            continue;
        if (candidate_end > block_end)
        {
            uint32_t gap = (uint32_t)(candidate_end - block_end);
            if (gap >= bytes)
            {
                uint32_t waste = gap - bytes;
                /* ===== ИСПРАВЛЕНО ===== */
                if (MXR_IS_SLIVER(waste) &&
                    (max_allowed == MXR_REGION_MAX_UNLIMITED || gap <= max_allowed))
                {
                    *out_off = block_end;
                    *out_alloc_bytes = gap;
                    s_stats.anti_sliver_expansions++;
                }
                else
                {
                    *out_off = candidate_end - (uint32_t)bytes;
                    *out_alloc_bytes = bytes;
                }
                /* ======================= */
                return true;
            }
        }
        if (off < candidate_end)
            candidate_end = off;
        if (candidate_end <= reg_start)
            break;
    }
    if (candidate_end > reg_start)
    {
        uint32_t gap = (uint32_t)(candidate_end - reg_start);
        if (gap >= bytes)
        {
            uint32_t waste = gap - bytes;
            /* ===== ИСПРАВЛЕНО ===== */
            if (MXR_IS_SLIVER(waste) &&
                (max_allowed == MXR_REGION_MAX_UNLIMITED || gap <= max_allowed))
            {
                *out_off = reg_start;
                *out_alloc_bytes = gap;
                s_stats.anti_sliver_expansions++;
            }
            else
            {
                *out_off = candidate_end - (uint32_t)bytes;
                *out_alloc_bytes = bytes;
            }
            /* ======================= */
            return true;
        }
    }
    return false;
}

#if defined(CONFIG_MXR_CROSS_REGION_FALLBACK) && \
    defined(CONFIG_MXR_IRAM_CROSS_ENABLED)
/* ================================================================
 *  IRAM fb free-block search + largest in one pass (last-fit)
 *
 *  Объединяет mxr_iram_fb_find_free_in_region и
 *  mxr_iram_fb_region_largest_free в один проход.
 *  Идёт с конца (last-fit), как оригинальная функция.
 * ================================================================ */
static bool MXR_IRAM_ATTR mxr_iram_fb_find_free_and_largest(
    int reg,
    uint32_t bytes,
    uint32_t *out_off,
    uint32_t *out_largest,
    uint32_t *out_alloc_bytes) /* НОВЫЙ параметр */
{
    if (reg < 0 || reg >= (int)s_iram_fb_region_count)
        return false;
    uint32_t reg_start = s_iram_fb_region[reg].start_byte;
    uint32_t reg_end = mxr_iram_fb_region_end(reg);
    uint32_t max_allowed = s_iram_fb_region[reg].max_bytes;

    if (s_iram_fb_region[reg].free_bytes < bytes)
        return false;
    uint32_t candidate_end = reg_end;
    uint32_t largest = 0;
    bool found = false;
    for (int i = (int)s_iram_desc_count - 1; i >= 0; i--)
    {
        uint32_t off = mxr_desc_off(&s_iram_desc[i]);
        uint32_t len = mxr_desc_len(&s_iram_desc[i]);
        uint32_t block_end = off + (uint32_t)len;
        if (block_end <= reg_start)
            break;
        if (off >= reg_end)
            continue;
        if (candidate_end > block_end)
        {
            uint32_t gap = (uint32_t)(candidate_end - block_end);
            if (gap > largest)
                largest = gap;
            if (!found && gap >= bytes)
            {
                uint32_t waste = gap - bytes;
                /* ===== ИСПРАВЛЕНО ===== */
                if (MXR_IS_SLIVER(waste) &&
                    (max_allowed == MXR_REGION_MAX_UNLIMITED || gap <= max_allowed))
                {
                    *out_off = block_end;
                    *out_alloc_bytes = gap;
                    s_stats.anti_sliver_expansions++; /* ДОБАВЛЕНО */
                }
                else
                {
                    *out_off = candidate_end - (uint32_t)bytes;
                    *out_alloc_bytes = bytes;
                }
                found = true;
            }
        }
        if (off < candidate_end)
            candidate_end = off;
        if (candidate_end <= reg_start)
            break;
    }
    if (candidate_end > reg_start)
    {
        uint32_t gap = (uint32_t)(candidate_end - reg_start);
        if (gap > largest)
            largest = gap;
        if (!found && gap >= bytes)
        {
            uint32_t waste = gap - bytes;
            /* ===== ИСПРАВЛЕНО ===== */
            if (MXR_IS_SLIVER(waste) &&
                (max_allowed == MXR_REGION_MAX_UNLIMITED || gap <= max_allowed))
            {
                *out_off = reg_start;
                *out_alloc_bytes = gap;
                s_stats.anti_sliver_expansions++; /* ДОБАВЛЕНО */
            }
            else
            {
                *out_off = candidate_end - (uint32_t)bytes;
                *out_alloc_bytes = bytes;
            }
            found = true;
        }
    }
    if (out_largest)
        *out_largest = largest;
    return found;
}

#endif

static inline uint32_t MXR_IRAM_INLINE_ATTR mxr_iram_reserve_bytes(void)
{
    uint32_t reserve = CONFIG_MXR_IRAM_RESERVE_BYTES;
    return (uint32_t)mxr_align4(reserve);
}

/* Верхняя граница EXEC-зоны. EXEC-блоки жёстко ограничены
 * диапазоном [0, reserve). reserve == 0 → EXEC-зоны нет. */
static inline uint32_t MXR_IRAM_INLINE_ATTR mxr_iram_exec_zone_end(void)
{
    return s_iram_fb_zone_start; /* == align4(CONFIG_MXR_IRAM_RESERVE_BYTES) */
}

/* ================================================================
 *  FIX(2.2): zone-aware largest for IRAM
 * ================================================================ */
static uint32_t MXR_IRAM_ATTR mxr_iram_exec_largest_free(void)
{
    if (!s_iram_enabled)
        return 0;

    uint32_t zone_end = mxr_iram_exec_zone_end();
    if (zone_end == 0)
        return 0;

    uint32_t cur = 0;
    uint32_t largest = 0;

    for (uint16_t i = 0; i < s_iram_desc_count; i++)
    {
        uint32_t off = mxr_desc_off(&s_iram_desc[i]);
        uint32_t len = mxr_desc_len(&s_iram_desc[i]);
        uint32_t block_end = off + (uint32_t)len;

        if (off >= zone_end)
            break;

        if (off > cur)
        {
            uint32_t gap = (uint32_t)(off - cur);
            if (gap > largest)
                largest = gap;
        }

        if (block_end > cur)
            cur = block_end;

        if (cur >= zone_end)
            break;
    }

    if (zone_end > cur)
    {
        uint32_t gap = (uint32_t)(zone_end - cur);
        if (gap > largest)
            largest = gap;
    }

    return largest;
}

static uint32_t MXR_IRAM_ATTR mxr_iram_largest_free_zone_aware(void)
{
    if (!s_iram_enabled)
        return 0;

    uint32_t largest = mxr_iram_exec_largest_free();

#ifdef CONFIG_MXR_IRAM_FALLBACK_ENABLED

    for (uint8_t i = 0; i < s_iram_fb_region_count; i++)
    {
        uint32_t lr = mxr_iram_fb_region_largest_free((int)i);

        if (s_iram_fb_region[i].max_bytes != MXR_REGION_MAX_UNLIMITED &&
            lr > (uint32_t)s_iram_fb_region[i].max_bytes)
        {
            lr = (uint32_t)s_iram_fb_region[i].max_bytes;
        }

        if (lr > largest)
            largest = lr;
    }
#endif

    return largest;
}

/* ================================================================
 *  IRAM fallback region helpers
 * ================================================================ */
static int MXR_IRAM_ATTR mxr_iram_fb_region_by_off(uint32_t off_bytes)
{
    for (uint8_t i = 0; i < s_iram_fb_region_count; i++)
    {
        uint32_t start = s_iram_fb_region[i].start_byte;
        uint32_t end = start + (uint32_t)s_iram_fb_region[i].total_bytes;
        if (off_bytes >= start && off_bytes < end)
            return (int)i;
    }
    return -1;
}

static int MXR_IRAM_ATTR mxr_iram_fb_region_for_size(uint32_t bytes)
{
    for (uint8_t i = 0; i < s_iram_fb_region_count; i++)
    {
        if (bytes < (uint32_t)s_iram_fb_region[i].min_bytes)
            continue;
        if (s_iram_fb_region[i].max_bytes != MXR_REGION_MAX_UNLIMITED &&
            bytes > (uint32_t)s_iram_fb_region[i].max_bytes)
            continue;
        return (int)i;
    }
    if (s_iram_fb_region_count > 0)
    {
        int last = (int)s_iram_fb_region_count - 1;
        if (bytes >= (uint32_t)s_iram_fb_region[last].min_bytes &&
            (s_iram_fb_region[last].max_bytes == MXR_REGION_MAX_UNLIMITED ||
             bytes <= (uint32_t)s_iram_fb_region[last].max_bytes))
        {
            return last;
        }
        if (bytes >= (uint32_t)s_iram_fb_region[0].min_bytes &&
            (s_iram_fb_region[0].max_bytes == MXR_REGION_MAX_UNLIMITED ||
             bytes <= (uint32_t)s_iram_fb_region[0].max_bytes))
        {
            return 0;
        }
    }
    return -1;
}

static bool MXR_IRAM_ATTR mxr_iram_fb_region_size_ok(int reg, uint32_t bytes)
{
    if (reg < 0 || reg >= (int)s_iram_fb_region_count)
        return false;
    if (bytes < (uint32_t)s_iram_fb_region[reg].min_bytes)
        return false;
    if (s_iram_fb_region[reg].max_bytes != MXR_REGION_MAX_UNLIMITED &&
        bytes > (uint32_t)s_iram_fb_region[reg].max_bytes)
        return false;
    return true;
}

static uint32_t MXR_IRAM_ATTR mxr_iram_fb_region_largest_free(int reg)
{
    if (reg < 0 || reg >= (int)s_iram_fb_region_count)
        return 0;

    uint32_t reg_start = s_iram_fb_region[reg].start_byte;
    uint32_t reg_end = mxr_iram_fb_region_end(reg);
    uint32_t cur = reg_start;
    uint32_t largest = 0;

    for (uint16_t i = 0; i < s_iram_desc_count; i++)
    {
        uint32_t off = mxr_desc_off(&s_iram_desc[i]);
        uint32_t len = mxr_desc_len(&s_iram_desc[i]);
        uint32_t block_end = off + (uint32_t)len;

        if (block_end <= reg_start)
            continue;
        if (off >= reg_end)
            break;

        if (off > cur)
        {
            uint32_t gap = (uint32_t)(off - cur);
            if (gap > largest)
                largest = gap;
        }
        if (block_end > cur)
            cur = block_end;
        if (cur >= reg_end)
            break;
    }

    if (reg_end > cur)
    {
        uint32_t gap = (uint32_t)(reg_end - cur);
        if (gap > largest)
            largest = gap;
    }
    return largest;
}

static void MXR_IRAM_ATTR mxr_iram_fb_region_allocated(int reg, uint32_t bytes,
                                                       bool count_block)
{
    if (reg < 0 || reg >= (int)s_iram_fb_region_count)
        return;
    if (s_iram_fb_region[reg].free_bytes >= bytes)
        s_iram_fb_region[reg].free_bytes -= bytes;
    else
        s_iram_fb_region[reg].free_bytes = 0;
    if (s_iram_fb_region[reg].free_bytes < s_iram_fb_region[reg].min_free_bytes)
        s_iram_fb_region[reg].min_free_bytes = s_iram_fb_region[reg].free_bytes;
    if (count_block)
        s_iram_fb_region[reg].alloc_count++;
    mxr_iram_fb_region_invalidate_cache(reg);
}

static void MXR_IRAM_ATTR mxr_iram_fb_region_released(int reg, uint32_t bytes,
                                                      bool count_block)
{
    if (reg < 0 || reg >= (int)s_iram_fb_region_count)
        return;
    uint32_t nf = s_iram_fb_region[reg].free_bytes + bytes;
    if (nf > s_iram_fb_region[reg].total_bytes)
        nf = s_iram_fb_region[reg].total_bytes;
    s_iram_fb_region[reg].free_bytes = nf;
    if (count_block && s_iram_fb_region[reg].alloc_count > 0)
        s_iram_fb_region[reg].alloc_count--;
    mxr_iram_fb_region_invalidate_cache(reg);
}

/* ================================================================
 *  IRAM global accounting (region-aware via offset)
 *
 *  ИСПРАВЛЕНО: добавлен параметр is_exec.
 *  - EXEC-блоки учитываются через overlap-функции
 *    mxr_iram_fb_account_alloc/_free (EXEC может пересекать
 *    границы fb-регионов).
 *  - Fallback-блоки учитываются через mxr_iram_fb_region_allocated/
 *    _released (полностью внутри одного региона).
 * ================================================================ */
static void MXR_IRAM_ATTR mxr_iram_allocated(uint32_t off_bytes,
                                             uint32_t bytes,
                                             bool is_exec, bool count_block)
{
    if (s_iram_free_bytes >= bytes)
        s_iram_free_bytes -= bytes;
    else
        s_iram_free_bytes = 0;
    if (s_iram_free_bytes < s_iram_min_free_bytes)
        s_iram_min_free_bytes = s_iram_free_bytes;

    if (s_stats.free_bytes >= (size_t)bytes)
        s_stats.free_bytes -= (size_t)bytes;
    else
        s_stats.free_bytes = 0;
    if (s_stats.free_bytes < s_stats.min_free_bytes)
        s_stats.min_free_bytes = s_stats.free_bytes;

    if (is_exec)
    {
        if (s_iram_exec_free_bytes >= bytes)
            s_iram_exec_free_bytes -= bytes;
        else
            s_iram_exec_free_bytes = 0;
        if (s_iram_exec_free_bytes < s_iram_exec_min_free_bytes)
            s_iram_exec_min_free_bytes = s_iram_exec_free_bytes;
    }
    else
        mxr_iram_fb_region_allocated(mxr_iram_fb_region_by_off(off_bytes), bytes, count_block);
}

static void MXR_IRAM_ATTR mxr_iram_released(uint32_t off_bytes,
                                            uint32_t bytes,
                                            bool is_exec, bool count_block)
{
    uint32_t new_free = s_iram_free_bytes + bytes;
    if (new_free > s_iram_total_bytes)
        new_free = s_iram_total_bytes;
    s_iram_free_bytes = new_free;

    s_stats.free_bytes += (size_t)bytes;
    if (s_stats.free_bytes > s_stats.total_bytes)
        s_stats.free_bytes = s_stats.total_bytes;

    if (is_exec)
    {
        uint32_t cap = mxr_iram_exec_zone_end();
        uint32_t nf = s_iram_exec_free_bytes + bytes;
        if (nf > cap)
            nf = cap;
        s_iram_exec_free_bytes = nf;
    }
    else
        mxr_iram_fb_region_released(mxr_iram_fb_region_by_off(off_bytes), bytes, count_block);
}

/* ================================================================
 *  IRAM EXEC search (first-fit from start of all IRAM)
 * ================================================================ */
/* ================================================================
 *  IRAM EXEC search (first-fit СТРОГО внутри EXEC-зоны [0, reserve))
 *  Дальше reserve EXEC-блок попасть не может. reserve == 0 → false.
 * ================================================================ */
static bool MXR_IRAM_ATTR mxr_iram_find_free_in_exec_zone(
    uint32_t bytes,
    uint32_t *out_off)
{
    if (!s_iram_enabled)
        return false;

    const uint32_t zone_end = mxr_iram_exec_zone_end();
    if (zone_end == 0) /* reserve == 0 → EXEC запрещены */
        return false;
    if (bytes == 0 || bytes > zone_end)
        return false;
    if (s_iram_exec_free_bytes < bytes)
        return false;

    uint32_t cur = 0;
    for (uint16_t i = 0; i < s_iram_desc_count; i++)
    {
        uint32_t off = mxr_desc_off(&s_iram_desc[i]);
        uint32_t len = mxr_desc_len(&s_iram_desc[i]);
        uint32_t block_end = off + (uint32_t)len;

        if (off >= zone_end)
            break; /* дескрипторы отсортированы по off */

        if (off > cur)
        {
            uint32_t gap = (uint32_t)(off - cur);
            if (gap >= bytes)
            {
                *out_off = cur;
                return true;
            }
        }
        if (block_end > cur)
            cur = block_end;
        if (cur >= zone_end)
            break;
    }
    if (zone_end > cur)
    {
        uint32_t gap = (uint32_t)(zone_end - cur);
        if (gap >= bytes)
        {
            *out_off = cur;
            return true;
        }
    }
    return false;
}

static bool MXR_IRAM_ATTR mxr_caps_allow_iram_fallback(uint32_t caps)
{
#ifndef CONFIG_MXR_IRAM_FALLBACK_ENABLED
    (void)caps;
    return false;
#else
    if (!s_iram_enabled)
        return false;

    if (caps & MALLOC_CAP_EXEC)
        return false;

    if (caps & (MALLOC_CAP_DMA | MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM))
        return false;

    if ((caps & MALLOC_CAP_32BIT) || caps == 0)
        return true;

    return false;
#endif
}

/*
 * Fallback admission check.
 * The reserve is enforced structurally: the fallback zone is
 * [reserve, iram_end), so a dynamic reserve check is not needed.
 */
static bool MXR_IRAM_ATTR mxr_iram_can_fallback(uint32_t bytes)
{
#ifndef CONFIG_MXR_IRAM_FALLBACK_ENABLED
    (void)bytes;
    return false;
#else
    if (!s_iram_enabled)
        return false;

    if (s_iram_fb_zone_total == 0)
        return false;

    if (s_iram_desc_count >= CONFIG_MXR_IRAM_MAX_DESC)
        return false;

    if (CONFIG_MXR_IRAM_FALLBACK_MAX_BYTES != 0)
    {
        if (bytes > (uint32_t)CONFIG_MXR_IRAM_FALLBACK_MAX_BYTES)
            return false;
    }

    return true;
#endif
}

/*
 * Check that a non-EXEC fallback block can grow in place within
 * its fallback region.
 */
static bool MXR_IRAM_ATTR mxr_iram_can_grow_fallback(
    uint32_t off_bytes,
    uint32_t old_bytes,
    uint32_t new_bytes)
{
    if (!s_iram_enabled)
        return false;
    if (new_bytes <= old_bytes)
        return true;

    int reg = mxr_iram_fb_region_by_off(off_bytes);
    if (reg < 0)
        return false;

    uint32_t extra = new_bytes - old_bytes;
    uint32_t block_end = off_bytes + (uint32_t)old_bytes;
    if (block_end + (uint32_t)extra > mxr_iram_fb_region_end(reg))
        return false;

    if (CONFIG_MXR_IRAM_FALLBACK_MAX_BYTES != 0)
    {
        if (new_bytes > (uint32_t)CONFIG_MXR_IRAM_FALLBACK_MAX_BYTES)
            return false;
    }
    return true;
}

/* ================================================================
 *  IRAM fallback region initialization
 * ================================================================ */
static bool mxr_init_iram_fb_regions(void)
{
    s_iram_fb_region_count = 0;

    if (s_iram_fb_zone_total == 0)
        return true; /* no fallback zone (all IRAM reserved for EXEC) */

    mxr_region_cfg_t cfg[MXR_IRAM_FB_REGION_COUNT];
    uint8_t total = mxr_parse_region_config(
        CONFIG_MXR_IRAM_FALLBACK_REGION_CONFIG,
        cfg,
        MXR_IRAM_FB_REGION_COUNT);

    /* Empty config -> one flat fallback region */
    if (total == 0)
    {
        s_iram_fb_region_count = 1;
        s_iram_fb_region[0].caps = (mxr_caps_t)MXR_IRAM_FB_CAPS_DEFAULT;
        s_iram_fb_region[0].start_byte = s_iram_fb_zone_start;
        s_iram_fb_region[0].total_bytes = s_iram_fb_zone_total;
        s_iram_fb_region[0].min_bytes = (mxr_class_t)MXR_ALIGN_SIZE;
        s_iram_fb_region[0].max_bytes = MXR_REGION_MAX_UNLIMITED;
        s_iram_fb_region[0].free_bytes = s_iram_fb_zone_total;
        s_iram_fb_region[0].min_free_bytes = s_iram_fb_zone_total;
        s_iram_fb_region[0].alloc_count = 0;
        s_iram_fb_region[0].largest_free_cache = s_iram_fb_zone_total;
        s_iram_fb_region[0].largest_cache_valid = 1;
        return true;
    }

    /* Validate percent sum */
    uint16_t percent_sum = 0;
    for (uint8_t i = 0; i < total; i++)
        percent_sum += cfg[i].percent;
    if (percent_sum > 100)
    {
        ESP_EARLY_LOGE(TAG, "IRAM fb region percent sum > 100 (%u)",
                       (unsigned)percent_sum);
        return false;
    }

    /* Align boundaries to 4 bytes */
    for (uint8_t i = 0; i < total; i++)
    {
        uint32_t b = (uint32_t)mxr_align4((uint32_t)cfg[i].min_bytes);
        if (b < MXR_ALIGN_SIZE)
            b = MXR_ALIGN_SIZE;
        cfg[i].min_bytes = (mxr_class_t)b;
    }

    /* Boundaries must be strictly increasing */
    for (uint8_t i = 1; i < total; i++)
    {
        if (cfg[i].min_bytes <= cfg[i - 1].min_bytes)
        {
            ESP_EARLY_LOGE(TAG, "IRAM fb boundaries must increase");
            return false;
        }
    }

    /* Build max_bytes from next boundary */
    for (uint8_t i = 0; i < total; i++)
    {
        if (i == (uint8_t)(total - 1))
            cfg[i].max_bytes = MXR_REGION_MAX_UNLIMITED;
        else
            cfg[i].max_bytes = (mxr_class_t)(cfg[i + 1].min_bytes - 1);
    }

    /* Distribute fallback zone memory across regions */
    uint32_t remaining = s_iram_fb_zone_total;
    s_iram_fb_region_count = 0;

    for (uint8_t i = 0; i < total; i++)
    {
        uint32_t bytes;
        if (i == (uint8_t)(total - 1) && cfg[i].percent == 0)
        {
            bytes = remaining;
        }
        else
        {
            bytes = (uint32_t)(((uint64_t)s_iram_fb_zone_total * cfg[i].percent) / 100);
            bytes = (uint32_t)mxr_align4((size_t)bytes);
        }
        if (bytes < (uint32_t)cfg[i].min_bytes)
            bytes = (uint32_t)cfg[i].min_bytes;
        if (bytes > remaining)
        {
            ESP_EARLY_LOGE(TAG, "IRAM fb region %u too large: %u > %u",
                           (unsigned)i, (unsigned)bytes, (unsigned)remaining);
            return false;
        }

        mxr_region_t *r = &s_iram_fb_region[s_iram_fb_region_count];
        r->caps = (mxr_caps_t)MXR_IRAM_FB_CAPS_DEFAULT;
        r->start_byte = (uint32_t)(s_iram_fb_zone_start + (s_iram_fb_zone_total - remaining));
        r->total_bytes = bytes;
        r->min_bytes = cfg[i].min_bytes;
        r->max_bytes = cfg[i].max_bytes;
        r->free_bytes = bytes;
        r->min_free_bytes = bytes;
        r->alloc_count = 0;
        r->largest_free_cache = bytes;
        r->largest_cache_valid = 1;

        remaining -= bytes;
        s_iram_fb_region_count++;
    }

    /* Leftover -> last region */
    if (remaining > 0)
    {
        mxr_region_t *last = &s_iram_fb_region[s_iram_fb_region_count - 1];
        last->total_bytes += remaining;
        last->free_bytes = last->total_bytes;
        last->min_free_bytes = last->free_bytes;
        last->largest_free_cache = last->total_bytes;
    }

    return true;
}

static void mxr_init_iram(void)
{
    extern char _iram_end;
#ifndef CONFIG_SOC_IRAM_SIZE
#define CONFIG_SOC_IRAM_SIZE 0xC000
#endif

    uint8_t *start = (uint8_t *)(((uint32_t)&_iram_end + 3) & ~3);
    uint8_t *end = (uint8_t *)(0x40100000 + CONFIG_SOC_IRAM_SIZE);

    s_iram_enabled = false;
    s_iram_base = NULL;
    s_iram_total_bytes = 0;
    s_iram_free_bytes = 0;
    s_iram_min_free_bytes = 0;
    s_iram_exec_allocs = 0;
    s_iram_fallback_allocs = 0;
    s_iram_fb_zone_start = 0;
    s_iram_fb_zone_total = 0;
    s_iram_fb_region_count = 0;
    s_iram_exec_free_bytes = 0;
    s_iram_exec_min_free_bytes = 0;

    if (end <= start)
        return;

    size_t bytes = (size_t)(end - start);

    /* ИСПРАВЛЕНО: мёртвая проверка заменена на защитную ошибку.
     * Условие bytes > CONFIG_SOC_IRAM_SIZE никогда не истинно
     * (start >= 0x40100000, end = 0x40100000 + CONFIG_SOC_IRAM_SIZE),
     * но если линкер-скрипт когда-нибудь изменится, лучше явно
     * отключить IRAM-кучу, чем тихо отрезать кусок. */
    if (bytes > CONFIG_SOC_IRAM_SIZE)
    {
        ESP_EARLY_LOGE(TAG, "invalid IRAM bounds (%u bytes), IRAM heap disabled",
                       (unsigned)bytes);
        return;
    }

    bytes &= ~(size_t)MXR_ALIGN_MASK;
    if (bytes <= 512 || bytes >= 0x00010000)
        return;

    s_iram_base = start;
    s_iram_total_bytes = (uint32_t)bytes;
    s_iram_free_bytes = (uint32_t)bytes;
    s_iram_min_free_bytes = (uint32_t)bytes;

    /* Compute fixed fallback zone = [reserve, iram_end) */
    uint32_t reserve = mxr_iram_reserve_bytes();
    if (reserve >= s_iram_total_bytes)
    {
        s_iram_fb_zone_start = s_iram_total_bytes;
        s_iram_fb_zone_total = 0;
    }
    else
    {
        s_iram_fb_zone_start = reserve;
        s_iram_fb_zone_total = s_iram_total_bytes - reserve;
    }

    /* EXEC-зона = [0, fb_zone_start): вся зарезервирована под EXEC */
    s_iram_exec_free_bytes = s_iram_fb_zone_start;
    s_iram_exec_min_free_bytes = s_iram_fb_zone_start;

    if (!mxr_init_iram_fb_regions())
    {
        ESP_EARLY_LOGW(TAG, "IRAM fb region init failed, using flat");

        /* FIX(4c): отражаем, что IRAM fb регионы упали в flat fallback */
        s_stats.iram_fb_region_init_fallback = true;

        /* Fall back to a single flat region */
        s_iram_fb_region_count = 1;
        s_iram_fb_region[0].caps = (mxr_caps_t)MXR_IRAM_FB_CAPS_DEFAULT;
        s_iram_fb_region[0].start_byte = s_iram_fb_zone_start;
        s_iram_fb_region[0].total_bytes = s_iram_fb_zone_total;
        s_iram_fb_region[0].min_bytes = (mxr_class_t)MXR_ALIGN_SIZE;
        s_iram_fb_region[0].max_bytes = MXR_REGION_MAX_UNLIMITED;
        s_iram_fb_region[0].free_bytes = s_iram_fb_zone_total;
        s_iram_fb_region[0].min_free_bytes = s_iram_fb_zone_total;
        s_iram_fb_region[0].alloc_count = 0;
        s_iram_fb_region[0].largest_free_cache = s_iram_fb_zone_total;
        s_iram_fb_region[0].largest_cache_valid = 1;
    }
    else
    {
        /* FIX(4c): успешная инициализация IRAM fb регионов */
        s_stats.iram_fb_region_init_fallback = false;
    }

    s_iram_enabled = true;
    ESP_EARLY_LOGD(TAG,
                   "IRAM heap ok: base=%p bytes=%u fb_zone=%u fb_regions=%u",
                   s_iram_base,
                   (unsigned)s_iram_total_bytes,
                   (unsigned)s_iram_fb_zone_total,
                   (unsigned)s_iram_fb_region_count);
}
#endif /* CONFIG_MXR_USE_IRAM */

#ifdef CONFIG_MXR_CROSS_REGION_FALLBACK
#ifdef CONFIG_MXR_DRAM_CROSS_ENABLED
static void *MXR_IRAM_ATTR mxr_try_cross_region(
    uint32_t bytes,
    uint32_t caps,
    int skip_region)
{
    uint8_t n = s_region_count;
    if (n == 0)
        return NULL;
    uint8_t order[MXR_REGIONS_MAX];
    uint8_t order_count = 0;
    if (skip_region < 0 || skip_region >= (int)n)
    {
        for (uint8_t i = 0; i < n; i++)
            order[order_count++] = i;
    }
    else if (skip_region < (int)(n / 2))
    {
        for (int i = skip_region + 1; i < (int)n; i++)
            order[order_count++] = (uint8_t)i;
        for (int i = skip_region - 1; i >= 0; i--)
            order[order_count++] = (uint8_t)i;
    }
    else
    {
        for (int i = skip_region - 1; i >= 0; i--)
            order[order_count++] = (uint8_t)i;
        for (int i = skip_region + 1; i < (int)n; i++)
            order[order_count++] = (uint8_t)i;
    }
    for (uint8_t k = 0; k < order_count; k++)
    {
        uint8_t i = order[k];

        /* FIX(3.2): учитываем причины пропуска */
        if (!mxr_region_caps_ok((int)i, caps))
        {
            s_stats.cross_caps_skips++;
            continue;
        }

        if (s_region[i].free_bytes < bytes)
        {
            s_stats.cross_free_skips++;
            continue;
        }

        if (s_region[i].largest_cache_valid &&
            s_region[i].largest_free_cache < bytes)
        {
            s_stats.cross_cache_skips++;
            continue;
        }

/* Правило 1: DRAM GUARD — защита региона от неподходящих блоков */
/* Правило 1: GUARD — только 32-bit, без вызовов libgcc */
#if defined(MXR_DRAM_GUARD_NUM) && defined(MXR_DRAM_GUARD_DEN)
        if (s_region[i].max_bytes != MXR_REGION_MAX_UNLIMITED &&
            bytes > ((uint32_t)s_region[i].max_bytes * MXR_DRAM_GUARD_NUM) /
                        MXR_DRAM_GUARD_DEN)
        {
            s_stats.cross_region_guard_rejects++;
            continue;
        }
#endif
        /* Правило 2: min_bytes guard — только 32-bit */
#if defined(MXR_DRAM_MIN_BYTES_DIVISOR)
        if (bytes < ((uint32_t)s_region[i].min_bytes) /
                        MXR_DRAM_MIN_BYTES_DIVISOR)
        {
            s_stats.cross_region_guard_rejects++;
            continue;
        }
#endif
        uint32_t off_bytes = 0;
        uint32_t largest = 0;
        uint32_t alloc_bytes = bytes;
        bool found = mxr_find_free_and_largest((int)i, bytes,
                                               &off_bytes, &largest,
                                               &alloc_bytes);
        s_region[i].largest_free_cache = largest;
        s_region[i].largest_cache_valid = 1;
        if (!found)
        {
            s_stats.cross_region_skip_fragmented++;
            continue;
        }
        if (!mxr_dram_desc_insert(off_bytes, alloc_bytes, 0))
        {
            continue;
        }

        s_region[i].alloc_count++;
        mxr_region_allocated((int)i, alloc_bytes);
        s_stats.cross_region_allocs++;

        return mxr_off_to_ptr(off_bytes);
    }
    return NULL;
}

#endif /* CONFIG_MXR_DRAM_CROSS_ENABLED */
#endif /* CONFIG_MXR_CROSS_REGION_FALLBACK */

/* ================================================================
 *  Locked allocation
 * ================================================================ */
static void *MXR_IRAM_ATTR mxr_malloc_caps_locked(size_t size, uint32_t caps)
{
    if (!s_initialized)
        return NULL;
    if (size == 0)
        size = 1;
    if (size > MXR_MAX_LEN_BYTES)
    {
        s_stats.alloc_fail_no_memory++;
        return NULL;
    }

    size = mxr_align4(size);

    if (size > MXR_MAX_LEN_BYTES)
    {
        s_stats.alloc_fail_no_memory++;
        return NULL;
    }
    uint32_t bytes = (uint32_t)size;

    {
        uint32_t max_possible = s_arena_total_bytes;
#ifdef CONFIG_MXR_USE_IRAM
        if (s_iram_enabled)
            max_possible += s_iram_total_bytes;
#endif
        if (bytes > max_possible)
        {
            s_stats.alloc_fail_no_memory++;
            return NULL;
        }
    }

#ifdef CONFIG_MXR_USE_IRAM
    /* EXEC allocations go only to IRAM */
    if (caps & MALLOC_CAP_EXEC)
    {
        if ((caps & ~(MALLOC_CAP_EXEC | MALLOC_CAP_32BIT | MALLOC_CAP_INTERNAL)) != 0)
        {
            s_stats.alloc_fail_no_memory++;
            return NULL;
        }
        if (!s_iram_enabled)
        {
            s_stats.alloc_fail_no_memory++;
            return NULL;
        }

        /* ЖЁСТКАЯ привязка EXEC к зоне [0, CONFIG_MXR_IRAM_RESERVE_BYTES).
         * reserve == 0 → EXEC-аллокации полностью отменяются. */
        if (mxr_iram_exec_zone_end() == 0)
        {
            s_stats.exec_zone_rejects++;
            return NULL;
        }
        uint32_t off_bytes = 0;
        if (!mxr_iram_find_free_in_exec_zone(bytes, &off_bytes))
        {
            if (bytes > mxr_iram_exec_zone_end())
                s_stats.exec_zone_rejects++;
            else
                s_stats.alloc_fail_no_memory++;
            return NULL;
        }
        if (!mxr_iram_desc_insert(off_bytes, bytes, MXR_LEN_FLAG_EXEC))
            return NULL;

        /* ИСПРАВЛЕНО: is_exec = true */
        mxr_iram_allocated(off_bytes, bytes, true, true);
        s_iram_exec_allocs++;
        s_stats.exec_allocs++;
        return mxr_iram_off_to_ptr(off_bytes);
    }
#else
    if (caps & MALLOC_CAP_EXEC)
    {
        s_stats.alloc_fail_no_memory++;
        return NULL;
    }
#endif

#ifdef CONFIG_MXR_USE_IRAM
    /* IRAM fallback for pure 32BIT (region-aware + cross-region) */
    if (s_iram_enabled &&
        mxr_caps_allow_iram_fallback(caps) &&
        mxr_iram_can_fallback(bytes))
    {
        int fb_reg = mxr_iram_fb_region_for_size(bytes);
        uint32_t off_bytes = 0;
        bool found = false;

        /* Step 1: свой fb-регион */
        if (fb_reg >= 0)
        {
            uint32_t alloc_bytes = bytes;

            /* НОВОЕ: быстрая проверка кэша перед полным поиском */
            if (s_iram_fb_region[fb_reg].largest_cache_valid &&
                s_iram_fb_region[fb_reg].largest_free_cache < bytes)
            {
                found = false; /* пропускаем Step 1, сразу в cross-region */
            }
            else
            {
                found = mxr_iram_fb_find_free_in_region(fb_reg, bytes,
                                                        &off_bytes, &alloc_bytes);

                /* FIX(4.2): если поиск не нашёл блок, обновляем largest cache,
                 * чтобы повторные запросы не делали полный поиск. */
                if (!found)
                {
                    uint32_t largest = mxr_iram_fb_region_largest_free(fb_reg);
                    s_iram_fb_region[fb_reg].largest_free_cache = largest;
                    s_iram_fb_region[fb_reg].largest_cache_valid = 1;
                }
            }

            if (found)
            {
                if (mxr_iram_desc_insert(off_bytes, alloc_bytes, 0))
                {
                    mxr_iram_allocated(off_bytes, alloc_bytes, false, true);
                    s_iram_fallback_allocs++;
                    s_stats.iram_fallback_allocs++;
                    return mxr_iram_off_to_ptr(off_bytes);
                }
                found = false;
            }
        }

        /* Step 2: cross-region внутри IRAM fb */
        uint32_t cross_alloc_bytes = bytes;
        if (!found)
        {
#if defined(CONFIG_MXR_CROSS_REGION_FALLBACK) && \
    defined(CONFIG_MXR_IRAM_CROSS_ENABLED)
            found = mxr_iram_fb_try_cross_region(bytes, fb_reg,
                                                 &off_bytes, &cross_alloc_bytes);
#endif
        }
        if (found)
        {
            if (mxr_iram_desc_insert(off_bytes, cross_alloc_bytes, 0))
            {
                mxr_iram_allocated(off_bytes, cross_alloc_bytes, false, true);
                s_iram_fallback_allocs++;
                s_stats.iram_fallback_allocs++;
                return mxr_iram_off_to_ptr(off_bytes);
            }
        }
    }
#endif

    /* DRAM allocation — Step 1: own size-class region */
    int region = mxr_region_for_size(bytes, caps);

    if (region >= 0)
    {
        uint32_t off_bytes = 0;
        uint32_t alloc_bytes = bytes;

        if (mxr_try_alloc_region(region, bytes, &off_bytes, &alloc_bytes))
        {
            if (mxr_dram_desc_insert(off_bytes, alloc_bytes, 0))
            {
                s_region[region].alloc_count++;
                mxr_region_allocated(region, alloc_bytes);
                return mxr_off_to_ptr(off_bytes);
            }
        }
    }

    /* Step 2: cross-region DRAM fallback (last resort) */
#if defined(CONFIG_MXR_CROSS_REGION_FALLBACK) && \
    defined(CONFIG_MXR_DRAM_CROSS_ENABLED)
    {
        void *fallback_ptr = mxr_try_cross_region(bytes, caps, region);
        if (fallback_ptr)
            return fallback_ptr;
    }
#endif

    s_stats.alloc_fail_no_memory++;
    return NULL;
}

/* ================================================================
 *  Locked free
 * ================================================================ */
static void MXR_IRAM_ATTR mxr_free_locked(void *ptr)
{
    if (!ptr)
        return;
    if (!s_initialized)
    {
        s_stats.invalid_free_attempts++;
        return;
    }

    mxr_arena_id_t arena = mxr_ptr_to_arena(ptr);
    if (arena == MXR_ARENA_NONE)
    {
        s_stats.invalid_free_attempts++;
        return;
    }

    if (arena == MXR_ARENA_DRAM)
    {
        uint32_t off_bytes = mxr_ptr_to_off(ptr);
        int index = mxr_dram_desc_find_key(off_bytes);
        if (index < 0)
        {
            s_stats.invalid_free_attempts++;
            return;
        }
        uint32_t len_bytes = mxr_desc_len(&s_dram_desc[index]);
        int region = mxr_region_by_off(off_bytes);

        mxr_dram_desc_remove(index);
        if (region >= 0)
        {
            if (s_region[region].alloc_count > 0)
                s_region[region].alloc_count--;
            mxr_region_released(region, len_bytes);
        }
        else
        {
            /* Дескриптор удалён, но регион не найден.
             * Возвращаем память в глобальный счётчик, чтобы не было drift.
             * Это НЕ invalid free — указатель валидный. */
            uint32_t new_free = s_dram_free_bytes + len_bytes;
            if (new_free > s_arena_total_bytes)
                new_free = s_arena_total_bytes;
            s_dram_free_bytes = new_free;
            s_stats.free_bytes += (size_t)len_bytes;
            if (s_stats.free_bytes > s_stats.total_bytes)
                s_stats.free_bytes = s_stats.total_bytes;
            s_stats.region_lookup_failures++;
        }
        return;
    }

#ifdef CONFIG_MXR_USE_IRAM
    if (arena == MXR_ARENA_IRAM)
    {
        uint32_t off_bytes = mxr_iram_ptr_to_off(ptr);
        int index = mxr_iram_desc_find_key(off_bytes);
        if (index < 0)
        {
            s_stats.invalid_free_attempts++;
            return;
        }

        /* ИСПРАВЛЕНО: сохранить is_exec ДО desc_remove */
        bool is_exec = mxr_desc_is_exec(&s_iram_desc[index]);
        uint32_t len_bytes = mxr_desc_len(&s_iram_desc[index]);

        mxr_iram_desc_remove(index);
        mxr_iram_released(off_bytes, len_bytes, is_exec, true);
        return;
    }
#endif
}

/* ================================================================
 *  Public API
 * ================================================================ */
void *MXR_IRAM_ATTR mxr_malloc_caps(size_t size, uint32_t caps)
{
    mxr_lock();
    void *p = mxr_malloc_caps_locked(size, caps);
    mxr_unlock();
    return p;
}

void MXR_IRAM_ATTR mxr_free(void *ptr)
{

    mxr_lock();
    mxr_free_locked(ptr);
    mxr_unlock();
}

void *MXR_IRAM_ATTR mxr_malloc(size_t size)
{
    return mxr_malloc_caps(size, MALLOC_CAP_32BIT);
}

void *MXR_IRAM_ALLOC_ATTR mxr_calloc_caps(size_t count, size_t size, uint32_t caps)
{
    size_t total_bytes;
    if (__builtin_mul_overflow(count, size, &total_bytes))
    {

        mxr_lock();
        s_stats.alloc_fail_no_memory++;
        mxr_unlock();
        return NULL;
    }
    void *ptr = mxr_malloc_caps(total_bytes, caps);
    if (ptr)
    {
        size_t clear_bytes = mxr_align4(total_bytes ? total_bytes : 1);
        mxr_memset4(ptr, clear_bytes);
    }
    return ptr;
}

void *MXR_IRAM_ALLOC_ATTR mxr_calloc(size_t count, size_t size)
{
    return mxr_calloc_caps(count, size, MALLOC_CAP_32BIT);
}

void *MXR_IRAM_ALLOC_ATTR mxr_zalloc_caps(size_t size, uint32_t caps)
{
    void *ptr = mxr_malloc_caps(size, caps);
    if (ptr)
    {
        size_t clear_bytes = mxr_align4(size ? size : 1);
        mxr_memset4(ptr, clear_bytes);
    }
    return ptr;
}

void *MXR_IRAM_ALLOC_ATTR mxr_zalloc(size_t size)
{
    return mxr_zalloc_caps(size, MALLOC_CAP_32BIT);
}

/* ================================================================
 *  Realloc
 * ================================================================ */
void *MXR_IRAM_ALLOC_ATTR mxr_realloc_caps(void *ptr, size_t newsize, uint32_t caps)
{
    if (!ptr)
        return mxr_malloc_caps(newsize, caps);
    if (!s_initialized)
        return NULL;

    if (newsize == 0)
    {
#if MXR_REALLOC_ZERO_FREES

        mxr_lock();
        mxr_free_locked(ptr);
        mxr_unlock();
        return NULL;
#else
        newsize = 1;
#endif
    }

    if (newsize > MXR_MAX_LEN_BYTES)
        return NULL;

    newsize = mxr_align4(newsize);
    if (newsize == 0 || newsize > MXR_MAX_LEN_BYTES)
        return NULL;

    uint32_t new_bytes = (uint32_t)newsize;
    if (new_bytes == 0)
        new_bytes = MXR_ALIGN_SIZE;

    mxr_lock();

    mxr_arena_id_t arena = mxr_ptr_to_arena(ptr);
    if (arena == MXR_ARENA_NONE)
    {
        s_stats.invalid_free_attempts++;
        mxr_unlock();
        return NULL;
    }

    /* ---- DRAM realloc ---- */
    if (arena == MXR_ARENA_DRAM)
    {
        uint32_t off_bytes = mxr_ptr_to_off(ptr);
        int index = mxr_dram_desc_find_key(off_bytes);
        if (index < 0)
        {
            s_stats.invalid_free_attempts++;
            mxr_unlock();
            return NULL;
        }

        uint32_t old_bytes = mxr_desc_len(&s_dram_desc[index]);
        int region = mxr_region_by_off(off_bytes);
        bool caps_ok = mxr_region_caps_ok(region, caps);
        bool in_place_allowed = caps_ok && mxr_region_size_ok(region, new_bytes);

        if (new_bytes == old_bytes && region >= 0 && in_place_allowed)
        {
            mxr_unlock();
            return ptr;
        }

        if (new_bytes < old_bytes && region >= 0 && in_place_allowed)
        {
            uint32_t diff = old_bytes - new_bytes;
            /* НОВОЕ: Anti-sliver — не разрезать, если хвост слишком мал */
            if (MXR_IS_SLIVER(diff))
            {
                mxr_unlock();
                return ptr;
            }
            s_dram_desc[index].len_flags =
                (new_bytes & MXR_LEN_MASK) |
                (s_dram_desc[index].len_flags & MXR_LEN_FLAGS_MASK);
            mxr_region_released(region, diff);
            mxr_unlock();
            return ptr;
        }

        if (new_bytes > old_bytes && region >= 0 && in_place_allowed)
        {
            uint32_t extra = new_bytes - old_bytes;
            uint32_t block_end = off_bytes + (uint32_t)old_bytes;
            uint32_t region_end =
                s_region[region].start_byte + (uint32_t)s_region[region].total_bytes;
            uint32_t next_boundary;
            if (index + 1 < (int)s_dram_desc_count)
            {
                uint32_t next_off = mxr_desc_off(&s_dram_desc[index + 1]);
                next_boundary = (next_off < region_end) ? next_off : region_end;
            }
            else
            {
                next_boundary = region_end;
            }
            if (next_boundary >= block_end)
            {
                uint32_t gap = (uint32_t)(next_boundary - block_end);
                if (gap >= extra)
                {
                    uint32_t tail = gap - extra;
                    uint32_t actual_new_bytes = new_bytes;
                    /* ===== ИСПРАВЛЕНО: ограничение max_bytes ===== */
                    uint32_t max_allowed = s_region[region].max_bytes;
                    if (MXR_IS_SLIVER(tail) &&
                        (max_allowed == MXR_REGION_MAX_UNLIMITED ||
                         old_bytes + gap <= max_allowed))
                    {
                        actual_new_bytes = old_bytes + gap;
                        s_stats.anti_sliver_expansions++;
                    }
                    /* ================================================ */
                    s_dram_desc[index].len_flags =
                        (actual_new_bytes & MXR_LEN_MASK) |
                        (s_dram_desc[index].len_flags & MXR_LEN_FLAGS_MASK);
                    mxr_region_allocated(region, actual_new_bytes - old_bytes);
                    mxr_unlock();
                    return ptr;
                }
            }
        }
        /* Move */
        uint32_t copy_bytes = (old_bytes < new_bytes) ? old_bytes : new_bytes;
        void *new_ptr = mxr_malloc_caps_locked(newsize, caps);
        if (!new_ptr)
        {
            mxr_unlock();
            return NULL;
        }
        mxr_memcpy4(new_ptr, ptr, (size_t)copy_bytes);
        mxr_free_locked(ptr);
        mxr_unlock();
        return new_ptr;
    }

#ifdef CONFIG_MXR_USE_IRAM
    /* ---- IRAM realloc ---- */
    if (arena == MXR_ARENA_IRAM)
    {
        uint32_t off_bytes = mxr_iram_ptr_to_off(ptr);
        int index = mxr_iram_desc_find_key(off_bytes);
        if (index < 0)
        {
            s_stats.invalid_free_attempts++;
            mxr_unlock();
            return NULL;
        }

        bool old_exec = mxr_desc_is_exec(&s_iram_desc[index]);
        uint32_t old_bytes = mxr_desc_len(&s_iram_desc[index]);

        bool want_exec = (caps & MALLOC_CAP_EXEC) != 0;
        bool in_place_allowed = false;

        if (want_exec)
        {
            in_place_allowed =
                old_exec &&
                ((caps & ~(MALLOC_CAP_EXEC | MALLOC_CAP_32BIT | MALLOC_CAP_INTERNAL)) == 0);
        }
        else if (!old_exec)
        {
            in_place_allowed = mxr_caps_allow_iram_fallback(caps);
        }
        else
        {
            in_place_allowed = false;
        }

        if (in_place_allowed)
        {
            if (new_bytes == old_bytes)
            {
                mxr_unlock();
                return ptr;
            }

            if (new_bytes < old_bytes)
            {
                uint32_t diff = old_bytes - new_bytes;
                /* НОВОЕ: Anti-sliver */
                if (MXR_IS_SLIVER(diff))
                {
                    mxr_unlock();
                    return ptr;
                }
                s_iram_desc[index].len_flags =
                    (new_bytes & MXR_LEN_MASK) |
                    (s_iram_desc[index].len_flags & MXR_LEN_FLAGS_MASK);
                mxr_iram_released(off_bytes, diff, old_exec, false);
                mxr_unlock();
                return ptr;
            }

            uint32_t extra = new_bytes - old_bytes;

            if (!old_exec)
            {
                int reg = mxr_iram_fb_region_by_off(off_bytes);
                if (!mxr_iram_fb_region_size_ok(reg, new_bytes))
                    in_place_allowed = false;
                else if (!mxr_iram_can_grow_fallback(off_bytes, old_bytes, new_bytes))
                    in_place_allowed = false;
            }

            if (in_place_allowed)
            {
                uint32_t block_end = off_bytes + (uint32_t)old_bytes;
                uint32_t next_boundary;
                if (index + 1 < (int)s_iram_desc_count)
                    next_boundary = mxr_desc_off(&s_iram_desc[index + 1]);
                else
                    next_boundary = s_iram_total_bytes;
                if (old_exec)
                {
                    /* FIX: EXEC-блок не может вырасти за пределы EXEC-зоны */
                    uint32_t zone_end = mxr_iram_exec_zone_end();
                    if (next_boundary > zone_end)
                        next_boundary = zone_end;
                }
                else
                {
                    int reg = mxr_iram_fb_region_by_off(off_bytes);
                    if (reg >= 0)
                    {
                        uint32_t reg_end = mxr_iram_fb_region_end(reg);
                        if (next_boundary > reg_end)
                            next_boundary = reg_end;
                    }
                }

                if (next_boundary >= block_end)
                {
                    uint32_t gap = (uint32_t)(next_boundary - block_end);
                    if (gap >= extra)
                    {
                        uint32_t tail = gap - extra;
                        uint32_t actual_new_bytes = new_bytes;
                        /* ===== ИСПРАВЛЕНО: ограничение max_bytes ===== */
                        bool can_expand = MXR_IS_SLIVER(tail);
                        if (can_expand && !old_exec)
                        {
                            int reg = mxr_iram_fb_region_by_off(off_bytes);
                            if (reg >= 0)
                            {
                                uint32_t max_allowed = s_iram_fb_region[reg].max_bytes;
                                if (max_allowed != MXR_REGION_MAX_UNLIMITED &&
                                    old_bytes + gap > max_allowed)
                                    can_expand = false;
                            }
                        }
                        if (can_expand)
                        {
                            actual_new_bytes = old_bytes + gap;
                            s_stats.anti_sliver_expansions++;
                        }
                        /* ================================================ */
                        s_iram_desc[index].len_flags =
                            (actual_new_bytes & MXR_LEN_MASK) |
                            (s_iram_desc[index].len_flags & MXR_LEN_FLAGS_MASK);
                        mxr_iram_allocated(off_bytes + old_bytes,
                                           actual_new_bytes - old_bytes,
                                           old_exec, false);
                        mxr_unlock();
                        return ptr;
                    }
                }
            }
        }

        /* Move */
        uint32_t copy_bytes = (old_bytes < new_bytes) ? old_bytes : new_bytes;
        void *new_ptr = mxr_malloc_caps_locked(newsize, caps);
        if (!new_ptr)
        {
            mxr_unlock();
            return NULL;
        }
        mxr_memcpy4(new_ptr, ptr, (size_t)copy_bytes);
        mxr_free_locked(ptr);
        mxr_unlock();
        return new_ptr;
    }
#endif

    mxr_unlock();
    return NULL;
}

void *MXR_IRAM_ALLOC_ATTR mxr_realloc(void *ptr, size_t newsize)
{
    return mxr_realloc_caps(ptr, newsize, MALLOC_CAP_32BIT);
}

/* ================================================================
 *  DRAM region initialization
 * ================================================================ */
static void mxr_init_regions_temp_single(void)
{
    /* ИСПРАВЛЕНО: memset -> mxr_memset4 (s_region может быть в IRAM) */
    mxr_memset4(s_region, sizeof(s_region));

    s_region_count = 1;
    s_region[0].caps = (mxr_caps_t)MXR_DRAM_CAPS_DEFAULT;
    s_region[0].start_byte = 0;
    s_region[0].total_bytes = s_arena_total_bytes;
    s_region[0].min_bytes = (mxr_class_t)MXR_ALIGN_SIZE;
    s_region[0].max_bytes = MXR_REGION_MAX_UNLIMITED;
    s_region[0].free_bytes = s_arena_total_bytes;
    s_region[0].min_free_bytes = s_arena_total_bytes;
    s_region[0].alloc_count = 0;
    s_region[0].largest_free_cache = s_arena_total_bytes;
    s_region[0].largest_cache_valid = 1;
}

static bool mxr_init_regions_exact(
    const mxr_region_cfg_t *cfg,
    uint8_t count)
{
    if (count < 2 || count > MXR_ACTIVE_TOTAL_REGIONS)
        return false;

    /* ИСПРАВЛЕНО: memset -> mxr_memset4 */
    mxr_memset4(s_region, sizeof(s_region));

    s_region_count = 0;

    uint16_t percent_sum = 0;
    for (uint8_t i = 0; i < count; i++)
        percent_sum += cfg[i].percent;
    if (percent_sum > 100)
    {
        ESP_EARLY_LOGE(TAG, "region percent sum must be <= 100, got %u",
                       (unsigned)percent_sum);
        return false;
    }

    uint32_t expected_min = 0;
    for (uint8_t i = 0; i < count; i++)
    {
        mxr_class_t min_b = cfg[i].min_bytes;
        mxr_class_t max_b = cfg[i].max_bytes;

        if (min_b == 0)
            min_b = (mxr_class_t)MXR_ALIGN_SIZE;

        if (max_b == MXR_REGION_MAX_UNLIMITED && i != (uint8_t)(count - 1))
        {
            ESP_EARLY_LOGE(TAG, "only last region may be unlimited: region %u",
                           (unsigned)i);
            return false;
        }
        if (max_b != MXR_REGION_MAX_UNLIMITED)
        {
            if (min_b > max_b)
            {
                ESP_EARLY_LOGE(TAG, "region %u bad min/max: %u/%u",
                               (unsigned)i, (unsigned)min_b, (unsigned)max_b);
                return false;
            }
        }

        if (i > 0)
        {
            if ((uint32_t)min_b < expected_min)
            {
                ESP_EARLY_LOGE(TAG, "region %u overlaps previous", (unsigned)i);
                return false;
            }
            if ((uint32_t)min_b > expected_min)
            {
                ESP_EARLY_LOGE(TAG, "gap before region %u", (unsigned)i);
                return false;
            }
        }

        if (max_b == MXR_REGION_MAX_UNLIMITED)
            expected_min = MXR_MAX_LEN_BYTES;
        else
            expected_min = (uint32_t)max_b + 1;
    }

    uint32_t remaining_bytes = s_arena_total_bytes;

    for (uint8_t i = 0; i < count; i++)
    {
        mxr_class_t min_b = cfg[i].min_bytes;
        mxr_class_t max_b = cfg[i].max_bytes;

        if (min_b == 0)
            min_b = (mxr_class_t)MXR_ALIGN_SIZE;

        uint32_t bytes;
        if (i == (uint8_t)(count - 1) && cfg[i].percent == 0)
        {
            bytes = remaining_bytes;
        }
        else
        {
            bytes = (uint32_t)(((uint64_t)s_arena_total_bytes * cfg[i].percent) / 100);
            bytes = (uint32_t)mxr_align4((size_t)bytes);
        }
        if (bytes < (uint32_t)min_b)
            bytes = (uint32_t)min_b;
        if (bytes > remaining_bytes)
        {
            ESP_EARLY_LOGE(TAG, "region %u too large", (unsigned)i);
            return false;
        }

        s_region[s_region_count].caps = (mxr_caps_t)MXR_DRAM_CAPS_DEFAULT;
        s_region[s_region_count].start_byte =
            (uint32_t)(s_arena_total_bytes - remaining_bytes);
        s_region[s_region_count].total_bytes = bytes;
        s_region[s_region_count].min_bytes = min_b;
        s_region[s_region_count].max_bytes = max_b;
        s_region[s_region_count].free_bytes = bytes;
        s_region[s_region_count].min_free_bytes = bytes;
        s_region[s_region_count].alloc_count = 0;
        s_region[s_region_count].largest_free_cache = bytes;
        s_region[s_region_count].largest_cache_valid = 1;

        remaining_bytes -= bytes;
        s_region_count++;
    }

    if (remaining_bytes > 0)
    {
        s_region[count - 1].total_bytes += remaining_bytes;
        s_region[count - 1].free_bytes = s_region[count - 1].total_bytes;
        s_region[count - 1].min_free_bytes = s_region[count - 1].free_bytes;
        s_region[count - 1].largest_free_cache = s_region[count - 1].total_bytes;
    }

    return true;
}

/* ================================================================
 *  Region config parser: "4-20%,56-1%,128-34%"
 *  (shared by DRAM and IRAM fallback)
 * ================================================================ */
static uint8_t mxr_parse_region_config(
    const char *s,
    mxr_region_cfg_t *out,
    uint8_t max_count)
{
    const char *p = s;
    uint8_t count = 0;

    while (count < max_count && p && *p)
    {
        while (*p == ' ' || *p == '\t' || *p == ',')
            p++;
        if (*p == '\0')
            break;

        /* --- min_bytes --- */
        uint32_t min_b = 0;
        bool has_digit = false;
        while (*p >= '0' && *p <= '9')
        {
            min_b = min_b * 10 + (uint32_t)(*p - '0');
            has_digit = true;
            p++;
            if (min_b > 0x7FFFFFFF)
                return count;
        }
        if (!has_digit)
            break;
        if (*p != '-')
            break;
        p++;

#ifdef CONFIG_MXR_COMPACT_TYPES
        if (min_b > 0xFFFF)
        {
            ESP_EARLY_LOGE(TAG, "boundary %u exceeds compact max 65535",
                           (unsigned)min_b);
            return count;
        }
#endif

        /* --- percent --- */
        uint32_t pct = 0;
        has_digit = false;
        while (*p >= '0' && *p <= '9')
        {
            pct = pct * 10 + (uint32_t)(*p - '0');
            has_digit = true;
            p++;
            if (pct > 100)
                return count;
        }
        if (!has_digit)
            break;
        if (*p == '%')
            p++;

        out[count].min_bytes = (mxr_class_t)min_b;
        out[count].percent = (uint8_t)pct;
        out[count].max_bytes = MXR_REGION_MAX_UNLIMITED;
        count++;
    }

    return count;
}

static bool mxr_init_regions_kconfig(void)
{
    mxr_region_cfg_t cfg[MXR_ACTIVE_TOTAL_REGIONS];
    uint8_t total = mxr_parse_region_config(
        CONFIG_MXR_REGION_CONFIG, cfg, MXR_ACTIVE_TOTAL_REGIONS);

    if (total == 0)
    {
        ESP_EARLY_LOGE(TAG, "no regions parsed from '%s'",
                       CONFIG_MXR_REGION_CONFIG);
        return false;
    }

    if (total == 1)
    {
        mxr_init_regions_temp_single();
        return true;
    }

    for (uint8_t i = 0; i < total; i++)
    {
        uint32_t b = (uint32_t)mxr_align4((uint32_t)cfg[i].min_bytes);
        if (b < MXR_ALIGN_SIZE)
            b = MXR_ALIGN_SIZE;
        cfg[i].min_bytes = (mxr_class_t)b;
    }

    for (uint8_t i = 1; i < total; i++)
    {
        if (cfg[i].min_bytes <= cfg[i - 1].min_bytes)
        {
            ESP_EARLY_LOGE(TAG, "boundaries must be strictly increasing");
            return false;
        }
    }

    for (uint8_t i = 0; i < total; i++)
    {
        if (i == (uint8_t)(total - 1))
            cfg[i].max_bytes = MXR_REGION_MAX_UNLIMITED;
        else
            cfg[i].max_bytes = (mxr_class_t)(cfg[i + 1].min_bytes - 1);
    }

    return mxr_init_regions_exact(cfg, total);
}

/* ================================================================
 *  Init
 * ================================================================ */
void mxr_init(void)
{
    if (s_initialized)
        return;

    extern char _bss_end;
    uint8_t *start = (uint8_t *)(((uint32_t)&_bss_end + 3) & ~3);
    uint8_t *end = (uint8_t *)0x40000000;

    s_initialized = false;

    if (end <= start)
    {
        ESP_EARLY_LOGE(TAG, "invalid heap bounds");
        return;
    }

    size_t bytes = (size_t)(end - start);
    bytes &= ~(size_t)MXR_ALIGN_MASK;
    if (bytes > MXR_MAX_ARENA_BYTES)
    {
        ESP_EARLY_LOGE(TAG, "arena too large: %u bytes", (unsigned)bytes);
        return;
    }

    s_arena_base = start;
    s_arena_total_bytes = (uint32_t)bytes;
    s_dram_free_bytes = (uint32_t)bytes;
    s_dram_min_free_bytes = (uint32_t)bytes;

    /* ИСПРАВЛЕНО: memset -> mxr_memset4 для данных в IRAM */
    mxr_memset4(s_dram_desc, sizeof(s_dram_desc));
    s_dram_desc_count = 0;

#ifdef CONFIG_MXR_USE_IRAM
    mxr_memset4(s_iram_desc, sizeof(s_iram_desc));
    s_iram_desc_count = 0;
#endif

    mxr_memset4(&s_stats, sizeof(s_stats));
    s_stats.dram_desc_capacity = CONFIG_MXR_MAX_DESC;
    s_stats.iram_desc_capacity = CONFIG_MXR_IRAM_MAX_DESC;

#ifdef CONFIG_MXR_USE_IRAM
    mxr_init_iram();
#endif

    bool regions_ok = mxr_init_regions_kconfig();
    if (!regions_ok)
    {
        ESP_EARLY_LOGW(TAG, "region init failed, using single region");
        mxr_init_regions_temp_single();
        s_stats.region_init_fallback = true;
    }
    else
    {
        s_stats.region_init_fallback = false;
    }

    uint32_t largest_bytes = 0;
    for (uint8_t i = 0; i < s_region_count; i++)
    {
        if (s_region[i].total_bytes > largest_bytes)
            largest_bytes = s_region[i].total_bytes;
    }

    size_t total_bytes = s_arena_total_bytes;
    size_t free_bytes = s_arena_total_bytes;

    s_stats.iram_total_bytes = 0;
    s_stats.iram_free_bytes = 0;
    s_stats.iram_min_free_bytes = 0;
    s_stats.iram_fb_zone_total_bytes = 0;
    s_stats.exec_allocs = 0;
    s_stats.iram_fallback_allocs = 0;
    s_stats.iram_fb_region_count = 0;

#ifdef CONFIG_MXR_USE_IRAM
    if (s_iram_enabled)
    {
        total_bytes += s_iram_total_bytes;
        free_bytes += s_iram_total_bytes;
        s_stats.iram_total_bytes = s_iram_total_bytes;
        s_stats.iram_free_bytes = s_iram_total_bytes;
        s_stats.iram_min_free_bytes = s_iram_total_bytes;
        s_stats.iram_fb_zone_total_bytes = s_iram_fb_zone_total;
        s_stats.iram_fb_region_count = s_iram_fb_region_count;
        uint32_t iram_largest = mxr_iram_largest_free_zone_aware();
        if (iram_largest > largest_bytes)
            largest_bytes = iram_largest;
    }
#endif

    s_stats.initialized = true;
    s_stats.region_count = s_region_count;
    s_stats.total_bytes = total_bytes;
    s_stats.free_bytes = free_bytes;
    s_stats.min_free_bytes = total_bytes;
    s_stats.largest_free_block_bytes = (size_t)largest_bytes;
    s_initialized = true;

    ESP_EARLY_LOGD(TAG,
                   "init ok: base=%p bytes=%u dram_desc=%u iram_desc=%u",
                   s_arena_base,
                   (unsigned)s_arena_total_bytes,
                   (unsigned)CONFIG_MXR_MAX_DESC,
                   (unsigned)CONFIG_MXR_IRAM_MAX_DESC);
}

/* ================================================================
 *  Status
 * ================================================================ */
static void mxr_collect_status_locked(mxr_status_t *status)
{
    if (!status)
        return;

    s_stats.dram_active_allocs = s_dram_desc_count;
    s_stats.iram_active_allocs = 0;
    s_stats.region_count = s_region_count;
    s_stats.iram_fb_region_count = 0;

    size_t total_bytes = 0;
    size_t free_bytes = 0;
    uint32_t largest_bytes = 0;

    for (uint8_t i = 0; i < s_region_count; i++)
    {
        total_bytes += (size_t)s_region[i].total_bytes;
        free_bytes += (size_t)s_region[i].free_bytes;
        uint32_t lr = mxr_region_largest_free_bytes(i);
        if (s_region[i].max_bytes != MXR_REGION_MAX_UNLIMITED &&
            lr > (uint32_t)s_region[i].max_bytes)
            lr = (uint32_t)s_region[i].max_bytes;
        if (lr > largest_bytes)
            largest_bytes = lr;
    }

    s_stats.iram_total_bytes = 0;
    s_stats.iram_free_bytes = 0;
    s_stats.iram_min_free_bytes = 0;
    s_stats.iram_fb_zone_total_bytes = 0;
    s_stats.exec_allocs = 0;
    s_stats.iram_fallback_allocs = 0;
    s_stats.iram_exec_zone_total_bytes = 0;
    s_stats.iram_exec_zone_free_bytes = 0;
    s_stats.iram_exec_zone_min_free_bytes = 0;

#ifdef CONFIG_MXR_USE_IRAM
    s_stats.iram_active_allocs = s_iram_desc_count;
    s_stats.iram_fb_region_count = s_iram_fb_region_count;
    if (s_iram_enabled)
    {
        total_bytes += s_iram_total_bytes;
        free_bytes += s_iram_free_bytes;
        s_stats.iram_total_bytes = s_iram_total_bytes;
        s_stats.iram_free_bytes = s_iram_free_bytes;
        s_stats.iram_min_free_bytes = s_iram_min_free_bytes;
        s_stats.iram_fb_zone_total_bytes = s_iram_fb_zone_total;
        s_stats.exec_allocs = s_iram_exec_allocs;
        s_stats.iram_fallback_allocs = s_iram_fallback_allocs;
        s_stats.iram_exec_zone_total_bytes = mxr_iram_exec_zone_end();
        s_stats.iram_exec_zone_free_bytes = s_iram_exec_free_bytes;
        s_stats.iram_exec_zone_min_free_bytes = s_iram_exec_min_free_bytes;
        uint32_t il = mxr_iram_largest_free_zone_aware();
        if (il > largest_bytes)
            largest_bytes = il;
    }
#endif

    s_stats.total_bytes = total_bytes;
    s_stats.free_bytes = free_bytes;

    s_stats.largest_free_block_bytes = (size_t)largest_bytes;
    if (s_stats.free_bytes < s_stats.min_free_bytes)
        s_stats.min_free_bytes = s_stats.free_bytes;

    /* НОВОЕ: метрики фрагментации DRAM */
    {
        uint32_t total_gap_bytes = 0;
        uint32_t gap_count = 0;
        uint32_t sliver_count = 0;
        uint32_t dram_largest = 0;

        for (uint8_t r = 0; r < s_region_count; r++)
        {
            uint32_t cur = s_region[r].start_byte;
            uint32_t end = cur + (uint32_t)s_region[r].total_bytes;

            for (uint16_t i = 0; i < s_dram_desc_count; i++)
            {
                uint32_t off = mxr_desc_off(&s_dram_desc[i]);
                uint32_t len = mxr_desc_len(&s_dram_desc[i]);
                uint32_t block_end = off + (uint32_t)len;

                if (block_end <= s_region[r].start_byte)
                    continue;
                if (off >= end)
                    break;

                if (off > cur)
                {
                    uint32_t gap = (uint32_t)(off - cur);
                    total_gap_bytes += gap;
                    gap_count++;
                    if (gap > dram_largest)
                        dram_largest = gap;
                    if (MXR_IS_SLIVER(gap))
                        sliver_count++;
                }
                if (block_end > cur)
                    cur = block_end;
            }
            if (end > cur)
            {
                uint32_t gap = (uint32_t)(end - cur);
                total_gap_bytes += gap;
                gap_count++;
                if (gap > dram_largest)
                    dram_largest = gap;
                if (MXR_IS_SLIVER(gap))
                    sliver_count++;
            }
        }

        s_stats.gap_count = gap_count;
        s_stats.sliver_count = sliver_count;
        if (total_gap_bytes > 0 && dram_largest < total_gap_bytes)
        {
            s_stats.fragmentation_pct = (uint32_t)(((uint64_t)(total_gap_bytes - dram_largest) * 100) / total_gap_bytes);
        }
        else
        {
            s_stats.fragmentation_pct = 0;
        }
    }

    *status = s_stats;
}

static bool mxr_collect_region_status_locked(int region_index, mxr_region_status_t *status)
{
    if (!status)
        return false;

    if (region_index < 0 || region_index >= s_region_count)
        return false;

    uint8_t i = (uint8_t)region_index;

    status->caps = s_region[i].caps;
    status->start_byte = s_region[i].start_byte;
    status->total_bytes = s_region[i].total_bytes;
    status->min_bytes = s_region[i].min_bytes;
    status->max_bytes = s_region[i].max_bytes;
    status->free_bytes = s_region[i].free_bytes;
    status->min_free_bytes = s_region[i].min_free_bytes;

    uint32_t lr = mxr_region_largest_free_bytes(i);
    if (s_region[i].max_bytes != MXR_REGION_MAX_UNLIMITED &&
        lr > (uint32_t)s_region[i].max_bytes)
        lr = (uint32_t)s_region[i].max_bytes;

    status->largest_free_bytes = lr;
    status->alloc_count = s_region[i].alloc_count;

    return true;
}

bool mxr_get_region_status(int region_index, mxr_region_status_t *status)
{
    mxr_lock();
    bool ok = mxr_collect_region_status_locked(region_index, status);
    mxr_unlock();
    return ok;
}
static bool mxr_collect_iram_fb_region_status_locked(int region_index, mxr_region_status_t *status)
{
#ifdef CONFIG_MXR_USE_IRAM
    if (!status)
        return false;

    if (!s_iram_enabled)
        return false;

    if (region_index < 0 || region_index >= s_iram_fb_region_count)
        return false;

    uint8_t i = (uint8_t)region_index;

    status->caps = s_iram_fb_region[i].caps;
    status->start_byte = s_iram_fb_region[i].start_byte;
    status->total_bytes = s_iram_fb_region[i].total_bytes;
    status->min_bytes = s_iram_fb_region[i].min_bytes;
    status->max_bytes = s_iram_fb_region[i].max_bytes;
    status->free_bytes = s_iram_fb_region[i].free_bytes;
    status->min_free_bytes = s_iram_fb_region[i].min_free_bytes;

    uint32_t lr = mxr_iram_fb_region_largest_free((int)i);
    if (s_iram_fb_region[i].max_bytes != MXR_REGION_MAX_UNLIMITED &&
        lr > (uint32_t)s_iram_fb_region[i].max_bytes)
        lr = (uint32_t)s_iram_fb_region[i].max_bytes;

    status->largest_free_bytes = lr;
    status->alloc_count = s_iram_fb_region[i].alloc_count;

    return true;
#else
    (void)region_index;
    (void)status;
    return false;
#endif
}

bool mxr_get_iram_fb_region_status(int region_index, mxr_region_status_t *status)
{
    mxr_lock();
    bool ok = mxr_collect_iram_fb_region_status_locked(region_index, status);
    mxr_unlock();
    return ok;
}

size_t mxr_get_free_size_caps(uint32_t caps)
{
    if (!s_initialized)
        return 0;

    size_t bytes = 0;

    mxr_lock();

    for (uint8_t i = 0; i < s_region_count; i++)
    {
        if (mxr_region_caps_ok(i, caps))
            bytes += (size_t)s_region[i].free_bytes;
    }

#ifdef CONFIG_MXR_USE_IRAM
    if (s_iram_enabled)
    {
        if (caps & MALLOC_CAP_EXEC)
        {
            if ((caps & ~(MALLOC_CAP_EXEC | MALLOC_CAP_32BIT | MALLOC_CAP_INTERNAL)) == 0)
                bytes += (size_t)s_iram_exec_free_bytes; /* было: s_iram_free_bytes */
        }
#ifdef CONFIG_MXR_IRAM_FALLBACK_ENABLED
        else if ((caps & MALLOC_CAP_32BIT) &&
                 !(caps & (MALLOC_CAP_DMA | MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM)))
        {
            for (uint8_t i = 0; i < s_iram_fb_region_count; i++)
                bytes += (size_t)s_iram_fb_region[i].free_bytes;
        }
        else if ((caps & MALLOC_CAP_INTERNAL) &&
                 !(caps & (MALLOC_CAP_DMA | MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM)))
        {
            /* INTERNAL без 32BIT: IRAM fb имеет INTERNAL в caps */
            for (uint8_t i = 0; i < s_iram_fb_region_count; i++)
                bytes += (size_t)s_iram_fb_region[i].free_bytes;
        }
        else if (caps == 0)
        {
            /* FIX(2.3): caps == 0 может использовать только fallback-зону,
             * EXEC-зона для caps == 0 недоступна.
             *
             * FIX(1.3): если fallback выключен, этот блок не должен
             * добавлять IRAM fb free. */
            for (uint8_t i = 0; i < s_iram_fb_region_count; i++)
                bytes += (size_t)s_iram_fb_region[i].free_bytes;
        }
#endif /* CONFIG_MXR_IRAM_FALLBACK_ENABLED */
    }
#endif

    mxr_unlock();
    return bytes;
}

size_t mxr_get_min_free_size_caps(uint32_t caps)
{
    if (!s_initialized)
        return 0;

    size_t bytes = 0;

    mxr_lock();

    for (uint8_t i = 0; i < s_region_count; i++)
    {
        if (mxr_region_caps_ok(i, caps))
            bytes += (size_t)s_region[i].min_free_bytes;
    }

#ifdef CONFIG_MXR_USE_IRAM
    if (s_iram_enabled)
    {
        if (caps & MALLOC_CAP_EXEC)
        {
            if ((caps & ~(MALLOC_CAP_EXEC | MALLOC_CAP_32BIT | MALLOC_CAP_INTERNAL)) == 0)
                bytes += (size_t)s_iram_exec_min_free_bytes;
        }
#ifdef CONFIG_MXR_IRAM_FALLBACK_ENABLED
        else if ((caps & MALLOC_CAP_32BIT) &&
                 !(caps & (MALLOC_CAP_DMA | MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM)))
        {
            for (uint8_t i = 0; i < s_iram_fb_region_count; i++)
                bytes += (size_t)s_iram_fb_region[i].min_free_bytes;
        }
        else if ((caps & MALLOC_CAP_INTERNAL) &&
                 !(caps & (MALLOC_CAP_DMA | MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM)))
        {
            for (uint8_t i = 0; i < s_iram_fb_region_count; i++)
                bytes += (size_t)s_iram_fb_region[i].min_free_bytes;
        }
        else if (caps == 0)
        {
            /* FIX(2.3): см. комментарий в mxr_get_free_size_caps()
             * FIX(1.3): если fallback выключен, IRAM fb min_free не
             * должен учитываться для 32BIT/caps==0. */
            for (uint8_t i = 0; i < s_iram_fb_region_count; i++)
                bytes += (size_t)s_iram_fb_region[i].min_free_bytes;
        }
#endif /* CONFIG_MXR_IRAM_FALLBACK_ENABLED */
    }
#endif

    mxr_unlock();
    return bytes;
}

/* ================================================================
 *  Dump
 * ================================================================ */
void mxr_dump(void)
{
    mxr_status_t st;

    static mxr_region_status_t rs[MXR_ACTIVE_TOTAL_REGIONS];
    static bool rs_ok[MXR_ACTIVE_TOTAL_REGIONS];

#ifdef CONFIG_MXR_USE_IRAM
    static mxr_region_status_t fb[MXR_IRAM_FB_REGION_COUNT];
    static bool fb_ok[MXR_IRAM_FB_REGION_COUNT];
#endif

    uint8_t *dram_base;
    uint32_t dram_total;
    uint32_t dram_free;
    uint32_t dram_min_free;

#ifdef CONFIG_MXR_USE_IRAM
    uint8_t *iram_base;
    uint32_t iram_total;
    uint32_t iram_free;
    uint32_t iram_min_free;
#endif

#if defined(CONFIG_MXR_DUMP_FULL)
    mxr_desc_t *dram_snap = NULL;
    uint16_t dram_snap_count = 0;

#ifdef CONFIG_MXR_USE_IRAM
    mxr_desc_t *iram_snap = NULL;
    uint16_t iram_snap_count = 0;
#endif
#endif

    mxr_lock();
    if (s_dump_in_progress)
    {
        mxr_unlock();
        return;
    }
    s_dump_in_progress = true;
    mxr_unlock();

#if defined(CONFIG_MXR_DUMP_FULL)
    dram_snap = (mxr_desc_t *)mxr_malloc_caps(
        (size_t)CONFIG_MXR_MAX_DESC * sizeof(mxr_desc_t),
        MALLOC_CAP_32BIT);
    if (dram_snap == NULL)
    {
        /* FIX(bug#6): явное предупреждение вместо silent skip */
        ESP_EARLY_LOGW(TAG, "mxr_dump: cannot allocate DRAM snapshot (%u bytes), "
                            "descriptor dump skipped",
                       (unsigned)(CONFIG_MXR_MAX_DESC * sizeof(mxr_desc_t)));
    }
#ifdef CONFIG_MXR_USE_IRAM
    iram_snap = (mxr_desc_t *)mxr_malloc_caps(
        (size_t)CONFIG_MXR_IRAM_MAX_DESC * sizeof(mxr_desc_t),
        MALLOC_CAP_32BIT);
    if (iram_snap == NULL)
    {
        ESP_EARLY_LOGW(TAG, "mxr_dump: cannot allocate IRAM snapshot (%u bytes), "
                            "descriptor dump skipped",
                       (unsigned)(CONFIG_MXR_IRAM_MAX_DESC * sizeof(mxr_desc_t)));
    }
#endif
#endif

    /* ===================== atomic snapshot ===================== */
    mxr_lock();

    mxr_collect_status_locked(&st);

    for (uint8_t i = 0; i < MXR_ACTIVE_TOTAL_REGIONS; i++)
        rs_ok[i] = mxr_collect_region_status_locked((int)i, &rs[i]);

#ifdef CONFIG_MXR_USE_IRAM
    for (uint8_t i = 0; i < MXR_IRAM_FB_REGION_COUNT; i++)
        fb_ok[i] = mxr_collect_iram_fb_region_status_locked((int)i, &fb[i]);
#endif

    dram_base = s_arena_base;
    dram_total = s_arena_total_bytes;
    dram_free = s_dram_free_bytes;
    dram_min_free = s_dram_min_free_bytes;

#ifdef CONFIG_MXR_USE_IRAM
    iram_base = s_iram_base;
    iram_total = s_iram_total_bytes;
    iram_free = s_iram_free_bytes;
    iram_min_free = s_iram_min_free_bytes;
#endif

#if defined(CONFIG_MXR_DUMP_FULL)
    if (dram_snap)
    {
        dram_snap_count = s_dram_desc_count;
        if (dram_snap_count > CONFIG_MXR_MAX_DESC)
            dram_snap_count = CONFIG_MXR_MAX_DESC;

        mxr_memcpy4(dram_snap, s_dram_desc,
                    (size_t)dram_snap_count * sizeof(mxr_desc_t));
    }

#ifdef CONFIG_MXR_USE_IRAM
    if (iram_snap)
    {
        iram_snap_count = s_iram_desc_count;
        if (iram_snap_count > CONFIG_MXR_IRAM_MAX_DESC)
            iram_snap_count = CONFIG_MXR_IRAM_MAX_DESC;

        mxr_memcpy4(iram_snap, s_iram_desc,
                    (size_t)iram_snap_count * sizeof(mxr_desc_t));
    }
#endif
#endif

    mxr_unlock();
    /* ============================================================ */

    ESP_EARLY_LOGI(TAG, "MxR dump: initialized=%d", (int)st.initialized);
    if (!st.initialized)
        goto cleanup;

    ESP_EARLY_LOGI(TAG,
                   "total=%u free=%u min_free=%u largest=%u",
                   (unsigned)st.total_bytes,
                   (unsigned)st.free_bytes,
                   (unsigned)st.min_free_bytes,
                   (unsigned)st.largest_free_block_bytes);

#if defined(CONFIG_MXR_DUMP_NORMAL) || defined(CONFIG_MXR_DUMP_FULL)

    ESP_EARLY_LOGI(TAG,
                   "desc dram=%u/%u iram=%u/%u max_active=%u",
                   (unsigned)st.dram_active_allocs,
                   (unsigned)st.dram_desc_capacity,
                   (unsigned)st.iram_active_allocs,
                   (unsigned)st.iram_desc_capacity,
                   (unsigned)st.max_active_allocs);

    /* FIX(3.2): больше диагностики cross-skip */
    ESP_EARLY_LOGI(TAG,
                   "exec=%u iram_fb=%u cross=%u cross_skip=%u guard_rej=%u "
                   "caps_skip=%u free_skip=%u cache_skip=%u",
                   (unsigned)st.exec_allocs,
                   (unsigned)st.iram_fallback_allocs,
                   (unsigned)st.cross_region_allocs,
                   (unsigned)st.cross_region_skip_fragmented,
                   (unsigned)st.cross_region_guard_rejects,
                   (unsigned)st.cross_caps_skips,
                   (unsigned)st.cross_free_skips,
                   (unsigned)st.cross_cache_skips);

    /* FIX(3.3): insert-fail counters */
    ESP_EARLY_LOGI(TAG,
                   "insert_fail: bounds=%u overlap=%u dup=%u table_full=%u",
                   (unsigned)st.desc_insert_fail_bounds,
                   (unsigned)st.desc_insert_fail_overlap,
                   (unsigned)st.desc_insert_fail_duplicate,
                   (unsigned)st.alloc_fail_table_full);

    /* FIX(4.3): region init fallback */
    /* FIX(4.3 + 4c): region init fallback */
    ESP_EARLY_LOGI(TAG,
                   "region_init=%s iram_fb_init=%s",
                   st.region_init_fallback ? "SINGLE_FALLBACK" : "ok",
                   st.iram_fb_region_init_fallback ? "FLAT_FALLBACK" : "ok");

    {
        uint32_t sliver_pct = 0;
        if (st.gap_count > 0)
            sliver_pct = (st.sliver_count * 100) / st.gap_count;

        /* FIX(4.4): явно помечаем, что это DRAM-фрагментация */
        ESP_EARLY_LOGI(TAG,
                       "DRAM frag: pct=%u%% gaps=%u slivers=%u(%u%%) "
                       "bf_early=%u anti_sliver=%u",
                       (unsigned)st.fragmentation_pct,
                       (unsigned)st.gap_count,
                       (unsigned)st.sliver_count,
                       (unsigned)sliver_pct,
                       (unsigned)st.best_fit_early_exits,
                       (unsigned)st.anti_sliver_expansions);
    }

    ESP_EARLY_LOGI(TAG,
                   "DRAM: base=%p total=%u free=%u min_free=%u",
                   dram_base,
                   (unsigned)dram_total,
                   (unsigned)dram_free,
                   (unsigned)dram_min_free);

#ifdef CONFIG_MXR_USE_IRAM
    if (s_iram_enabled)
    {
        ESP_EARLY_LOGI(TAG,
                       "IRAM: base=%p total=%u free=%u min_free=%u fb_zone=%u "
                       "exec_zone=%u exec_free=%u exec_rejects=%u",
                       iram_base,
                       (unsigned)iram_total,
                       (unsigned)iram_free,
                       (unsigned)iram_min_free,
                       (unsigned)st.iram_fb_zone_total_bytes,
                       (unsigned)st.iram_exec_zone_total_bytes,
                       (unsigned)st.iram_exec_zone_free_bytes,
                       (unsigned)st.exec_zone_rejects);

        for (uint8_t i = 0;
             i < st.iram_fb_region_count && i < MXR_IRAM_FB_REGION_COUNT;
             i++)
        {
            if (!fb_ok[i])
                continue;

            /* FIX(4.5): max=0 печатаем как -1 */
            ESP_EARLY_LOGI(TAG,
                           "iram_fb %u: start=%u total=%u min=%u max=%d "
                           "free=%u min_free=%u largest=%u alloc=%u",
                           (unsigned)i,
                           (unsigned)fb[i].start_byte,
                           (unsigned)fb[i].total_bytes,
                           (unsigned)fb[i].min_bytes,
                           (int)(fb[i].max_bytes == MXR_REGION_MAX_UNLIMITED
                                     ? -1
                                     : fb[i].max_bytes),
                           (unsigned)fb[i].free_bytes,
                           (unsigned)fb[i].min_free_bytes,
                           (unsigned)fb[i].largest_free_bytes,
                           (unsigned)fb[i].alloc_count);
        }
    }
    else
    {
        ESP_EARLY_LOGI(TAG, "IRAM: disabled");
    }
#endif

    for (uint8_t i = 0; i < st.region_count && i < MXR_ACTIVE_TOTAL_REGIONS; i++)
    {
        if (!rs_ok[i])
            continue;

        /* FIX(4.5): max=0 печатаем как -1 */
        ESP_EARLY_LOGI(TAG,
                       "region %u: caps=0x%08x start=%u total=%u min=%u max=%d "
                       "free=%u min_free=%u largest=%u alloc=%u",
                       (unsigned)i,
                       (unsigned)rs[i].caps,
                       (unsigned)rs[i].start_byte,
                       (unsigned)rs[i].total_bytes,
                       (unsigned)rs[i].min_bytes,
                       (int)(rs[i].max_bytes == MXR_REGION_MAX_UNLIMITED
                                 ? -1
                                 : rs[i].max_bytes),
                       (unsigned)rs[i].free_bytes,
                       (unsigned)rs[i].min_free_bytes,
                       (unsigned)rs[i].largest_free_bytes,
                       (unsigned)rs[i].alloc_count);
    }

    ESP_EARLY_LOGI(TAG,
                   "stats: fail_mem=%u fail_table=%u invalid_free=%u",
                   (unsigned)st.alloc_fail_no_memory,
                   (unsigned)st.alloc_fail_table_full,
                   (unsigned)st.invalid_free_attempts);

#endif /* NORMAL || FULL */

#if defined(CONFIG_MXR_DUMP_FULL)
    if (dram_snap)
    {
        for (uint16_t i = 0; i < dram_snap_count; i++)
        {
            ESP_EARLY_LOGI(TAG, "dram[%u]: off=%u len=%u",
                           (unsigned)i,
                           (unsigned)mxr_desc_off(&dram_snap[i]),
                           (unsigned)mxr_desc_len(&dram_snap[i]));
        }
    }

#ifdef CONFIG_MXR_USE_IRAM
    if (iram_snap)
    {
        for (uint16_t i = 0; i < iram_snap_count; i++)
        {
            ESP_EARLY_LOGI(TAG, "iram[%u]: off=%u len=%u exec=%d",
                           (unsigned)i,
                           (unsigned)mxr_desc_off(&iram_snap[i]),
                           (unsigned)mxr_desc_len(&iram_snap[i]),
                           (int)mxr_desc_is_exec(&iram_snap[i]));
        }
    }
#endif
#endif /* FULL */

cleanup:;
#if defined(CONFIG_MXR_DUMP_FULL)
    if (dram_snap)
        mxr_free(dram_snap);

#ifdef CONFIG_MXR_USE_IRAM
    if (iram_snap)
        mxr_free(iram_snap);
#endif
#endif

    mxr_lock();
    s_dump_in_progress = false;
    mxr_unlock();
}

void mxr_get_status(mxr_status_t *status)
{
    if (!status)
        return;

    mxr_lock();
    mxr_collect_status_locked(status);
    mxr_unlock();
}