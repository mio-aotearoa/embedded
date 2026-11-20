// SPDX-License-Identifier: GPL-3.0-or-later
//
// Prefer PSRAM, fall back to internal. Shared by Lexbor, JPEG buffers, sprites,
// task helpers, etc. — one ladder instead of copy-pasted heap_caps calls.
#pragma once

#include <esp_heap_caps.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

inline void *psramPreferAlloc(size_t n)
{
    if (n == 0) n = 1;
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);
}

inline void *psramPreferCalloc(size_t count, size_t size)
{
    if (size && count > (size_t)-1 / size) return nullptr;
    void *p = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_calloc(count, size, MALLOC_CAP_8BIT);
    return p;
}

inline void *psramPreferRealloc(void *p, size_t n)
{
    if (!p) return psramPreferAlloc(n);
    void *q = psramPreferAlloc(n);
    if (!q) return nullptr;
    const size_t old = heap_caps_get_allocated_size(p);
    memcpy(q, p, old < n ? old : n);
    heap_caps_free(p);
    return q;
}

inline void psramPreferFree(void *p) { heap_caps_free(p); }
