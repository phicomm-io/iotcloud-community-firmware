/* Allocator shim for the lab writer. No JSON library or DOM is linked. */
#pragma once
#include <stdlib.h>
#define json_heap_malloc malloc
#define json_heap_calloc calloc
#define json_heap_free free
static inline void *json_heap_resize(void *p, size_t old, size_t n) {
    (void)old; return realloc(p, n);
}
