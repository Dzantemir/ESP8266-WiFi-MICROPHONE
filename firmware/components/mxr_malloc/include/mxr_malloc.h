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

#ifdef CONFIG_MXR_DESC_BINARY_SEARCH
#define MXR_DESC_BINARY_SEARCH_ACTIVE 1
#else
#define MXR_DESC_BINARY_SEARCH_ACTIVE 0
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
 *  Quick-fit hint — O(1) быстрый путь аллокации в DRAM
 *
 *  По каждому DRAM-региону хранится «подсказка» — смещение и длина
 *  одного гарантированно свободного gap'а, поддерживаемая при
 *  free/realloc за O(1). Если gap удовлетворяет запрос по тем же
 *  критериям, что и early-exit best-fit (точный fit, waste в пределах
 *  лимита или anti-sliver расширение), аллокация обходится БЕЗ
 *  сканирования таблицы дескрипторов — это самый горячий выигрыш
 *  для LIFO/повторяющихся размеров.
 *
 *  Стоимость: +8 байт на регион + O(1) логики на free/realloc.
 *  Включение: CONFIG_MXR_QUICK_FIT_HINT_ENABLE (menuconfig).
 *  При выключенном символе fast path вырезается на этапе компиляции.
 * ================================================================ */
#if !defined(CONFIG_MXR_QUICK_FIT_HINT_ENABLE)
#define MXR_QUICK_FIT_HINT_ACTIVE 0
#else
#define MXR_QUICK_FIT_HINT_ACTIVE 1
#endif

/* ================================================================
 *  v2: защита / отладка / ускорение / динамика — всё через menuconfig
 * ================================================================ */

/* SEGREGATED FIT (BINNING): несколько LRU gap'ов по логарифмическим
 * классам размера. Требует Quick-fit hint. 0 = выключено. */
#if defined(CONFIG_MXR_BINNING) && !MXR_QUICK_FIT_HINT_ACTIVE
#error "CONFIG_MXR_BINNING requires the quick-fit hint (set CONFIG_MXR_QUICK_FIT_HINT_ENABLE)"
#endif
#ifdef CONFIG_MXR_BINNING
#define MXR_BINNING_ACTIVE 1
#ifndef CONFIG_MXR_BIN_COUNT
#define CONFIG_MXR_BIN_COUNT 4 /* menuconfig default; ручная сборка без sdkconfig */
#endif
#define MXR_BIN_COUNT CONFIG_MXR_BIN_COUNT       /* 2..8 */
#else
#define MXR_BINNING_ACTIVE 0
#define MXR_BIN_COUNT 4
#endif
#if MXR_BINNING_ACTIVE && (MXR_BIN_COUNT < 2 || MXR_BIN_COUNT > 8)
#error "MXR_BIN_COUNT must be 2..8"
#endif
/* Первый порог классов: bin0 = MXR_BIN_BASE байт, bin_k = BASE*2^k */
#define MXR_BIN_BASE 32u

/* BIG-GAP GUARD («батонная» политика): запрет на преждевременное откусы-
 * вание от больших gap'ов под мелкие запросы. 0 = выключено. */
#ifdef CONFIG_MXR_BIG_GAP_GUARD
#define MXR_BGG_ACTIVE 1
#ifndef CONFIG_MXR_BIG_GAP_MIN
#define CONFIG_MXR_BIG_GAP_MIN 1024
#endif
#ifndef CONFIG_MXR_BIG_GAP_FACTOR_SHIFT
#define CONFIG_MXR_BIG_GAP_FACTOR_SHIFT 2
#endif
#define MXR_BGG_GAP_MIN CONFIG_MXR_BIG_GAP_MIN
#define MXR_BGG_FACTOR_SHIFT CONFIG_MXR_BIG_GAP_FACTOR_SHIFT
#else
#define MXR_BGG_ACTIVE 0
#define MXR_BGG_GAP_MIN 1024
#define MXR_BGG_FACTOR_SHIFT 2
#endif

/* CANARY: магические dword'ы вокруг каждого блока (подрез/перезапись). */
#ifdef CONFIG_MXR_CANARY
#define MXR_CANARY_ACTIVE 1
#else
#define MXR_CANARY_ACTIVE 0
#endif
#if MXR_CANARY_ACTIVE
#define MXR_BLOCK_OVERHEAD 8u        /* 4 head + 4 tail */
#define MXR_CANARY_HEAD_BYTES 4u     /* user-ptr сдвигается на HEAD от начала блока */
#define MXR_CANARY_TAIL_BYTES 4u
#else
#define MXR_BLOCK_OVERHEAD 0u        /* layout идентичен v1 */
#define MXR_CANARY_HEAD_BYTES 0u
#define MXR_CANARY_TAIL_BYTES 0u
#endif

