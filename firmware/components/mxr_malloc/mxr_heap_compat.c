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