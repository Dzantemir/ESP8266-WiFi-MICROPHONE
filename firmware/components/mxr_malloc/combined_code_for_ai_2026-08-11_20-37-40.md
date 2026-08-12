# Project Source Code Export
This file contains the project structure, source code, and binary hex-dumps generated for AI analysis.

## File: `mxr_malloc.c` (32597 tokens)
```c
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
/* Скаляр оставляем всегда (1 байт): нужен в status/dump,
 * при выключенном fallback всегда == 0 */
static uint8_t s_iram_fb_region_count;
#ifdef CONFIG_MXR_IRAM_FALLBACK_ENABLED
static mxr_region_t s_iram_fb_region[MXR_IRAM_FB_REGION_COUNT] MXR_IRAM_DATA_ATTR;
#endif

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

static inline uint32_t mxr_percent_of(uint32_t total, uint32_t percent)
{
    return (total / 100u) * percent + ((total % 100u) * percent) / 100u;
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
#ifdef CONFIG_MXR_IRAM_FALLBACK_ENABLED
static bool MXR_IRAM_ATTR mxr_iram_fb_find_free_in_region(
    int reg,
    uint32_t bytes,
    uint32_t *out_off,
    uint32_t *out_alloc_bytes);
static uint32_t MXR_IRAM_ATTR mxr_iram_fb_region_largest_free(int reg);
#endif /* CONFIG_MXR_IRAM_FALLBACK_ENABLED */

#if defined(CONFIG_MXR_CROSS_REGION_FALLBACK) && \
    defined(CONFIG_MXR_IRAM_CROSS_ENABLED) &&    \
    defined(CONFIG_MXR_IRAM_FALLBACK_ENABLED)

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

#ifdef CONFIG_MXR_IRAM_FALLBACK_ENABLED
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
#endif /* CONFIG_MXR_IRAM_FALLBACK_ENABLED */

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
#ifdef CONFIG_MXR_IRAM_FALLBACK_ENABLED
/* ================================================================
 *  FIX(3.3): BEST-FIT с early-exit внутри одного fb-региона.
 *  Раньше: last-fit first-match (первый gap с конца).
 *  Теперь: наименьший подходящий gap; при равных gap предпочтение
 *  более высокому адресу (дальше от EXEC-зоны). Блок размещается
 *  у ВЕРХНЕЙ границы gap, чтобы низ оставался свободным.
 * ================================================================ */
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
    uint32_t max_allowed = s_iram_fb_region[reg].max_bytes;
    if (s_iram_fb_region[reg].free_bytes < bytes)
        return false;

    uint32_t cur = reg_start;
    uint32_t best_off = 0;
    uint32_t best_gap = UINT32_MAX;
    bool found = false;

#if MXR_EARLY_EXIT_ACTIVE
    uint32_t waste_limit = bytes >> MXR_BEST_FIT_WASTE_SHIFT;
    if (waste_limit < MXR_ALIGN_SIZE)
        waste_limit = MXR_ALIGN_SIZE;
#else
    uint32_t waste_limit = 0;
#endif

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
            if (gap >= bytes)
            {
                uint32_t waste = gap - bytes;
                if (waste <= waste_limit)
                {
                    /* Early-exit: gap достаточно хорош */
                    if (MXR_IS_SLIVER(waste) &&
                        (max_allowed == MXR_REGION_MAX_UNLIMITED || gap <= max_allowed))
                    {
                        *out_off = cur;
                        *out_alloc_bytes = gap;
                        s_stats.anti_sliver_expansions++;
                    }
                    else
                    {
                        *out_off = (cur + gap) - bytes; /* прижать к верху gap */
                        *out_alloc_bytes = bytes;
                    }
                    s_stats.best_fit_early_exits++;
                    return true;
                }
                /* меньший gap лучше; при равенстве — выше адрес */
                if (gap < best_gap || (gap == best_gap && cur > best_off))
                {
                    best_gap = gap;
                    best_off = cur;
                    found = true;
                }
            }
        }
        if (block_end > cur)
            cur = block_end;
        if (cur >= reg_end)
            break;
    }

    /* Хвост региона */
    if (reg_end > cur)
    {
        uint32_t gap = (uint32_t)(reg_end - cur);
        if (gap >= bytes)
        {
            uint32_t waste = gap - bytes;
            if (waste <= waste_limit)
            {
                if (MXR_IS_SLIVER(waste) &&
                    (max_allowed == MXR_REGION_MAX_UNLIMITED || gap <= max_allowed))
                {
                    *out_off = cur;
                    *out_alloc_bytes = gap;
                    s_stats.anti_sliver_expansions++;
                }
                else
                {
                    *out_off = (cur + gap) - bytes;
                    *out_alloc_bytes = bytes;
                }
                s_stats.best_fit_early_exits++;
                return true;
            }
            if (gap < best_gap || (gap == best_gap && cur > best_off))
            {
                best_gap = gap;
                best_off = cur;
                found = true;
            }
        }
    }

    if (!found)
        return false;

    uint32_t waste = best_gap - bytes;
    if (MXR_IS_SLIVER(waste) &&
        (max_allowed == MXR_REGION_MAX_UNLIMITED || best_gap <= max_allowed))
    {
        *out_off = best_off;
        *out_alloc_bytes = best_gap;
        s_stats.anti_sliver_expansions++;
    }
    else
    {
        *out_off = (best_off + best_gap) - bytes;
        *out_alloc_bytes = bytes;
    }
    return true;
}
#endif /* CONFIG_MXR_IRAM_FALLBACK_ENABLED (find_free_in_region) */

#if defined(CONFIG_MXR_CROSS_REGION_FALLBACK) && \
    defined(CONFIG_MXR_IRAM_CROSS_ENABLED) &&    \
    defined(CONFIG_MXR_IRAM_FALLBACK_ENABLED)
/* ================================================================
 *  FIX(3.3): IRAM fb search + largest, BEST-FIT (полный проход,
 *  т.к. largest всё равно нужно досчитать).
 * ================================================================ */
static bool MXR_IRAM_ATTR mxr_iram_fb_find_free_and_largest(
    int reg,
    uint32_t bytes,
    uint32_t *out_off,
    uint32_t *out_largest,
    uint32_t *out_alloc_bytes)
{
    if (reg < 0 || reg >= (int)s_iram_fb_region_count)
        return false;
    uint32_t reg_start = s_iram_fb_region[reg].start_byte;
    uint32_t reg_end = mxr_iram_fb_region_end(reg);
    uint32_t max_allowed = s_iram_fb_region[reg].max_bytes;
    if (s_iram_fb_region[reg].free_bytes < bytes)
        return false;

    uint32_t cur = reg_start;
    uint32_t largest = 0;
    uint32_t best_off = 0;
    uint32_t best_gap = UINT32_MAX;
    bool found = false;

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
            if (gap >= bytes)
            {
                if (!found || gap < best_gap || (gap == best_gap && cur > best_off))
                {
                    best_off = cur;
                    best_gap = gap;
                    found = true;
                }
            }
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
        if (gap >= bytes)
        {
            if (!found || gap < best_gap || (gap == best_gap && cur > best_off))
            {
                best_off = cur;
                best_gap = gap;
                found = true;
            }
        }
    }

    if (out_largest)
        *out_largest = largest;
    if (!found)
        return false;

    uint32_t waste = best_gap - bytes;
    if (MXR_IS_SLIVER(waste) &&
        (max_allowed == MXR_REGION_MAX_UNLIMITED || best_gap <= max_allowed))
    {
        *out_off = best_off;
        *out_alloc_bytes = best_gap;
        s_stats.anti_sliver_expansions++;
    }
    else
    {
        *out_off = (best_off + best_gap) - bytes;
        *out_alloc_bytes = bytes;
    }
    return true;
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
#ifdef CONFIG_MXR_IRAM_FALLBACK_ENABLED
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

/* FIX: убран избыточный fallback на last/first регион — он маскировал
 * ошибки конфигурации. Основной цикл всегда находит подходящий регион
 * (последний регион unlimited), иначе конфигурация некорректна и
 * cross-region пусть разбирается сам. */
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
#endif /* CONFIG_MXR_IRAM_FALLBACK_ENABLED (IRAM fallback region helpers) */
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
#ifdef CONFIG_MXR_IRAM_FALLBACK_ENABLED
    else
        mxr_iram_fb_region_allocated(mxr_iram_fb_region_by_off(off_bytes), bytes, count_block);
#endif
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
#ifdef CONFIG_MXR_IRAM_FALLBACK_ENABLED
    else
        mxr_iram_fb_region_released(mxr_iram_fb_region_by_off(off_bytes), bytes, count_block);
#endif
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

    /* EXEC уходит в отдельную EXEC-зону, не в fallback */
    if (caps & MALLOC_CAP_EXEC)
        return false;

    /* 8BIT/DMA/SPIRAM в IRAM нельзя никогда */
    if (caps & (MALLOC_CAP_DMA | MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM))
        return false;

    /* ESP32-совместимость: MALLOC_CAP_INTERNAL = любая внутренняя память
     * (DRAM или IRAM). Чистый INTERNAL теперь может использовать IRAM fb
     * точно так же, как 32BIT. При дефолтном порядке DRAM-first INTERNAL
     * сначала пытается в DRAM и уходит в IRAM только при нехватке DRAM. */
    if ((caps & MALLOC_CAP_32BIT) || (caps & MALLOC_CAP_INTERNAL) || caps == 0)
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
#ifdef CONFIG_MXR_IRAM_FALLBACK_ENABLED
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
#endif
#ifdef CONFIG_MXR_IRAM_FALLBACK_ENABLED
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
            bytes = mxr_percent_of(s_iram_fb_zone_total, cfg[i].percent); /* FIX(1.5) */
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

#endif /* CONFIG_MXR_IRAM_FALLBACK_ENABLED (mxr_init_iram_fb_regions) */

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
#ifdef CONFIG_MXR_IRAM_FALLBACK_ENABLED
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
#else
    /* FIX(1.2/1.3): fallback выключен.
     * По умолчанию (MXR_IRAM_EXEC_WHOLE_IF_NO_FB=y) вся IRAM отдаётся
     * EXEC-зоне, чтобы память не простаивала.
     * Иначе EXEC остаётся [0, reserve), а неиспользуемый остаток
     * исключается из арены и из статистики. */
#ifdef CONFIG_MXR_IRAM_EXEC_WHOLE_IF_NO_FB
    s_iram_fb_zone_start = s_iram_total_bytes; /* EXEC-зона = вся IRAM */
    s_iram_fb_zone_total = 0;
#else
    if (reserve > s_iram_total_bytes)
        reserve = s_iram_total_bytes;
    s_iram_fb_zone_start = reserve;
    s_iram_fb_zone_total = 0;
    /* Урезаем арену до реально используемой EXEC-зоны */
    s_iram_total_bytes = reserve;
    s_iram_free_bytes = reserve;
    s_iram_min_free_bytes = reserve;
    if (s_iram_total_bytes == 0)
    {
        ESP_EARLY_LOGD(TAG, "IRAM heap: reserve=0 and fallback disabled, IRAM arena off");
        return; /* s_iram_enabled остаётся false */
    }
#endif
#endif /* CONFIG_MXR_IRAM_FALLBACK_ENABLED */

    /* EXEC-зона = [0, fb_zone_start) */
    s_iram_exec_free_bytes = s_iram_fb_zone_start;
    s_iram_exec_min_free_bytes = s_iram_fb_zone_start;

#ifdef CONFIG_MXR_IRAM_FALLBACK_ENABLED
    if (!mxr_init_iram_fb_regions())
    {
        ESP_EARLY_LOGW(TAG, "IRAM fb region init failed, using flat");
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
        s_stats.iram_fb_region_init_fallback = false;
    }
#else
    /* FIX(1.3): fb-регионы существуют только при включённом fallback */
    s_iram_fb_region_count = 0;
    s_stats.iram_fb_region_init_fallback = false;
#endif

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
 *  FIX(2.2): попытка 32BIT IRAM fallback вынесена в отдельную
 *  функцию, чтобы порядок (IRAM-first / DRAM-first) задавался
 *  конфигурацией CONFIG_MXR_IRAM_FB_ORDER_*.
 * ================================================================ */
#if defined(CONFIG_MXR_USE_IRAM) && defined(CONFIG_MXR_IRAM_FALLBACK_ENABLED)
static void *MXR_IRAM_ATTR mxr_try_iram_fallback(uint32_t bytes, uint32_t caps)
{
    if (!s_iram_enabled)
        return NULL;
    if (!mxr_caps_allow_iram_fallback(caps))
        return NULL;
    if (!mxr_iram_can_fallback(bytes))
        return NULL;

    int fb_reg = mxr_iram_fb_region_for_size(bytes);
    uint32_t off_bytes = 0;
    bool found = false;

    /* Step 1: свой fb-регион */
    if (fb_reg >= 0)
    {
        uint32_t alloc_bytes = bytes;
        if (s_iram_fb_region[fb_reg].largest_cache_valid &&
            s_iram_fb_region[fb_reg].largest_free_cache < bytes)
        {
            found = false; /* пропускаем Step 1, сразу в cross-region */
        }
        else
        {
            found = mxr_iram_fb_find_free_in_region(fb_reg, bytes,
                                                    &off_bytes, &alloc_bytes);
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
#if defined(CONFIG_MXR_CROSS_REGION_FALLBACK) && \
    defined(CONFIG_MXR_IRAM_CROSS_ENABLED)
    if (!found)
    {
        found = mxr_iram_fb_try_cross_region(bytes, fb_reg,
                                             &off_bytes, &cross_alloc_bytes);
    }
#endif
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
    return NULL;
}
#endif /* CONFIG_MXR_USE_IRAM && CONFIG_MXR_IRAM_FALLBACK_ENABLED */
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

#if defined(CONFIG_MXR_USE_IRAM) && defined(CONFIG_MXR_IRAM_FB_ORDER_IRAM_FIRST)
    /* FIX(2.2): IRAM-first — старое поведение MxR */
    {
        void *iram_ptr = mxr_try_iram_fallback(bytes, caps);
        if (iram_ptr)
            return iram_ptr;
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

#if defined(CONFIG_MXR_USE_IRAM) && defined(CONFIG_MXR_IRAM_FB_ORDER_DRAM_FIRST)
    /* FIX(2.2): DRAM-first — IRAM только как настоящий fallback */
    {
        void *iram_ptr = mxr_try_iram_fallback(bytes, caps);
        if (iram_ptr)
            return iram_ptr;
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

/* FIX(1.4): region_size_ok проверяется ДО любой модификации блока,
 * и для shrink, и для grow. Раньше IRAM shrink мог оставить
 * fallback-блок меньше min_bytes его региона. */
#ifdef CONFIG_MXR_IRAM_FALLBACK_ENABLED
        if (in_place_allowed && !old_exec)
        {
            int reg = mxr_iram_fb_region_by_off(off_bytes);
            if (!mxr_iram_fb_region_size_ok(reg, new_bytes))
                in_place_allowed = false;
            else if (new_bytes > old_bytes &&
                     !mxr_iram_can_grow_fallback(off_bytes, old_bytes, new_bytes))
                in_place_allowed = false;
        }
#endif
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
                /* Anti-sliver */
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
#ifdef CONFIG_MXR_IRAM_FALLBACK_ENABLED
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
#endif
                if (next_boundary >= block_end)
                {
                    uint32_t gap = (uint32_t)(next_boundary - block_end);
                    if (gap >= extra)
                    {
                        uint32_t tail = gap - extra;
                        uint32_t actual_new_bytes = new_bytes;
                        /* ===== ИСПРАВЛЕНО: ограничение max_bytes ===== */
                        bool can_expand = MXR_IS_SLIVER(tail);
#ifdef CONFIG_MXR_IRAM_FALLBACK_ENABLED
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
#endif
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
            bytes = mxr_percent_of(s_arena_total_bytes, cfg[i].percent); /* FIX(1.5) */
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
            /* FIX(1.5): 32-бит достаточно (значение <= arena_total * 100) */
            s_stats.fragmentation_pct = ((total_gap_bytes - dram_largest) * 100u) / total_gap_bytes;
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
#if defined(CONFIG_MXR_USE_IRAM) && defined(CONFIG_MXR_IRAM_FALLBACK_ENABLED)
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
 *  FIX(2.3): дополнительные стандартные heap_caps query API
 * ================================================================ */
size_t mxr_get_total_size_caps(uint32_t caps)
{
    if (!s_initialized)
        return 0;
    size_t bytes = 0;
    mxr_lock();
    for (uint8_t i = 0; i < s_region_count; i++)
    {
        if (mxr_region_caps_ok(i, caps))
            bytes += (size_t)s_region[i].total_bytes;
    }
#ifdef CONFIG_MXR_USE_IRAM
    if (s_iram_enabled)
    {
        if (caps & MALLOC_CAP_EXEC)
        {
            if ((caps & ~(MALLOC_CAP_EXEC | MALLOC_CAP_32BIT | MALLOC_CAP_INTERNAL)) == 0)
                bytes += (size_t)mxr_iram_exec_zone_end();
        }
#ifdef CONFIG_MXR_IRAM_FALLBACK_ENABLED
        else if ((caps & MALLOC_CAP_32BIT) &&
                 !(caps & (MALLOC_CAP_DMA | MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM)))
        {
            bytes += (size_t)s_iram_fb_zone_total;
        }
        else if ((caps & MALLOC_CAP_INTERNAL) &&
                 !(caps & (MALLOC_CAP_DMA | MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM)))
        {
            bytes += (size_t)s_iram_fb_zone_total;
        }
        else if (caps == 0)
        {
            bytes += (size_t)s_iram_fb_zone_total;
        }
#endif /* CONFIG_MXR_IRAM_FALLBACK_ENABLED */
    }
#endif /* CONFIG_MXR_USE_IRAM */
    mxr_unlock();
    return bytes;
}

size_t mxr_get_largest_free_block_caps(uint32_t caps)
{
    if (!s_initialized)
        return 0;
    uint32_t largest = 0;
    mxr_lock();
    for (uint8_t i = 0; i < s_region_count; i++)
    {
        if (!mxr_region_caps_ok(i, caps))
            continue;
        uint32_t lr = mxr_region_largest_free_bytes(i);
        if (s_region[i].max_bytes != MXR_REGION_MAX_UNLIMITED &&
            lr > (uint32_t)s_region[i].max_bytes)
            lr = (uint32_t)s_region[i].max_bytes;
        if (lr > largest)
            largest = lr;
    }
#ifdef CONFIG_MXR_USE_IRAM
    if (s_iram_enabled)
    {
        if (caps & MALLOC_CAP_EXEC)
        {
            if ((caps & ~(MALLOC_CAP_EXEC | MALLOC_CAP_32BIT | MALLOC_CAP_INTERNAL)) == 0)
            {
                uint32_t lr = mxr_iram_exec_largest_free();
                if (lr > largest)
                    largest = lr;
            }
        }
#ifdef CONFIG_MXR_IRAM_FALLBACK_ENABLED
        else if (((caps & MALLOC_CAP_32BIT) &&
                  !(caps & (MALLOC_CAP_DMA | MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM))) ||
                 ((caps & MALLOC_CAP_INTERNAL) &&
                  !(caps & (MALLOC_CAP_DMA | MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM))) ||
                 caps == 0)
        {
            for (uint8_t i = 0; i < s_iram_fb_region_count; i++)
            {
                uint32_t lr = mxr_iram_fb_region_largest_free((int)i);
                if (s_iram_fb_region[i].max_bytes != MXR_REGION_MAX_UNLIMITED &&
                    lr > (uint32_t)s_iram_fb_region[i].max_bytes)
                    lr = (uint32_t)s_iram_fb_region[i].max_bytes;
                if (lr > largest)
                    largest = lr;
            }
        }
#endif /* CONFIG_MXR_IRAM_FALLBACK_ENABLED */
    }
#endif /* CONFIG_MXR_USE_IRAM */
    mxr_unlock();
    return (size_t)largest;
}

size_t mxr_get_allocated_size_caps(uint32_t caps)
{
    size_t total = mxr_get_total_size_caps(caps);
    size_t free_bytes = mxr_get_free_size_caps(caps);
    return (total > free_bytes) ? (total - free_bytes) : 0;
}

/* ================================================================
 *  Dump
 * ================================================================ */
void mxr_dump(void)
{
    mxr_status_t st;

    static mxr_region_status_t rs[MXR_ACTIVE_TOTAL_REGIONS];
    static bool rs_ok[MXR_ACTIVE_TOTAL_REGIONS];

#if defined(CONFIG_MXR_USE_IRAM) && defined(CONFIG_MXR_IRAM_FALLBACK_ENABLED)
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

#if defined(CONFIG_MXR_USE_IRAM) && defined(CONFIG_MXR_IRAM_FALLBACK_ENABLED)
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
#if defined(CONFIG_MXR_USE_IRAM) && defined(CONFIG_MXR_IRAM_FALLBACK_ENABLED)
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
#endif
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
```

