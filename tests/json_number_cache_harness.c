/* Pinned ARM/newlib-nano only. Native allocation ledger; no host libc model. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reent.h>
#include "json_number_reent.h"

static union { uint64_t alignment; unsigned char bytes[1024]; } arena[512];
static size_t sizes[512], live_bytes, live_blocks, calls;
static int fail_next;
void _exit(int code) {
    register int r0 __asm__("r0") = code;
    register int r7 __asm__("r7") = 1;
    __asm__ volatile("svc 0" :: "r"(r0), "r"(r7) : "memory");
    for (;;) {}
}
#define CHECK(condition) do { if (!(condition)) _exit(__LINE__ % 190 + 20); } while (0)
static void out(const char *s) {
    size_t length = strlen(s);
    register int r0 __asm__("r0") = 1;
    register const char *r1 __asm__("r1") = s;
    register size_t r2 __asm__("r2") = length;
    register int r7 __asm__("r7") = 4;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
}
static void number(size_t n) {
    char text[24]; unsigned i = sizeof(text)-1; text[i] = 0;
    do { text[--i] = '0'+n%10; n /= 10; } while(n); out(text+i);
}
static size_t index_of(void *p) {
    for (size_t i = 0; i < 512; ++i) if (p == arena[i].bytes) return i;
    _exit(220); return 0;
}
void *__wrap_malloc(size_t n) {
    ++calls;
    if (fail_next) { fail_next = 0; return NULL; }
    CHECK(n <= sizeof(arena[0].bytes));
    for (size_t i = 0; i < 512; ++i) if (!sizes[i]) {
        sizes[i] = n ? n : 1; live_bytes += sizes[i]; ++live_blocks;
        memset(arena[i].bytes, 0xa5, sizeof(arena[i].bytes)); return arena[i].bytes;
    }
    _exit(221); return NULL;
}
void __wrap_free(void *p) {
    if (!p) return;
    size_t i = index_of(p); CHECK(sizes[i]);
    live_bytes -= sizes[i]; --live_blocks; sizes[i] = 0;
    memset(arena[i].bytes, 0xdd, sizeof(arena[i].bytes));
}
void *__wrap_calloc(size_t count, size_t n) {
    CHECK(!n || count <= SIZE_MAX/n);
    void *p = __wrap_malloc(count*n); if (p) memset(p, 0, count*n); return p;
}
void *__wrap__malloc_r(struct _reent *r, size_t n) { (void)r; return __wrap_malloc(n); }
void *__wrap__calloc_r(struct _reent *r, size_t c, size_t n) { (void)r; return __wrap_calloc(c,n); }
void __wrap__free_r(struct _reent *r, void *p) { (void)r; __wrap_free(p); }
void *__wrap_realloc(void *p, size_t n) { (void)p; (void)n; _exit(222); return NULL; }
void *__wrap__realloc_r(struct _reent *r, void *p, size_t n) { (void)r; return __wrap_realloc(p,n); }
void *_sbrk(ptrdiff_t n) { (void)n; _exit(223); return NULL; }
static void empty(void) { CHECK(!live_bytes && !live_blocks); }
static size_t disjoint_cache(struct _reent *r) {
    size_t count = 0;
    if (!r->_mp) return 0;
    for (struct _Bigint *p = _REENT_MP_P5S(r); p; p = p->_next) {
        CHECK(++count < 32 && sizes[index_of(p)] >= sizeof(*p));
        CHECK(p != _REENT_MP_RESULT(r));
        struct _Bigint **freelist = _REENT_MP_FREELIST(r);
        if (freelist) {
            size_t slots = sizes[index_of(freelist)]/sizeof(*freelist);
            for (size_t i = 0; i < slots; ++i) {
                unsigned steps = 0;
                for (struct _Bigint *f = freelist[i]; f; f = f->_next) {
                    CHECK(++steps < 64 && sizes[index_of(f)] >= sizeof(*f)); CHECK(p != f);
                }
            }
        }
    }
    return count;
}
struct json_object { union { double c_double; } o; };
struct printbuf { char buf[32]; };
static int printbuf_memappend(struct printbuf *p, const char *s, int n) {
    CHECK(n >= 0 && n < (int)sizeof(p->buf));
    memcpy(p->buf, s, n); p->buf[n] = 0; return n;
}
/* PRODUCTION_DOUBLE_CONVERTER */

