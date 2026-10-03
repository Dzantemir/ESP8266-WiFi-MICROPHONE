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
 *  ПРАВИЛО: MXR_IRAM_DATA_ATTR / MXR_STATE_DATA_ATTR допустимы ТОЛЬКО
 *  для объектов, все поля которых 32-битные: ESP8266 не умеет 8/16-битные
 *  доступы к IRAM (их эмулирует LoadStoreErrorHandler из xtensa_vectors.S,
 *  остальное -> _xt_ext_panic).
 *
 *  Размещение состояния (s_stats, s_region[], s_iram_fb_region[]) выбирается
 *  в menuconfig, MXR_STATE_PLACEMENT:
 *    "IRAM" (вариант B) — атрибут сохраняется, поля принудительно 32-битные
 *         (MXR_FIELD_*), работает _Static_assert-барьер; DRAM не расходуется;
 *    "DRAM" (вариант A) — MXR_STATE_DATA_ATTR пуст, объекты уезжают в .bss,
 *         ширины полей обычные (COMPACT_TYPES действует), барьер не нужен.
 *
 *  Второе ограничение: startup (call_start_cpu) обнуляет .iram0.bss
 *  словами по 4 байта — невыровненных полей там быть не должно.
 * ================================================================ */
#if MXR_DESC_DYNAMIC_ACTIVE
/* Таблица дескрипторов живёт в хвосте DRAM-арены:
 * [s_arena_base + s_arena_total_bytes, конец арены).
 * Растёт вниз чанками MXR_DESC_CHUNK, сжимается с гистерезисом. */
static mxr_desc_t *s_dram_desc;
static uint16_t s_dram_desc_cap; /* ёмкость в дескрипторах */
#else
static mxr_desc_t s_dram_desc[CONFIG_MXR_MAX_DESC] MXR_IRAM_DATA_ATTR;
#define s_dram_desc_cap ((uint16_t)CONFIG_MXR_MAX_DESC)
#endif

#ifdef CONFIG_MXR_USE_IRAM
#if MXR_IRAM_DESC_DYNAMIC_ACTIVE
/* Хвост IRAM-арены (после fb-регионов). */
static mxr_desc_t *s_iram_desc;
static uint16_t s_iram_desc_cap;
#else
static mxr_desc_t s_iram_desc[CONFIG_MXR_IRAM_MAX_DESC] MXR_IRAM_DATA_ATTR;
#define s_iram_desc_cap ((uint16_t)CONFIG_MXR_IRAM_MAX_DESC)
#endif
#endif /* CONFIG_MXR_USE_IRAM */

static volatile bool s_dump_in_progress;

#if MXR_CANARY_ACTIVE
#define MXR_CANARY_HEAD_MAGIC ((uint32_t)0xC4A55EF1u)
#define MXR_CANARY_TAIL_MAGIC ((uint32_t)0xC4A557A1u)
#endif

#if MXR_DFD_ACTIVE
/* Кольцо последних освобождённых блоков для классификации double-free.
 * 8 байт на запись: offset (4) + len (3) + arena (1). */
typedef struct
{
    uint32_t off;
    /* Packed: low 24 bits = length, high 8 bits = arena id. */
    uint32_t len_arena;
} mxr_dfd_entry_t;

static mxr_dfd_entry_t s_dfd_ring[MXR_DFD_RING_SIZE];
static uint16_t s_dfd_pos;
static uint16_t s_dfd_count; /* сколько слотов реально занято */
#endif

/* Все скаляры — только DRAM (без MXR_IRAM_DATA_ATTR) */
static uint16_t s_dram_desc_count;
static uint8_t s_region_count;
static uint8_t *s_arena_base;
static uint32_t s_arena_total_bytes MXR_IRAM_DATA_ATTR;
static uint32_t s_dram_free_bytes MXR_IRAM_DATA_ATTR;
static uint32_t s_dram_min_free_bytes MXR_IRAM_DATA_ATTR;
/* Historical minimum for DRAM + non-EXEC IRAM fallback, i.e. the aggregate
 * pool used by caps==0 and pure 32-bit/internal queries. */
static uint32_t s_dram_fb_min_free_bytes MXR_IRAM_DATA_ATTR;
static bool s_initialized;

/* Размещение по MXR_STATE_PLACEMENT (см. шапку файла) */
static mxr_status_t s_stats MXR_STATE_DATA_ATTR;

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
static uint32_t s_iram_fb_free_bytes MXR_IRAM_DATA_ATTR;
/* Скаляр оставляем всегда (1 байт): нужен в status/dump,
 * при выключенном fallback всегда == 0 */
static uint8_t s_iram_fb_region_count;
#ifdef CONFIG_MXR_IRAM_FALLBACK_ENABLED
/* Размещение по MXR_STATE_PLACEMENT (см. шапку файла) */
static mxr_region_t s_iram_fb_region[MXR_IRAM_FB_REGION_COUNT] MXR_STATE_DATA_ATTR;
#endif

#endif

#define MXR_ACTIVE_TOTAL_REGIONS MXR_USER_REGIONS

/* Размещение по MXR_STATE_PLACEMENT (см. шапку файла) */
static mxr_region_t s_region[MXR_ACTIVE_TOTAL_REGIONS] MXR_STATE_DATA_ATTR;

static int16_t mxr_parse_region_config(const char *s, mxr_region_cfg_t *out, uint8_t max_count);
static size_t mxr_get_largest_free_block_caps_locked(uint32_t caps);
static size_t mxr_get_largest_user_block_locked(void);
#if MXR_DESC_DYNAMIC_ACTIVE
static bool mxr_dram_table_can_grow_for(uint32_t off_bytes, uint32_t len_bytes);
static bool mxr_dram_table_grow(void);
static void mxr_dram_table_maybe_shrink(void);
#endif
#ifdef CONFIG_MXR_USE_IRAM
#if MXR_IRAM_DESC_DYNAMIC_ACTIVE
static bool mxr_iram_table_can_grow_for(uint32_t off_bytes, uint32_t len_bytes);
static bool mxr_iram_table_grow(void);
static void mxr_iram_table_maybe_shrink(void);
#endif
#endif
static size_t mxr_get_total_size_caps_locked(uint32_t caps);
static size_t mxr_get_free_size_caps_locked(uint32_t caps);
/* ================================================================
 *  Word-aligned memory helpers (IRAM-safe, no libc)
 * ================================================================ */
/* FIX(#7): было MXR_IRAM_ALLOC_ATTR — при CORE-only этот атрибут пуст,
 * а helpers вызываются из IRAM core-путей (сдвиги таблиц, grow/shrink).
 * inline не гарантирует встраивания: невстроенная функция осталась бы
 * во flash и была бы недоступна при отключённом кэше. */
/* Volatile word lvalues prevent compiler replacement with bytewise libc routines. */
static inline void MXR_IRAM_ATTR mxr_memset4(void *ptr, size_t bytes)
{
    volatile uint32_t *p = (volatile uint32_t *)ptr;
    size_t words = bytes >> 2;
    for (size_t i = 0; i < words; i++)
        p[i] = 0;
}

static inline void MXR_IRAM_ATTR mxr_memcpy4(void *dst, const void *src, size_t bytes)
{
    volatile uint32_t *d = (volatile uint32_t *)dst;
    const volatile uint32_t *s = (const volatile uint32_t *)src;
    size_t words = bytes >> 2;
    for (size_t i = 0; i < words; i++)
        d[i] = s[i];
}

/* Перекрытие-безопасная пословная копия (IRAM-safe, без libc memmove). */
static inline void MXR_IRAM_ATTR mxr_memmove4(void *dst, const void *src, size_t bytes)
{
    volatile uint32_t *d = (volatile uint32_t *)dst;
    const volatile uint32_t *s = (const volatile uint32_t *)src;
    size_t words = bytes >> 2;
    if (d == s)
        return;
    if ((uintptr_t)d < (uintptr_t)s)
    {
        for (size_t i = 0; i < words; i++)
            d[i] = s[i];
    }
    else
    {
        for (size_t i = words; i > 0; i--)
            d[i - 1] = s[i - 1];
    }
}

static inline uint32_t mxr_percent_of(uint32_t total, uint32_t percent)
{
    return (total / 100u) * percent + ((total % 100u) * percent) / 100u;
}

/* Largest-free APIs return the largest user payload, not the raw descriptor
 * length. Raw gaps and descriptor limits are aligned to four bytes; canary
 * bytes are part of the descriptor length when enabled. */
static uint32_t MXR_IRAM_ATTR mxr_largest_user_block_from_internal(uint32_t bytes)
{
    bytes &= ~(uint32_t)MXR_ALIGN_MASK;
    if (bytes <= (uint32_t)MXR_BLOCK_OVERHEAD)
        return 0;
    return bytes - (uint32_t)MXR_BLOCK_OVERHEAD;
}

static uint32_t MXR_IRAM_ATTR mxr_largest_user_block_in_gap(
    uint32_t raw_gap,
    uint32_t min_internal,
    uint32_t class_max_internal,
    uint32_t policy_max_internal)
{
    uint32_t internal = raw_gap & ~(uint32_t)MXR_ALIGN_MASK;
    if (class_max_internal != MXR_REGION_MAX_UNLIMITED &&
        internal > class_max_internal)
        internal = class_max_internal & ~(uint32_t)MXR_ALIGN_MASK;
    if (policy_max_internal != 0 && internal > policy_max_internal)
        internal = policy_max_internal & ~(uint32_t)MXR_ALIGN_MASK;
    if (internal < min_internal)
        return 0;
    return mxr_largest_user_block_from_internal(internal);
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
    /* один проход memmove4: меньше загрузок/ветвлений, чем поэлементный сдвиг */
    mxr_memmove4(&s_dram_desc[pos + 1], &s_dram_desc[pos],
                 (size_t)(s_dram_desc_count - pos) * sizeof(mxr_desc_t));
}