## File: `mxr_heap_wrap.c` (1080 tokens)
```c
#include "mxr_malloc.h"
#include <stdint.h>
#include <stddef.h>
#include "esp_attr.h"
#include <string.h>
#include "esp_log.h"

#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

#ifdef CONFIG_MXR_IRAM_HOT_PATH_DISABLED
#define MXR_WRAP_IRAM
#define MXR_WRAP_ALLOC_ATTR
#else
#define MXR_WRAP_IRAM IRAM_ATTR
#ifdef CONFIG_MXR_IRAM_PATH_ALLOC_FAMILY
#define MXR_WRAP_ALLOC_ATTR IRAM_ATTR
#else
#define MXR_WRAP_ALLOC_ATTR
#endif
#endif

/* ================================================================
 *  Base wraps
 * ================================================================ */
void __wrap_heap_caps_init(void)
{
    mxr_init();
}

void *MXR_WRAP_IRAM __wrap__heap_caps_malloc(
    size_t size, uint32_t caps, const char *file, size_t line)
{
    (void)file;
    (void)line;
    return mxr_malloc_caps(size, caps);
}

void MXR_WRAP_IRAM __wrap__heap_caps_free(
    void *ptr, const char *file, size_t line)
{
    (void)file;
    (void)line;
    mxr_free(ptr);
}

void *MXR_WRAP_ALLOC_ATTR __wrap__heap_caps_realloc(
    void *mem, size_t newsize, uint32_t caps, const char *file, size_t line)
{
    (void)file;
    (void)line;
    return mxr_realloc_caps(mem, newsize, caps);
}

void *MXR_WRAP_ALLOC_ATTR __wrap__heap_caps_calloc(
    size_t count, size_t size, uint32_t caps, const char *file, size_t line)
{
    (void)file;
    (void)line;
    return mxr_calloc_caps(count, size, caps);
}

void *MXR_WRAP_ALLOC_ATTR __wrap__heap_caps_zalloc(
    size_t size, uint32_t caps, const char *file, size_t line)
{
    (void)file;
    (void)line;
    return mxr_zalloc_caps(size, caps);
}

/* ================================================================
 *  Heap query wraps
 * ================================================================ */
#ifdef CONFIG_MXR_WRAP_HEAP_QUERY
size_t __wrap_heap_caps_get_free_size(uint32_t caps)
{
    return mxr_get_free_size_caps(caps);
}

size_t __wrap_heap_caps_get_minimum_free_size(uint32_t caps)
{
    return mxr_get_min_free_size_caps(caps);
}

size_t __wrap_heap_caps_get_dram_free_size(void)
{
    return mxr_get_free_size_caps(
        MALLOC_CAP_8BIT | MALLOC_CAP_32BIT | MALLOC_CAP_DMA);
}
/* FIX(2.3) */
size_t __wrap_heap_caps_get_total_size(uint32_t caps)
{
    return mxr_get_total_size_caps(caps);
}

size_t __wrap_heap_caps_get_allocated_size(uint32_t caps)
{
    return mxr_get_allocated_size_caps(caps);
}

size_t __wrap_heap_caps_get_largest_free_block(uint32_t caps)
{
    return mxr_get_largest_free_block_caps(caps);
}
#endif /* CONFIG_MXR_WRAP_HEAP_QUERY */

/* ================================================================
 *  Default pool wraps
 * ================================================================ */
#ifdef CONFIG_MXR_WRAP_DEFAULT_POOL
void *__wrap_heap_caps_malloc_default(size_t size)
{
    return mxr_malloc_caps(size, MALLOC_CAP_32BIT);
}

void *MXR_WRAP_ALLOC_ATTR __wrap_heap_caps_realloc_default(void *ptr, size_t size)
{
    return mxr_realloc_caps(ptr, size, MALLOC_CAP_32BIT);
}
#endif /* CONFIG_MXR_WRAP_DEFAULT_POOL */

/* ================================================================
 *  ESP system heap wraps
 * ================================================================ */
#ifdef CONFIG_MXR_WRAP_ESP_SYSTEM
size_t __wrap_esp_get_free_heap_size(void)
{
    return mxr_get_free_size_caps(MALLOC_CAP_32BIT);
}

size_t __wrap_esp_get_minimum_free_heap_size(void)
{
    return mxr_get_min_free_size_caps(MALLOC_CAP_32BIT);
}

size_t __wrap_esp_get_free_internal_heap_size(void)
{
    return mxr_get_free_size_caps(MALLOC_CAP_INTERNAL);
}
#endif /* CONFIG_MXR_WRAP_ESP_SYSTEM */

/* ================================================================
 *  Optional libc wraps
 * ================================================================ */
#ifdef CONFIG_MXR_WRAP_LIBC
void *MXR_WRAP_IRAM __wrap_malloc(size_t n)
{
    return mxr_malloc_caps(n, MALLOC_CAP_32BIT);
}

void MXR_WRAP_IRAM __wrap_free(void *ptr)
{
    mxr_free(ptr);
}

void *MXR_WRAP_ALLOC_ATTR __wrap_calloc(size_t c, size_t s)
{
    return mxr_calloc_caps(c, s, MALLOC_CAP_32BIT);
}

void *MXR_WRAP_ALLOC_ATTR __wrap_realloc(void *old_ptr, size_t n)
{
    return mxr_realloc_caps(old_ptr, n, MALLOC_CAP_32BIT);
}

void *MXR_WRAP_ALLOC_ATTR __wrap_zalloc(size_t n)
{
    return mxr_zalloc_caps(n, MALLOC_CAP_32BIT);
}
#endif /* CONFIG_MXR_WRAP_LIBC */
```

