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
void  __wrap_heap_caps_init(void)
{
    mxr_init();
}

void *  MXR_WRAP_IRAM __wrap__heap_caps_malloc(
    size_t size, uint32_t caps, const char *file, size_t line)
{
    (void)file; (void)line;
    return mxr_malloc_caps(size, caps);
}

void  MXR_WRAP_IRAM __wrap__heap_caps_free(
    void *ptr, const char *file, size_t line)
{
    (void)file; (void)line;
    mxr_free(ptr);
}

void *MXR_WRAP_ALLOC_ATTR __wrap__heap_caps_realloc(
    void *mem, size_t newsize, uint32_t caps, const char *file, size_t line)
{
    (void)file; (void)line;
    return mxr_realloc_caps(mem, newsize, caps);
}

void *MXR_WRAP_ALLOC_ATTR __wrap__heap_caps_calloc(
    size_t count, size_t size, uint32_t caps, const char *file, size_t line)
{
    (void)file; (void)line;
    return mxr_calloc_caps(count, size, caps);
}

void *MXR_WRAP_ALLOC_ATTR __wrap__heap_caps_zalloc(
    size_t size, uint32_t caps, const char *file, size_t line)
{
    (void)file; (void)line;
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