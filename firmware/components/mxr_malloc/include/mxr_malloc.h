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


  /* Запрет 64-битных операций в IRAM-коде — они генерируют
   вызовы __muldi3/__udivdi3 из flash, что недопустимо
   при отключённом flash cache */
#ifdef CONFIG_MXR_USE_IRAM
#pragma GCC poison __muldi3 __udivdi3 __umulsidi3
#endif
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