## File: `mxr_heap_port.c` (230 tokens)
```c
#include "mxr_malloc.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Optional direct libc wrappers.
 * Do NOT compile this file in wrap mode.
 */

void *malloc(size_t n)
{
    void *ra = (void *)__builtin_return_address(0);
    return _heap_caps_malloc(n, MALLOC_CAP_32BIT, (const char *)ra, 0);
}

void free(void *ptr)
{
    void *ra = (void *)__builtin_return_address(0);
    _heap_caps_free(ptr, (const char *)ra, 0);
}

void *calloc(size_t c, size_t s)
{
    void *ra = (void *)__builtin_return_address(0);
    return _heap_caps_calloc(c, s, MALLOC_CAP_32BIT, (const char *)ra, 0);
}

void *realloc(void *old_ptr, size_t n)
{
    void *ra = (void *)__builtin_return_address(0);
    return _heap_caps_realloc(old_ptr, n, MALLOC_CAP_32BIT, (const char *)ra, 0);
}

void *zalloc(size_t n)
{
    void *ra = (void *)__builtin_return_address(0);
    return _heap_caps_zalloc(n, MALLOC_CAP_32BIT, (const char *)ra, 0);
}
```

## File: `mxr_heap_compat.c` (580 tokens)
```c
#include "mxr_malloc.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "esp_attr.h"

#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

#ifdef CONFIG_MXR_IRAM_HOT_PATH_DISABLED
#define MXR_COMPAT_IRAM
#define MXR_COMPAT_ALLOC_ATTR
#else
#define MXR_COMPAT_IRAM IRAM_ATTR
#ifdef CONFIG_MXR_IRAM_PATH_ALLOC_FAMILY
#define MXR_COMPAT_ALLOC_ATTR IRAM_ATTR
#else
#define MXR_COMPAT_ALLOC_ATTR
#endif
#endif

void heap_caps_init(void)
{
    mxr_init();
}

void *MXR_COMPAT_IRAM _heap_caps_malloc(
    size_t size, uint32_t caps, const char *file, size_t line)
{
    (void)file;
    (void)line;
    return mxr_malloc_caps(size, caps);
}

void MXR_COMPAT_IRAM _heap_caps_free(
    void *ptr, const char *file, size_t line)
{
    (void)file;
    (void)line;
    mxr_free(ptr);
}

void *MXR_COMPAT_ALLOC_ATTR _heap_caps_calloc(
    size_t count, size_t size, uint32_t caps, const char *file, size_t line)
{
    (void)file;
    (void)line;
    return mxr_calloc_caps(count, size, caps);
}

void *MXR_COMPAT_ALLOC_ATTR _heap_caps_realloc(
    void *mem, size_t newsize, uint32_t caps, const char *file, size_t line)
{
    (void)file;
    (void)line;
    return mxr_realloc_caps(mem, newsize, caps);
}

void *MXR_COMPAT_ALLOC_ATTR _heap_caps_zalloc(
    size_t size, uint32_t caps, const char *file, size_t line)
{
    (void)file;
    (void)line;
    return mxr_zalloc_caps(size, caps);
}

size_t heap_caps_get_free_size(uint32_t caps)
{
    return mxr_get_free_size_caps(caps);
}

size_t heap_caps_get_minimum_free_size(uint32_t caps)
{
    return mxr_get_min_free_size_caps(caps);
}

size_t heap_caps_get_dram_free_size(void)
{
    return mxr_get_free_size_caps(
        MALLOC_CAP_8BIT | MALLOC_CAP_32BIT | MALLOC_CAP_DMA);
}

void *heap_caps_malloc_default(size_t size)
{
    return mxr_malloc_caps(size, MALLOC_CAP_32BIT);
}

void *heap_caps_realloc_default(void *ptr, size_t size)
{
    return mxr_realloc_caps(ptr, size, MALLOC_CAP_32BIT);
}
/* FIX(2.3): дополнительные query API */
size_t heap_caps_get_total_size(uint32_t caps)
{
    return mxr_get_total_size_caps(caps);
}

size_t heap_caps_get_allocated_size(uint32_t caps)
{
    return mxr_get_allocated_size_caps(caps);
}

size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    return mxr_get_largest_free_block_caps(caps);
}
```