/* DOUBLE-FREE DETECT: кольцо последних освобождённых блоков. */
#ifdef CONFIG_MXR_DOUBLE_FREE_DETECT
#define MXR_DFD_ACTIVE 1
#ifndef CONFIG_MXR_DFD_RING_SIZE
#define CONFIG_MXR_DFD_RING_SIZE 16
#endif
#define MXR_DFD_RING_SIZE CONFIG_MXR_DFD_RING_SIZE
#else
#define MXR_DFD_ACTIVE 0
#define MXR_DFD_RING_SIZE 16
#endif

/* DYNAMIC DESCRIPTOR TABLES: таблица(ы) вырезаются из хвоста арены
 * и растут/сжимаются чанками. Потолок = старые CONFIG_MXR_MAX_DESC /
 * CONFIG_MXR_IRAM_MAX_DESC. Пиковое резервирование не меняется,
 * зато «пустые» резервы не висят мёртвым грузом. */
#if defined(CONFIG_MXR_DESC_DYNAMIC) && (defined(CONFIG_MXR_DESC_IN_IRAM_TEXT) || \
    defined(CONFIG_MXR_DESC_IN_IRAM_BSS))
#error "CONFIG_MXR_DESC_DYNAMIC is incompatible with MXR_DESC_IN_IRAM_* (desc must live in the DRAM arena tail)"
#endif
#ifdef CONFIG_MXR_DESC_DYNAMIC
#define MXR_DESC_DYNAMIC_ACTIVE 1
#else
#define MXR_DESC_DYNAMIC_ACTIVE 0
#endif

/* FIX(#21): INIT/CHUNK общие для DRAM- и IRAM-динамики. Раньше они
 * объявлялись только под CONFIG_MXR_DESC_DYNAMIC, и отдельное включение
 * CONFIG_MXR_IRAM_DESC_DYNAMIC (допустимая по Kconfig комбинация)
 * оставляло IRAM-код с неопределёнными макросами. */
#if MXR_DESC_DYNAMIC_ACTIVE || \
    (defined(CONFIG_MXR_USE_IRAM) && defined(CONFIG_MXR_IRAM_FALLBACK_ENABLED) && \
     defined(CONFIG_MXR_IRAM_DESC_DYNAMIC))
#ifndef CONFIG_MXR_DESC_INIT
#define CONFIG_MXR_DESC_INIT 32
#endif
#ifndef CONFIG_MXR_DESC_CHUNK
#define CONFIG_MXR_DESC_CHUNK 16
#endif
#define MXR_DESC_INIT CONFIG_MXR_DESC_INIT
#define MXR_DESC_CHUNK CONFIG_MXR_DESC_CHUNK
#endif
#if MXR_DESC_DYNAMIC_ACTIVE
#define MXR_DRAM_DESC_INIT ((MXR_DESC_INIT < CONFIG_MXR_MAX_DESC) ? MXR_DESC_INIT : CONFIG_MXR_MAX_DESC)
#endif

#if defined(CONFIG_MXR_USE_IRAM) && defined(CONFIG_MXR_IRAM_FALLBACK_ENABLED) && \
    defined(CONFIG_MXR_IRAM_DESC_DYNAMIC)
#define MXR_IRAM_DESC_DYNAMIC_ACTIVE 1
#else
#define MXR_IRAM_DESC_DYNAMIC_ACTIVE 0
#endif
#if MXR_IRAM_DESC_DYNAMIC_ACTIVE && !defined(CONFIG_MXR_USE_IRAM)
#error "MXR_IRAM_DESC_DYNAMIC requires CONFIG_MXR_USE_IRAM"
#endif
#if MXR_IRAM_DESC_DYNAMIC_ACTIVE && !defined(CONFIG_MXR_IRAM_FALLBACK_ENABLED)
#error "MXR_IRAM_DESC_DYNAMIC requires CONFIG_MXR_IRAM_FALLBACK_ENABLED (fb region owns the arena tail)"
#endif
#if MXR_IRAM_DESC_DYNAMIC_ACTIVE
#define MXR_IRAM_DESC_INIT ((MXR_DESC_INIT < CONFIG_MXR_IRAM_MAX_DESC) ? MXR_DESC_INIT : CONFIG_MXR_IRAM_MAX_DESC)
#endif
#if MXR_DESC_DYNAMIC_ACTIVE || MXR_IRAM_DESC_DYNAMIC_ACTIVE
#if MXR_DESC_INIT < 1 || MXR_DESC_CHUNK < 1 || MXR_DESC_CHUNK > 4096
#error "Dynamic descriptors require INIT >= 1 and CHUNK in 1..4096"
#endif
#endif