static void partial_contexts(void) {
    struct _reent r; _REENT_INIT_PTR(&r);
    json_number_reclaim(NULL); json_number_reclaim(&r); empty();
    _REENT_INIT_PTR(&r);
    r._mp = __wrap_calloc(1, sizeof(*r._mp)); CHECK(r._mp);
    json_number_reclaim(&r); empty();
    _REENT_INIT_PTR(&r);
    r._mp = __wrap_calloc(1, sizeof(*r._mp)); CHECK(r._mp);
    struct _Bigint *a = __wrap_calloc(1, sizeof(*a)); CHECK(a);
    struct _Bigint *b = __wrap_calloc(1, sizeof(*b)); CHECK(b);
    a->_next = b; _REENT_MP_P5S(&r) = a;
    fail_next = 1; CHECK(!__wrap_malloc(sizeof(*a)));
    CHECK(disjoint_cache(&r) == 2); json_number_reclaim(&r); empty();
}
void _start(void) {
#ifdef NEGATIVE_CASE
    double parsed = 0;
    if (NEGATIVE_CASE == 1) CHECK(json_number_scan("0.12345678901234567", &parsed) == 1);
    if (NEGATIVE_CASE == 2) CHECK(json_number_scan("1e100", &parsed) == 1);
    if (NEGATIVE_CASE == 3) {
        struct json_object o = {.o.c_double = 999999.5}; struct printbuf p;
        CHECK(json_object_double_to_json_string(&o, &p) > 0 && !strcmp(p.buf, "1e+06"));
    }
    out("old cleanup residual bytes="); number(live_bytes); out(" blocks="); number(live_blocks); out("\n");
    const size_t expected_bytes[] = {0,84,172,28}, expected_blocks[] = {0,3,5,1};
    _exit(live_bytes == expected_bytes[NEGATIVE_CASE] && live_blocks == expected_blocks[NEGATIVE_CASE] ? 73 : 74);
#else
    void *global_mp = _impure_ptr->_mp;
    partial_contexts();
    const struct { const char *text; double expected; } scans[] = {
        {"0",0}, {"12.5",12.5}, {"0.12345678901234567",0.12345678901234567},
        {"1e100",1e100}, {"-1e-100",-1e-100}, {"999999.5",999999.5},
        {"0.00009999995",0.00009999995}, {"0.000009999995",0.000009999995}
    };
    const struct { double value; const char *wire; } formats[] = {
        {0,"0"}, {12.5,"12.5"}, {-0.125,"-0.125"}, {1000.25,"1000.25"},
        {999999.5,"1e+06"}, {0.00009999995,"0.0001"},
        {0.000009999995,"1e-05"}, {1e100,"1e+100"}
    };
    size_t cache_nodes = 0;
    for (unsigned cycle = 0; cycle < 1000; ++cycle) {
        for (unsigned i = 0; i < sizeof(scans)/sizeof(scans[0]); ++i) {
            struct _reent r; _REENT_INIT_PTR(&r); double direct = 0, parsed = 0;
            CHECK(_sscanf_r(&r, scans[i].text, "%lf", &direct) == 1);
            cache_nodes += disjoint_cache(&r); json_number_reclaim(&r); empty();
            CHECK(json_number_scan(scans[i].text, &parsed) == 1 && parsed == scans[i].expected && direct == parsed); empty();
        }
        for (unsigned i = 0; i < sizeof(formats)/sizeof(formats[0]); ++i) {
            struct _reent r; _REENT_INIT_PTR(&r); char direct[32];
            CHECK(_snprintf_r(&r,direct,sizeof(direct),"%g",formats[i].value) > 0);
            cache_nodes += disjoint_cache(&r); json_number_reclaim(&r); empty();
            struct json_object o = {.o.c_double = formats[i].value}; struct printbuf p;
            CHECK(json_object_double_to_json_string(&o,&p) > 0 && !strcmp(p.buf,formats[i].wire) && !strcmp(p.buf,direct)); empty();
        }
        double unused = 0;
        CHECK(json_number_scan("not-a-number",&unused) != 1); empty();
        CHECK(_impure_ptr->_mp == global_mp);
    }
    CHECK(cache_nodes > 0);
    out("private nano cache cleanup PASS: 33000 conversions, live bytes="); number(live_bytes);
    out(" blocks="); number(live_blocks); out(" observed disjoint cache nodes="); number(cache_nodes);
    out(" allocator calls="); number(calls); out("\n"); _exit(0);
#endif
}