## File: `Kconfig.projbuild` (8347 tokens)
```projbuild
menu "MxR-Malloc"

    # ============================================================
    #  DRAM descriptors
    # ============================================================

    config MXR_MAX_DESC
        int "Maximum simultaneous DRAM allocations"
        range 1 4096
        default 256
        help
            Maximum number of active DRAM allocation descriptors.

            Each DRAM descriptor uses 8 bytes:
                256 descriptors = 2048 bytes
                512 descriptors = 4096 bytes

            This limit is separate from the IRAM descriptor limit.
            If the allocator runs out of DRAM descriptors, further DRAM
            allocations will fail even if free DRAM bytes are still
            available.

            Increase this value if your application keeps many small
            DRAM allocations alive at the same time.

    config MXR_IRAM_MAX_DESC
        int "Maximum simultaneous IRAM allocations"
        depends on MXR_USE_IRAM
        range 1 4096
        default 128
        help
            Maximum number of active IRAM allocation descriptors.

            Each IRAM descriptor uses 8 bytes:
                128 descriptors = 1024 bytes
                256 descriptors = 2048 bytes

            This pool is separate from the DRAM descriptor pool.
            It is used for:
                - MALLOC_CAP_EXEC allocations
                - non-EXEC 32-bit allocations that are placed into IRAM

            If this table becomes full, new IRAM allocations will fail
            even if free IRAM bytes are still available.

    # ============================================================
    #  Descriptor table placement
    # ============================================================

    choice
        prompt "Descriptor table placement"
        depends on MXR_USE_IRAM
        default MXR_DESC_IN_DRAM
        help
            Selects where the allocation descriptor tables are stored.

            Only the descriptor arrays themselves can be moved to IRAM.
            Scalar allocator state is intentionally kept in DRAM because
            ESP8266 IRAM does not support byte/half-word accesses safely.

            DRAM placement is the safest default.
            IRAM placement may reduce DRAM usage, but consumes precious
            IRAM and may not be compatible with all linker scripts.

    config MXR_DESC_IN_DRAM
        bool "DRAM (.bss) - default, safest"
        help
            Keep descriptor tables in normal DRAM (.bss).

            This is the safest and most portable mode.
            It does not require any linker script modification.

            Recommended for most projects.

    config MXR_DESC_IN_IRAM_TEXT
        bool "IRAM (.iram0.text) - no linker patch needed"
        help
            Place descriptor tables into the .iram0.text section.

            This usually works without patching the linker script, but
            it consumes IRAM that would otherwise be available for code.

            Use this only if you need to save DRAM and you understand
            the IRAM usage of your firmware.

    config MXR_DESC_IN_IRAM_BSS
        bool "IRAM (.iram0.bss) - requires patched linker script"
        help
            Place descriptor tables into the .iram0.bss section.

            This is cleaner than .iram0.text for data, but ESP8266
            linker scripts usually do not define .iram0.bss by default.

            Use this only if you have a patched linker script that
            properly defines and reserves the .iram0.bss section.

    endchoice

    # ============================================================
    #  Region configuration
    # ============================================================

    config MXR_REGION_CONFIG
        string "Region boundaries with weights"
        default "4-8%,32-10%,64-10%,128-12%,256-10%,512-20%,1024-0%"
        help
            Configures DRAM size-class regions using a single string.
            Format:
                <min_bytes>-<percent>%,<min_bytes>-<percent>%,...
            Example:
                "4-8%,32-10%,64-10%,128-12%,256-10%,512-20%,1024-0%"
            This creates 7 regions:
                region 0: block size >= 4 bytes    (small structs)
                region 1: block size >= 32 bytes   (i2c_cmd_link_t)
                region 2: block size >= 64 bytes   (spi buf, adc)
                region 3: block size >= 128 bytes  (i2c_cmd_desc_t)
                region 4: block size >= 256 bytes  (medium buffers)
                region 5: block size >= 512 bytes  (DMA buffers)
                region 6: block size >= 1024 bytes (large buffers)

            The min_bytes value is the lower boundary of the region.
            The next region's min_bytes defines the previous region's
            upper boundary. The last region is unlimited and accepts
            all larger blocks.

            The percent value is the memory weight assigned to that
            region during initialization. The sum of all percents must
            be <= 100. Any leftover memory is added to the last region.

            Boundaries are automatically aligned to 4 bytes and must be
            strictly increasing.

            The number of entries determines the number of DRAM regions.
            CMake validates this string during project configuration.

    # ============================================================
    #  Global settings
    # ============================================================

    config MXR_COMPACT_TYPES
        bool "Compact types (uint16 where possible, for ESP8266)"
        default y
        help
            Use compact 16-bit types where possible for ESP8266.

            On ESP8266 this is normally correct because:
                - heap arena is <= 128 KB
                - size-class boundaries are usually <= 64 KB
                - allocation counters are small

            Enabling this reduces RAM usage of region/state structures.

            Disable this only if you need size-class boundaries above
            64 KB or if you are adapting MxR-malloc to another target.

    config MXR_IRAM_HOT_PATH_DISABLED
        bool "Disable placing malloc/free hot path in IRAM"
        default n
        help
            By default, MxR-malloc places the core malloc/free hot path
            into IRAM.

            Enable this option if IRAM is too small and you need to save
            IRAM space.

            Warning:
            If malloc/free are called while flash cache is disabled,
            for example from some flash-write/erase related paths,
            flash-resident code may crash. Keep the hot path in IRAM
            unless you know this is safe for your project.

    choice
        prompt "IRAM hot path scope"
        depends on !MXR_IRAM_HOT_PATH_DISABLED
        default MXR_IRAM_PATH_CORE
        help
            Selects which allocator functions are placed into IRAM.

            Core:
                Only malloc/free hot path is placed in IRAM.
                This uses the least IRAM.

            Allocation family:
                malloc/free/calloc/zalloc/realloc are placed in IRAM.
                This uses significantly more IRAM, especially because
                realloc is large.

            Use Allocation family only if these functions may be called
            in contexts where flash cache is disabled.

    config MXR_IRAM_PATH_CORE
        bool "Core (malloc/free only)"
        help
            Place only the core malloc/free path into IRAM.

            This is the recommended default because it gives the most
            important ISR-safe behavior while consuming the least IRAM.

    config MXR_IRAM_PATH_ALLOC_FAMILY
        bool "Allocation family (malloc/free/calloc/zalloc/realloc)"
        help
            Place the full allocation family into IRAM:
                malloc
                free
                calloc
                zalloc
                realloc

            This increases IRAM usage noticeably.
            Check idf.py size output before using this option.

    endchoice

    # ============================================================
    #  IRAM heap
    # ============================================================

    config MXR_USE_IRAM
        bool "Enable IRAM heap (EXEC + 32BIT fallback)"
        default y if !CONFIG_HEAP_DISABLE_IRAM
        default n
        help
            Enables the IRAM heap arena.

            When enabled:
                - MALLOC_CAP_EXEC allocations are served from IRAM
                - pure 32-bit allocations may also use IRAM if allowed
                  by reserve and size-limit settings
                - allocations requiring MALLOC_CAP_8BIT, MALLOC_CAP_DMA
                  or MALLOC_CAP_SPIRAM are never placed into IRAM

            Disable this if you want all normal allocations to stay in
            DRAM only.

    config MXR_IRAM_RESERVE_BYTES
        int "Reserve IRAM bytes for EXEC allocations"
        depends on MXR_USE_IRAM
        range 0 32768
        default 2048
        help
            Defines the EXEC zone [0, reserve) at the start of IRAM.

            HARD binding rules:
              - MALLOC_CAP_EXEC allocations are placed ONLY inside
                [0, reserve). They can never cross this boundary,
                neither on malloc nor on realloc grow.
              - Non-EXEC 32-bit fallback allocations can never enter
                this zone (as before).
              - Setting this to 0 completely disables EXEC
                allocations: every MALLOC_CAP_EXEC request returns
                NULL and increments exec_zone_rejects.

            Recommended default: 2048.
    
    config MXR_IRAM_FALLBACK_ENABLED
        bool "Enable 32BIT fallback into IRAM (non-EXEC)"
        depends on MXR_USE_IRAM
        default y
        help
            If enabled, pure 32-bit allocations such as malloc(),
            _malloc_r(), heap_caps_malloc(size, MALLOC_CAP_32BIT),
            heap_caps_malloc(size, MALLOC_CAP_INTERNAL) and
            heap_caps_malloc(size, 0) may be placed into the
            IRAM fallback zone.

            If disabled, MALLOC_CAP_EXEC allocations still use the
            EXEC zone, but all non-EXEC 32-bit allocations are forced
            to DRAM.

            Disable this if you do not need IRAM for data and want to
            avoid the risk of byte/half-word accesses to IRAM memory.

    config MXR_IRAM_FALLBACK_MAX_BYTES
        int "Maximum block size for IRAM fallback"
        depends on MXR_USE_IRAM && MXR_IRAM_FALLBACK_ENABLED
        range 0 65536
        default 0
        help
            Maximum size of a non-EXEC block that is allowed to be
            placed into IRAM.

            Set this to prevent large non-executable buffers from
            consuming IRAM.

            Use 0 for no limit.

            EXEC allocations are not limited by this option.

    config MXR_IRAM_EXEC_WHOLE_IF_NO_FB
        bool "Give whole IRAM to EXEC zone when fallback is disabled"
        depends on MXR_USE_IRAM && !MXR_IRAM_FALLBACK_ENABLED
        default y
        help
            When 32BIT fallback is disabled, the IRAM area beyond
            CONFIG_MXR_IRAM_RESERVE_BYTES would otherwise be unusable
            but still counted in heap statistics (overreported free
            memory).
            If enabled (recommended), the EXEC zone is expanded to
            cover the whole IRAM arena, so MALLOC_CAP_EXEC allocations
            can use all IRAM.
            If disabled, the EXEC zone stays [0, reserve) and the rest
            of IRAM is excluded from the arena and from all statistics.

    choice
        prompt "IRAM fallback allocation order"
        depends on MXR_USE_IRAM && MXR_IRAM_FALLBACK_ENABLED
        default MXR_IRAM_FB_ORDER_DRAM_FIRST
        help
            IRAM first:
                Pure 32-bit allocations try the IRAM fallback zone
                BEFORE DRAM (original MxR behavior).
            DRAM first (recommended):
                Pure 32-bit allocations try DRAM first and only use
                the IRAM fallback zone when DRAM allocation fails.
                This keeps arbitrary byte-accessed data out of IRAM
                as long as DRAM has room.

    config MXR_IRAM_FB_ORDER_IRAM_FIRST
        bool "IRAM first (original MxR behavior)"
        help
            32-bit allocations prefer the IRAM fallback zone.

    config MXR_IRAM_FB_ORDER_DRAM_FIRST
        bool "DRAM first, IRAM as true fallback (recommended)"
        help
            32-bit allocations use IRAM only after DRAM fails.

    endchoice
    
    # ============================================================
    #  IRAM fallback region configuration
    # ============================================================
    config MXR_IRAM_FALLBACK_REGION_CONFIG
        string "IRAM fallback region layout"
        depends on MXR_USE_IRAM && MXR_IRAM_FALLBACK_ENABLED
        default "4-8%,32-10%,64-10%,128-12%,256-10%,512-20%,1024-0%"
        help
            Configures size-class regions inside the IRAM fallback zone.
            The fallback zone is the IRAM area NOT reserved for EXEC:
                [CONFIG_MXR_IRAM_RESERVE_BYTES, iram_end)

            Format is identical to CONFIG_MXR_REGION_CONFIG:
                <min_bytes>-<percent>%,<min_bytes>-<percent>%,...

            Empty string (default) means a single flat fallback region
            covering the whole fallback zone (original behavior).

            Examples:
                ""                    -> single flat fallback region
                "4-40%,128-0%"        -> blocks >=4 get 40% of the zone,
                                         blocks >=128 get the rest
                "4-30%,256-30%,1024-0%"

            EXEC allocations are NOT affected by this setting.
            They always use the EXEC zone [0, reserve) protected by
            CONFIG_MXR_IRAM_RESERVE_BYTES.

            The number of entries determines the number of IRAM
            fallback regions. CMake validates this string during
            project configuration.

    # ============================================================
    #  Cross-region fallback (DRAM + IRAM)
    # ============================================================
    config MXR_CROSS_REGION_FALLBACK
        bool "Enable cross-region fallback (master switch)"
        default y
        help
            Master switch for the cross-region fallback mechanism.
            When enabled, a block that cannot be allocated in its own
            size-class region may be placed into another region as a
            last resort - subject to the per-arena enable switches and
            guard settings below.
            DRAM and IRAM are controlled independently:
              - CONFIG_MXR_DRAM_CROSS_ENABLED
              - CONFIG_MXR_IRAM_CROSS_ENABLED

    # ------------------------------------------------------------
    #  DRAM cross-region
    # ------------------------------------------------------------
    config MXR_DRAM_CROSS_ENABLED
        bool "Enabled - use cross-region fallback for DRAM"
        depends on MXR_CROSS_REGION_FALLBACK
        default y
        help
            Enables DRAM cross-region fallback.
            Allocations that cannot fit in their own DRAM size-class
            region will try other DRAM regions as a last resort,
            subject to the guard settings below.
            If disabled, allocations that do not fit in their own DRAM
            region fail with alloc_fail_no_memory.
            IRAM cross-region is controlled separately by
            CONFIG_MXR_IRAM_CROSS_ENABLED.

    choice
        prompt "DRAM cross-region max_bytes guard aggressiveness"
        depends on MXR_CROSS_REGION_FALLBACK
        depends on MXR_DRAM_CROSS_ENABLED
        default MXR_DRAM_CROSS_MODERATE
        help
            Controls the max_bytes GUARD applied during DRAM
            cross-region fallback. The GUARD protects a target region
            from blocks that are too large for its size class.

            Conservative: reject blocks > 50% of target max_bytes.
                Best region isolation, more allocation failures.
            Moderate (recommended): reject blocks > 75%.
                Balanced between isolation and success rate.
            Aggressive: reject blocks > 90%.
                Fewer allocation failures, more fragmentation.
            All: no max_bytes check at all.
                Any block can go into any DRAM region.

    config MXR_DRAM_CROSS_CONSERVATIVE
        bool "Conservative - strict DRAM region isolation (50%)"
        help
            Use cross-region only as an absolute last resort.
            Small DRAM regions are strongly protected from large blocks.
            Use this if your workload is stable and predictable.

    config MXR_DRAM_CROSS_MODERATE
        bool "Moderate - balanced (recommended, 75%)"
        help
            Balanced between DRAM region isolation and allocation success.
            This is the recommended default for most workloads.

    config MXR_DRAM_CROSS_AGGRESSIVE
        bool "Aggressive - prefer allocation success (90%)"
        help
            Cross-region is used more eagerly to avoid allocation
            failures. DRAM region boundaries are weakly enforced.

    config MXR_DRAM_CROSS_ALL
        bool "All - no max_bytes guard"
        help
            No GUARD is applied: a cross-region block may occupy any
            DRAM region regardless of its max_bytes. Maximizes the
            allocation success rate, provides no region protection.
            Note: large blocks may then consume small-class regions.

    endchoice

    choice
        prompt "DRAM cross-region min_bytes guard aggressiveness"
        depends on MXR_CROSS_REGION_FALLBACK
        depends on MXR_DRAM_CROSS_ENABLED
        default MXR_DRAM_CROSS_MIN_BYTES_MODERATE
        help
            Controls the min_bytes guard during DRAM cross-region.
            This guard protects large-block DRAM regions from being
            fragmented by tiny allocations.

            The check is:
                if (bytes * DIVISOR < region.min_bytes) -> skip region

            Conservative (DIVISOR=1): block must be >= region.min_bytes.
            Moderate (DIVISOR=2, recommended): >= region.min_bytes / 2.
            Aggressive (DIVISOR=4): >= region.min_bytes / 4.
            All: no min_bytes check at all.

    config MXR_DRAM_CROSS_MIN_BYTES_CONSERVATIVE
        bool "Conservative - strict min_bytes check (divisor 1)"
        help
            Block must be >= DRAM region.min_bytes.
            Strictest protection of large-block DRAM regions.

    config MXR_DRAM_CROSS_MIN_BYTES_MODERATE
        bool "Moderate - min_bytes / 2 (recommended, divisor 2)"
        help
            Block must be >= DRAM region.min_bytes / 2.
            Recommended balance between isolation and success.

    config MXR_DRAM_CROSS_MIN_BYTES_AGGRESSIVE
        bool "Aggressive - min_bytes / 4 (divisor 4)"
        help
            Block must be >= DRAM region.min_bytes / 4.
            Allows more cross-region placements in DRAM.

    config MXR_DRAM_CROSS_MIN_BYTES_ALL
        bool "All - no min_bytes check"
        help
            Completely disable the min_bytes guard for DRAM.
            Any block can be placed in any DRAM region.

    endchoice

    # ------------------------------------------------------------
    #  IRAM fallback cross-region
    # ------------------------------------------------------------
    config MXR_IRAM_CROSS_ENABLED
        bool "Enabled - use cross-region fallback for IRAM"
        depends on MXR_CROSS_REGION_FALLBACK
        depends on MXR_USE_IRAM
        depends on MXR_IRAM_FALLBACK_ENABLED
        default y
        help
            Enables cross-region fallback inside the IRAM fallback zone.
            Non-EXEC 32-bit allocations that cannot fit in their own
            IRAM fb region will try other IRAM fb regions as a last
            resort, subject to the guard settings below.
            If disabled, such allocations fall through to DRAM instead
            of trying other IRAM fb regions.
            DRAM cross-region is controlled separately by
            CONFIG_MXR_DRAM_CROSS_ENABLED.

    choice
        prompt "IRAM fallback cross-region max_bytes guard aggressiveness"
        depends on MXR_CROSS_REGION_FALLBACK
        depends on MXR_IRAM_CROSS_ENABLED
        depends on MXR_USE_IRAM
        default MXR_IRAM_CROSS_CONSERVATIVE
        help
            Controls the max_bytes GUARD inside the IRAM fallback zone.
            IRAM is a scarce resource that competes with EXEC
            allocations, so the recommended default is more
            conservative than for DRAM.

            Conservative (recommended): reject blocks > 50% of max_bytes.
            Moderate: reject blocks > 75%.
            Aggressive: reject blocks > 90%.
            All: no max_bytes check at all.

    config MXR_IRAM_CROSS_CONSERVATIVE
        bool "Conservative - strict IRAM region isolation (recommended, 50%)"
        help
            Use IRAM cross-region only as an absolute last resort.
            Protects the limited IRAM fallback zone from fragmentation.

    config MXR_IRAM_CROSS_MODERATE
        bool "Moderate - balanced (75%)"
        help
            Balanced between IRAM region isolation and allocation success.

    config MXR_IRAM_CROSS_AGGRESSIVE
        bool "Aggressive - prefer allocation success (90%)"
        help
            IRAM cross-region is used more eagerly.
            May fragment the limited IRAM fallback zone.

    config MXR_IRAM_CROSS_ALL
        bool "All - no max_bytes guard"
        help
            No GUARD is applied inside the IRAM fallback zone.
            Any block can go into any IRAM fb region.

    endchoice

    choice
        prompt "IRAM fallback cross-region min_bytes guard aggressiveness"
        depends on MXR_CROSS_REGION_FALLBACK
        depends on MXR_IRAM_CROSS_ENABLED
        depends on MXR_USE_IRAM
        default MXR_IRAM_CROSS_MIN_BYTES_MODERATE
        help
            Controls the min_bytes guard inside the IRAM fallback zone.
            The check is:
                if (bytes * DIVISOR < region.min_bytes) -> skip region

            Conservative (DIVISOR=1): block must be >= region.min_bytes.
            Moderate (DIVISOR=2, recommended): >= region.min_bytes / 2.
            Aggressive (DIVISOR=4): >= region.min_bytes / 4.
            All: no min_bytes check at all.

    config MXR_IRAM_CROSS_MIN_BYTES_CONSERVATIVE
        bool "Conservative - strict min_bytes check (divisor 1)"

    config MXR_IRAM_CROSS_MIN_BYTES_MODERATE
        bool "Moderate - min_bytes / 2 (recommended, divisor 2)"

    config MXR_IRAM_CROSS_MIN_BYTES_AGGRESSIVE
        bool "Aggressive - min_bytes / 4 (divisor 4)"

    config MXR_IRAM_CROSS_MIN_BYTES_ALL
        bool "All - no min_bytes check"

    endchoice


    config MXR_ANTI_SLIVER
        bool "Enable anti-sliver expansion (consume tiny leftover gaps)"
        default y
        help
            When a block is cut from a gap and the leftover tail would be
            smaller than MXR_MIN_SLICE_BYTES, the block is expanded to
            consume the entire gap. Also prevents realloc shrink from
            creating tiny tails.
            Disabling removes all block expansions: every allocation gets
            exactly the requested (4-aligned) size and every realloc shrink
            splits the block.
            Disable only for experiments/benchmarking: it increases the
            number of unusable micro-gaps (slivers) and makes future
            searches slower.


    config MXR_MIN_SLICE_BYTES
        int "Minimum useful gap size (anti-sliver threshold)"
        depends on MXR_ANTI_SLIVER
        range 4 64
        default 8
        help
            If the remainder after cutting a block from a gap is smaller
            than this value, the block is expanded to consume the entire
            gap. This prevents creation of unusable micro-fragments.
            Also applies to realloc shrink: if the tail would be smaller
            than this, the block is not split.
            Recommended: 8 bytes (2 alignment units).

    config MXR_BEST_FIT_EARLY_EXIT
        bool "Enable best-fit early-exit (stop on a good-enough gap)"
        default y
        help
            When enabled, the free-gap search stops as soon as a gap with
            waste <= requested_size >> MXR_BEST_FIT_WASTE_SHIFT is found.
            When disabled, the allocator performs a strict best-fit scan
            of the whole region: only an exact-fit gap (waste == 0) stops
            the search early.
            Disabling reduces fragmentation but makes every allocation do
            a full descriptor scan with interrupts disabled. Use only if
            fragmentation matters more than worst-case allocation latency.


    config MXR_BEST_FIT_WASTE_SHIFT
        int "Best-fit early-exit waste threshold (shift)"
        depends on MXR_BEST_FIT_EARLY_EXIT
        range 1 4
        default 2
        help
            Controls the best-fit early-exit threshold.
            If (gap - requested_size) <= requested_size >> SHIFT,
            the gap is considered "good enough" and the search stops.
            SHIFT=1: 50% waste tolerance (faster, more fragmentation)
            SHIFT=2: 25% waste tolerance (balanced, recommended)
            SHIFT=3: 12.5% waste tolerance (tighter, slower)
            SHIFT=4: 6.25% waste tolerance (near-exact best-fit)

    # ============================================================
    #  Integration mode
    # ============================================================

    choice
        prompt "Integration mode"
        default MXR_INTEGRATION_WRAP
        help
            Selects how MxR-malloc is integrated into the ESP8266
            RTOS SDK build.

            Wrap mode:
                Safest and recommended.
                The original heap component remains in the build, but
                linker --wrap redirects heap_caps_* calls to MxR.

            Compat mode:
                MxR directly provides heap_caps_* API.
                The original heap component must be excluded from the
                build, otherwise duplicate symbols will appear.

            Port mode:
                MxR provides both heap_caps_* and standard libc-style
                malloc/free/calloc/realloc/zalloc symbols.
                Also requires excluding conflicting original sources.

    config MXR_INTEGRATION_WRAP
        bool "Wrap mode (linker --wrap, default & safest)"
        help
            Use linker --wrap to redirect original heap API calls to
            MxR-malloc.

            This is the safest integration mode:
                - original heap component can stay in the build
                - easy to enable/disable
                - no manual source exclusion required in most projects

            Recommended default.

    config MXR_INTEGRATION_COMPAT
        bool "Compat mode (replaces original heap_caps_* API)"
        help
            Compile MxR as a direct replacement for the original
            heap_caps_* implementation.

            Important:
            You must exclude the original heap component or its
            conflicting source files from the build. Otherwise the
            linker will report duplicate symbols.

    config MXR_INTEGRATION_PORT
        bool "Port mode (replaces standard libc malloc/free)"
        help
            Compile MxR as a replacement for both heap_caps_* and
            standard libc-style allocation functions:
                malloc
                free
                calloc
                realloc
                zalloc

            This mode is more intrusive.
            You must make sure original implementations of these
            functions are not linked into the firmware.

    endchoice

    # ============================================================
    #  Linker integration (Wrap mode only)
    # ============================================================

    menu "Linker integration (Wrap mode only)"
        depends on MXR_INTEGRATION_WRAP

        config MXR_WRAP_HEAP_QUERY
            bool "Wrap heap_caps_get_free_size / minimum / dram"
            default y
            help
                Wrap heap query functions:
                    heap_caps_get_free_size
                    heap_caps_get_minimum_free_size
                    heap_caps_get_dram_free_size

                Keep this enabled if you want heap statistics reported
                by MxR-malloc instead of the original heap code.

                If disabled, query functions may return incorrect values
                because the original heap may not be initialized when
                MxR replaces heap_caps_init().

        config MXR_WRAP_DEFAULT_POOL
            bool "Wrap heap_caps_malloc_default / realloc_default"
            default y
            help
                Wrap:
                    heap_caps_malloc_default
                    heap_caps_realloc_default

                These functions are used by some SDK components.
                Keep this enabled for consistent default-pool behavior
                through MxR-malloc.

        config MXR_WRAP_ESP_SYSTEM
            bool "Wrap esp_get_free_heap_size / minimum / internal"
            default y
            help
                Wrap system heap query functions:
                    esp_get_free_heap_size
                    esp_get_minimum_free_heap_size
                    esp_get_free_internal_heap_size

                Keep this enabled if you want correct free-heap values
                in application code and logs.

        config MXR_WRAP_LIBC
            bool "Wrap plain malloc / free / calloc / realloc / zalloc"
            default y
            help
                Wrap plain libc-style allocation functions:
                    malloc
                    free
                    calloc
                    realloc
                    zalloc

                This can intercept more allocations, but may conflict
                with newlib/port layer implementations depending on SDK
                configuration.

                Enable only if you specifically need this behavior.

        config MXR_WARN_HEAP_TRACING
            bool "Warn if CONFIG_HEAP_TRACING is enabled"
            default y
            help
                Warn at CMake configure time if CONFIG_HEAP_TRACING is
                enabled together with MxR wrap mode.

                Original heap tracing is not compatible with MxR wrap
                mode because the original allocator is bypassed.

                Keep this warning enabled.

    endmenu

    # ============================================================
    #  Diagnostics
    # ============================================================

    choice
        prompt "Diagnostics output level"
        default MXR_DUMP_NORMAL
        help
            Controls how much information is printed by mxr_dump().

            Minimal:
                Very short summary.
                Suitable for periodic logging.

            Normal:
                Summary plus region state, descriptor usage and error
                counters.
                Recommended for normal debugging.

            Full:
                Everything from Normal plus all active descriptors.
                Output can be very large.
                Use only for deep heap debugging.

    config MXR_DUMP_MINIMAL
        bool "Minimal - totals only"
        help
            Print only the most important totals:
                total bytes
                free bytes
                minimum free bytes
                largest free block

            Best for periodic runtime logging.

    config MXR_DUMP_NORMAL
        bool "Normal - totals, regions and counters"
        help
            Print totals, region information, descriptor usage,
            IRAM information and allocation failure counters.

            Recommended default for debugging.

    config MXR_DUMP_FULL
        bool "Full - normal plus all descriptors"
        help
            Print everything from Normal mode and also dump every
            active allocation descriptor.

            This may print hundreds of lines if many allocations are
            active. Use only for deep heap analysis.

    endchoice

endmenu
```