/* FIX(#19): отрицательный Kconfig-int без range (например BIG_GAP_MIN=-1)
 * после приведения к uint32_t превращался в огромный порог. Ручные сборки
 * без menuconfig-валидации теперь падают здесь, а не на устройстве. */
#if defined(CONFIG_MXR_BIG_GAP_MIN) && (CONFIG_MXR_BIG_GAP_MIN < 0)
#error "CONFIG_MXR_BIG_GAP_MIN must be >= 0"
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
 *  IRAM placement => только 32-битные поля
 *
 *  ESP8266 (LX106) допускает к IRAM (0x40100000..) ТОЛЬКО 32-битные
 *  обращения. SDK ставит LoadStoreErrorHandler (xtensa_vectors.S),
 *  который эмулирует l8ui / l16si / l16ui / s8i / s16i — поэтому
 *  8/16-битные доступы к IRAM "работают", но каждое такое обращение =
 *  вход в исключение (фиксированный 28-байтный стек, один слот повтора).
 *  Всё, что не эмулируется (в т.ч. невыровненные 32-битные), уходит
 *  в _xt_ext_panic().
 *
 *  Следствие: если состояние аллокатора размещено в IRAM
 *  (CONFIG_MXR_DESC_IN_IRAM_TEXT / _IRAM_BSS), ВСЕ поля этих структур
 *  обязаны быть 32-битными; COMPACT_TYPES в этом режиме игнорируется.
 * ================================================================ */
#if defined(CONFIG_MXR_DESC_IN_IRAM_TEXT) || defined(CONFIG_MXR_DESC_IN_IRAM_BSS)
#define MXR_IRAM_PLACEMENT_ACTIVE 1
#else
#define MXR_IRAM_PLACEMENT_ACTIVE 0
#endif

/* Размещение состояния аллокатора (s_stats, s_region[], s_iram_fb_region[])
 * задаётся в menuconfig, MXR_STATE_PLACEMENT:
 *   "DRAM" (вариант A) — состояние в .bss, MXR_STATE_DATA_ATTR пуст,
 *                        ширины полей обычные (COMPACT_TYPES работает);
 *   "IRAM" (вариант B) — состояние в IRAM, атрибут сохраняется, но ВСЕ
 *                        его поля принудительно 32-битные, COMPACT_TYPES
 *                        для этих двух типов не действует.
 * Оба варианта корректны; различие — цена (DRAM vs IRAM) и форма правки. */
#if MXR_IRAM_PLACEMENT_ACTIVE && defined(CONFIG_MXR_STATE_IN_IRAM)
#define MXR_STATE_PLACED_IN_IRAM 1
#else
#define MXR_STATE_PLACED_IN_IRAM 0
#endif

#if defined(CONFIG_MXR_STATE_IN_IRAM) && !MXR_IRAM_PLACEMENT_ACTIVE
#error "CONFIG_MXR_STATE_IN_IRAM requires IRAM descriptor placement: set MXR_STATE_PLACEMENT only together with CONFIG_MXR_DESC_IN_IRAM_TEXT/BSS"
#endif

/* Ширина "мелких" полей: в IRAM — только 32 бита. */
#if MXR_STATE_PLACED_IN_IRAM
#define MXR_FIELD_BOOL uint32_t
#define MXR_FIELD_U8   uint32_t
#define MXR_FIELD_U16  uint32_t
#else
#define MXR_FIELD_BOOL bool
#define MXR_FIELD_U8   uint8_t
#define MXR_FIELD_U16  uint16_t
#endif

/* ================================================================
 *  Platform-dependent type widths
 * ================================================================ */
#if defined(CONFIG_MXR_COMPACT_TYPES) && !MXR_STATE_PLACED_IN_IRAM
  typedef uint16_t mxr_caps_t;
  typedef uint16_t mxr_class_t;
  typedef uint16_t mxr_count_t;
#else
/* 32-битные принудительно, если состояние размещено в IRAM (MXR_STATE_PLACEMENT = IRAM) */
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

