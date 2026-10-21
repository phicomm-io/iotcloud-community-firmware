/* Actual patched SDK JSON, with strict allocation spans and the unsafe SDK
 * growth behavior available only as an ASan negative control. No DUT traffic. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
#include "json_heap.h"
#include "arraylist.h"
#include "json_owned.h"

static struct { void *p; size_t n; } allocations[8192];
static size_t blocks, bytes, peak_bytes, capacity = SIZE_MAX;
static size_t resize_calls, fail_at, deny_at, native_realloc_calls;
static unsigned locked, last_failure;
static int resizing;
static void *old_pointer;
static size_t old_bytes, new_bytes;
static unsigned char *old_copy;
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void __real_free(void *);
void *__real_json_heap_resize(void *, size_t, size_t);

static size_t find(void *p) {
    for (size_t i = 0; i < 8192; ++i) if (allocations[i].p == p) return i;
    assert(!"untracked allocation"); return 0;
}
static void add(void *p, size_t n) {
    if (!p) return;
    size_t i = find(NULL); allocations[i].p = p; allocations[i].n = n;
    ++blocks; bytes += n; if (bytes > peak_bytes) peak_bytes = bytes;
}
static void unchanged(void) {
    if (old_pointer) {
        assert(allocations[find(old_pointer)].n == old_bytes);
        assert(!memcmp(old_pointer, old_copy, old_bytes));
    }
}
void *__wrap_malloc(size_t n) {
    if (resizing) {
        assert(locked && n == new_bytes); unchanged();
        if (resize_calls == fail_at) return NULL;
    }
    void *p = __real_malloc(n); add(p, n); return p;
}
void *__wrap_calloc(size_t c, size_t n) {
    assert(!n || c <= SIZE_MAX/n);
    void *p = __real_calloc(c, n); add(p, c*n); return p;
}
char *__wrap_strdup(const char *s) {
    size_t n = strlen(s)+1; char *p = __real_malloc(n);
    if (p) { memcpy(p, s, n); add(p, n); } return p;
}
void __wrap_free(void *p) {
    if (p) { size_t i = find(p); --blocks; bytes -= allocations[i].n; allocations[i].p = NULL; }
    __real_free(p);
}
/* Match the pinned kernel's new-size memcpy. Safe paths must never call it. */
void *__wrap_realloc(void *p, size_t n) {
    ++native_realloc_calls;
    void *q = __wrap_malloc(n);
    if (q) { if (p) memcpy(q, p, n); __wrap_free(p); }
    return q;
}
void json_heap_lock(void) { assert(!locked); locked = 1; }
void json_heap_unlock(void) { assert(locked); locked = 0; }
int json_heap_permit(size_t n) {
    assert(locked);
    if (resizing) { assert(n == new_bytes); unchanged(); if (resize_calls == deny_at) return 0; }
    return bytes <= capacity && n <= capacity-bytes;
}
void json_heap_failure(unsigned reason) { assert(locked); last_failure = reason; }
void *__wrap_json_heap_resize(void *p, size_t old, size_t n) {
    assert(!resizing);
    json_heap_stats_t before, after; json_heap_snapshot(&before);
    size_t before_bytes = bytes, before_blocks = blocks;
    if (p) assert(allocations[find(p)].n == old || !old);
    old_pointer = p; old_bytes = p ? allocations[find(p)].n : 0; new_bytes = n;
    old_copy = old_bytes ? __real_malloc(old_bytes) : NULL;
    if (old_copy) memcpy(old_copy, p, old_bytes);
    ++resize_calls; resizing = 1; last_failure = 0;
    void *q = __real_json_heap_resize(p, old, n);
    resizing = 0; json_heap_snapshot(&after);
    if (!q && n) {
        unchanged(); assert(bytes == before_bytes && blocks == before_blocks);
        assert(after.live_blocks == before.live_blocks);
        assert(last_failure == 1 || last_failure == 2 || last_failure == 4);
        assert(after.allocation_failures == before.allocation_failures + (last_failure == 2));
        assert(after.guard_denied == before.guard_denied + (last_failure != 2));
    } else if (p && q) {
        assert(after.live_blocks == before.live_blocks && blocks == before_blocks);
        assert(bytes == before_bytes-old+n);
        assert(!memcmp(q, old_copy, old < n ? old : n));
    }
    __real_free(old_copy); old_copy = NULL; old_pointer = NULL;
    return q;
}
static void empty(void) {
    json_heap_stats_t s; json_heap_snapshot(&s);
    assert(!s.live_blocks && !blocks && !bytes && !locked && !resizing);
}
static void reset(size_t fail, size_t deny) { resize_calls = 0; fail_at = fail; deny_at = deny; }
static void semantics(void) {
    assert(!json_heap_resize(NULL, 0, 0));
    assert(!json_heap_resize(NULL, 1, 16) && last_failure == 4);
    unsigned char *p = json_heap_resize(NULL, 0, 16); assert(p); memset(p, 0x5a, 16);
    assert(!json_heap_resize(p, 0, 32) && last_failure == 4);
    capacity = bytes;
    assert(json_heap_resize(p, 16, 16) == p);
    assert(!json_heap_resize(p, 16, 32) && last_failure == 1);
    assert(!json_heap_resize(p, 16, 8) && last_failure == 1); /* Full replacement headroom. */
    capacity = SIZE_MAX;
    p = json_heap_resize(p, 16, 8); assert(p);
    assert(!json_heap_resize(p, 8, SIZE_MAX) && last_failure == 1);
    assert(!json_heap_resize(p, 8, 0)); empty();
    assert(!json_heap_calloc(SIZE_MAX, 2) && last_failure == 4); empty();
}
static void printbuf_growth(void) {
    struct printbuf *p = printbuf_new(); assert(p); printbuf_reset(p);
    char expected[4097]; size_t length = 0;
    for (unsigned i = 0; i < 128; ++i) {
        char piece[32]; memset(piece, 'a'+i%26, sizeof(piece));
        char *old = p->buf; int size = p->size, used = p->bpos;
        if (printbuf_memappend(p, piece, sizeof(piece)) < 0) {
            assert(p->buf == old && p->size == size && p->bpos == used && p->failed);
            assert(!memcmp(p->buf, expected, length)); break;
        }
        memcpy(expected+length, piece, sizeof(piece)); length += sizeof(piece);
        assert(p->bpos == (int)length && !memcmp(p->buf, expected, length) && !p->buf[length]);
    }
    printbuf_free(p); empty();
}
static void no_free(void *p) { (void)p; }
static void array_growth(void) {
    struct array_list *a = array_list_new(no_free); assert(a);
    for (int i = 0; i < 1024; ++i) {
        void **old = a->array; int size = a->size, length = a->length;
        if (array_list_add(a, (void *)(uintptr_t)(i+1)) < 0) {
            assert(a->array == old && a->size == size && a->length == length); break;
        }
        for (int j = 0; j <= i; ++j) assert(a->array[j] == (void *)(uintptr_t)(j+1));
        for (int j = i+1; j < a->size; ++j) assert(!a->array[j]);
    }
    array_list_free(a); empty();
}
static void owned_growth(void) {
    /* A retained root requires every failure path to preserve the graph. */
    reset(0, 0);
    char payload[2049]; memset(payload, 'x', sizeof(payload)-1); payload[2048] = 0;
    json_object *o = json_object_new_object(); assert(o);
    json_object_object_add(o, "payload", json_object_new_string(payload));
    const char *expected = json_object_to_json_string(o); assert(expected);
    /* Failure selection is installed by the caller after fixture allocation. */
    extern size_t owned_fail, owned_deny;
    reset(owned_fail, owned_deny);
    struct printbuf *wire = json_owned_to_wire(json_object_get(o));
    if (wire) { assert(!strcmp(wire->buf, expected)); printbuf_free(wire); }
    assert(!strcmp(json_object_to_json_string(o), expected));
    json_object_put(o); empty();
}
size_t owned_fail, owned_deny;
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "unsafe")) {
        void *p = __wrap_malloc(4); memset(p, 'x', 4);
        void *q = __wrap_realloc(p, 256); __wrap_free(q); return 0;
    }
    semantics();
    void (*cases[])(void) = { printbuf_growth, array_growth, owned_growth };
    for (unsigned c = 0; c < 3; ++c) {
        owned_fail = owned_deny = 0; reset(0, 0); cases[c](); size_t count = resize_calls;
        assert(count > 1);
        for (size_t i = 1; i <= count; ++i) {
            owned_fail = i; owned_deny = 0; reset(i, 0); cases[c](); assert(resize_calls == i);
            owned_fail = 0; owned_deny = i; reset(0, i); cases[c](); assert(resize_calls == i);
        }
        printf("JSON sized resize path %u: %zu growth failures + denials, preserved old allocation\n", c, count);
    }
    assert(!native_realloc_calls); empty();
    printf("JSON sized resize PASS: native realloc=0, live=0, peak temporary bytes=%zu\n", peak_bytes);
    return 0;
}