## File: `CMakeLists.txt` (2787 tokens)
```txt
# ============================================================
#  1. Select source files based on integration mode
# ============================================================
set(MXR_SRCS "mxr_malloc.c")

if(CONFIG_MXR_INTEGRATION_WRAP)
    list(APPEND MXR_SRCS "mxr_heap_wrap.c")
elseif(CONFIG_MXR_INTEGRATION_COMPAT)
    list(APPEND MXR_SRCS "mxr_heap_compat.c")
elseif(CONFIG_MXR_INTEGRATION_PORT)
    list(APPEND MXR_SRCS "mxr_heap_compat.c" "mxr_heap_port.c")
endif()

idf_component_register(
    SRCS ${MXR_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES
        log
        newlib
        freertos
        esp8266
)

# ============================================================
#  2. Compatibility warning
# ============================================================
if(CONFIG_MXR_WARN_HEAP_TRACING AND CONFIG_HEAP_TRACING AND CONFIG_MXR_INTEGRATION_WRAP)
    message(WARNING
        "MxR-malloc: CONFIG_HEAP_TRACING is enabled. "
        "Original heap tracing is not compatible with MxR wrap mode. "
        "Disable CONFIG_HEAP_TRACING or disable this warning."
    )
endif()

# ============================================================
#  3. Validate MXR_REGION_CONFIG at configure time (DRAM)
# ============================================================
set(_MXR_CFG "${CONFIG_MXR_REGION_CONFIG}")
if(_MXR_CFG)
    string(REPLACE "," ";" _MXR_ENTRIES "${_MXR_CFG}")
    list(LENGTH _MXR_ENTRIES _MXR_DRAM_COUNT)

    if(_MXR_DRAM_COUNT GREATER 32)
        message(FATAL_ERROR
            "MxR-malloc: Too many DRAM regions (${_MXR_DRAM_COUNT}). "
            "Maximum is 32.")
    endif()

    set(_MXR_PCT_SUM 0)
    set(_MXR_PREV_ALIGNED -1)
    set(_MXR_INDEX 0)

    foreach(_entry ${_MXR_ENTRIES})
        string(STRIP "${_entry}" _entry)

        # FIX(4.2): строгий формат <bytes>-<percent>%
        string(REGEX MATCH "^([0-9]+)-([0-9]+)%$" _MATCH "${_entry}")
        if(NOT _MATCH)
            message(FATAL_ERROR
                "MxR-malloc: Invalid DRAM region entry '${_entry}' in "
                "CONFIG_MXR_REGION_CONFIG='${CONFIG_MXR_REGION_CONFIG}'. "
                "Expected format: <bytes>-<percent>%")
        endif()

        set(_BOUNDARY ${CMAKE_MATCH_1})
        set(_PCT ${CMAKE_MATCH_2})

        # FIX(4.2): percent 0..100 для каждой записи
        if(_PCT GREATER 100)
            message(FATAL_ERROR
                "MxR-malloc: DRAM region entry '${_entry}' has percent "
                "${_PCT} > 100")
        endif()

        # FIX(4.2): при COMPACT_TYPES граница не может быть > 65535
        if(CONFIG_MXR_COMPACT_TYPES AND _BOUNDARY GREATER 65535)
            message(FATAL_ERROR
                "MxR-malloc: DRAM region boundary ${_BOUNDARY} exceeds 65535 "
                "while CONFIG_MXR_COMPACT_TYPES=y")
        endif()

        # FIX(4.2): границы должны строго возрастать ПОСЛЕ align4
        math(EXPR _ALIGNED "((${_BOUNDARY} + 3) / 4) * 4")
        if(_ALIGNED LESS 4)
            set(_ALIGNED 4)
        endif()
        if(NOT _ALIGNED GREATER _MXR_PREV_ALIGNED)
            message(FATAL_ERROR
                "MxR-malloc: DRAM region boundaries must be strictly "
                "increasing after 4-byte alignment. Entry #${_MXR_INDEX} "
                "'${_entry}' aligns to ${_ALIGNED}, but previous aligned "
                "boundary is ${_MXR_PREV_ALIGNED}.")
        endif()
        set(_MXR_PREV_ALIGNED ${_ALIGNED})

        math(EXPR _MXR_PCT_SUM "${_MXR_PCT_SUM} + ${_PCT}")
        math(EXPR _MXR_INDEX "${_MXR_INDEX} + 1")
    endforeach()

    # первая граница должна быть <= 4 (MXR_ALIGN_SIZE)
    list(GET _MXR_ENTRIES 0 _MXR_FIRST_ENTRY)
    string(STRIP "${_MXR_FIRST_ENTRY}" _MXR_FIRST_ENTRY)
    if(_MXR_FIRST_ENTRY MATCHES "^([0-9]+)-")
        if(CMAKE_MATCH_1 GREATER 4)
            message(FATAL_ERROR
                "MxR-malloc: first DRAM region boundary is ${CMAKE_MATCH_1}, "
                "must be <= 4. Blocks smaller than ${CMAKE_MATCH_1} bytes "
                "would be unallocatable.")
        endif()
    endif()

    if(_MXR_PCT_SUM GREATER 100)
        message(FATAL_ERROR
            "MxR-malloc: DRAM region percent sum is ${_MXR_PCT_SUM}%, "
            "must be <= 100%")
    endif()

    message(STATUS
        "MxR-malloc: DRAM region percent sum = ${_MXR_PCT_SUM}%")
    message(STATUS
        "MxR-malloc: ${_MXR_DRAM_COUNT} DRAM region(s) from "
        "'${CONFIG_MXR_REGION_CONFIG}'")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        "MXR_PARSED_REGION_COUNT=${_MXR_DRAM_COUNT}")
else()
    message(STATUS
        "MxR-malloc: DRAM region config empty, using single flat region")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        "MXR_PARSED_REGION_COUNT=1")
endif()

# ============================================================
#  3b. Validate MXR_IRAM_FALLBACK_REGION_CONFIG (IRAM fallback)
#  FIX(1.3): валидация только при включённом fallback
# ============================================================
if(CONFIG_MXR_USE_IRAM AND CONFIG_MXR_IRAM_FALLBACK_ENABLED)
    set(_MXR_IRAM_CFG "${CONFIG_MXR_IRAM_FALLBACK_REGION_CONFIG}")
    if(_MXR_IRAM_CFG)
        string(REPLACE "," ";" _MXR_IRAM_ENTRIES "${_MXR_IRAM_CFG}")
        list(LENGTH _MXR_IRAM_ENTRIES _MXR_IRAM_COUNT)

        if(_MXR_IRAM_COUNT GREATER 32)
            message(FATAL_ERROR
                "MxR-malloc: Too many IRAM fallback regions "
                "(${_MXR_IRAM_COUNT}). Maximum is 32.")
        endif()

        set(_MXR_PCT_SUM 0)
        set(_MXR_PREV_ALIGNED -1)
        set(_MXR_INDEX 0)

        foreach(_entry ${_MXR_IRAM_ENTRIES})
            string(STRIP "${_entry}" _entry)

            # FIX(4.2): строгий формат
            string(REGEX MATCH "^([0-9]+)-([0-9]+)%$" _MATCH "${_entry}")
            if(NOT _MATCH)
                message(FATAL_ERROR
                    "MxR-malloc: Invalid IRAM fallback region entry "
                    "'${_entry}' in CONFIG_MXR_IRAM_FALLBACK_REGION_CONFIG="
                    "'${CONFIG_MXR_IRAM_FALLBACK_REGION_CONFIG}'. "
                    "Expected format: <bytes>-<percent>%")
            endif()

            set(_BOUNDARY ${CMAKE_MATCH_1})
            set(_PCT ${CMAKE_MATCH_2})

            # FIX(4.2): percent 0..100
            if(_PCT GREATER 100)
                message(FATAL_ERROR
                    "MxR-malloc: IRAM fallback region entry '${_entry}' "
                    "has percent ${_PCT} > 100")
            endif()

            # FIX(4.2): ограничение COMPACT_TYPES
            if(CONFIG_MXR_COMPACT_TYPES AND _BOUNDARY GREATER 65535)
                message(FATAL_ERROR
                    "MxR-malloc: IRAM fallback region boundary ${_BOUNDARY} "
                    "exceeds 65535 while CONFIG_MXR_COMPACT_TYPES=y")
            endif()

            # FIX(4.2): строго возрастающие границы после align4
            math(EXPR _ALIGNED "((${_BOUNDARY} + 3) / 4) * 4")
            if(_ALIGNED LESS 4)
                set(_ALIGNED 4)
            endif()
            if(NOT _ALIGNED GREATER _MXR_PREV_ALIGNED)
                message(FATAL_ERROR
                    "MxR-malloc: IRAM fallback region boundaries must be "
                    "strictly increasing after 4-byte alignment. Entry "
                    "#${_MXR_INDEX} '${_entry}' aligns to ${_ALIGNED}, but "
                    "previous aligned boundary is ${_MXR_PREV_ALIGNED}.")
            endif()
            set(_MXR_PREV_ALIGNED ${_ALIGNED})

            math(EXPR _MXR_PCT_SUM "${_MXR_PCT_SUM} + ${_PCT}")
            math(EXPR _MXR_INDEX "${_MXR_INDEX} + 1")
        endforeach()

        # первая граница <= 4
        list(GET _MXR_IRAM_ENTRIES 0 _MXR_IRAM_FIRST_ENTRY)
        string(STRIP "${_MXR_IRAM_FIRST_ENTRY}" _MXR_IRAM_FIRST_ENTRY)
        if(_MXR_IRAM_FIRST_ENTRY MATCHES "^([0-9]+)-")
            if(CMAKE_MATCH_1 GREATER 4)
                message(FATAL_ERROR
                    "MxR-malloc: first IRAM fallback region boundary is "
                    "${CMAKE_MATCH_1}, must be <= 4.")
            endif()
        endif()

        if(_MXR_PCT_SUM GREATER 100)
            message(FATAL_ERROR
                "MxR-malloc: IRAM region percent sum is ${_MXR_PCT_SUM}%, "
                "must be <= 100%")
        endif()

        message(STATUS
            "MxR-malloc: IRAM region percent sum = ${_MXR_PCT_SUM}%")
        message(STATUS
            "MxR-malloc: ${_MXR_IRAM_COUNT} IRAM fallback region(s) from "
            "'${CONFIG_MXR_IRAM_FALLBACK_REGION_CONFIG}'")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE
            "MXR_IRAM_FB_PARSED_REGION_COUNT=${_MXR_IRAM_COUNT}")
    else()
        message(STATUS
            "MxR-malloc: IRAM fallback region config empty, using single "
            "flat region")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE
            "MXR_IRAM_FB_PARSED_REGION_COUNT=1")
    endif()
endif()

# ============================================================
#  4. Linker wraps (ONLY applied in Wrap mode!)
# ============================================================
if(CONFIG_MXR_INTEGRATION_WRAP)
    set(MXR_WRAP_FLAGS
        "-Wl,--wrap=heap_caps_init"
        "-Wl,--wrap=_heap_caps_malloc"
        "-Wl,--wrap=_heap_caps_free"
        "-Wl,--wrap=_heap_caps_realloc"
        "-Wl,--wrap=_heap_caps_calloc"
        "-Wl,--wrap=_heap_caps_zalloc"
    )
    if(CONFIG_MXR_WRAP_HEAP_QUERY)
        list(APPEND MXR_WRAP_FLAGS
            "-Wl,--wrap=heap_caps_get_free_size"
            "-Wl,--wrap=heap_caps_get_minimum_free_size"
            "-Wl,--wrap=heap_caps_get_dram_free_size"
            "-Wl,--wrap=heap_caps_get_total_size"
            "-Wl,--wrap=heap_caps_get_allocated_size"
            "-Wl,--wrap=heap_caps_get_largest_free_block"
        )
    endif()
    if(CONFIG_MXR_WRAP_DEFAULT_POOL)
        list(APPEND MXR_WRAP_FLAGS
            "-Wl,--wrap=heap_caps_malloc_default"
            "-Wl,--wrap=heap_caps_realloc_default"
        )
    endif()
    if(CONFIG_MXR_WRAP_ESP_SYSTEM)
        list(APPEND MXR_WRAP_FLAGS
            "-Wl,--wrap=esp_get_free_heap_size"
            "-Wl,--wrap=esp_get_minimum_free_heap_size"
            "-Wl,--wrap=esp_get_free_internal_heap_size"
        )
    endif()
    if(CONFIG_MXR_WRAP_LIBC)
        list(APPEND MXR_WRAP_FLAGS
            "-Wl,--wrap=malloc"
            "-Wl,--wrap=free"
            "-Wl,--wrap=calloc"
            "-Wl,--wrap=realloc"
            "-Wl,--wrap=zalloc"
        )
    endif()
    target_link_libraries(${COMPONENT_LIB} INTERFACE ${MXR_WRAP_FLAGS})
endif()
if(CONFIG_MXR_INTEGRATION_PORT)
    message(WARNING
        "MxR-malloc: PORT mode defines malloc/free/calloc/realloc/zalloc "
        "directly. Ensure original newlib/libc implementations are excluded "
        "from the build, otherwise duplicate symbol errors will occur."
    )
endif()
```