/* Ordinary malloc family only; explicit caps and SDK heap queries are unchanged. */
#ifdef CONFIG_MXR_DEFAULT_DRAM_ONLY
#define MXR_DEFAULT_CAPS (MALLOC_CAP_8BIT | MALLOC_CAP_32BIT)
#else
#define MXR_DEFAULT_CAPS MALLOC_CAP_32BIT
#endif

/* ================================================================
 *  MxR extension: placement hint "prefer IRAM"
 *
 *  Regular MALLOC_CAP_* bits describe what the caller REQUIRES. This bit
 *  is a soft preference: try the IRAM fallback zone first (even when the
 *  global order is DRAM_FIRST), fall back to DRAM as usual. It is stripped
 *  before region matching, so it never makes an allocation fail. Only
 *  meaningful for buffers that are accessed with 32-bit loads/stores ONLY
 *  (no 8/16-bit access!) - e.g. the audio pipeline's int32 read buffer -
 *  to keep DRAM free for byte-addressable users (pbufs, stacks, queues).
 *  Ignored when IRAM / IRAM fallback are disabled. NOT understood by the
 *  stock SDK heap (there it makes the request unsatisfiable), so
 *  applications must only set it when MxR is linked (see pipeline.c).
 * ================================================================ */
#define MXR_CAP_PREFER_IRAM (1u << 20)

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
#define MXR_IRAM_DATA_ATTR __attribute__((section(".bss.mxriram"), aligned(4)))
#else
#define MXR_IRAM_DATA_ATTR
#endif

/* Атрибут состояния аллокатора. В варианте B (MXR_STATE_PLACEMENT = IRAM)
 * состояние лежит в IRAM — там поля уже расширены до 32 бит (MXR_FIELD_*),
 * поэтому атрибут допустим. В варианте A и в DRAM-режиме он пуст: объекты
 * уходят в обычный .bss. */
#if MXR_STATE_PLACED_IN_IRAM
#define MXR_STATE_DATA_ATTR MXR_IRAM_DATA_ATTR
#else
#define MXR_STATE_DATA_ATTR
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
    MXR_FIELD_U8 largest_cache_valid;
#if MXR_QUICK_FIT_HINT_ACTIVE
#if MXR_BINNING_ACTIVE
    /* Binning: LRU gap каждого размерного класса.
     * Тот же инвариант: [*][*+len) свободны и внутри региона. */
    uint32_t hint_off[MXR_BIN_COUNT];
    uint32_t hint_len[MXR_BIN_COUNT];
#else
    /* Quick-fit hint: гарантированно свободный gap региона.
     * Инвариант: [hint_off, hint_off + hint_len) свободны и внутри региона.
     * hint_len == 0 -> подсказки нет. Используется только для DRAM. */
    uint32_t hint_off;
    uint32_t hint_len;
#endif
#endif
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
    uint32_t largest_free_bytes; /* largest raw internal gap, class-limited */
    mxr_count_t alloc_count;
  } mxr_region_status_t;

  /* ================================================================
   *  Global allocator status
   * ================================================================ */
  typedef struct
  {
    MXR_FIELD_BOOL initialized;
    MXR_FIELD_U8 region_count;
    MXR_FIELD_U8 iram_fb_region_count;
    MXR_FIELD_U16 dram_desc_capacity;
    MXR_FIELD_U16 iram_desc_capacity;
    MXR_FIELD_U16 dram_active_allocs;
    MXR_FIELD_U16 iram_active_allocs;
    MXR_FIELD_U16 max_active_allocs;
    size_t total_bytes;
    size_t free_bytes;
    size_t min_free_bytes;
    size_t largest_free_block_bytes; /* largest allocatable user payload estimate */
    size_t iram_total_bytes;
    size_t iram_free_bytes;
    size_t iram_min_free_bytes;
    size_t iram_fb_zone_total_bytes;
    uint32_t exec_allocs;
    uint32_t iram_fallback_allocs;
    uint32_t prefer_iram_hits;   /* MXR_CAP_PREFER_IRAM: served from IRAM */
    uint32_t prefer_iram_misses; /* MXR_CAP_PREFER_IRAM: IRAM full -> DRAM */
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

    /* Quick-fit hint статистика (O(1) быстрый путь DRAM) */
    uint32_t quick_fit_hint_hits;   /* подсказка дала блок без сканирования таблицы */
    uint32_t quick_fit_hint_misses; /* подсказка была, но не подошла (waste велик) */

    /* v2: защита/отладка/динамика (учитываются при включённых CONFIG *) */
    uint32_t canary_violations;   /* CANARY: порча границ блока при free/realloc */
    uint32_t double_free_detects; /* DFD: классифицированные double-free */
    uint32_t bgg_relaxed_accepts; /* BGG: early-exit по расслабленному правилу (gap не «батон») */
    uint32_t desc_growth_events;  /* DESC_DYNAMIC: расширения таблиц (DRAM+IRAM) */
    uint32_t desc_shrink_events;  /* DESC_DYNAMIC: сжатия таблиц (DRAM+IRAM) */

    /* FIX(4.3): region init fallback */
    MXR_FIELD_BOOL region_init_fallback;
    MXR_FIELD_BOOL iram_fb_region_init_fallback;
  } mxr_status_t;

  /* ДОБАВЛЕНО: проверка кратности 4 для mxr_memset4 */
  _Static_assert(sizeof(mxr_status_t) % 4 == 0,
                 "mxr_status_t size must be multiple of 4 for mxr_memset4");

