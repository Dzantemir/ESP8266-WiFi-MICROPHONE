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