## File: `esp8266.project.ld.in` (1691 tokens)
```in
/*  Default entry point:  */
ENTRY(call_start_cpu);

SECTIONS
{
  /* RTC data section holds RTC wake data/rodata
     marked with RTC_DATA_ATTR, RTC_RODATA_ATTR attributes.
  */
  .rtc.data :
  {
    _rtc_data_start = ABSOLUTE(.);

    mapping[rtc_data]

    _rtc_data_end = ABSOLUTE(.);
  } > rtc_data_seg

  /* RTC bss */
  .rtc.bss (NOLOAD) :
  {
    _rtc_bss_start = ABSOLUTE(.);

    mapping[rtc_bss]

    _rtc_bss_end = ABSOLUTE(.);
  } > rtc_data_seg

  /* This section holds data that should not be initialized at power up
     and will be retained during deep sleep.
     User data marked with RTC_NOINIT_ATTR will be placed
     into this section. See the file "esp_attr.h" for more information.
  */
  .rtc_noinit (NOLOAD):
  {
    . = ALIGN(4);
    _rtc_noinit_start = ABSOLUTE(.);
    *(.rtc_noinit .rtc_noinit.*)
    . = ALIGN(4) ;
    _rtc_noinit_end = ABSOLUTE(.);
  } > rtc_data_seg

  ASSERT(((_rtc_noinit_end - ORIGIN(rtc_data_seg)) <= LENGTH(rtc_data_seg)),
        "RTC segment data does not fit.")

  /* Send .iram0 code to iram */
  .iram0.vectors :
  {
    _iram_start = ABSOLUTE(.);
    /* Vectors go to IRAM */
    _init_start = ABSOLUTE(.);
    KEEP(*(.SystemInfoVector.text));
    . = 0x10;
    KEEP(*(.DebugExceptionVector.text));
    . = 0x20;
    KEEP(*(.NMIExceptionVector.text));
    . = 0x30;
    KEEP(*(.KernelExceptionVector.text));
    . = 0x50;
    KEEP(*(.UserExceptionVector.text));
    . = 0x70;
    KEEP(*(.DoubleExceptionVector.text));

    *(.*Vector.literal)

    *(.UserEnter.literal);
    *(.UserEnter.text);
    . = ALIGN (16);
    *(.entry.text)
    *(.init.literal)
    *(.init)
    _init_end = ABSOLUTE(.);
  } > iram0_0_seg

  .iram0.text :
  {
    /* Code marked as runnning out of IRAM */
    _iram_text_start = ABSOLUTE(.);

    mapping[iram0_text]

    _iram_text_end = ABSOLUTE(.);
  } > iram0_0_seg

  .iram0.bss (NOLOAD) :
  {
    . = ALIGN (4);
    /* Code marked as runnning out of IRAM */
    _iram_bss_start = ABSOLUTE(.);

    mapping[iram0_bss]
*(.iram0.bss .iram0.bss.*)

    . = ALIGN (4);
    _iram_bss_end = ABSOLUTE(.);
    _iram_end = ABSOLUTE(.);
  } > iram0_0_seg

  ASSERT(((_iram_end - ORIGIN(iram0_0_seg)) <= LENGTH(iram0_0_seg)),
          "IRAM0 segment data does not fit.")

  .dram0.data :
  {
    _data_start = ABSOLUTE(.);
    *(.gnu.linkonce.d.*)
    *(.data1)
    *(.sdata)
    *(.sdata.*)
    *(.gnu.linkonce.s.*)
    *(.sdata2)
    *(.sdata2.*)
    *(.gnu.linkonce.s2.*)
    *(.jcr)
    *(.dram0 .dram0.*)

    mapping[dram0_data]

    _data_end = ABSOLUTE(.);
    . = ALIGN(4);
  } > dram0_0_seg

  /*This section holds data that should not be initialized at power up.
    The section located in Internal SRAM memory region. The macro _NOINIT
    can be used as attribute to place data into this section.
    See the esp_attr.h file for more information.
  */
  .noinit (NOLOAD):
  {
    . = ALIGN(4);
    _noinit_start = ABSOLUTE(.);
    *(.noinit .noinit.*)
    . = ALIGN(4) ;
    _noinit_end = ABSOLUTE(.);
  } > dram0_0_seg

  /* Shared RAM */
  .dram0.bss (NOLOAD) :
  {
    . = ALIGN (8);
    _bss_start = ABSOLUTE(.);

    mapping[dram0_bss]

    *(.dynsbss)
    *(.sbss)
    *(.sbss.*)
    *(.gnu.linkonce.sb.*)
    *(.scommon)
    *(.sbss2)
    *(.sbss2.*)
    *(.gnu.linkonce.sb2.*)
    *(.dynbss)
    *(.share.mem)
    *(.gnu.linkonce.b.*)

    . = ALIGN (8);
    _bss_end = ABSOLUTE(.);
  } > dram0_0_seg

  ASSERT(((_bss_end - ORIGIN(dram0_0_seg)) <= LENGTH(dram0_0_seg)),
          "DRAM segment data does not fit.")

  .flash.text :
  {
    _stext = .;
    _text_start = ABSOLUTE(.);

    mapping[flash_text]

    /* For ESP8266 library function */
    *(.irom0.literal .irom0.text)
    *(.irom.literal .irom.text .irom.text.literal)
    *(.text2 .text2.* .literal2 .literal2.*)

    *(.stub .gnu.warning .gnu.linkonce.literal.* .gnu.linkonce.t.*.literal .gnu.linkonce.t.*)
    *(.irom0.text) /* catch stray ICACHE_RODATA_ATTR */
    *(.fini.literal)
    *(.fini)
    *(.gnu.version)
    _text_end = ABSOLUTE(.);
    _etext = .;

    /* Similar to _iram_start, this symbol goes here so it is
       resolved by addr2line in preference to the first symbol in
       the flash.text segment.
    */
    _flash_cache_start = ABSOLUTE(0);
  } >iram0_2_seg

  .flash.rodata ALIGN(4) :
  {
    _rodata_start = ABSOLUTE(.);

   /**
      Insert 8 bytes data to make realy rodata section's link address offset to be 0x8,
      esptool will remove these data and add real segment header
    */
    . = 0x8;

    *(.rodata_desc .rodata_desc.*)               /* Should be the first.  App version info.        DO NOT PUT ANYTHING BEFORE IT! */
    *(.rodata_custom_desc .rodata_custom_desc.*) /* Should be the second. Custom app version info. DO NOT PUT ANYTHING BEFORE IT! */

    *(.rodata2 .rodata2.*)                       /* For ESP8266 library function */

    mapping[flash_rodata]

    *(.irom1.text) /* catch stray ICACHE_RODATA_ATTR */
    *(.gnu.linkonce.r.*)
    *(.rodata1)
    __XT_EXCEPTION_TABLE_ = ABSOLUTE(.);
    *(.xt_except_table)
    *(.gcc_except_table .gcc_except_table.*)
    *(.gnu.linkonce.e.*)
    *(.gnu.version_r)
    . = (. + 3) & ~ 3;
    __eh_frame = ABSOLUTE(.);
    KEEP(*(.eh_frame))
    . = (. + 7) & ~ 3;
    /*  C++ constructor and destructor tables

        Make a point of not including anything from crtbegin.o or crtend.o, as IDF doesn't use toolchain crt
      */
    __init_array_start = ABSOLUTE(.);
    KEEP (*(EXCLUDE_FILE (*crtend.* *crtbegin.*) .ctors .ctors.*))
    __init_array_end = ABSOLUTE(.);
    KEEP (*crtbegin.*(.dtors))
    KEEP (*(EXCLUDE_FILE (*crtend.*) .dtors))
    KEEP (*(SORT(.dtors.*)))
    KEEP (*(.dtors))
    /*  C++ exception handlers table:  */
    __XT_EXCEPTION_DESCS_ = ABSOLUTE(.);
    *(.xt_except_desc)
    *(.gnu.linkonce.h.*)
    __XT_EXCEPTION_DESCS_END__ = ABSOLUTE(.);
    *(.xt_except_desc_end)
    *(.dynamic)
    *(.gnu.version_d)
    /* Addresses of memory regions reserved via
       SOC_RESERVE_MEMORY_REGION() */
    soc_reserved_memory_region_start = ABSOLUTE(.);
    KEEP (*(.reserved_memory_address))
    soc_reserved_memory_region_end = ABSOLUTE(.);
    _rodata_end = ABSOLUTE(.);
    /* Literals are also RO data. */
    _lit4_start = ABSOLUTE(.);
    *(*.lit4)
    *(.lit4.*)
    *(.gnu.linkonce.lit4.*)
    _lit4_end = ABSOLUTE(.);
    . = ALIGN(4);
    _thread_local_start = ABSOLUTE(.);
    *(.tdata)
    *(.tdata.*)
    *(.tbss)
    *(.tbss.*)
    _thread_local_end = ABSOLUTE(.);
    . = ALIGN(4);
  } >iram0_2_seg
}

```

