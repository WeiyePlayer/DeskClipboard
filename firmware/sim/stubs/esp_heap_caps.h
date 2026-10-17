#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static inline void *heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }
static inline void *heap_caps_aligned_alloc(size_t a, size_t n, int caps) { (void)caps; void *p = NULL; return posix_memalign(&p, a < sizeof(void*) ? sizeof(void*) : a, n) ? NULL : p; }
