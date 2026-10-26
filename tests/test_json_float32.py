"""Binary32 round trips and transactional failure on host and pinned ARM nano.

QEMU exercises the real checked numeric sources, not a firmware/device runtime.
The native nano printf oracle is linked only by the test harness.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from tests.sdk_json import copy_patched_json

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <errno.h>
#include "json_number_checked.h"
#ifdef __NEWLIB__
#include <reent.h>
#include "json_heap.h"
#include "json_numeric/context.h"
static union { double alignment; unsigned char bytes[8192]; } arena[128];
static size_t sizes[128], live, calls, fail_at, peak;
static unsigned lock_depth, failures, denied;
static void output(const char *s) {
    size_t length = strlen(s);
    register int r0 __asm__("r0") = 1;
    register const char *r1 __asm__("r1") = s;
    register size_t r2 __asm__("r2") = length;
    register int r7 __asm__("r7") = 4;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
}
void _exit(int code) {
    register int r0 __asm__("r0") = code;
    register int r7 __asm__("r7") = 1;
    __asm__ volatile("svc 0" :: "r"(r0), "r"(r7) : "memory"); for (;;) {}
}
#else
#include <unistd.h>
static void output(const char *s) { fputs(s, stdout); }
#endif
static void integer(size_t n) {
    char b[24]; unsigned i = sizeof(b)-1; b[i] = 0;
    do { b[--i] = (char)('0'+n%10); n /= 10; } while (n); output(b+i);
}
static void checked(int good, unsigned line) {
    if (!good) { output("FAIL float32 line="); integer(line); output("\n"); _exit(1); }
}
#define CHECK(c) checked(!!(c), __LINE__)
#ifdef __NEWLIB__
void *__wrap_malloc(size_t n) {
    ++calls; if (fail_at && calls == fail_at) return NULL;
    CHECK(n <= sizeof(arena[0].bytes));
    for (unsigned i=0; i<128; ++i) if (!sizes[i]) {
        sizes[i] = n?n:1; live += sizes[i]; if (live > peak) peak = live;
        memset(arena[i].bytes, 0xa5, sizeof(arena[i].bytes)); return arena[i].bytes;
    }
    CHECK(0); return NULL;
}
void __wrap_free(void *p) {
    if (!p) return;
    for (unsigned i=0; i<128; ++i) if (p == arena[i].bytes) {
        CHECK(sizes[i]); live -= sizes[i]; sizes[i] = 0;
        memset(arena[i].bytes, 0xdd, sizeof(arena[i].bytes)); return;
    }
    CHECK(0);
}
void *__wrap_calloc(size_t n, size_t s) {
    CHECK(!s || n <= SIZE_MAX/s); void *p = __wrap_malloc(n*s);
    if (p) memset(p, 0, n*s); return p;
}
void *__wrap_realloc(void *p, size_t n) {
    if (!p) return __wrap_malloc(n);
    for (unsigned i=0; i<128; ++i) if (p == arena[i].bytes) {
        size_t old = sizes[i]; CHECK(old);
        void *q = __wrap_malloc(n); if (!q) return NULL;
        memcpy(q, p, old<n?old:n); __wrap_free(p); return q;
    }
    CHECK(0); return NULL;
}
void *__wrap__malloc_r(struct _reent *r, size_t n) { (void)r; return __wrap_malloc(n); }
void *__wrap__calloc_r(struct _reent *r, size_t n, size_t s) { (void)r; return __wrap_calloc(n,s); }
void *__wrap__realloc_r(struct _reent *r, void *p, size_t n) { (void)r; return __wrap_realloc(p,n); }
void __wrap__free_r(struct _reent *r, void *p) { (void)r; __wrap_free(p); }
void *_sbrk(ptrdiff_t n) { (void)n; CHECK(0); return NULL; }
void json_heap_lock(void) { CHECK(!lock_depth); ++lock_depth; }
void json_heap_unlock(void) { CHECK(lock_depth == 1); --lock_depth; }
int json_heap_permit(size_t bytes) { (void)bytes; return !denied; }
void json_heap_failure(unsigned reason) { CHECK(reason); CHECK(lock_depth == 1); ++failures; }
static void native_cleanup(struct _reent *c) {
    if (c->_mp) {
        struct _Bigint *p = _REENT_MP_P5S(c); _REENT_MP_P5S(c) = NULL;
        while (p) { struct _Bigint *next = p->_next; __wrap_free(p); p = next; }
    }
    _reclaim_reent(c);
}
static int native_format(char *b, unsigned cap, double value, int precision) {
    struct _reent c; _REENT_INIT_PTR(&c);
    int n = _snprintf_r(&c, b, cap, "%.*g", precision, value);
    native_cleanup(&c); CHECK(!live); return n;
}
static double parse(const char *text) {
    struct json_num_context c = {0}; char *end;
    double value = json_num_strtod_r(&c, text, &end);
    CHECK(!c.failed && *text && !*end); json_num_cleanup(&c); CHECK(!live);
    /* The frozen nano scanf token frontend normalizes "-0" to positive zero.
     * Check its numeric equivalence separately; raw checked strtod above must
     * preserve all float bits, including the sign of a serialized zero. */
    double scanned = 42; CHECK(json_number_scan(text, &scanned) == 1);
    CHECK(scanned == value); CHECK(!live); return value;
}
#else
static int native_format(char *b, unsigned cap, double value, int precision) {
    return snprintf(b, cap, "%.*g", precision, value);
}
static double parse(const char *text) {
    char *end; double value = strtod(text, &end); CHECK(*text && !*end); return value;
}
#endif
static uint32_t float_bits(float value) {
    uint32_t bits; memcpy(&bits, &value, sizeof(bits)); return bits;
}
static float from_bits(uint32_t bits) {
    float value; memcpy(&value, &bits, sizeof(value)); return value;
}
static void sentinel(const char *b, unsigned start, unsigned end) {
    for (unsigned i=start; i<end; ++i) CHECK(b[i] == 0x5a);
}
static void capacity_checks(float value) {
    char expected[32], b[64];
    int n = json_number_format_float32(expected, sizeof(expected), value); CHECK(n > 0);
    for (unsigned capacity=0; capacity<=(unsigned)n; ++capacity) {
        memset(b, 0x5a, sizeof(b));
        CHECK(json_number_format_float32(b, capacity, value) == -1);
        sentinel(b, 0, sizeof(b));
    }
    memset(b, 0x5a, sizeof(b));
    CHECK(json_number_format_float32(b, (unsigned)n+1, value) == n);
    CHECK(!strcmp(b, expected)); sentinel(b, (unsigned)n+1, sizeof(b));
#ifdef __NEWLIB__
    CHECK(!live && !lock_depth);
#endif
}
static void roundtrip(float value) {
    char b[32], native[32], old[32];
    int n = json_number_format_float32(b, sizeof(b), value); CHECK(n > 0);
    CHECK(n == native_format(native, sizeof(native), (double)value, 9));
    if (strcmp(b, native)) { output("new mismatch "); output(b); output(" / "); output(native); output("\n"); CHECK(0); }
    CHECK((unsigned)n == strlen(b));
    if (isfinite(value)) {
        uint32_t decoded = float_bits((float)parse(b));
        if (decoded != float_bits(value)) {
            output("roundtrip mismatch "); output(b); output(" input-bits="); integer(float_bits(value));
            output(" decoded-bits="); integer(decoded); output("\n"); CHECK(0);
        }
    }
    n = json_number_format(old, sizeof(old), (double)value);
    CHECK(n == native_format(native, sizeof(native), (double)value, 6));
    CHECK(!strcmp(old, native));
#ifdef __NEWLIB__
    CHECK(!live && !lock_depth);
#endif
}
#ifdef __NEWLIB__
static void allocation_failures(float value) {
    char b[64]; calls = 0;
    CHECK(json_number_format_float32(b, sizeof(b), value) > 0); size_t count = calls;
    CHECK(!live);
    for (size_t ordinal=1; ordinal<=count; ++ordinal) {
        calls = 0; fail_at = ordinal; failures = 0; errno = 0;
        memset(b, 0x5a, sizeof(b));
        CHECK(json_number_format_float32(b, sizeof(b), value) == -1);
        sentinel(b, 0, sizeof(b)); CHECK(errno == ENOMEM); CHECK(failures);
        CHECK(!live && !lock_depth); fail_at = 0;
        CHECK(json_number_format_float32(b, sizeof(b), value) > 0); CHECK(!live);
    }
}
#endif
static void run(void) {
    CHECK(sizeof(float) == sizeof(uint32_t)); CHECK(FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128);
    const uint32_t boundaries[] = {
        0, 0x80000000u, 0x3f800000u, 0x3f800001u, 0x3f7fffffu,
        0xbf800000u, 0xbf800001u, 0xbf7fffffu, 0x00800000u, 0x007fffffu,
        1, 0x80000001u, 0x7f7fffffu, 0xff7fffffu, 0x4e6e6b28u, 0x4e6e6b27u,
        0x7f800000u, 0xff800000u, 0x7fc00000u
    };
    for (unsigned i=0; i<sizeof(boundaries)/sizeof(*boundaries); ++i) {
        float value = from_bits(boundaries[i]); roundtrip(value); capacity_checks(value);
#ifdef __NEWLIB__
        allocation_failures(value);
#endif
    }
    const float cutoffs[] = {0.0001f, 0.00001f, 999999.f, 1000000.f, 999999936.f, 1000000000.f};
    for (unsigned i=0; i<sizeof(cutoffs)/sizeof(*cutoffs); ++i) {
        roundtrip(cutoffs[i]); capacity_checks(cutoffs[i]);
    }
    char lower[32], upper[32], legacy[32];
    CHECK(json_number_format_float32(lower, sizeof(lower), 1.f) == 1); CHECK(!strcmp(lower, "1"));
    CHECK(json_number_format_float32(upper, sizeof(upper), from_bits(0x3f800001u)) == 10);
    CHECK(!strcmp(upper, "1.00000012")); CHECK(strcmp(lower, upper));
    CHECK(json_number_format(legacy, sizeof(legacy), (double)from_bits(0x3f800001u)) == 1);
    CHECK(!strcmp(legacy, "1"));
    CHECK(json_number_format_float32(lower, sizeof(lower), -0.f) == 2); CHECK(!strcmp(lower, "-0"));
    uint32_t state = 0x9e3779b9u; unsigned samples = 0;
    for (unsigned i=0; i<8192; ++i) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        if ((state & 0x7f800000u) == 0x7f800000u) continue;
        roundtrip(from_bits(state)); ++samples;
    }
#ifdef __NEWLIB__
    denied = 1; failures = 0; errno = 0; memset(upper, 0x5a, sizeof(upper));
    CHECK(json_number_format_float32(upper, sizeof(upper), 1.0000001192092896f) == -1);
    sentinel(upper, 0, sizeof(upper)); CHECK(errno == ENOMEM && failures);
    CHECK(!live && !lock_depth); denied = 0;
    CHECK(json_number_format_float32(upper, sizeof(upper), 1.0000001192092896f) > 0);
    CHECK(!live && !lock_depth);
#endif
    output("PASS float32 roundtrip samples="); integer(samples);
    output("; native9 and legacy6 parity; capacity rollback");
#ifdef __NEWLIB__
    output("; fail-each allocation and headroom cleanup; peak-live-bytes="); integer(peak);
#endif
    output("\n");
}
#ifdef __NEWLIB__
void _start(void) { run(); _exit(0); }
#else
int main(void) { run(); return 0; }
#endif
'''


class JsonFloat32(unittest.TestCase):
    def test_host_roundtrip_and_capacity(self):
        cc = shutil.which('cc')
        self.assertTrue(cc, 'host C compiler required')
        with tempfile.TemporaryDirectory(prefix='json-float32-host-') as tmp:
            directory = Path(tmp)
            source = directory/'test.c'
            source.write_text(HARNESS)
            exe = directory/'test'
            subprocess.run([cc, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                            '-I'+str(ROOT/'common'), str(source),
                            str(ROOT/'common/json_numeric/format.c'), '-lm', '-o', str(exe)],
                           check=True, capture_output=True, text=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            self.assertIn('PASS float32 roundtrip samples=', result.stdout)
            print('host', result.stdout.strip())

    def test_pinned_arm_roundtrip_and_allocation_failures(self):
        compiler = Path(os.environ.get('ARM_CC', ROOT/'build/tc1-tools/'
            'gcc-arm-none-eabi-5_4-2016q2/bin/arm-none-eabi-gcc'))
        qemu = shutil.which('qemu-arm') or shutil.which('qemu-arm-static')
        self.assertTrue(compiler.exists(), 'pinned ARM compiler required')
        self.assertTrue(qemu, 'qemu-arm required')
        self.assertEqual(subprocess.check_output([str(compiler), '-dumpversion'], text=True).strip(), '5.4.1')
        sdk = Path(os.environ.get('M1_SDK_DIR', ROOT/'build/m1-sdk/mico-os'))
        json_source = Path(os.environ.get('PHICOMM_TEST_JSON_DIR', sdk/'libraries/utilities/json_c'))
        for abi in ('hard', 'soft'):
            with self.subTest(abi=abi), tempfile.TemporaryDirectory(prefix='json-float32-'+abi+'-') as tmp:
                directory = Path(tmp)
                json = copy_patched_json(json_source, directory/'sdk')
                source = directory/'test.c'
                source.write_text(HARNESS)
                flags = ['-mcpu=cortex-m4', '-mthumb', '-Os', '-g', '-std=gnu11',
                         '-mfloat-abi='+abi, '-fno-builtin',
                         '-fstack-usage', '--specs=nano.specs', '-I'+str(json)]
                if abi == 'hard':
                    flags += ['-mfpu=fpv4-sp-d16']
                sources = [source, json/'json_heap.c', *sorted((json/'json_numeric').glob('*.c'))]
                objects = []
                for src in sources:
                    obj = directory/(src.stem+'.o')
                    subprocess.run([str(compiler), *flags, '-c', str(src), '-o', str(obj)],
                                   check=True, capture_output=True, text=True)
                    objects.append(obj)
                nm = compiler.with_name('arm-none-eabi-nm')
                numeric_objects = [directory/(name+'.o') for name in
                    ('context', 'mprec', 'dtoa', 'strtod', 'match', 'scan', 'format')]
                undefined = subprocess.check_output([str(nm), '-u', *map(str, numeric_objects)], text=True)
                forbidden = re.findall(r'\b(?:_?(?:malloc|calloc|realloc|free)(?:_r)?|_?(?:dtoa|strtod)(?:_r)?|_?(?:s?scanf|s?n?printf)(?:_r)?)$', undefined, re.M)
                self.assertEqual(forbidden, [], undefined)
                symbols = subprocess.check_output([str(nm), str(directory/'format.o')], text=True)
                self.assertNotIn('json_number_format_general', symbols)
                frames = (directory/'format.su').read_text()
                self.assertIn(':json_number_format\t', frames)
                self.assertIn(':json_number_format_float32\t', frames)
                elf = directory/'test.elf'
                wrapped = ('malloc', 'calloc', 'realloc', 'free',
                           '_malloc_r', '_calloc_r', '_realloc_r', '_free_r')
                subprocess.run([str(compiler), *flags, '-nostartfiles', '--specs=nosys.specs',
                    '-u', '_printf_float', '-u', '_scanf_float', '-Wl,-e,_start,-Ttext=0x10000',
                    *['-Wl,--wrap='+name for name in wrapped], *map(str, objects),
                    '-lm', '-o', str(elf)], check=True, capture_output=True, text=True)
                result = subprocess.run([qemu, str(elf)], capture_output=True, text=True, timeout=90)
                if os.environ.get('JSON_FLOAT32_ARTIFACTS'):
                    (directory/'qemu.txt').write_text(result.stdout+result.stderr)
                    shutil.copytree(directory, Path(os.environ['JSON_FLOAT32_ARTIFACTS'])/abi, dirs_exist_ok=True)
                self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                self.assertIn('fail-each allocation and headroom cleanup', result.stdout)
                print(abi, result.stdout.strip())


if __name__ == '__main__':
    unittest.main()