## File: `mxr_malloc.h` (4547 tokens)
```cpp
#pragma once

#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#else
#include "sdkconfig.h"
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

#ifndef MXR_IRAM_INLINE_ATTR
#define MXR_IRAM_INLINE_ATTR
#endif

#ifndef MXR_REALLOC_ZERO_FREES
#define MXR_REALLOC_ZERO_FREES 1
#endif

  /* ================================================================
   *  MxR-malloc v3 for ESP8266 RTOS SDK
   * ================================================================ */

#ifndef CONFIG_MXR_MAX_DESC
#define CONFIG_MXR_MAX_DESC 256
#endif

#ifndef CONFIG_MXR_IRAM_MAX_DESC
#define CONFIG_MXR_IRAM_MAX_DESC 128
#endif

#define MXR_REGIONS_MAX 32
#define MXR_REGIONS_MIN 1

#ifndef MXR_PARSED_REGION_COUNT
#define MXR_PARSED_REGION_COUNT 1
#endif

#if MXR_PARSED_REGION_COUNT > MXR_REGIONS_MAX
#undef MXR_PARSED_REGION_COUNT
#define MXR_PARSED_REGION_COUNT MXR_REGIONS_MAX
#elif MXR_PARSED_REGION_COUNT < MXR_REGIONS_MIN
#undef MXR_PARSED_REGION_COUNT
#define MXR_PARSED_REGION_COUNT MXR_REGIONS_MIN
#endif

#define MXR_USER_REGIONS MXR_PARSED_REGION_COUNT

#ifndef MXR_IRAM_FB_PARSED_REGION_COUNT
#define MXR_IRAM_FB_PARSED_REGION_COUNT 1
#endif

#define MXR_IRAM_FB_REGIONS_MAX 32
#define MXR_IRAM_FB_REGIONS_MIN 1

#if MXR_IRAM_FB_PARSED_REGION_COUNT > MXR_IRAM_FB_REGIONS_MAX
#undef MXR_IRAM_FB_PARSED_REGION_COUNT
#define MXR_IRAM_FB_PARSED_REGION_COUNT MXR_IRAM_FB_REGIONS_MAX

#elif MXR_IRAM_FB_PARSED_REGION_COUNT < MXR_IRAM_FB_REGIONS_MIN
#undef MXR_IRAM_FB_PARSED_REGION_COUNT
#define MXR_IRAM_FB_PARSED_REGION_COUNT MXR_IRAM_FB_REGIONS_MIN
#endif

#define MXR_IRAM_FB_REGION_COUNT MXR_IRAM_FB_PARSED_REGION_COUNT

#define MXR_ALIGN_SIZE 4
#define MXR_ALIGN_MASK (MXR_ALIGN_SIZE - 1)

/* ================================================================
 *  Anti-fragmentation tuning constants
 * ================================================================ */
/* Минимальный размер полезного gap. Если остаток после вырезания
 * блока меньше этого значения, блок расширяется на весь gap,
 * чтобы не создавать неиспользуемый "осколок".
 *
 * При выключенном CONFIG_MXR_ANTI_SLIVER порог равен 0: все проверки
 * вида (x < MXR_MIN_SLICE_BYTES) для uint32_t становятся всегда-ложными,
 * поэтому расширения и anti-sliver-защита realloc отключаются везде
 * автоматически, без дополнительных #ifdef в коде. */
/* ================================================================
 *  Anti-sliver switch
 * ================================================================ */
#ifdef CONFIG_MXR_ANTI_SLIVER
#ifndef CONFIG_MXR_MIN_SLICE_BYTES
#define MXR_MIN_SLICE_BYTES 8
#else
#define MXR_MIN_SLICE_BYTES CONFIG_MXR_MIN_SLICE_BYTES
#endif
/* FIX(4.1): waste == 0 больше не считается sliver.
   Иначе exact-fit попадал в anti_sliver_expansions. */
#define MXR_IS_SLIVER(x) \
  ((uint32_t)(x) > 0 && (uint32_t)(x) < (uint32_t)MXR_MIN_SLICE_BYTES)
#else
#define MXR_MIN_SLICE_BYTES 0
#define MXR_IS_SLIVER(x) ((void)(x), 0)
#endif

/* Best-fit early-exit: если waste (gap - bytes) <= bytes >> N,
 * считаем gap "достаточно хорошим" и прекращаем поиск. N=2 = 25%.
 *
 * При выключенном CONFIG_MXR_BEST_FIT_EARLY_EXIT включается строгий
 * best-fit: поиск останавливает только точное совпадение (waste == 0). */
#ifdef CONFIG_MXR_BEST_FIT_EARLY_EXIT
#define MXR_EARLY_EXIT_ACTIVE 1
#ifndef CONFIG_MXR_BEST_FIT_WASTE_SHIFT
#define MXR_BEST_FIT_WASTE_SHIFT 2
#else
#define MXR_BEST_FIT_WASTE_SHIFT CONFIG_MXR_BEST_FIT_WASTE_SHIFT
#endif
#else
#define MXR_EARLY_EXIT_ACTIVE 0
#define MXR_BEST_FIT_WASTE_SHIFT 2 /* не используется */
#endif

/* ================================================================
 *  DRAM cross-region max_bytes GUARD tuning
 *  Gate: CROSS_REGION_FALLBACK && DRAM_CROSS_ENABLED
 * ================================================================ */
#if defined(CONFIG_MXR_CROSS_REGION_FALLBACK) && \
    defined(CONFIG_MXR_DRAM_CROSS_ENABLED)
#if defined(CONFIG_MXR_DRAM_CROSS_ALL)
/* All: MXR_DRAM_GUARD_NUM/DEN намеренно НЕ определены ->
   проверка max_bytes в mxr_try_cross_region() пропускается. */
#elif defined(CONFIG_MXR_DRAM_CROSS_CONSERVATIVE)
/* Conservative: 50% GUARD */
#define MXR_DRAM_GUARD_NUM 1ul
#define MXR_DRAM_GUARD_DEN 2ul
#elif defined(CONFIG_MXR_DRAM_CROSS_AGGRESSIVE)
/* Aggressive: 90% GUARD */
#define MXR_DRAM_GUARD_NUM 9ul
#define MXR_DRAM_GUARD_DEN 10ul
#else
/* Moderate (default): 75% GUARD */
#define MXR_DRAM_GUARD_NUM 3ul
#define MXR_DRAM_GUARD_DEN 4ul
#endif
#endif /* CROSS_REGION_FALLBACK && DRAM_CROSS_ENABLED */

/* ================================================================
 *  IRAM fallback cross-region max_bytes GUARD tuning
 * ================================================================ */
#if defined(CONFIG_MXR_CROSS_REGION_FALLBACK) && \
    defined(CONFIG_MXR_IRAM_CROSS_ENABLED) &&    \
    defined(CONFIG_MXR_USE_IRAM)
#if defined(CONFIG_MXR_IRAM_CROSS_ALL)
/* All: MXR_IRAM_GUARD_NUM/DEN намеренно НЕ определены */
#elif defined(CONFIG_MXR_IRAM_CROSS_CONSERVATIVE)
#define MXR_IRAM_GUARD_NUM 1ul
#define MXR_IRAM_GUARD_DEN 2ul
#elif defined(CONFIG_MXR_IRAM_CROSS_AGGRESSIVE)
#define MXR_IRAM_GUARD_NUM 9ul
#define MXR_IRAM_GUARD_DEN 10ul
#else
#define MXR_IRAM_GUARD_NUM 3ul
#define MXR_IRAM_GUARD_DEN 4ul
#endif
#endif /* CROSS_REGION_FALLBACK && IRAM_CROSS_ENABLED && USE_IRAM */

/* ================================================================
 *  DRAM cross-region min_bytes guard tuning
 *
 *  Конвенция (единообразно с MXR_DRAM_GUARD_NUM/DEN):
 *    макрос определён   -> guard активен, значение = divisor
 *    макрос не определён -> guard выключен (пресет Disabled/All
 *                          или выключен сам cross-region)
 * ================================================================ */
#if defined(CONFIG_MXR_CROSS_REGION_FALLBACK) && \
    defined(CONFIG_MXR_DRAM_CROSS_ENABLED)
#if defined(CONFIG_MXR_DRAM_CROSS_MIN_BYTES_CONSERVATIVE)
#define MXR_DRAM_MIN_BYTES_DIVISOR 1ul
#elif defined(CONFIG_MXR_DRAM_CROSS_MIN_BYTES_AGGRESSIVE)
#define MXR_DRAM_MIN_BYTES_DIVISOR 4ul
#elif defined(CONFIG_MXR_DRAM_CROSS_MIN_BYTES_ALL)
/* Disabled: макрос намеренно НЕ определён */
#else /* MODERATE (default) */
#define MXR_DRAM_MIN_BYTES_DIVISOR 2ul
#endif
#endif /* CROSS_REGION_FALLBACK && !DRAM_CROSS_DISABLED */

/* ================================================================
 *  IRAM fallback cross-region min_bytes guard tuning
 * ================================================================ */
#if defined(CONFIG_MXR_CROSS_REGION_FALLBACK) && \
    defined(CONFIG_MXR_IRAM_CROSS_ENABLED) &&    \
    defined(CONFIG_MXR_USE_IRAM)
#if defined(CONFIG_MXR_IRAM_CROSS_MIN_BYTES_CONSERVATIVE)
#define MXR_IRAM_MIN_BYTES_DIVISOR 1ul
#elif defined(CONFIG_MXR_IRAM_CROSS_MIN_BYTES_AGGRESSIVE)
#define MXR_IRAM_MIN_BYTES_DIVISOR 4ul
#elif defined(CONFIG_MXR_IRAM_CROSS_MIN_BYTES_ALL)
/* Disabled: макрос намеренно НЕ определён */
#else /* MODERATE (default) */
#define MXR_IRAM_MIN_BYTES_DIVISOR 2ul
#endif
#endif /* CROSS_REGION_FALLBACK && !IRAM_CROSS_DISABLED && USE_IRAM */

#define MXR_OFF_BITS 31
#define MXR_LEN_BITS 31
#define MXR_OFF_MASK ((uint32_t)((1u << MXR_OFF_BITS) - 1u))
#define MXR_LEN_MASK ((uint32_t)((1u << MXR_LEN_BITS) - 1u))
#define MXR_OFF_FLAGS_MASK ((uint32_t)~MXR_OFF_MASK)
#define MXR_LEN_FLAGS_MASK ((uint32_t)~MXR_LEN_MASK)
#define MXR_LEN_FLAG_EXEC ((uint32_t)(1u << 31))
#define MXR_MAX_OFFSET_BYTES MXR_OFF_MASK
#define MXR_MAX_LEN_BYTES ((size_t)MXR_LEN_MASK)
#define MXR_MAX_ARENA_BYTES MXR_MAX_LEN_BYTES
#define MXR_REGION_MAX_UNLIMITED 0

/* ================================================================
 *  Platform-dependent type widths
 * ================================================================ */
#ifdef CONFIG_MXR_COMPACT_TYPES
  typedef uint16_t mxr_caps_t;
  typedef uint16_t mxr_class_t;
  typedef uint16_t mxr_count_t;
#else
typedef uint32_t mxr_caps_t;
typedef uint32_t mxr_class_t;
typedef uint32_t mxr_count_t;
#endif

/* ================================================================
 *  Capability bits
 * ================================================================ */
#ifndef MALLOC_CAP_EXEC
#define MALLOC_CAP_EXEC (1 << 0)
#endif
#ifndef MALLOC_CAP_32BIT
#define MALLOC_CAP_32BIT (1 << 1)
#endif
#ifndef MALLOC_CAP_8BIT
#define MALLOC_CAP_8BIT (1 << 2)
#endif
#ifndef MALLOC_CAP_DMA
#define MALLOC_CAP_DMA (1 << 3)
#endif
#ifndef MALLOC_CAP_SPIRAM
#define MALLOC_CAP_SPIRAM (1 << 10)
#endif
#ifndef MALLOC_CAP_INTERNAL
#define MALLOC_CAP_INTERNAL (1 << 11)
#endif

#define MXR_DRAM_CAPS_DEFAULT \
  (MALLOC_CAP_8BIT | MALLOC_CAP_32BIT | MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
#define MXR_IRAM_CAPS_DEFAULT \
  (MALLOC_CAP_32BIT | MALLOC_CAP_EXEC)
#define MXR_IRAM_FB_CAPS_DEFAULT \
  (MALLOC_CAP_32BIT | MALLOC_CAP_INTERNAL)

/* ================================================================
 *  Descriptor table placement attribute
 * ================================================================ */
#if defined(CONFIG_MXR_DESC_IN_IRAM_TEXT)
#define MXR_IRAM_DATA_ATTR __attribute__((section(".iram0.text"), aligned(4)))
#elif defined(CONFIG_MXR_DESC_IN_IRAM_BSS)
#define MXR_IRAM_DATA_ATTR __attribute__((section(".iram0.bss"), aligned(4)))
#else
#define MXR_IRAM_DATA_ATTR
#endif

  /* ================================================================
   *  Allocation descriptor — always 8 bytes
   * ================================================================ */
  typedef struct
  {
    uint32_t off_flags;
    uint32_t len_flags;
  } mxr_desc_t;

  _Static_assert(sizeof(mxr_desc_t) == 8, "desc must be 8 bytes");

  /* ================================================================
   *  Region configuration (build-time)
   * ================================================================ */
  typedef struct
  {
    uint8_t percent;
    mxr_class_t min_bytes;
    mxr_class_t max_bytes;
  } mxr_region_cfg_t;

  /* ================================================================
   *  Runtime region state (DRAM + IRAM fallback)
   * ================================================================ */
  typedef struct
  {
    mxr_caps_t caps;
    uint32_t start_byte;
    uint32_t total_bytes;
    mxr_class_t min_bytes;
    mxr_class_t max_bytes;
    uint32_t free_bytes;
    uint32_t min_free_bytes;
    mxr_count_t alloc_count;
    uint32_t largest_free_cache;
    uint8_t largest_cache_valid;
  } mxr_region_t;

  _Static_assert(sizeof(mxr_region_t) % 4 == 0,
                 "mxr_region_t size must be multiple of 4 for mxr_memset4");

  /* ================================================================
   *  Region status for diagnostics
   * ================================================================ */
  typedef struct
  {
    mxr_caps_t caps;
    uint32_t start_byte;
    uint32_t total_bytes;
    mxr_class_t min_bytes;
    mxr_class_t max_bytes;
    uint32_t free_bytes;
    uint32_t min_free_bytes;
    uint32_t largest_free_bytes;
    mxr_count_t alloc_count;
  } mxr_region_status_t;

  /* ================================================================
   *  Global allocator status
   * ================================================================ */
  typedef struct
  {
    bool initialized;
    uint8_t region_count;
    uint8_t iram_fb_region_count;
    uint16_t dram_desc_capacity;
    uint16_t iram_desc_capacity;
    uint16_t dram_active_allocs;
    uint16_t iram_active_allocs;
    uint16_t max_active_allocs;
    size_t total_bytes;
    size_t free_bytes;
    size_t min_free_bytes;
    size_t largest_free_block_bytes;
    size_t iram_total_bytes;
    size_t iram_free_bytes;
    size_t iram_min_free_bytes;
    size_t iram_fb_zone_total_bytes;
    uint32_t exec_allocs;
    uint32_t iram_fallback_allocs;
    uint32_t exec_zone_rejects;        /* EXEC отклонены: нет зоны/нет места в [0,reserve) */
    size_t iram_exec_zone_total_bytes; /* размер EXEC-зоны (= IRAM_RESERVE_BYTES) */
    size_t iram_exec_zone_free_bytes;  /* свободно в EXEC-зоне сейчас */
    size_t iram_exec_zone_min_free_bytes;
    uint32_t cross_region_allocs;
    uint32_t cross_region_guard_rejects; /* отказы по max_bytes GUARD / min_bytes guard */
    uint32_t alloc_fail_no_memory;
    uint32_t alloc_fail_table_full;
    uint32_t invalid_free_attempts;
    uint32_t region_lookup_failures;
    uint32_t cross_region_skip_fragmented;
    uint32_t fragmentation_pct;      /* (free - largest) / free * 100 */
    uint32_t gap_count;              /* количество свободных gaps */
    uint32_t sliver_count;           /* gaps < MXR_MIN_SLICE_BYTES */
    uint32_t best_fit_early_exits;   /* сколько раз best-fit сработал рано */
    uint32_t anti_sliver_expansions; /* сколько раз блок расширен до полного gap */
                                     /* FIX(3.2): причины пропуска cross-region */
    uint32_t cross_caps_skips;
    uint32_t cross_free_skips;
    uint32_t cross_cache_skips;

    /* FIX(3.3): причины отказа вставки дескриптора */
    uint32_t desc_insert_fail_bounds;
    uint32_t desc_insert_fail_overlap;
    uint32_t desc_insert_fail_duplicate;

    /* FIX(4.3): region init fallback */
    bool region_init_fallback;
    bool iram_fb_region_init_fallback;
  } mxr_status_t;

  /* ДОБАВЛЕНО: проверка кратности 4 для mxr_memset4 */
  _Static_assert(sizeof(mxr_status_t) % 4 == 0,
                 "mxr_status_t size must be multiple of 4 for mxr_memset4");

  /* ================================================================
   *  Alignment helper
   * ================================================================ */
  static inline size_t MXR_IRAM_INLINE_ATTR mxr_align4(size_t bytes)
  {
    return (bytes + MXR_ALIGN_MASK) & ~(size_t)MXR_ALIGN_MASK;
  }

  /* ================================================================
   *  Descriptor helpers
   * ================================================================ */
  static inline uint32_t MXR_IRAM_INLINE_ATTR mxr_desc_off(const mxr_desc_t *d)
  {
    return d->off_flags & MXR_OFF_MASK;
  }

  static inline uint32_t MXR_IRAM_INLINE_ATTR mxr_desc_len(const mxr_desc_t *d)
  {
    return d->len_flags & MXR_LEN_MASK;
  }

  static inline bool MXR_IRAM_INLINE_ATTR mxr_desc_is_exec(const mxr_desc_t *d)
  {
    return (d->len_flags & MXR_LEN_FLAG_EXEC) != 0;
  }

  static inline void MXR_IRAM_INLINE_ATTR mxr_desc_clear(mxr_desc_t *d)
  {
    d->off_flags = 0;
    d->len_flags = 0;
  }

  static inline void MXR_IRAM_INLINE_ATTR mxr_desc_set(
      mxr_desc_t *d,
      uint32_t off_bytes,
      uint32_t len_bytes,
      uint32_t len_flags)
  {
    d->off_flags = off_bytes & MXR_OFF_MASK;
    d->len_flags = (len_bytes & MXR_LEN_MASK) | (len_flags & MXR_LEN_FLAGS_MASK);
  }

  /* ================================================================
   *  MxR API
   * ================================================================ */
  void mxr_init(void);
  void *mxr_malloc(size_t size);
  void mxr_free(void *ptr);
  void *mxr_calloc(size_t count, size_t size);
  void *mxr_realloc(void *ptr, size_t size);
  void *mxr_zalloc(size_t size);
  void mxr_get_status(mxr_status_t *status);
  bool mxr_get_region_status(int region_index, mxr_region_status_t *status);
  bool mxr_get_iram_fb_region_status(int region_index, mxr_region_status_t *status);
  void mxr_dump(void);
  size_t mxr_get_total_size_caps(uint32_t caps);
  size_t mxr_get_largest_free_block_caps(uint32_t caps);
  size_t mxr_get_allocated_size_caps(uint32_t caps);

  /* ESP heap compatibility layer */
  void _heap_caps_free(void *ptr, const char *file, size_t line);
  void *_heap_caps_malloc(size_t size, uint32_t caps, const char *file, size_t line);
  void *_heap_caps_calloc(size_t count, size_t size, uint32_t caps, const char *file, size_t line);
  void *_heap_caps_realloc(void *mem, size_t newsize, uint32_t caps, const char *file, size_t line);
  void *_heap_caps_zalloc(size_t size, uint32_t caps, const char *file, size_t line);
  size_t heap_caps_get_free_size(uint32_t caps);
  size_t heap_caps_get_minimum_free_size(uint32_t caps);
  size_t heap_caps_get_dram_free_size(void);
  void heap_caps_init(void);
  void *heap_caps_malloc_default(size_t size);
  void *heap_caps_realloc_default(void *ptr, size_t size);
  size_t heap_caps_get_total_size(uint32_t caps);
  size_t heap_caps_get_allocated_size(uint32_t caps);
  size_t heap_caps_get_largest_free_block(uint32_t caps);

  /* Capability-aware MxR API */
  void *mxr_malloc_caps(size_t size, uint32_t caps);
  void *mxr_calloc_caps(size_t count, size_t size, uint32_t caps);
  void *mxr_realloc_caps(void *ptr, size_t newsize, uint32_t caps);
  void *mxr_zalloc_caps(size_t size, uint32_t caps);
  size_t mxr_get_free_size_caps(uint32_t caps);
  size_t mxr_get_min_free_size_caps(uint32_t caps);

#ifdef __cplusplus
}
#endif
```