#if MXR_STATE_PLACED_IN_IRAM
  /* ================================================================
   *  Компиляционный барьер правила "IRAM = только 32 бита".
   *  Добавили в IRAM-размещаемый тип поле уже 4 байт — сборка падает
   *  здесь, а не превращается в LoadStoreError на устройстве.
   * ================================================================ */
#define MXR_ASSERT_WORD_FIELD(T, F)                                     \
  _Static_assert(sizeof(((T *)0)->F) == 4,                              \
                 "IRAM placement: " #T "." #F " must be 32-bit"         \
                 " (8/16-bit access to IRAM = LoadStoreError)")

  MXR_ASSERT_WORD_FIELD(mxr_desc_t, off_flags);
  MXR_ASSERT_WORD_FIELD(mxr_desc_t, len_flags);

  MXR_ASSERT_WORD_FIELD(mxr_region_t, caps);
  MXR_ASSERT_WORD_FIELD(mxr_region_t, min_bytes);
  MXR_ASSERT_WORD_FIELD(mxr_region_t, max_bytes);
  MXR_ASSERT_WORD_FIELD(mxr_region_t, alloc_count);
  MXR_ASSERT_WORD_FIELD(mxr_region_t, largest_cache_valid);
  MXR_ASSERT_WORD_FIELD(mxr_region_t, start_byte);
  MXR_ASSERT_WORD_FIELD(mxr_region_t, total_bytes);

  MXR_ASSERT_WORD_FIELD(mxr_status_t, initialized);
  MXR_ASSERT_WORD_FIELD(mxr_status_t, region_count);
  MXR_ASSERT_WORD_FIELD(mxr_status_t, iram_fb_region_count);
  MXR_ASSERT_WORD_FIELD(mxr_status_t, dram_desc_capacity);
  MXR_ASSERT_WORD_FIELD(mxr_status_t, iram_desc_capacity);
  MXR_ASSERT_WORD_FIELD(mxr_status_t, dram_active_allocs);
  MXR_ASSERT_WORD_FIELD(mxr_status_t, iram_active_allocs);
  MXR_ASSERT_WORD_FIELD(mxr_status_t, max_active_allocs);
  MXR_ASSERT_WORD_FIELD(mxr_status_t, region_init_fallback);
  MXR_ASSERT_WORD_FIELD(mxr_status_t, iram_fb_region_init_fallback);
#undef MXR_ASSERT_WORD_FIELD
#endif /* MXR_STATE_PLACED_IN_IRAM */

  /* ================================================================
   *  Alignment helper
   * ================================================================ */
  static inline size_t MXR_IRAM_INLINE_ATTR mxr_align4(size_t bytes)
  {
    if (bytes > (SIZE_MAX - MXR_ALIGN_MASK))
    {
      return SIZE_MAX;
    }
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
   *  Platform arena bounds.
   *
   *  Реализации в mxr_malloc.c объявлены __attribute__((weak)):
   *  приложение (или host-тест) может переопределить их, чтобы
   *  задать иные границы DRAM/IRAM арены без правки компонента.
   * ================================================================ */
  void mxr_dram_arena_bounds(uint8_t **start, uint8_t **end);
#ifdef CONFIG_MXR_USE_IRAM
  void mxr_iram_arena_bounds(uint8_t **start, uint8_t **end);
#endif

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
  /* Maximum user payload estimate for current qualifying gaps. Not a reservation. */
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