static void MXR_IRAM_ATTR mxr_dram_desc_shift_left(int pos)
{
    mxr_memmove4(&s_dram_desc[pos], &s_dram_desc[pos + 1],
                 (size_t)(s_dram_desc_count - (uint16_t)pos - 1u) * sizeof(mxr_desc_t));
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
    if (s_dram_desc_count >= s_dram_desc_cap)
    {
#if MXR_DESC_DYNAMIC_ACTIVE
        /* Descriptor growth consumes the top delta bytes. Preflight the
         * candidate against the post-growth end before changing any state. */
        if (!mxr_dram_table_can_grow_for(off_bytes, len_bytes) ||
            !mxr_dram_table_grow())
        {
            s_stats.alloc_fail_table_full++;
            return false;
        }
#else
        s_stats.alloc_fail_table_full++;
        return false;
#endif
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

#if MXR_DESC_BINARY_SEARCH_ACTIVE
/* Возвращает индекс первого DRAM-дескриптора, у которого конец блока
 * (off + len) > min_off. Дескрипторы отсортированы по off и не
 * пересекаются, поэтому (off+len) строго возрастает и бинарный поиск
 * корректен. Все дескрипторы с индексом < результата лежат ЦЕЛИКОМ
 * до min_off и не могут создавать gap внутри региона. */
static uint16_t MXR_IRAM_ATTR mxr_dram_desc_first_after(uint32_t min_off)
{
    uint16_t left = 0;
    uint16_t right = s_dram_desc_count;
    while (left < right)
    {
        uint16_t mid = (uint16_t)((left + right) >> 1);
        uint32_t end = mxr_desc_off(&s_dram_desc[mid]) +
                       mxr_desc_len(&s_dram_desc[mid]);
        if (end <= min_off)
            left = (uint16_t)(mid + 1);
        else
            right = mid;
    }
    return left;
}
#endif /* MXR_DESC_BINARY_SEARCH_ACTIVE */

/* ================================================================
 *  IRAM descriptor array operations
 * ================================================================ */
#ifdef CONFIG_MXR_USE_IRAM
#ifdef CONFIG_MXR_IRAM_FALLBACK_ENABLED
static bool MXR_IRAM_ATTR mxr_iram_fb_find_free_in_region(
    int reg,
    uint32_t bytes,
    uint32_t *out_off,
    uint32_t *out_alloc_bytes,
    uint32_t *out_largest,
    bool *out_largest_exact);
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
    mxr_memmove4(&s_iram_desc[pos + 1], &s_iram_desc[pos],
                 (size_t)(s_iram_desc_count - pos) * sizeof(mxr_desc_t));
}

static void MXR_IRAM_ATTR mxr_iram_desc_shift_left(int pos)
{
    mxr_memmove4(&s_iram_desc[pos], &s_iram_desc[pos + 1],
                 (size_t)(s_iram_desc_count - (uint16_t)pos - 1u) * sizeof(mxr_desc_t));
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
    if (s_iram_desc_count >= s_iram_desc_cap)
    {
#if MXR_IRAM_DESC_DYNAMIC_ACTIVE
        /* IRAM descriptor growth also consumes the fallback tail. Preflight
         * against the post-growth arena before mutating allocator state. */
        if (!mxr_iram_table_can_grow_for(off_bytes, len_bytes) ||
            !mxr_iram_table_grow())
        {
            s_stats.alloc_fail_table_full++;
            return false;
        }
#else
        s_stats.alloc_fail_table_full++;
        return false;
#endif
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

#if MXR_DESC_BINARY_SEARCH_ACTIVE
/* Аналог mxr_dram_desc_first_after для IRAM-дескрипторов. */
static uint16_t MXR_IRAM_ATTR mxr_iram_desc_first_after(uint32_t min_off)
{
    uint16_t left = 0;
    uint16_t right = s_iram_desc_count;
    while (left < right)
    {
        uint16_t mid = (uint16_t)((left + right) >> 1);
        uint32_t end = mxr_desc_off(&s_iram_desc[mid]) +
                       mxr_desc_len(&s_iram_desc[mid]);
        if (end <= min_off)
            left = (uint16_t)(mid + 1);
        else
            right = mid;
    }
    return left;
}
#endif /* MXR_DESC_BINARY_SEARCH_ACTIVE */

#endif /* CONFIG_MXR_USE_IRAM */

/* ================================================================
 *  DRAM region helpers
 * ================================================================ */
/* Оптимизация hot-path: регионы DRAM непрерывны и отсортированы по
 * start_byte (это гарантируется mxr_init_regions_exact и
 * mxr_init_regions_temp_single), поэтому линейный скан O(N) заменён
 * бинарным поиском O(log N): ищется последний start_byte <= off. */
static int MXR_IRAM_ATTR mxr_region_by_off(uint32_t off_bytes)
{
    int lo = 0;
    int hi = (int)s_region_count;
    while (lo < hi)
    {
        int mid = (lo + hi) >> 1;
        if (s_region[mid].start_byte <= off_bytes)
            lo = mid + 1;
        else
            hi = mid;
    }
    int i = lo - 1;
    if (i < 0)
        return -1;
    if (off_bytes < s_region[i].start_byte + (uint32_t)s_region[i].total_bytes)
        return i;
    return -1;
}

static inline bool MXR_IRAM_INLINE_ATTR mxr_region_caps_ok(int region_index, uint32_t caps)
{
    if (region_index < 0 || region_index >= s_region_count)
        return false;
    return ((uint32_t)s_region[region_index].caps & caps) == caps;
}

static inline bool MXR_IRAM_INLINE_ATTR mxr_region_size_ok(int region_index, uint32_t bytes)
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

static int MXR_IRAM_ATTR mxr_region_for_size(uint32_t len_bytes, uint32_t caps)
{
    /* Быстрый путь O(log N): классы размеров непрерывны и упорядочены
     * (валидируется в mxr_init_regions_exact), поэтому подходящий
     * регион — первый, у которого eff_max >= len_bytes. */
    int lo = 0;
    int hi = (int)s_region_count;
    while (lo < hi)
    {
        int mid = (lo + hi) >> 1;
        uint32_t eff_max = (s_region[mid].max_bytes == MXR_REGION_MAX_UNLIMITED)
                               ? UINT32_MAX
                               : (uint32_t)s_region[mid].max_bytes;
        if (eff_max < len_bytes)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < (int)s_region_count &&
        mxr_region_caps_ok(lo, caps) &&
        mxr_region_size_ok(lo, len_bytes))
    {
        return lo;
    }

    /* Защитный fallback: линейный скан (если у регионов когда-либо
     * появятся различающиеся caps — поведение прежнее). */
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

static uint32_t MXR_IRAM_ATTR mxr_region_largest_free_bytes(uint8_t region_index)
{
    if (region_index >= s_region_count)
        return 0;

    if (s_region[region_index].largest_cache_valid)
        return s_region[region_index].largest_free_cache;
    uint32_t region_start = s_region[region_index].start_byte;
    uint32_t region_end = region_start + (uint32_t)s_region[region_index].total_bytes;
    uint32_t cur = region_start;
    uint32_t largest = 0;
    uint16_t start_idx = 0;
#if MXR_DESC_BINARY_SEARCH_ACTIVE
    start_idx = mxr_dram_desc_first_after(region_start);
#endif
    for (uint16_t i = start_idx; i < s_dram_desc_count; i++)
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
    s_region[region_index].largest_free_cache = largest;
    s_region[region_index].largest_cache_valid = 1;
    return largest;
}

static inline void MXR_IRAM_INLINE_ATTR mxr_region_invalidate_cache(int region_index)
{
    if (region_index >= 0 && region_index < s_region_count)
        s_region[region_index].largest_cache_valid = 0;
}

static inline void MXR_IRAM_INLINE_ATTR mxr_update_dram_fb_min_free(void)
{
#ifdef CONFIG_MXR_USE_IRAM
    const uint32_t cur = s_dram_free_bytes +
        (s_iram_enabled ? s_iram_fb_free_bytes : 0u);
#else
    const uint32_t cur = s_dram_free_bytes;
#endif
    if (cur < s_dram_fb_min_free_bytes)
        s_dram_fb_min_free_bytes = cur;
}

static inline void MXR_IRAM_INLINE_ATTR mxr_region_allocated(int region_index, uint32_t bytes)
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

    mxr_update_dram_fb_min_free();
    mxr_region_invalidate_cache(region_index);
}

static inline void MXR_IRAM_INLINE_ATTR mxr_region_released(int region_index, uint32_t bytes)
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
    mxr_update_dram_fb_min_free();
}

/* ================================================================
 *  Единые правила приёма gap'а (quick-fit / binning / early-exit)
 *
 *  anti-sliver: waste>0 и waste<MXR_MIN_SLICE_BYTES -> расширяем до
 *  всего gap'а (если это не нарушает max_bytes региона).
 *  big-gap guard: с waste выше обычного лимита gap принимается рано
 *  ТОЛЬКО если он не «батон» (<= max(GAP_MIN, bytes<<SHIFT));
 *  счётчик bgg_relaxed_accepts. «Батоны» преждевременная резка
 *  не трогает — они остаются для крупных запросов.
 * ================================================================ */
/* Big-gap guard («батонная» политика).
 *
 * Порог: gap не «батон», если gap <= max(GAP_MIN, bytes << FACTOR_SHIFT).
 * Раннее принятие (early-exit / hint / bin) РАССЛАБЛЯЕТСЯ: gap с waste
 * больше обычного лимита принимается преждевременно, ТОЛЬКО если он
 * не «батон». Gap'ы выше порога ранней резка никогда не касается —
 * они остаются целыми для крупных запросов (полный скан выберет их
 * best-fit'ом только если нет ничего меньше). */
static inline uint32_t MXR_IRAM_INLINE_ATTR mxr_bgg_limit(uint32_t bytes)
{
    uint32_t limit;
    if (bytes >= (UINT32_MAX >> MXR_BGG_FACTOR_SHIFT))
        limit = UINT32_MAX;
    else
    {
        limit = bytes << MXR_BGG_FACTOR_SHIFT;
        if (limit < (uint32_t)MXR_BGG_GAP_MIN)
            limit = (uint32_t)MXR_BGG_GAP_MIN;
    }
    return limit;
}

/* true -> gap можно принять по расслабленному правилу (waste > обычного
 * лимита), и тогда это учитывается в статистике. */
static inline bool MXR_IRAM_INLINE_ATTR
mxr_bgg_relaxed(uint32_t gap, uint32_t bytes, uint32_t waste_limit)
{
#if MXR_BGG_ACTIVE
    if ((gap - bytes) > waste_limit && gap <= mxr_bgg_limit(bytes))
    {
        s_stats.bgg_relaxed_accepts++;
        return true;
    }
#else
    (void)gap;
    (void)bytes;
    (void)waste_limit;
#endif
    return false;
}

/* Приём gap'а по общим правилам. Возвращает true и *out_use
 * (== bytes либо gap при anti-sliver расширении).
 * Вызывается только из hint/binning путей. */
#if MXR_QUICK_FIT_HINT_ACTIVE
static bool MXR_IRAM_ATTR mxr_gap_acceptable(
    uint32_t gap, uint32_t bytes, uint32_t waste_limit,
    uint32_t region_max_bytes, uint32_t *out_use)
{
    uint32_t waste = gap - bytes;

    if (MXR_IS_SLIVER(waste) &&
        (region_max_bytes == MXR_REGION_MAX_UNLIMITED || gap <= region_max_bytes))
    {
        /* waste==0 сюда не попадает (см. FIX(4.1)): точный fit без счётчика */
        if (waste > 0)
        {
            *out_use = gap;
            s_stats.anti_sliver_expansions++;
        }
        else
        {
            *out_use = bytes;
        }
        return true;
    }
    if (waste <= waste_limit || mxr_bgg_relaxed(gap, bytes, waste_limit))
    {
        *out_use = bytes;
        return true;
    }
    return false;
}

#endif /* MXR_QUICK_FIT_HINT_ACTIVE */

/* ================================================================
 *  Quick-fit hint / Binning — O(1) быстрый путь аллокации в DRAM
 *
 *  Инвариант: запомненный gap — гарантированно свободные байты,
 *  полностью внутри региона; len == 0 -> слота/подсказки нет.
 *
 *  Корректность опирается на два свойства аллокатора:
 *   1) дескрипторы DRAM отсортированы по off и не пересекаются;
 *   2) блок никогда не пересекает границу региона.
 *  Поэтому gap, содержащий освобождённый блок, полностью
 *  определяется соседними живыми дескрипторами и границами региона.
 *
 *  Политика LRU: запоминаем gap от ПОСЛЕДНЕГО освобождения
 *  (при binning — в слот его размерного класса). Полный пустой
 *  регион не запоминаем: на пустой куче скан тривиален, а лишняя
 *  проверка подсказки только замедляет.
 * ================================================================ */
#if MXR_BINNING_ACTIVE

/* Логарифмический класс размера: пороги MXR_BIN_BASE*2^k. */
static inline uint8_t MXR_IRAM_INLINE_ATTR mxr_bin_of(uint32_t size)
{
    uint8_t idx = 0;
    uint32_t t = (uint32_t)MXR_BIN_BASE;
    while (idx < (uint8_t)(MXR_BIN_COUNT - 1) && size > t)
    {
        idx++;
        t <<= 1;
    }
    return idx;
}

/* Попытка O(1) аллокации из bin-слотов: от класса запроса и выше. */
static bool MXR_IRAM_ATTR
mxr_bins_try_alloc(int region_index, uint32_t bytes,
                   uint32_t *out_off, uint32_t *out_alloc_bytes)
{
#if MXR_EARLY_EXIT_ACTIVE
    uint32_t waste_limit = bytes >> MXR_BEST_FIT_WASTE_SHIFT;
    if (waste_limit < MXR_ALIGN_SIZE)
        waste_limit = MXR_ALIGN_SIZE;
#else
    uint32_t waste_limit = 0;
#endif
    uint32_t region_max = (uint32_t)s_region[region_index].max_bytes;
    uint8_t b0 = mxr_bin_of(bytes);

    for (uint8_t b = b0; b < (uint8_t)MXR_BIN_COUNT; b++)
    {
        uint32_t gl = s_region[region_index].hint_len[b];
        if (gl < bytes)
            continue;
        uint32_t use = bytes;
        if (mxr_gap_acceptable(gl, bytes, waste_limit, region_max, &use))
        {
            *out_off = s_region[region_index].hint_off[b];
            *out_alloc_bytes = use;
            s_stats.quick_fit_hint_hits++;
            return true;
        }
        s_stats.quick_fit_hint_misses++;
    }
    return false;
}

/* Вырезание аллоцированного блока из ВСЕХ bin-слотов региона:
 * аллокация могла прийтись из полного скана и задеть любой запомненный gap. */
static void MXR_IRAM_ATTR
mxr_bins_after_alloc(int region_index, uint32_t off, uint32_t len)
{
    uint32_t aend = off + len;
    for (uint8_t b = 0; b < (uint8_t)MXR_BIN_COUNT; b++)
    {
        uint32_t hlen = s_region[region_index].hint_len[b];
        if (hlen == 0)
            continue;
        uint32_t hoff = s_region[region_index].hint_off[b];
        uint32_t hend = hoff + hlen;
        if (off >= hend || aend <= hoff)
            continue;
        uint32_t left_len = (off > hoff) ? (off - hoff) : 0;
        uint32_t right_len = (aend < hend) ? (hend - aend) : 0;
        if (left_len >= right_len)
        {
            s_region[region_index].hint_off[b] = hoff;
            s_region[region_index].hint_len[b] = left_len;
        }
        else
        {
            s_region[region_index].hint_off[b] = aend;
            s_region[region_index].hint_len[b] = right_len;
        }
    }
}

/* merged gap (от free/realloc-shrink) -> в слот его класса (LRU). */
static inline void MXR_IRAM_INLINE_ATTR
mxr_bins_store(int region_index, uint32_t off, uint32_t merged)
{
    if (merged == 0 || merged >= (uint32_t)s_region[region_index].total_bytes)
        return; /* пустой регион не запоминаем (см. шапку) */
    uint8_t b = mxr_bin_of(merged);
    s_region[region_index].hint_off[b] = off;
    s_region[region_index].hint_len[b] = merged;
}

#define MXR_REGION_HINT_RESET(r)                                        \
    do                                                                  \
    {                                                                   \
        for (uint8_t b_ = 0; b_ < (uint8_t)MXR_BIN_COUNT; b_++)         \
        {                                                               \
            (r)->hint_off[b_] = 0;                                      \
            (r)->hint_len[b_] = 0;                                      \
        }                                                               \
    } while (0)

#elif MXR_QUICK_FIT_HINT_ACTIVE

static inline void MXR_IRAM_INLINE_ATTR
mxr_region_hint_put(int region_index, uint32_t off, uint32_t len)
{
    /* пустой регион не запоминаем: скан пустой таблицы тривиален */
    if (len >= (uint32_t)s_region[region_index].total_bytes)
    {
        s_region[region_index].hint_off = 0;
        s_region[region_index].hint_len = 0;
        return;
    }
    s_region[region_index].hint_off = off;
    s_region[region_index].hint_len = len;
}

/* Попытка O(1) аллокации из единственной подсказки. */
static bool MXR_IRAM_ATTR
mxr_hint_try_alloc(int region_index, uint32_t bytes,
                   uint32_t *out_off, uint32_t *out_alloc_bytes)
{
    uint32_t gl = s_region[region_index].hint_len;
    if (gl < bytes)
        return false;
#if MXR_EARLY_EXIT_ACTIVE
    uint32_t waste_limit = bytes >> MXR_BEST_FIT_WASTE_SHIFT;
    if (waste_limit < MXR_ALIGN_SIZE)
        waste_limit = MXR_ALIGN_SIZE;
#else
    uint32_t waste_limit = 0;
#endif
    uint32_t use = bytes;
    if (mxr_gap_acceptable(gl, bytes, waste_limit,
                           (uint32_t)s_region[region_index].max_bytes, &use))
    {
        *out_off = s_region[region_index].hint_off;
        *out_alloc_bytes = use;
        s_stats.quick_fit_hint_hits++;
        return true;
    }
    s_stats.quick_fit_hint_misses++;
    return false;
}

/* Вырезает только что выделенный блок из подсказки (если он её
 * пересекает): оставляет большую из двух оставшихся частей gap'а. */
static void MXR_IRAM_ATTR
mxr_hint_after_alloc_one(int region_index, uint32_t off, uint32_t len)
{
    if (len == 0)
        return;
    uint32_t hlen = s_region[region_index].hint_len;
    if (hlen == 0)
        return;
    uint32_t hoff = s_region[region_index].hint_off;
    uint32_t hend = hoff + hlen;
    uint32_t aend = off + len;
    if (off >= hend || aend <= hoff)
        return;
    uint32_t left_len = (off > hoff) ? (off - hoff) : 0;
    uint32_t right_len = (aend < hend) ? (hend - aend) : 0;
    if (left_len >= right_len)
        mxr_region_hint_put(region_index, hoff, left_len);
    else
        mxr_region_hint_put(region_index, aend, right_len);
}

#define MXR_REGION_HINT_RESET(r)                \
    do                                          \
    {                                           \
        (r)->hint_off = 0;                      \
        (r)->hint_len = 0;                      \
    } while (0)

#else /* !MXR_QUICK_FIT_HINT_ACTIVE */

#define MXR_REGION_HINT_RESET(r) ((void)0)

#endif /* MXR_BINNING_ACTIVE / MXR_QUICK_FIT_HINT_ACTIVE */

/* ---- Унифицированный dispatch для вызывающего кода ---- */
#if MXR_BINNING_ACTIVE

static inline void MXR_IRAM_INLINE_ATTR
mxr_region_hint_after_alloc(int region_index, uint32_t off, uint32_t len)
{
    if (region_index < 0 || region_index >= s_region_count || len == 0)
        return;
    mxr_bins_after_alloc(region_index, off, len);
}

/* Вычисляет gap освобождённой памяти по живым дескрипторам и
 * складывает в bins. left_bound != 0 -> вариант shrink (gap от нового
 * конца блока). */
static void MXR_IRAM_ATTR
mxr_region_store_released_gap(int region_index, int left_idx, int right_idx, uint32_t left_bound)
{
    if (region_index < 0 || region_index >= s_region_count)
        return;

    uint32_t region_start = s_region[region_index].start_byte;
    uint32_t region_end =
        region_start + (uint32_t)s_region[region_index].total_bytes;
    (void)region_start;

    uint32_t left = (left_bound != 0) ? left_bound : region_start;
    if (left_bound == 0 && left_idx >= 0)
    {
        uint32_t e = mxr_desc_off(&s_dram_desc[left_idx]) +
                     mxr_desc_len(&s_dram_desc[left_idx]);
        if (e > left)
            left = e;
    }

    uint32_t right = region_end;
    if (right_idx < (int)s_dram_desc_count)
    {
        uint32_t o = mxr_desc_off(&s_dram_desc[right_idx]);
        if (o < right)
            right = o;
    }

    uint32_t merged = (right > left) ? (right - left) : 0;
    mxr_bins_store(region_index, left, merged);
}

static void MXR_IRAM_ATTR
mxr_region_hint_after_free(int region_index, int left_idx, int right_idx)
{
    mxr_region_store_released_gap(region_index, left_idx, right_idx, 0);
}

static void MXR_IRAM_ATTR
mxr_region_hint_after_free_tail(int region_index, int right_idx, uint32_t tail_off)
{
    mxr_region_store_released_gap(region_index, -1, right_idx, tail_off);
}

#elif MXR_QUICK_FIT_HINT_ACTIVE

static inline void MXR_IRAM_INLINE_ATTR
mxr_region_hint_after_alloc(int region_index, uint32_t off, uint32_t len)
{
    if (region_index < 0 || region_index >= s_region_count)
        return;
    mxr_hint_after_alloc_one(region_index, off, len);
}

/* Полный free (после desc_remove по index): left = index-1, right = index */
static void MXR_IRAM_ATTR
mxr_region_hint_after_free(int region_index, int left_idx, int right_idx)
{
    if (region_index < 0 || region_index >= s_region_count)
        return;

    uint32_t region_start = s_region[region_index].start_byte;
    uint32_t region_end =
        region_start + (uint32_t)s_region[region_index].total_bytes;

    uint32_t left = region_start;
    if (left_idx >= 0)
    {
        uint32_t e = mxr_desc_off(&s_dram_desc[left_idx]) +
                     mxr_desc_len(&s_dram_desc[left_idx]);
        if (e > left)
            left = e;
    }

    uint32_t right = region_end;
    if (right_idx < (int)s_dram_desc_count)
    {
        uint32_t o = mxr_desc_off(&s_dram_desc[right_idx]);
        if (o < right)
            right = o;
    }

    uint32_t merged = (right > left) ? (right - left) : 0;
    mxr_region_hint_put(region_index, left, merged);
}

/* Shrink в realloc: блок остаётся; gap хвоста от tail_off до правого соседа */
static void MXR_IRAM_ATTR
mxr_region_hint_after_free_tail(int region_index, int right_idx, uint32_t tail_off)
{
    if (region_index < 0 || region_index >= s_region_count)
        return;

    uint32_t region_end = s_region[region_index].start_byte +
                          (uint32_t)s_region[region_index].total_bytes;
    uint32_t left = tail_off;
    if (left < s_region[region_index].start_byte)
        left = s_region[region_index].start_byte;

    uint32_t right = region_end;
    if (right_idx < (int)s_dram_desc_count)
    {
        uint32_t o = mxr_desc_off(&s_dram_desc[right_idx]);
        if (o < right)
            right = o;
    }

    uint32_t merged = (right > left) ? (right - left) : 0;
    mxr_region_hint_put(region_index, left, merged);
}

#else /* !MXR_QUICK_FIT_HINT_ACTIVE && !MXR_BINNING_ACTIVE */

static inline void MXR_IRAM_INLINE_ATTR
mxr_region_hint_after_alloc(int region_index, uint32_t off, uint32_t len)
{
    (void)region_index;
    (void)off;
    (void)len;
}

static inline void MXR_IRAM_INLINE_ATTR
mxr_region_hint_after_free(int region_index, int left_idx, int right_idx)
{
    (void)region_index;
    (void)left_idx;
    (void)right_idx;
}

static inline void MXR_IRAM_INLINE_ATTR
mxr_region_hint_after_free_tail(int region_index, int right_idx, uint32_t tail_off)
{
    (void)region_index;
    (void)right_idx;
    (void)tail_off;
}

#endif /* dispatch */

/* ================================================================
 *  Canary helpers (CONFIG_MXR_CANARY)
 * ================================================================ */
#if MXR_CANARY_ACTIVE

static inline void MXR_IRAM_INLINE_ATTR
mxr_canary_write_block(void *block_base, uint32_t len_bytes)
{
    uint32_t *p = (uint32_t *)block_base;
    p[0] = MXR_CANARY_HEAD_MAGIC;
    p[(len_bytes >> 2) - 1] = MXR_CANARY_TAIL_MAGIC;
}

static inline bool MXR_IRAM_INLINE_ATTR
mxr_canary_ok_block(const void *block_base, uint32_t len_bytes)
{
    const uint32_t *p = (const uint32_t *)block_base;
    return p[0] == MXR_CANARY_HEAD_MAGIC &&
           p[(len_bytes >> 2) - 1] == MXR_CANARY_TAIL_MAGIC;
}

static void MXR_IRAM_ATTR
mxr_canary_report(uint32_t off_bytes, uint32_t len_bytes, bool is_iram)
{
    (void)off_bytes;
    (void)len_bytes;
    (void)is_iram;
    /* Cache-off safe: report later through mxr_get_status()/mxr_dump(). */
    s_stats.canary_violations++;
}

#else /* !MXR_CANARY_ACTIVE */

static inline void MXR_IRAM_INLINE_ATTR
mxr_canary_write_block(void *block_base, uint32_t len_bytes)
{
    (void)block_base;
    (void)len_bytes;
}

static inline bool MXR_IRAM_INLINE_ATTR
mxr_canary_ok_block(const void *block_base, uint32_t len_bytes)
{
    (void)block_base;
    (void)len_bytes;
    return true;
}

#endif /* MXR_CANARY_ACTIVE */

/* ================================================================
 *  Double-free кольцо (CONFIG_MXR_DOUBLE_FREE_DETECT)
 * ================================================================ */
#if MXR_DFD_ACTIVE

static inline void MXR_IRAM_INLINE_ATTR
mxr_dfd_push(uint32_t off_bytes, uint32_t len_bytes, uint8_t arena)
{
    uint32_t len24 = (len_bytes > 0xFFFFFFu) ? 0xFFFFFFu : len_bytes;
    s_dfd_ring[s_dfd_pos].off = off_bytes;
    s_dfd_ring[s_dfd_pos].len_arena = (len24 & 0x00FFFFFFu) | ((uint32_t)arena << 24);
    s_dfd_pos = (uint16_t)((s_dfd_pos + 1) % (uint16_t)MXR_DFD_RING_SIZE);
    if (s_dfd_count < (uint16_t)MXR_DFD_RING_SIZE)
        s_dfd_count++;
}

static bool MXR_IRAM_ATTR
mxr_dfd_contains(uint32_t off_bytes, uint8_t arena)
{
    for (uint16_t i = 0; i < s_dfd_count; i++)
    {
        if (s_dfd_ring[i].off == off_bytes && ((s_dfd_ring[i].len_arena >> 24) & 0xFFu) == arena)
            return true;
    }
    return false;
}

/* Обработка free без дескриптора: возвращает true, если это double-free. */
static bool MXR_IRAM_ATTR
mxr_dfd_classify(uint32_t off_bytes, uint8_t arena)
{
    if (!mxr_dfd_contains(off_bytes, arena))
        return false;
    /* No flash strings or logger calls while the allocator lock is held. */
    s_stats.double_free_detects++;
    return true;
}

#else /* !MXR_DFD_ACTIVE */

static inline void MXR_IRAM_INLINE_ATTR
mxr_dfd_push(uint32_t off_bytes, uint32_t len_bytes, uint8_t arena)
{
    (void)off_bytes;
    (void)len_bytes;
    (void)arena;
}

static inline bool MXR_IRAM_INLINE_ATTR
mxr_dfd_classify(uint32_t off_bytes, uint8_t arena)
{
    (void)off_bytes;
    (void)arena;
    return false;
}

#endif /* MXR_DFD_ACTIVE */

/* ================================================================
 *  Динамические таблицы дескрипторов (CONFIG_MXR_DESC_DYNAMIC)
 *
 *  Таблица занимает хвост арены: [s_arena_total_bytes, raw_end).
 *  Рост: дно таблицы двигается ВНИЗ на CHUNK*8, если хвост свободен
 *  (записей — memmove, редко; амортизированно O(1)).
 *  Сжатие: при запасе >= 2*CHUNK пустых слотов (гистерезис),
 *  до дна не ниже MXR_DESC_INIT. Учёт: хвост принадлежит последнему
 *  региону — его total/free и глобальные счётчики двигаются согласованно.
 * ================================================================ */
#if MXR_DESC_DYNAMIC_ACTIVE
static bool MXR_IRAM_ATTR mxr_dram_table_can_grow_for(uint32_t off_bytes, uint32_t len_bytes)
{
    const uint32_t delta =
        (uint32_t)MXR_DESC_CHUNK * (uint32_t)sizeof(mxr_desc_t);

    if ((uint32_t)s_dram_desc_cap + (uint32_t)MXR_DESC_CHUNK >
        (uint32_t)CONFIG_MXR_MAX_DESC)
        return false;
    if (s_region_count == 0 || s_arena_total_bytes < delta)
        return false;

    const uint32_t new_end = s_arena_total_bytes - delta;
    if (s_dram_desc_count > 0)
    {
        const mxr_desc_t *last = &s_dram_desc[s_dram_desc_count - 1];
        const uint32_t top_used = mxr_desc_off(last) + mxr_desc_len(last);
        if (top_used > new_end)
            return false;
    }
    else if ((uint32_t)s_region[s_region_count - 1].start_byte > new_end)
    {
        return false;
    }

    /* Candidate must still fit after metadata consumes delta. */
    if (off_bytes > new_end || len_bytes > new_end - off_bytes)
        return false;
    return true;
}

static bool MXR_IRAM_ATTR mxr_dram_table_grow(void)
{
    const uint32_t delta = (uint32_t)MXR_DESC_CHUNK * (uint32_t)sizeof(mxr_desc_t);

    if ((uint32_t)s_dram_desc_cap + (uint32_t)MXR_DESC_CHUNK >
        (uint32_t)CONFIG_MXR_MAX_DESC)
        return false; /* потолок ёмкости */
    if (s_region_count == 0)
        return false;

    int r = (int)s_region_count - 1;

    /* верх живых данных: последний дескриптор = максимальный конец блока */
    uint32_t top_used;
    if (s_dram_desc_count > 0)
    {
        const mxr_desc_t *last = &s_dram_desc[s_dram_desc_count - 1];
        top_used = mxr_desc_off(last) + mxr_desc_len(last);
    }
    else
    {
        top_used = s_region[r].start_byte; /* арена пуста */
    }

    uint32_t arena_end = s_arena_total_bytes;
    if ((uint32_t)s_region[r].total_bytes < delta)
        return false;
    if (arena_end < delta || top_used > arena_end - delta)
        return false; /* хвост занят пользовательскими блоками */

    uint32_t new_off = arena_end - delta;

    /* учёт: последний регион отдаёт delta из хвоста */
    s_arena_total_bytes = new_off;
    s_region[r].total_bytes -= delta;
    if ((uint32_t)s_region[r].free_bytes >= delta)
        s_region[r].free_bytes -= delta;
    else
        s_region[r].free_bytes = 0;
    if ((uint32_t)s_region[r].free_bytes < s_region[r].min_free_bytes)
        s_region[r].min_free_bytes = s_region[r].free_bytes;
    mxr_region_invalidate_cache(r);

    if (s_dram_free_bytes >= delta)
        s_dram_free_bytes -= delta;
    else
        s_dram_free_bytes = 0;
    if (s_dram_free_bytes < s_dram_min_free_bytes)
        s_dram_min_free_bytes = s_dram_free_bytes;
    if (s_stats.free_bytes >= (size_t)delta)
        s_stats.free_bytes -= (size_t)delta;
    else
        s_stats.free_bytes = 0;
    if (s_stats.free_bytes < s_stats.min_free_bytes)
        s_stats.min_free_bytes = s_stats.free_bytes;
    mxr_update_dram_fb_min_free();
    if (s_stats.total_bytes >= (size_t)delta)
        s_stats.total_bytes -= (size_t)delta;

    /* записи таблицы вниз (dst < src — прямой проход безопасен) */
    mxr_memmove4(s_arena_base + new_off, s_dram_desc,
                 (size_t)s_dram_desc_count * sizeof(mxr_desc_t));
    s_dram_desc = (mxr_desc_t *)(s_arena_base + new_off);
    s_dram_desc_cap = (uint16_t)(s_dram_desc_cap + MXR_DESC_CHUNK);
    s_stats.dram_desc_capacity = s_dram_desc_cap;

    /* съеденный хвост = «аллокация» [new_off, +delta) для gap-кэшей */
    mxr_region_hint_after_alloc(r, new_off, delta);

    s_stats.desc_growth_events++;
    return true;
}

static void MXR_IRAM_ATTR mxr_dram_table_maybe_shrink(void)
{
    if ((uint32_t)s_dram_desc_cap <= (uint32_t)MXR_DRAM_DESC_INIT)
        return;
    if ((uint32_t)s_dram_desc_cap - s_dram_desc_count < 2u * (uint32_t)MXR_DESC_CHUNK)
        return;
    if (s_region_count == 0)
        return;

    const uint32_t delta = (uint32_t)MXR_DESC_CHUNK * (uint32_t)sizeof(mxr_desc_t);
    int r = (int)s_region_count - 1;
    uint32_t new_off = s_arena_total_bytes + delta;

    /* записи таблицы вверх (dst > src — обратный проход) */
    mxr_memmove4(s_arena_base + new_off, s_dram_desc,
                 (size_t)s_dram_desc_count * sizeof(mxr_desc_t));
    s_dram_desc = (mxr_desc_t *)(s_arena_base + new_off);
    s_dram_desc_cap = (uint16_t)(s_dram_desc_cap - MXR_DESC_CHUNK);
    s_stats.dram_desc_capacity = s_dram_desc_cap;

    /* возвращаем delta последнему региону */
    s_arena_total_bytes = new_off;
    s_region[r].total_bytes += delta;
    s_region[r].free_bytes += delta;
    if ((uint32_t)s_region[r].free_bytes > (uint32_t)s_region[r].total_bytes)
        s_region[r].free_bytes = s_region[r].total_bytes;
    mxr_region_invalidate_cache(r);

    uint32_t nf = s_dram_free_bytes + delta;
    if (nf > s_arena_total_bytes)
        nf = s_arena_total_bytes;
    s_dram_free_bytes = nf;
    s_stats.total_bytes += (size_t)delta;
    s_stats.free_bytes += (size_t)delta;
    if (s_stats.free_bytes > s_stats.total_bytes)
        s_stats.free_bytes = s_stats.total_bytes;

    /* освобождённый хвост -> hint/bin (правого соседа нет) */
    mxr_region_hint_after_free(r, (int)s_dram_desc_count - 1, (int)s_dram_desc_count);

    s_stats.desc_shrink_events++;
}
#endif /* MXR_DESC_DYNAMIC_ACTIVE */

#if MXR_IRAM_DESC_DYNAMIC_ACTIVE
static bool MXR_IRAM_ATTR mxr_iram_table_can_grow_for(uint32_t off_bytes, uint32_t len_bytes)
{
    const uint32_t delta =
        (uint32_t)MXR_DESC_CHUNK * (uint32_t)sizeof(mxr_desc_t);

    if ((uint32_t)s_iram_desc_cap + (uint32_t)MXR_DESC_CHUNK >
        (uint32_t)CONFIG_MXR_IRAM_MAX_DESC)
        return false;
    if (s_iram_fb_region_count == 0 || s_iram_total_bytes < delta)
        return false;

    const uint32_t new_end = s_iram_total_bytes - delta;
    if (s_iram_desc_count > 0)
    {
        const mxr_desc_t *last = &s_iram_desc[s_iram_desc_count - 1];
        const uint32_t top_used = mxr_desc_off(last) + mxr_desc_len(last);
        if (top_used > new_end)
            return false;
    }
    else if ((uint32_t)s_iram_fb_region[s_iram_fb_region_count - 1].start_byte > new_end)
    {
        return false;
    }

    if (off_bytes > new_end || len_bytes > new_end - off_bytes)
        return false;

    return true;
}

static bool MXR_IRAM_ATTR mxr_iram_table_grow(void)
{
    const uint32_t delta = (uint32_t)MXR_DESC_CHUNK * (uint32_t)sizeof(mxr_desc_t);

    if ((uint32_t)s_iram_desc_cap + (uint32_t)MXR_DESC_CHUNK >
        (uint32_t)CONFIG_MXR_IRAM_MAX_DESC)
        return false;
    if (s_iram_fb_region_count == 0)
        return false; /* нет fb-региона-владельца хвоста */

    int r = (int)s_iram_fb_region_count - 1;

    uint32_t top_used;
    if (s_iram_desc_count > 0)
    {
        const mxr_desc_t *last = &s_iram_desc[s_iram_desc_count - 1];
        top_used = mxr_desc_off(last) + mxr_desc_len(last);
    }
    else
    {
        top_used = s_iram_fb_region[r].start_byte;
    }

    uint32_t arena_end = s_iram_total_bytes;
    if ((uint32_t)s_iram_fb_region[r].total_bytes < delta ||
        s_iram_fb_zone_total < delta)
        return false;
    if (arena_end < delta || top_used > arena_end - delta)
        return false;

    uint32_t new_off = arena_end - delta;

    s_iram_total_bytes = new_off;
    s_iram_fb_zone_total -= delta;
    s_iram_fb_region[r].total_bytes -= delta;
    if ((uint32_t)s_iram_fb_region[r].free_bytes >= delta)
        s_iram_fb_region[r].free_bytes -= delta;
    else
        s_iram_fb_region[r].free_bytes = 0;
    if (s_iram_fb_free_bytes >= delta)
        s_iram_fb_free_bytes -= delta;
    else
        s_iram_fb_free_bytes = 0;
    if ((uint32_t)s_iram_fb_region[r].free_bytes < s_iram_fb_region[r].min_free_bytes)
        s_iram_fb_region[r].min_free_bytes = s_iram_fb_region[r].free_bytes;
    s_iram_fb_region[r].largest_cache_valid = 0;

    if (s_iram_free_bytes >= delta)
        s_iram_free_bytes -= delta;
    else
        s_iram_free_bytes = 0;
    if (s_iram_free_bytes < s_iram_min_free_bytes)
        s_iram_min_free_bytes = s_iram_free_bytes;
    if (s_stats.free_bytes >= (size_t)delta)
        s_stats.free_bytes -= (size_t)delta;
    else
        s_stats.free_bytes = 0;
    if (s_stats.free_bytes < s_stats.min_free_bytes)
        s_stats.min_free_bytes = s_stats.free_bytes;
    mxr_update_dram_fb_min_free();
    if (s_stats.total_bytes >= (size_t)delta)
        s_stats.total_bytes -= (size_t)delta;

    mxr_memmove4(s_iram_base + new_off, s_iram_desc,
                 (size_t)s_iram_desc_count * sizeof(mxr_desc_t));
    s_iram_desc = (mxr_desc_t *)(s_iram_base + new_off);
    s_iram_desc_cap = (uint16_t)(s_iram_desc_cap + MXR_DESC_CHUNK);
    s_stats.iram_desc_capacity = s_iram_desc_cap;

    s_stats.desc_growth_events++;
    return true;
}

static void MXR_IRAM_ATTR mxr_iram_table_maybe_shrink(void)
{
    if ((uint32_t)s_iram_desc_cap <= (uint32_t)MXR_IRAM_DESC_INIT)
        return;
    if ((uint32_t)s_iram_desc_cap - s_iram_desc_count < 2u * (uint32_t)MXR_DESC_CHUNK)
        return;
    if (s_iram_fb_region_count == 0)
        return;

    const uint32_t delta = (uint32_t)MXR_DESC_CHUNK * (uint32_t)sizeof(mxr_desc_t);
    int r = (int)s_iram_fb_region_count - 1;
    uint32_t new_off = s_iram_total_bytes + delta;

    /* записи таблицы вверх (dst > src — обратный проход) */
    mxr_memmove4(s_iram_base + new_off, s_iram_desc,
                 (size_t)s_iram_desc_count * sizeof(mxr_desc_t));
    s_iram_desc = (mxr_desc_t *)(s_iram_base + new_off);
    s_iram_desc_cap = (uint16_t)(s_iram_desc_cap - MXR_DESC_CHUNK);
    s_stats.iram_desc_capacity = s_iram_desc_cap;

    s_iram_total_bytes = new_off;
    s_iram_fb_zone_total += delta;
    s_iram_fb_region[r].total_bytes += delta;
    s_iram_fb_region[r].free_bytes += delta;
    if ((uint32_t)s_iram_fb_region[r].free_bytes > (uint32_t)s_iram_fb_region[r].total_bytes)
        s_iram_fb_region[r].free_bytes = s_iram_fb_region[r].total_bytes;
    s_iram_fb_region[r].largest_cache_valid = 0;

    uint32_t nf = s_iram_free_bytes + delta;
    if (nf > s_iram_total_bytes)
        nf = s_iram_total_bytes;
    s_iram_free_bytes = nf;
    s_stats.total_bytes += (size_t)delta;
    s_stats.free_bytes += (size_t)delta;
    if (s_stats.free_bytes > s_stats.total_bytes)
        s_stats.free_bytes = s_stats.total_bytes;

    s_stats.desc_shrink_events++;
}
#endif /* MXR_IRAM_DESC_DYNAMIC_ACTIVE */

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
    uint16_t start_idx = 0;
#if MXR_DESC_BINARY_SEARCH_ACTIVE
    start_idx = mxr_dram_desc_first_after(region_start);
#endif
#if MXR_EARLY_EXIT_ACTIVE
    uint32_t waste_limit = bytes >> MXR_BEST_FIT_WASTE_SHIFT;
    if (waste_limit < MXR_ALIGN_SIZE)
        waste_limit = MXR_ALIGN_SIZE;
#else
    /* Строгий best-fit: ранний выход только при точном совпадении
     * (waste == 0) — лучше найти невозможно. */
    uint32_t waste_limit = 0;
#endif

    for (uint16_t i = start_idx; i < s_dram_desc_count; i++)
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
                if ((waste <= waste_limit || mxr_bgg_relaxed(gap, bytes, waste_limit)))
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
    int region_index, uint32_t bytes, uint32_t *out_off,
    uint32_t *out_largest, uint32_t *out_alloc_bytes,
    bool *out_largest_exact)
{
    return mxr_find_best_free(region_index, bytes, out_off, out_alloc_bytes,
                              out_largest, out_largest_exact);
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

#if MXR_BINNING_ACTIVE
    /* Binning O(q): слоты от класса запроса и выше */
    if (mxr_bins_try_alloc(region_index, bytes, out_off, out_alloc_bytes))
        return true;
#elif MXR_QUICK_FIT_HINT_ACTIVE
    /* Quick-fit O(1): единственный LRU gap региона */
    if (mxr_hint_try_alloc(region_index, bytes, out_off, out_alloc_bytes))
        return true;
#endif

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
 *  более высокому адресу (дальше от EXEC-зоны). В dynamic IRAM mode
 *  allocation from the physical tail is placed at the LOWER edge of that
 *  gap so the tail remains available for future descriptor-table growth.
 *  Non-tail gaps retain the normal upper-edge placement policy.
 * ================================================================ */
static bool MXR_IRAM_ATTR mxr_iram_fb_find_free_in_region(
    int reg,
    uint32_t bytes,
    uint32_t *out_off,
    uint32_t *out_alloc_bytes,
    uint32_t *out_largest,
    bool *out_largest_exact)
{
    *out_largest = 0;
    *out_largest_exact = false;
    if (reg < 0 || reg >= (int)s_iram_fb_region_count)
        return false;
    uint32_t reg_start = s_iram_fb_region[reg].start_byte;
    uint32_t reg_end = mxr_iram_fb_region_end(reg);
    uint32_t max_allowed = s_iram_fb_region[reg].max_bytes;
    /* Limit the descriptor size AFTER anti-sliver, including canaries. */
    if (CONFIG_MXR_IRAM_FALLBACK_MAX_BYTES != 0 &&
        (max_allowed == MXR_REGION_MAX_UNLIMITED ||
         max_allowed > (uint32_t)CONFIG_MXR_IRAM_FALLBACK_MAX_BYTES))
        max_allowed = (uint32_t)CONFIG_MXR_IRAM_FALLBACK_MAX_BYTES;
    if (s_iram_fb_region[reg].free_bytes < bytes)
        return false;

    uint32_t largest = 0;
    uint32_t cur = reg_start;
    uint32_t best_off = 0;
    uint32_t best_gap = UINT32_MAX;
    bool found = false;
    bool preserve_tail_for_growth = false;
#if MXR_IRAM_DESC_DYNAMIC_ACTIVE
    if (reg == (int)s_iram_fb_region_count - 1 &&
        mxr_iram_table_can_grow_for(0, 0))
        preserve_tail_for_growth = true;
#endif
    uint16_t start_idx = 0;
#if MXR_DESC_BINARY_SEARCH_ACTIVE
    start_idx = mxr_iram_desc_first_after(reg_start);
#endif
#if MXR_EARLY_EXIT_ACTIVE
    uint32_t waste_limit = bytes >> MXR_BEST_FIT_WASTE_SHIFT;
    if (waste_limit < MXR_ALIGN_SIZE)
        waste_limit = MXR_ALIGN_SIZE;
#else
    uint32_t waste_limit = 0;
#endif

    for (uint16_t i = start_idx; i < s_iram_desc_count; i++)
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
            if (gap > largest) largest = gap;
            if (gap >= bytes)
            {
                uint32_t waste = gap - bytes;
                if ((waste <= waste_limit || mxr_bgg_relaxed(gap, bytes, waste_limit)))
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
                    *out_largest = largest;
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
        if (gap > largest) largest = gap;
        if (gap >= bytes)
        {
            uint32_t waste = gap - bytes;
            if (waste <= waste_limit)
            {
                if (preserve_tail_for_growth)
                {
                    /* Keep the physical IRAM tail available for the next
                     * descriptor-table growth. */
                    *out_off = cur;
                    *out_alloc_bytes = bytes;
                }
                else if (MXR_IS_SLIVER(waste) &&
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
                *out_largest = largest;
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

    *out_largest = largest;
    *out_largest_exact = true;
    if (!found)
        return false;

    uint32_t waste = best_gap - bytes;
    if (preserve_tail_for_growth &&
        best_off + best_gap == reg_end)
    {
        /* The tail is the backing store for future descriptor growth. */
        *out_off = best_off;
        *out_alloc_bytes = bytes;
        return true;
    }
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
 *  т.к. largest всё равно нужно досчитать). В dynamic mode the physical
 *  tail is kept free for subsequent descriptor growth.
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
    /* Limit the descriptor size AFTER anti-sliver, including canaries. */
    if (CONFIG_MXR_IRAM_FALLBACK_MAX_BYTES != 0 &&
        (max_allowed == MXR_REGION_MAX_UNLIMITED ||
         max_allowed > (uint32_t)CONFIG_MXR_IRAM_FALLBACK_MAX_BYTES))
        max_allowed = (uint32_t)CONFIG_MXR_IRAM_FALLBACK_MAX_BYTES;
    if (s_iram_fb_region[reg].free_bytes < bytes)
        return false;

    uint32_t cur = reg_start;
    uint32_t largest = 0;
    uint32_t best_off = 0;
    uint32_t best_gap = UINT32_MAX;
    bool found = false;
    bool preserve_tail_for_growth = false;
#if MXR_IRAM_DESC_DYNAMIC_ACTIVE
    if (reg == (int)s_iram_fb_region_count - 1 &&
        mxr_iram_table_can_grow_for(0, 0))
        preserve_tail_for_growth = true;
#endif
    uint16_t start_idx = 0;
#if MXR_DESC_BINARY_SEARCH_ACTIVE
    start_idx = mxr_iram_desc_first_after(reg_start);
#endif
    for (uint16_t i = start_idx; i < s_iram_desc_count; i++)
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
    if (preserve_tail_for_growth &&
        best_off + best_gap == reg_end)
    {
        *out_off = best_off;
        *out_alloc_bytes = bytes;
        return true;
    }
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

/* ================================================================
 *  IRAM fallback region helpers
 * ================================================================ */
#ifdef CONFIG_MXR_IRAM_FALLBACK_ENABLED
/* fb-регионы непрерывны и отсортированы по start_byte (mxr_init_iram_fb_regions
 * + flat-fallback), поэтому — бинарный поиск вместо линейного скана. */
static int MXR_IRAM_ATTR mxr_iram_fb_region_by_off(uint32_t off_bytes)
{
    int lo = 0;
    int hi = (int)s_iram_fb_region_count;
    while (lo < hi)
    {
        int mid = (lo + hi) >> 1;
        if (s_iram_fb_region[mid].start_byte <= off_bytes)
            lo = mid + 1;
        else
            hi = mid;
    }
    int i = lo - 1;
    if (i < 0)
        return -1;
    if (off_bytes <
        s_iram_fb_region[i].start_byte + (uint32_t)s_iram_fb_region[i].total_bytes)
        return i;
    return -1;
}

static inline bool MXR_IRAM_INLINE_ATTR mxr_iram_fb_region_size_ok(int reg, uint32_t bytes)
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

/* FIX: убран избыточный fallback на last/first регион — он маскировал
 * ошибки конфигурации. Бинарный поиск первого eff_max >= bytes (классы
 * непрерывны), ниже — защитный линейный скан на случай нарушения
 * инварианта, иначе конфигурация некорректна и cross-region пусть
 * разбирается сам. */
static int MXR_IRAM_ATTR mxr_iram_fb_region_for_size(uint32_t bytes)
{
    int lo = 0;
    int hi = (int)s_iram_fb_region_count;
    while (lo < hi)
    {
        int mid = (lo + hi) >> 1;
        uint32_t eff_max = (s_iram_fb_region[mid].max_bytes == MXR_REGION_MAX_UNLIMITED)
                               ? UINT32_MAX
                               : (uint32_t)s_iram_fb_region[mid].max_bytes;
        if (eff_max < bytes)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < (int)s_iram_fb_region_count &&
        mxr_iram_fb_region_size_ok(lo, bytes))
    {
        return lo;
    }

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

static uint32_t MXR_IRAM_ATTR mxr_iram_fb_region_largest_free(int reg)
{
    if (reg < 0 || reg >= (int)s_iram_fb_region_count)
        return 0;

    if (s_iram_fb_region[reg].largest_cache_valid)
        return s_iram_fb_region[reg].largest_free_cache;
    uint32_t reg_start = s_iram_fb_region[reg].start_byte;
    uint32_t reg_end = mxr_iram_fb_region_end(reg);
    uint32_t cur = reg_start;
    uint32_t largest = 0;
    uint16_t start_idx = 0;
#if MXR_DESC_BINARY_SEARCH_ACTIVE
    start_idx = mxr_iram_desc_first_after(reg_start);
#endif
    for (uint16_t i = start_idx; i < s_iram_desc_count; i++)
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
    s_iram_fb_region[reg].largest_free_cache = largest;
    s_iram_fb_region[reg].largest_cache_valid = 1;
    return largest;
}

static inline void MXR_IRAM_INLINE_ATTR mxr_iram_fb_region_allocated(int reg, uint32_t bytes,
                                                       bool count_block)
{
    if (reg < 0 || reg >= (int)s_iram_fb_region_count)
        return;
    if (s_iram_fb_region[reg].free_bytes >= bytes)
        s_iram_fb_region[reg].free_bytes -= bytes;
    else
        s_iram_fb_region[reg].free_bytes = 0;
    if (s_iram_fb_free_bytes >= bytes)
        s_iram_fb_free_bytes -= bytes;
    else
        s_iram_fb_free_bytes = 0;
    if (s_iram_fb_region[reg].free_bytes < s_iram_fb_region[reg].min_free_bytes)
        s_iram_fb_region[reg].min_free_bytes = s_iram_fb_region[reg].free_bytes;
    if (count_block)
        s_iram_fb_region[reg].alloc_count++;
    mxr_iram_fb_region_invalidate_cache(reg);
}

static inline void MXR_IRAM_INLINE_ATTR mxr_iram_fb_region_released(int reg, uint32_t bytes,
                                                      bool count_block)
{
    if (reg < 0 || reg >= (int)s_iram_fb_region_count)
        return;
    uint32_t nf = s_iram_fb_region[reg].free_bytes + bytes;
    if (nf > s_iram_fb_region[reg].total_bytes)
        nf = s_iram_fb_region[reg].total_bytes;
    s_iram_fb_region[reg].free_bytes = nf;
    uint32_t zone_nf = s_iram_fb_free_bytes + bytes;
    if (zone_nf > s_iram_fb_zone_total)
        zone_nf = s_iram_fb_zone_total;
    s_iram_fb_free_bytes = zone_nf;
    if (count_block && s_iram_fb_region[reg].alloc_count > 0)
        s_iram_fb_region[reg].alloc_count--;
    mxr_iram_fb_region_invalidate_cache(reg);
}
#endif /* CONFIG_MXR_IRAM_FALLBACK_ENABLED (IRAM fallback region helpers) */
/* ================================================================
 *  IRAM global accounting (region-aware via offset)
 *
 *  EXEC-зона [0, reserve) и fb-зона [reserve, end) не пересекаются.
 *  - EXEC-блоки: обновляют s_iram_exec_free_bytes напрямую.
 *  - Fallback-блоки: обновляют fb_region через
 *    mxr_iram_fb_region_allocated/_released.
 * ================================================================ */
static inline void MXR_IRAM_INLINE_ATTR mxr_iram_allocated(uint32_t off_bytes,
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
    {
        mxr_iram_fb_region_allocated(mxr_iram_fb_region_by_off(off_bytes), bytes, count_block);
        mxr_update_dram_fb_min_free();
    }
#endif
}

static inline void MXR_IRAM_INLINE_ATTR mxr_iram_released(uint32_t off_bytes,
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
    {
        mxr_iram_fb_region_released(mxr_iram_fb_region_by_off(off_bytes), bytes, count_block);
        mxr_update_dram_fb_min_free();
    }
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
    int16_t parsed = mxr_parse_region_config(
        CONFIG_MXR_IRAM_FALLBACK_REGION_CONFIG,
        cfg,
        MXR_IRAM_FB_REGION_COUNT);
    if (parsed < 0)
    {
        ESP_EARLY_LOGE(TAG, "invalid IRAM fallback region config '%s'",
                       CONFIG_MXR_IRAM_FALLBACK_REGION_CONFIG);
        return false;
    }

    /* Empty config -> one flat fallback region */
    if (parsed == 0)
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
        MXR_REGION_HINT_RESET(&s_iram_fb_region[0]);
        return true;
    }

    uint8_t total = (uint8_t)parsed;

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
    for (uint8_t i = 0; i < (uint8_t)(total - 1); i++)
    {
        if (cfg[i].percent == 0)
        {
            ESP_EARLY_LOGE(TAG,
                           "IRAM fb region %u has percent 0 but is not last",
                           (unsigned)i);
            return false;
        }
    }
    /* Align boundaries to 4 bytes */
    for (uint8_t i = 0; i < total; i++)
    {
        uint32_t b = (uint32_t)mxr_align4((uint32_t)cfg[i].min_bytes);
        if (b < MXR_ALIGN_SIZE)
            b = MXR_ALIGN_SIZE;
        cfg[i].min_bytes = (mxr_class_t)b;
    }

    if ((uint32_t)cfg[0].min_bytes > MXR_ALIGN_SIZE)
    {
        ESP_EARLY_LOGE(TAG, "first IRAM fb boundary must be <= %u",
                       (unsigned)MXR_ALIGN_SIZE);
        return false;
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
        MXR_REGION_HINT_RESET(r);

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

/* Weak: приложение/host-тест может переопределить границы IRAM-арены */
#if defined(__GNUC__)
__attribute__((weak))
#endif
void mxr_iram_arena_bounds(uint8_t **start, uint8_t **end)
{
    extern char _iram_end;
#ifndef CONFIG_SOC_IRAM_SIZE
#define CONFIG_SOC_IRAM_SIZE 0xC000
#endif
    *start = (uint8_t *)(((uint32_t)&_iram_end + 3u) & ~3u);
    *end = (uint8_t *)(0x40100000 + CONFIG_SOC_IRAM_SIZE);
}

static void mxr_init_iram(void)
{
    uint8_t *start = NULL;
    uint8_t *end = NULL;
    mxr_iram_arena_bounds(&start, &end);

    s_iram_enabled = false;
    s_iram_base = NULL;
    s_iram_total_bytes = 0;
    s_iram_free_bytes = 0;
    s_iram_min_free_bytes = 0;
    s_iram_exec_allocs = 0;
    s_iram_fallback_allocs = 0;
    s_iram_fb_zone_start = 0;
    s_iram_fb_zone_total = 0;
    s_iram_fb_free_bytes = 0;
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

#if MXR_IRAM_DESC_DYNAMIC_ACTIVE
    /* Хвост IRAM-арены под таблицу дескрипторов */
    {
        uint32_t idesc_bytes =
            (uint32_t)MXR_IRAM_DESC_INIT * (uint32_t)sizeof(mxr_desc_t);
        if ((uint32_t)bytes <= idesc_bytes + 256u)
        {
            ESP_EARLY_LOGE(TAG, "IRAM arena too small for dynamic desc table, IRAM heap disabled");
            return; /* s_iram_enabled остаётся false */
        }
        bytes -= idesc_bytes;
        s_iram_desc = (mxr_desc_t *)(start + bytes);
        s_iram_desc_cap = MXR_IRAM_DESC_INIT;
    }
#endif

    if (bytes <= 512 || bytes >= 0x00010000)
        return;

#ifdef CONFIG_MXR_USE_IRAM
#if MXR_IRAM_DESC_DYNAMIC_ACTIVE
    mxr_memset4(s_iram_desc, (size_t)s_iram_desc_cap * sizeof(mxr_desc_t));
#endif
#endif

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

    s_iram_fb_free_bytes = s_iram_fb_zone_total;

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
        MXR_REGION_HINT_RESET(&s_iram_fb_region[0]);
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
        bool largest_exact = false;
        bool found = mxr_find_free_and_largest((int)i, bytes,
            &off_bytes, &largest, &alloc_bytes, &largest_exact);
        s_region[i].largest_free_cache = largest;
        s_region[i].largest_cache_valid = largest_exact;
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
        mxr_canary_write_block(mxr_off_to_ptr(off_bytes), alloc_bytes);
        mxr_region_hint_after_alloc((int)i, off_bytes, alloc_bytes);
        mxr_region_allocated((int)i, alloc_bytes);
        s_stats.cross_region_allocs++;

        return mxr_off_to_ptr(off_bytes + MXR_CANARY_HEAD_BYTES);
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
            uint32_t largest = 0;
            bool largest_exact = false;
            found = mxr_iram_fb_find_free_in_region(fb_reg, bytes,
                &off_bytes, &alloc_bytes, &largest, &largest_exact);
            s_iram_fb_region[fb_reg].largest_cache_valid = largest_exact;
            if (largest_exact)
                s_iram_fb_region[fb_reg].largest_free_cache = largest;
        }
        if (found)
        {
            if (mxr_iram_desc_insert(off_bytes, alloc_bytes, 0))
            {
                mxr_canary_write_block(mxr_iram_off_to_ptr(off_bytes), alloc_bytes);
                mxr_iram_allocated(off_bytes, alloc_bytes, false, true);
                s_iram_fallback_allocs++;
                s_stats.iram_fallback_allocs++;
                return mxr_iram_off_to_ptr(off_bytes + MXR_CANARY_HEAD_BYTES);
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
            mxr_canary_write_block(mxr_iram_off_to_ptr(off_bytes), cross_alloc_bytes);
            mxr_iram_allocated(off_bytes, cross_alloc_bytes, false, true);
            s_iram_fallback_allocs++;
            s_stats.iram_fallback_allocs++;
            s_stats.cross_region_allocs++;
            return mxr_iram_off_to_ptr(off_bytes + MXR_CANARY_HEAD_BYTES);
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
    /* MXR_CAP_PREFER_IRAM is a placement hint, not a requirement: note it
     * and strip it so region matching below never sees it. */
    bool prefer_iram = (caps & MXR_CAP_PREFER_IRAM) != 0;
    caps &= ~MXR_CAP_PREFER_IRAM;

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

    /* CANARY: блок = head(4) + payload + tail(4) на уровне дескрипторов */
    if (size > MXR_MAX_LEN_BYTES - MXR_BLOCK_OVERHEAD)
    {
        s_stats.alloc_fail_no_memory++;
        return NULL;
    }
    size += MXR_BLOCK_OVERHEAD;

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
        if ((caps & ~((uint32_t)MALLOC_CAP_EXEC | (uint32_t)MALLOC_CAP_32BIT | (uint32_t)MALLOC_CAP_INTERNAL)) != 0)
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

        mxr_canary_write_block(mxr_iram_off_to_ptr(off_bytes), bytes);

        /* ИСПРАВЛЕНО: is_exec = true */
        mxr_iram_allocated(off_bytes, bytes, true, true);
        s_iram_exec_allocs++;
        s_stats.exec_allocs++;
        return mxr_iram_off_to_ptr(off_bytes + MXR_CANARY_HEAD_BYTES);
    }
#else
    if (caps & MALLOC_CAP_EXEC)
    {
        s_stats.alloc_fail_no_memory++;
        return NULL;
    }
#endif

#if defined(CONFIG_MXR_USE_IRAM) && defined(CONFIG_MXR_IRAM_FALLBACK_ENABLED)
    /* Caller asked for IRAM first (MXR_CAP_PREFER_IRAM) — independent of the
     * global IRAM_FIRST / DRAM_FIRST order. Same eligibility rules as the
     * fallback path (no 8BIT/DMA/EXEC), DRAM below if IRAM has no room. */
    if (prefer_iram)
    {
        void *iram_ptr = mxr_try_iram_fallback(bytes, caps);
        if (iram_ptr)
        {
            s_stats.prefer_iram_hits++;
            return iram_ptr;
        }
        s_stats.prefer_iram_misses++;
    }
#else
    (void)prefer_iram;
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
                mxr_canary_write_block(mxr_off_to_ptr(off_bytes), alloc_bytes);
                mxr_region_hint_after_alloc(region, off_bytes, alloc_bytes);
                mxr_region_allocated(region, alloc_bytes);
                return mxr_off_to_ptr(off_bytes + MXR_CANARY_HEAD_BYTES);
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
        /* CANARY: user-ptr сдвинут на HEAD байт вперёд от начала блока */
        uint32_t off_bytes = mxr_ptr_to_off(ptr) - MXR_CANARY_HEAD_BYTES;
        int index = mxr_dram_desc_find_key(off_bytes);
        if (index < 0)
        {
            /* отдельная классификация double-free */
            if (mxr_dfd_classify(off_bytes, (uint8_t)MXR_ARENA_DRAM))
                return;
            s_stats.invalid_free_attempts++;
            return;
        }
        uint32_t len_bytes = mxr_desc_len(&s_dram_desc[index]);
        int region = mxr_region_by_off(off_bytes);

        /* CANARY: блок с порченными границами не освобождаем (утечка,
         * но куча остаётся консистентной — evidence сохраняется) */
#if MXR_CANARY_ACTIVE
        if (!mxr_canary_ok_block(mxr_off_to_ptr(off_bytes), len_bytes))
        {
            mxr_canary_report(off_bytes, len_bytes, false);
            return;
        }
#endif

        mxr_dram_desc_remove(index);
        if (region >= 0)
        {
            /* После remove(index): desc[index] — бывший index+1 (правый сосед) */
            mxr_region_hint_after_free(region, index - 1, index);
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
        mxr_dfd_push(off_bytes, len_bytes, (uint8_t)MXR_ARENA_DRAM);
#if MXR_DESC_DYNAMIC_ACTIVE
        /* после полного освобождения — попытка сжать таблицу (гистерезис) */
        mxr_dram_table_maybe_shrink();
#endif
        return;
    }

#ifdef CONFIG_MXR_USE_IRAM
    if (arena == MXR_ARENA_IRAM)
    {
        uint32_t off_bytes = mxr_iram_ptr_to_off(ptr) - MXR_CANARY_HEAD_BYTES;
        int index = mxr_iram_desc_find_key(off_bytes);
        if (index < 0)
        {
            if (mxr_dfd_classify(off_bytes, (uint8_t)MXR_ARENA_IRAM))
                return;
            s_stats.invalid_free_attempts++;
            return;
        }

        /* ИСПРАВЛЕНО: сохранить is_exec ДО desc_remove */
        bool is_exec = mxr_desc_is_exec(&s_iram_desc[index]);
        uint32_t len_bytes = mxr_desc_len(&s_iram_desc[index]);

#if MXR_CANARY_ACTIVE
        if (!mxr_canary_ok_block(mxr_iram_off_to_ptr(off_bytes), len_bytes))
        {
            mxr_canary_report(off_bytes, len_bytes, true);
            return;
        }
#endif

        mxr_iram_desc_remove(index);
        mxr_iram_released(off_bytes, len_bytes, is_exec, true);
        mxr_dfd_push(off_bytes, len_bytes, (uint8_t)MXR_ARENA_IRAM);
#if MXR_IRAM_DESC_DYNAMIC_ACTIVE
        mxr_iram_table_maybe_shrink();
#endif
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
    /* free(NULL) — частый no-op из libc: не входим в критическую секцию */
    if (!ptr)
        return;
    mxr_lock();
    mxr_free_locked(ptr);
    mxr_unlock();
}

void *MXR_IRAM_ATTR mxr_malloc(size_t size)
{
    return mxr_malloc_caps(size, MXR_DEFAULT_CAPS);
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
    return mxr_calloc_caps(count, size, MXR_DEFAULT_CAPS);
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
    return mxr_zalloc_caps(size, MXR_DEFAULT_CAPS);
}

/* ================================================================
 *  Realloc
 * ================================================================ */
void *MXR_IRAM_ALLOC_ATTR mxr_realloc_caps(void *ptr, size_t newsize, uint32_t caps)
{
    if (!ptr)
        return mxr_malloc_caps(newsize, caps);

    /* The IRAM preference only matters for a fresh block; in-place growth
     * and region checks below must not see the hint bit. */
    caps &= ~MXR_CAP_PREFER_IRAM;
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
    {
        mxr_lock();
        s_stats.alloc_fail_no_memory++;
        mxr_unlock();
        return NULL;
    }

    newsize = mxr_align4(newsize);
    if (newsize == 0 || newsize > MXR_MAX_LEN_BYTES - MXR_BLOCK_OVERHEAD)
    {
        mxr_lock();
        s_stats.alloc_fail_no_memory++;
        mxr_unlock();
        return NULL;
    }

    /* newsize — пользовательский размер (уходит в malloc, который сам
     * добавит overhead). new_bytes — размер БЛОКА на уровне дескрипторов
     * (payload + canary), используется для in-place сравнений. */
    uint32_t new_bytes = (uint32_t)newsize + MXR_BLOCK_OVERHEAD;
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
        uint32_t off_bytes = mxr_ptr_to_off(ptr) - MXR_CANARY_HEAD_BYTES;
        int index = mxr_dram_desc_find_key(off_bytes);
        if (index < 0)
        {
            s_stats.invalid_free_attempts++;
            mxr_unlock();
            return NULL;
        }

        uint32_t old_bytes = mxr_desc_len(&s_dram_desc[index]);
        int region = mxr_region_by_off(off_bytes);

#if MXR_CANARY_ACTIVE
        /* Перед любой модификацией блока: убедиться, что его границы целы */
        if (!mxr_canary_ok_block(mxr_off_to_ptr(off_bytes), old_bytes))
        {
            mxr_canary_report(off_bytes, old_bytes, false);
            mxr_unlock();
            return NULL; /* блок не трогаем */
        }
#endif
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
            /* CANARY: tail переезжает на новый конец блока */
            mxr_canary_write_block(mxr_off_to_ptr(off_bytes), new_bytes);
            /* Shrink освобождает хвост: gap начинается от НОВОГО конца
             * блока, правый сосед — desc[index+1] */
            mxr_region_hint_after_free_tail(region, index + 1,
                                            off_bytes + new_bytes);
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
                    /* CANARY: tail переезжает на новый конец блока */
                    mxr_canary_write_block(mxr_off_to_ptr(off_bytes), actual_new_bytes);
                    /* Grow потребляет gap после блока: вырезаем его из подсказки */
                    mxr_region_hint_after_alloc(region, block_end,
                                                actual_new_bytes - old_bytes);
                    mxr_region_allocated(region, actual_new_bytes - old_bytes);
                    mxr_unlock();
                    return ptr;
                }
            }
        }
        /* Move */
        uint32_t copy_bytes = (old_bytes < new_bytes) ? old_bytes : new_bytes;
#if MXR_CANARY_ACTIVE
        /* Копируем только payload: исходная точка — user-ptr (на +HEAD от
         * начала блока), писать можно не далее tail нового блока.
         * min(block) - OVERHEAD == min(payload) — стандартная гарантия
         * сохранности содержимого при этом выполнена ровно. */
        copy_bytes -= MXR_BLOCK_OVERHEAD;
#endif
        void *new_ptr = mxr_malloc_caps_locked(newsize, caps);
        if (!new_ptr)
        {
            mxr_unlock();
            return NULL;
        }
        /* Both payloads stay allocated. Other tasks may move descriptor tables,
         * but cannot relocate these payloads. Re-find old by pointer on free.
         * As with libc realloc, caller must exclude concurrent use/free of ptr. */
        mxr_unlock();
        mxr_memcpy4(new_ptr, ptr, (size_t)copy_bytes);
        mxr_lock();
        mxr_free_locked(ptr);
        mxr_unlock();
        return new_ptr;
    }

#ifdef CONFIG_MXR_USE_IRAM
    /* ---- IRAM realloc ---- */
    if (arena == MXR_ARENA_IRAM)
    {
        uint32_t off_bytes = mxr_iram_ptr_to_off(ptr) - MXR_CANARY_HEAD_BYTES;
        int index = mxr_iram_desc_find_key(off_bytes);
        if (index < 0)
        {
            s_stats.invalid_free_attempts++;
            mxr_unlock();
            return NULL;
        }

        bool old_exec = mxr_desc_is_exec(&s_iram_desc[index]);
        uint32_t old_bytes = mxr_desc_len(&s_iram_desc[index]);

#if MXR_CANARY_ACTIVE
        if (!mxr_canary_ok_block(mxr_iram_off_to_ptr(off_bytes), old_bytes))
        {
            mxr_canary_report(off_bytes, old_bytes, true);
            mxr_unlock();
            return NULL;
        }
#endif

        bool want_exec = (caps & MALLOC_CAP_EXEC) != 0;
        bool in_place_allowed = false;

        if (want_exec)
        {
            in_place_allowed =
                old_exec &&
                ((caps & ~((uint32_t)MALLOC_CAP_EXEC | (uint32_t)MALLOC_CAP_32BIT | (uint32_t)MALLOC_CAP_INTERNAL)) == 0);
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
                mxr_canary_write_block(mxr_iram_off_to_ptr(off_bytes), new_bytes);
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
                            if (can_expand &&
                                CONFIG_MXR_IRAM_FALLBACK_MAX_BYTES != 0 &&
                                old_bytes + gap > (uint32_t)CONFIG_MXR_IRAM_FALLBACK_MAX_BYTES)
                            {
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
                        mxr_canary_write_block(mxr_iram_off_to_ptr(off_bytes), actual_new_bytes);
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
#if MXR_CANARY_ACTIVE
        /* Копируем только payload: исходная точка — user-ptr (на +HEAD от
         * начала блока), писать можно не далее tail нового блока.
         * min(block) - OVERHEAD == min(payload) — стандартная гарантия
         * сохранности содержимого при этом выполнена ровно. */
        copy_bytes -= MXR_BLOCK_OVERHEAD;
#endif
        void *new_ptr = mxr_malloc_caps_locked(newsize, caps);
        if (!new_ptr)
        {
            mxr_unlock();
            return NULL;
        }
        /* Both payloads stay allocated. Other tasks may move descriptor tables,
         * but cannot relocate these payloads. Re-find old by pointer on free.
         * As with libc realloc, caller must exclude concurrent use/free of ptr. */
        mxr_unlock();
        mxr_memcpy4(new_ptr, ptr, (size_t)copy_bytes);
        mxr_lock();
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
    return mxr_realloc_caps(ptr, newsize, MXR_DEFAULT_CAPS);
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
    MXR_REGION_HINT_RESET(&s_region[0]);
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
    for (uint8_t i = 0; i < (uint8_t)(count - 1); i++)
    {
        if (cfg[i].percent == 0)
        {
            ESP_EARLY_LOGE(TAG,
                           "region %u has percent 0 but is not last "
                           "(only last region may have 0%%)",
                           (unsigned)i);
            return false;
        }
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
        MXR_REGION_HINT_RESET(&s_region[s_region_count]);

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
static int16_t mxr_parse_region_config(
    const char *s,
    mxr_region_cfg_t *out,
    uint8_t max_count)
{
    const char *p = s;
    uint8_t count = 0;

    if (!p)
        return 0;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '\0')
        return 0; /* empty config intentionally means one flat region */
    if (!out || max_count == 0)
        return -1;

    for (;;)
    {
        if (count >= max_count)
            return -1;

        uint32_t min_b = 0;
        bool has_digit = false;
        while (*p >= '0' && *p <= '9')
        {
            uint32_t digit = (uint32_t)(*p - '0');
            if (min_b > (0x7FFFFFFFu - digit) / 10u)
                return -1;
            min_b = min_b * 10u + digit;
            has_digit = true;
            p++;
        }
        if (!has_digit || *p != '-')
            return -1;
        p++;

#if defined(CONFIG_MXR_COMPACT_TYPES) && !MXR_STATE_PLACED_IN_IRAM
        /* The stored value is the 4-byte-aligned boundary. A textual
         * boundary of 65534 or 65535 becomes 65536 and would wrap in the
         * compact uint16_t representation. */
        uint32_t aligned_min_b = (uint32_t)mxr_align4((size_t)min_b);
        if (aligned_min_b < MXR_ALIGN_SIZE)
            aligned_min_b = MXR_ALIGN_SIZE;
        if (aligned_min_b > 0xFFFFu)
        {
            ESP_EARLY_LOGE(TAG,
                           "aligned boundary %u exceeds compact max 65535",
                           (unsigned)aligned_min_b);
            return -1;
        }
#endif

        uint32_t pct = 0;
        has_digit = false;
        while (*p >= '0' && *p <= '9')
        {
            uint32_t digit = (uint32_t)(*p - '0');
            if (pct > (100u - digit) / 10u)
                return -1;
            pct = pct * 10u + digit;
            has_digit = true;
            p++;
        }
        if (!has_digit || *p != '%')
            return -1;
        p++;

        out[count].min_bytes = (mxr_class_t)min_b;
        out[count].percent = (uint8_t)pct;
        out[count].max_bytes = MXR_REGION_MAX_UNLIMITED;
        count++;

        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            return (int16_t)count;
        if (*p != ',')
            return -1;
        p++;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0' || *p == ',')
            return -1;
    }
}

static bool mxr_init_regions_kconfig(void)
{
    mxr_region_cfg_t cfg[MXR_ACTIVE_TOTAL_REGIONS];
    int16_t parsed = mxr_parse_region_config(
        CONFIG_MXR_REGION_CONFIG, cfg, MXR_ACTIVE_TOTAL_REGIONS);

    if (parsed < 0)
    {
        ESP_EARLY_LOGE(TAG, "invalid DRAM region config '%s'",
                       CONFIG_MXR_REGION_CONFIG);
        return false;
    }
    if (parsed == 0)
    {
        mxr_init_regions_temp_single();
        return true; /* empty config intentionally means one flat region */
    }

    uint8_t total = (uint8_t)parsed;
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

    if ((uint32_t)cfg[0].min_bytes > MXR_ALIGN_SIZE)
    {
        ESP_EARLY_LOGE(TAG, "first DRAM region boundary must be <= %u",
                       (unsigned)MXR_ALIGN_SIZE);
        return false;
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
/* Weak: приложение/host-тест может переопределить границы DRAM-арены */
#if defined(__GNUC__)
__attribute__((weak))
#endif
void mxr_dram_arena_bounds(uint8_t **start, uint8_t **end)
{
    extern char _bss_end;
    *start = (uint8_t *)(((uint32_t)&_bss_end + 3u) & ~3u);
    *end = (uint8_t *)0x40000000;
}

void mxr_init(void)
{
    if (s_initialized)
        return;

    uint8_t *start = NULL;
    uint8_t *end = NULL;
    mxr_dram_arena_bounds(&start, &end);

    s_initialized = false;

    if (end <= start)
    {
        ESP_EARLY_LOGE(TAG, "invalid heap bounds");
        return;
    }

    size_t bytes = (size_t)(end - start);
    bytes &= ~(size_t)MXR_ALIGN_MASK;
    if (bytes == 0)
    {
        ESP_EARLY_LOGE(TAG, "DRAM arena size is 0 bytes, heap cannot be initialized");
        return;
    }
    if (bytes > MXR_MAX_ARENA_BYTES)
    {
        ESP_EARLY_LOGE(TAG, "arena too large: %u bytes", (unsigned)bytes);
        return;
    }

#if MXR_DESC_DYNAMIC_ACTIVE
    /* Эпюр арены: [start .. s_arena_total_bytes .. raw_end), таблица
     * дескрипторов занимает хвост raw арены размером INIT*8 байт. */
    {
        uint32_t desc_bytes_init =
            (uint32_t)MXR_DRAM_DESC_INIT * (uint32_t)sizeof(mxr_desc_t);
        if ((uint32_t)bytes <= desc_bytes_init + 512u)
        {
            ESP_EARLY_LOGE(TAG, "heap arena too small for dynamic desc table");
            return;
        }
        bytes -= desc_bytes_init;
        s_dram_desc = (mxr_desc_t *)(start + bytes);
        s_dram_desc_cap = MXR_DRAM_DESC_INIT;
    }
#endif

    s_arena_base = start;
    s_arena_total_bytes = (uint32_t)bytes;
    s_dram_free_bytes = (uint32_t)bytes;
    s_dram_min_free_bytes = (uint32_t)bytes;

    /* ИСПРАВЛЕНО: memset -> mxr_memset4 для данных в IRAM */
    mxr_memset4(s_dram_desc, (size_t)s_dram_desc_cap * sizeof(mxr_desc_t));
    s_dram_desc_count = 0;

#ifdef CONFIG_MXR_USE_IRAM
#if !MXR_IRAM_DESC_DYNAMIC_ACTIVE
    mxr_memset4(s_iram_desc, (size_t)s_iram_desc_cap * sizeof(mxr_desc_t));
#endif
    s_iram_desc_count = 0;
#endif

    mxr_memset4(&s_stats, sizeof(s_stats));
    s_stats.dram_desc_capacity = s_dram_desc_cap;
#ifdef CONFIG_MXR_USE_IRAM
    s_stats.iram_desc_capacity = s_iram_desc_cap;
#else
    s_stats.iram_desc_capacity = 0;
#endif

#if MXR_DFD_ACTIVE
    s_dfd_pos = 0;
    s_dfd_count = 0;
#endif

#ifdef CONFIG_MXR_USE_IRAM
    mxr_init_iram();
    /* Dynamic IRAM descriptor capacity is known only after mxr_init_iram(),
     * so publish the effective capacity now rather than leaving status at 0
     * until the first table growth. */
    s_stats.iram_desc_capacity = s_iram_desc_cap;
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
    }
#endif

    s_stats.initialized = true;
    s_stats.region_count = s_region_count;
    s_stats.total_bytes = total_bytes;
    s_stats.free_bytes = free_bytes;
    s_stats.min_free_bytes = total_bytes;
    s_dram_fb_min_free_bytes = s_dram_free_bytes +
#ifdef CONFIG_MXR_USE_IRAM
        (s_iram_enabled ? s_iram_fb_free_bytes : 0u);
#else
        0u;
#endif
    s_stats.largest_free_block_bytes = mxr_get_largest_user_block_locked();
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
static void mxr_collect_status_locked(mxr_status_t *status, bool detailed)
{
    if (!status)
        return;

    s_stats.dram_active_allocs = s_dram_desc_count;
    s_stats.iram_active_allocs = 0;
    s_stats.region_count = s_region_count;
    s_stats.iram_fb_region_count = 0;

    size_t total_bytes = 0;
    size_t free_bytes = 0;

    uint32_t total_gap_bytes = 0, gap_count = 0, sliver_count = 0;
    uint32_t dram_largest = 0;
    uint16_t cursor = 0;
    /* Regions and descriptors are address-sorted, so each descriptor is
     * visited once, not once per region. largest caches store RAW gaps. */
    for (uint8_t r = 0; r < s_region_count; ++r)
    {
        total_bytes += s_region[r].total_bytes;
        free_bytes += s_region[r].free_bytes;
        uint32_t cur = s_region[r].start_byte;
        const uint32_t end = cur + s_region[r].total_bytes;
        uint32_t raw_largest = 0;
        while (cursor < s_dram_desc_count)
        {
            const mxr_desc_t *d = &s_dram_desc[cursor];
            uint32_t off = mxr_desc_off(d);
            uint32_t block_end = off + mxr_desc_len(d);
            if (off >= end) break;
            if (off > cur)
            {
                uint32_t gap = off - cur;
                if (gap > raw_largest) raw_largest = gap;
                if (detailed)
                {
                    total_gap_bytes += gap;
                    ++gap_count;
                    if (MXR_IS_SLIVER(gap)) ++sliver_count;
                }
            }
            if (block_end > cur) cur = block_end;
            ++cursor;
        }
        if (end > cur)
        {
            uint32_t gap = end - cur;
            if (gap > raw_largest) raw_largest = gap;
            if (detailed)
            {
                total_gap_bytes += gap;
                ++gap_count;
                if (MXR_IS_SLIVER(gap)) ++sliver_count;
            }
        }
        s_region[r].largest_free_cache = raw_largest;
        s_region[r].largest_cache_valid = 1;
        if (raw_largest > dram_largest) dram_largest = raw_largest;
    }
    /* Public mxr_get_status always requests detailed=true. Minimal dump
     * has a private snapshot and does not expose these skipped metrics. */
    if (detailed)
    {
        s_stats.gap_count = gap_count;
        s_stats.sliver_count = sliver_count;
        s_stats.fragmentation_pct = total_gap_bytes > 0
            ? ((total_gap_bytes - dram_largest) * 100u) / total_gap_bytes : 0;
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
    }
#endif

    s_stats.total_bytes = total_bytes;
    s_stats.free_bytes = free_bytes;

    s_stats.largest_free_block_bytes = mxr_get_largest_user_block_locked();
    if (s_stats.free_bytes < s_stats.min_free_bytes)
        s_stats.min_free_bytes = s_stats.free_bytes;

    /* FIX(#6): было `*status = s_stats` — при MXR_STATE_PLACEMENT=IRAM
     * s_stats лежит в IRAM, и компилятор вправе сгенерировать вызов
     * memcpy с 8/16-битными доступами (по одному исключению LoadStore
     * на каждый доступ). sizeof(s_stats) утверждён кратным 4 в
     * заголовке, поэтому копируем строго пословно. */
    mxr_memcpy4(status, &s_stats, sizeof(s_stats));
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
    mxr_lock();
    size_t bytes = mxr_get_free_size_caps_locked(caps);
    mxr_unlock();
    return bytes;
}

size_t mxr_get_min_free_size_caps(uint32_t caps)
{
    if (!s_initialized)
        return 0;

    mxr_lock();
    caps &= ~MXR_CAP_PREFER_IRAM;

    /* Historical minimum: summing per-region minima is incorrect because
     * different regions can reach their individual minima at different
     * times. Return the minimum of the same aggregate pool used by
     * mxr_get_free_size_caps_locked(). */
    if (caps & MALLOC_CAP_EXEC)
    {
#ifdef CONFIG_MXR_USE_IRAM
        if (s_iram_enabled &&
            (caps & ~((uint32_t)MALLOC_CAP_EXEC | (uint32_t)MALLOC_CAP_32BIT | (uint32_t)MALLOC_CAP_INTERNAL)) == 0)
        {
            const size_t bytes = s_iram_exec_min_free_bytes;
            mxr_unlock();
            return bytes;
        }
#endif
        mxr_unlock();
        return 0;
    }

    const uint32_t dram_caps = (uint32_t)MXR_DRAM_CAPS_DEFAULT;
    const bool dram_matches = (caps & ~dram_caps) == 0;

    /* 8BIT and/or DMA force DRAM even when 32BIT/INTERNAL is also present. */
    if (dram_matches && caps != 0 &&
        (caps & (MALLOC_CAP_8BIT | MALLOC_CAP_DMA)))
    {
        const size_t bytes = s_dram_min_free_bytes;
        mxr_unlock();
        return bytes;
    }

#ifdef CONFIG_MXR_USE_IRAM
#ifdef CONFIG_MXR_IRAM_FALLBACK_ENABLED
    if (s_iram_enabled &&
        (caps & (MALLOC_CAP_32BIT | MALLOC_CAP_INTERNAL)) &&
        !(caps & (MALLOC_CAP_DMA | MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM)))
    {
        const size_t bytes = s_dram_fb_min_free_bytes;
        mxr_unlock();
        return bytes;
    }
#endif
#endif

    /* caps==0 is the non-EXEC pool exposed by the existing free-size query. */
    if (caps == 0)
    {
        const size_t bytes = s_dram_fb_min_free_bytes;
        mxr_unlock();
        return bytes;
    }

    /* DRAM-only 32BIT/INTERNAL queries when IRAM fallback is disabled. */
    if (dram_matches &&
        (caps & (MALLOC_CAP_32BIT | MALLOC_CAP_INTERNAL)) &&
        !(caps & (MALLOC_CAP_DMA | MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM)))
    {
        const size_t bytes = s_dram_min_free_bytes;
        mxr_unlock();
        return bytes;
    }

    mxr_unlock();
    return 0;
}

/* ================================================================
 *  FIX(2.3): дополнительные стандартные heap_caps query API
 * ================================================================ */
size_t mxr_get_total_size_caps(uint32_t caps)
{
    if (!s_initialized)
        return 0;
    mxr_lock();
    size_t bytes = mxr_get_total_size_caps_locked(caps);
    mxr_unlock();
    return bytes;
}

static uint32_t MXR_IRAM_ATTR
mxr_region_largest_free_bytes_bounded(uint8_t region_index, uint32_t end_cap)
{
    if (region_index >= s_region_count)
        return 0;

    const uint32_t region_start = s_region[region_index].start_byte;
    uint32_t region_end = region_start +
                          (uint32_t)s_region[region_index].total_bytes;
    if (end_cap < region_end)
        region_end = end_cap;
    if (region_end <= region_start)
        return 0;

    uint32_t cur = region_start;
    uint32_t largest = 0;
    uint16_t start_idx = 0;
#if MXR_DESC_BINARY_SEARCH_ACTIVE
    start_idx = mxr_dram_desc_first_after(region_start);
#endif
    for (uint16_t i = start_idx; i < s_dram_desc_count; ++i)
    {
        const uint32_t off = mxr_desc_off(&s_dram_desc[i]);
        const uint32_t len = mxr_desc_len(&s_dram_desc[i]);
        const uint32_t block_end = off + len;
        if (block_end <= region_start)
            continue;
        if (off >= region_end)
            break;
        if (off > cur && off - cur > largest)
            largest = off - cur;
        if (block_end > cur)
            cur = block_end;
        if (cur >= region_end)
            break;
    }
    if (region_end > cur && region_end - cur > largest)
        largest = region_end - cur;
    return largest;
}

#ifdef CONFIG_MXR_USE_IRAM
#ifdef CONFIG_MXR_IRAM_FALLBACK_ENABLED
#if MXR_IRAM_DESC_DYNAMIC_ACTIVE
static uint32_t MXR_IRAM_ATTR
mxr_iram_fb_region_largest_free_bounded(int reg, uint32_t end_cap)
{
    if (reg < 0 || reg >= (int)s_iram_fb_region_count)
        return 0;

    const uint32_t reg_start = s_iram_fb_region[reg].start_byte;
    uint32_t reg_end = mxr_iram_fb_region_end(reg);
    if (end_cap < reg_end)
        reg_end = end_cap;
    if (reg_end <= reg_start)
        return 0;

    uint32_t cur = reg_start;
    uint32_t largest = 0;
    uint16_t start_idx = 0;
#if MXR_DESC_BINARY_SEARCH_ACTIVE
    start_idx = mxr_iram_desc_first_after(reg_start);
#endif
    for (uint16_t i = start_idx; i < s_iram_desc_count; ++i)
    {
        const uint32_t off = mxr_desc_off(&s_iram_desc[i]);
        const uint32_t len = mxr_desc_len(&s_iram_desc[i]);
        const uint32_t block_end = off + len;
        if (block_end <= reg_start)
            continue;
        if (off >= reg_end)
            break;
        if (off > cur && off - cur > largest)
            largest = off - cur;
        if (block_end > cur)
            cur = block_end;
        if (cur >= reg_end)
            break;
    }
    if (reg_end > cur && reg_end - cur > largest)
        largest = reg_end - cur;
    return largest;
}
#endif
#endif
#endif

static size_t mxr_get_largest_free_block_caps_locked(uint32_t caps)
{
    uint32_t largest_user = 0;
    caps &= ~MXR_CAP_PREFER_IRAM;

    bool dram_desc_available = s_dram_desc_count < s_dram_desc_cap;
#if MXR_DESC_DYNAMIC_ACTIVE
    uint32_t dram_last_end_cap = s_arena_total_bytes;
    if (!dram_desc_available && mxr_dram_table_can_grow_for(0, 0))
    {
        dram_desc_available = true;
        dram_last_end_cap = s_arena_total_bytes -
            (uint32_t)MXR_DESC_CHUNK * (uint32_t)sizeof(mxr_desc_t);
    }
#else
    const uint32_t dram_last_end_cap = s_arena_total_bytes;
#endif

    if (dram_desc_available)
    {
        for (uint8_t i = 0; i < s_region_count; i++)
        {
            if (!mxr_region_caps_ok(i, caps))
                continue;
            uint32_t raw_gap = mxr_region_largest_free_bytes_bounded(
                i, (i == (uint8_t)(s_region_count - 1)) ? dram_last_end_cap :
                    s_region[i].start_byte + (uint32_t)s_region[i].total_bytes);
            uint32_t candidate = mxr_largest_user_block_in_gap(
                raw_gap,
                (uint32_t)s_region[i].min_bytes,
                (uint32_t)s_region[i].max_bytes,
                0);
            if (candidate > largest_user)
                largest_user = candidate;
        }

#if defined(CONFIG_MXR_CROSS_REGION_FALLBACK) && defined(CONFIG_MXR_DRAM_CROSS_ENABLED)
        /* Cross-region placement may exceed a target region's own class
         * maximum, but only as far as the configured guard permits. */
        for (uint8_t i = 0; i < s_region_count; i++)
        {
            if (!mxr_region_caps_ok(i, caps))
                continue;
            uint32_t min_internal = MXR_ALIGN_SIZE;
#ifdef MXR_DRAM_MIN_BYTES_DIVISOR
            uint32_t guard_min =
                (uint32_t)s_region[i].min_bytes / MXR_DRAM_MIN_BYTES_DIVISOR;
            if (guard_min > min_internal)
                min_internal = guard_min;
#endif
            uint32_t guard_max = 0;
#ifdef MXR_DRAM_GUARD_NUM
            if (s_region[i].max_bytes != MXR_REGION_MAX_UNLIMITED)
            {
                uint32_t max_bytes = (uint32_t)s_region[i].max_bytes;
                const uint32_t q = max_bytes / (uint32_t)MXR_DRAM_GUARD_DEN;
                const uint32_t rem = max_bytes % (uint32_t)MXR_DRAM_GUARD_DEN;
                guard_max = q * (uint32_t)MXR_DRAM_GUARD_NUM +
                    (rem * (uint32_t)MXR_DRAM_GUARD_NUM) /
                    (uint32_t)MXR_DRAM_GUARD_DEN;
                if (guard_max == 0)
                    continue;
            }
#endif
            uint32_t candidate = mxr_largest_user_block_in_gap(
                mxr_region_largest_free_bytes_bounded(
                    i, (i == (uint8_t)(s_region_count - 1)) ? dram_last_end_cap :
                        s_region[i].start_byte + (uint32_t)s_region[i].total_bytes),
                min_internal, 0, guard_max);
            if (candidate > largest_user)
                largest_user = candidate;
        }
#endif
    }

#ifdef CONFIG_MXR_USE_IRAM
    bool iram_desc_available = s_iram_desc_count < s_iram_desc_cap;
#if MXR_IRAM_DESC_DYNAMIC_ACTIVE
    uint32_t iram_last_end_cap = s_iram_total_bytes;
    if (!iram_desc_available && mxr_iram_table_can_grow_for(0, 0))
    {
        iram_desc_available = true;
        iram_last_end_cap = s_iram_total_bytes -
            (uint32_t)MXR_DESC_CHUNK * (uint32_t)sizeof(mxr_desc_t);
    }
#endif
    if (s_iram_enabled && iram_desc_available)
    {
        if (caps & MALLOC_CAP_EXEC)
        {
            if ((caps & ~((uint32_t)MALLOC_CAP_EXEC | (uint32_t)MALLOC_CAP_32BIT | (uint32_t)MALLOC_CAP_INTERNAL)) == 0)
            {
                uint32_t candidate = mxr_largest_user_block_from_internal(
                    mxr_iram_exec_largest_free());
                if (candidate > largest_user)
                    largest_user = candidate;
            }
        }
#ifdef CONFIG_MXR_IRAM_FALLBACK_ENABLED
        else if (mxr_caps_allow_iram_fallback(caps))
        {
            uint32_t policy_max = (uint32_t)CONFIG_MXR_IRAM_FALLBACK_MAX_BYTES;
            for (uint8_t i = 0; i < s_iram_fb_region_count; i++)
            {
                uint32_t fb_gap = mxr_iram_fb_region_largest_free((int)i);
#if MXR_IRAM_DESC_DYNAMIC_ACTIVE
                if (i == (uint8_t)(s_iram_fb_region_count - 1) &&
                    iram_last_end_cap < mxr_iram_fb_region_end((int)i))
                {
                    fb_gap = mxr_iram_fb_region_largest_free_bounded(
                        (int)i, iram_last_end_cap);
                }
#endif
                uint32_t candidate = mxr_largest_user_block_in_gap(
                    fb_gap,
                    (uint32_t)s_iram_fb_region[i].min_bytes,
                    (uint32_t)s_iram_fb_region[i].max_bytes,
                    policy_max);
                if (candidate > largest_user)
                    largest_user = candidate;
            }

#if defined(CONFIG_MXR_CROSS_REGION_FALLBACK) && defined(CONFIG_MXR_IRAM_CROSS_ENABLED)
            for (uint8_t i = 0; i < s_iram_fb_region_count; i++)
            {
                uint32_t min_internal = MXR_ALIGN_SIZE;
#ifdef MXR_IRAM_MIN_BYTES_DIVISOR
                uint32_t guard_min =
                    (uint32_t)s_iram_fb_region[i].min_bytes / MXR_IRAM_MIN_BYTES_DIVISOR;
                if (guard_min > min_internal)
                    min_internal = guard_min;
#endif
                uint32_t guard_max = policy_max;
#ifdef MXR_IRAM_GUARD_NUM
                if (s_iram_fb_region[i].max_bytes != MXR_REGION_MAX_UNLIMITED)
                {
                    uint32_t max_bytes = (uint32_t)s_iram_fb_region[i].max_bytes;
                    const uint32_t q = max_bytes / (uint32_t)MXR_IRAM_GUARD_DEN;
                    const uint32_t rem = max_bytes % (uint32_t)MXR_IRAM_GUARD_DEN;
                    uint32_t class_guard =
                        q * (uint32_t)MXR_IRAM_GUARD_NUM +
                        (rem * (uint32_t)MXR_IRAM_GUARD_NUM) /
                        (uint32_t)MXR_IRAM_GUARD_DEN;
                    if (class_guard == 0)
                        continue;
                    if (guard_max == 0 || class_guard < guard_max)
                        guard_max = class_guard;
                }
#endif
                uint32_t fb_gap = mxr_iram_fb_region_largest_free((int)i);
#if MXR_IRAM_DESC_DYNAMIC_ACTIVE
                if (i == (uint8_t)(s_iram_fb_region_count - 1) &&
                    iram_last_end_cap < mxr_iram_fb_region_end((int)i))
                {
                    fb_gap = mxr_iram_fb_region_largest_free_bounded(
                        (int)i, iram_last_end_cap);
                }
#endif
                uint32_t candidate = mxr_largest_user_block_in_gap(
                    fb_gap, min_internal, 0, guard_max);
                if (candidate > largest_user)
                    largest_user = candidate;
            }
#endif
        }
#endif /* CONFIG_MXR_IRAM_FALLBACK_ENABLED */
    }
#endif /* CONFIG_MXR_USE_IRAM */

    return (size_t)largest_user;
}

static size_t mxr_get_largest_user_block_locked(void)
{
    uint32_t largest = (uint32_t)mxr_get_largest_free_block_caps_locked(0);
#ifdef CONFIG_MXR_USE_IRAM
    if (s_iram_enabled && s_iram_desc_count < (uint32_t)CONFIG_MXR_IRAM_MAX_DESC)
    {
        uint32_t exec_largest = mxr_largest_user_block_from_internal(
            mxr_iram_exec_largest_free());
        if (exec_largest > largest)
            largest = exec_largest;
    }
#endif
    return (size_t)largest;
}

size_t mxr_get_largest_free_block_caps(uint32_t caps)
{
    if (!s_initialized)
        return 0;
    mxr_lock();
    size_t largest = mxr_get_largest_free_block_caps_locked(caps);
    mxr_unlock();
    return largest;
}

/* ================================================================
 *  Locked internals для total/free/allocated query
 * ================================================================ */
static size_t mxr_get_total_size_caps_locked(uint32_t caps)
{
    size_t bytes = 0;
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
            if ((caps & ~((uint32_t)MALLOC_CAP_EXEC | (uint32_t)MALLOC_CAP_32BIT | (uint32_t)MALLOC_CAP_INTERNAL)) == 0)
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
#endif
    }
#endif
    return bytes;
}

static size_t mxr_get_free_size_caps_locked(uint32_t caps)
{
    size_t bytes = 0;
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
            if ((caps & ~((uint32_t)MALLOC_CAP_EXEC | (uint32_t)MALLOC_CAP_32BIT | (uint32_t)MALLOC_CAP_INTERNAL)) == 0)
                bytes += (size_t)s_iram_exec_free_bytes;
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
            for (uint8_t i = 0; i < s_iram_fb_region_count; i++)
                bytes += (size_t)s_iram_fb_region[i].free_bytes;
        }
        else if (caps == 0)
        {
            for (uint8_t i = 0; i < s_iram_fb_region_count; i++)
                bytes += (size_t)s_iram_fb_region[i].free_bytes;
        }
#endif
    }
#endif
    return bytes;
}

size_t mxr_get_allocated_size_caps(uint32_t caps)
{
    if (!s_initialized)
        return 0;
    mxr_lock();
    size_t total = mxr_get_total_size_caps_locked(caps);
    size_t free_bytes = mxr_get_free_size_caps_locked(caps);
    mxr_unlock();
    return (total > free_bytes) ? (total - free_bytes) : 0;
}

/* ================================================================
 *  Dump
 * ================================================================ */
void mxr_dump(void)
{
    mxr_status_t st;

#if defined(CONFIG_MXR_DUMP_NORMAL) || defined(CONFIG_MXR_DUMP_FULL)
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

#endif /* detailed dump locals */

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

#if defined(CONFIG_MXR_DUMP_NORMAL) || defined(CONFIG_MXR_DUMP_FULL)
    mxr_collect_status_locked(&st, true);

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

#else
    mxr_collect_status_locked(&st, false);
#endif /* detailed snapshots */

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

    if (st.canary_violations || st.double_free_detects)
        ESP_EARLY_LOGW(TAG, "deferred heap errors: canary=%u double_free=%u",
                       (unsigned)st.canary_violations, (unsigned)st.double_free_detects);

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
                   "exec=%u iram_fb=%u prefer_iram=%u/%u cross=%u cross_skip=%u guard_rej=%u "
                   "caps_skip=%u free_skip=%u cache_skip=%u",
                   (unsigned)st.exec_allocs,
                   (unsigned)st.iram_fallback_allocs,
                   (unsigned)st.prefer_iram_hits,
                   (unsigned)st.prefer_iram_misses,
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
                       "bf_early=%u anti_sliver=%u qfit_hit=%u qfit_miss=%u",
                       (unsigned)st.fragmentation_pct,
                       (unsigned)st.gap_count,
                       (unsigned)st.sliver_count,
                       (unsigned)sliver_pct,
                       (unsigned)st.best_fit_early_exits,
                       (unsigned)st.anti_sliver_expansions,
                       (unsigned)st.quick_fit_hint_hits,
                       (unsigned)st.quick_fit_hint_misses);
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
    mxr_collect_status_locked(status, true);
    mxr_unlock();
}


