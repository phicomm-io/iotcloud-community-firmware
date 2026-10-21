"""Run the production JSON number converters against the actual ARM nano libc.

Host libc tests cannot catch a full-newlib struct _reent passed to libc_nano.
The Linux syscall exit shim is only for qemu-arm; this does not emulate MiCO.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from tests.sdk_json import copy_patched_json

ROOT = Path(__file__).resolve().parents[1]


class JsonNewlibNano(unittest.TestCase):
    def test_private_context_abi_and_number_roundtrips(self):
        gcc = Path(os.environ.get('ARM_CC', ROOT / 'build/tc1-tools/'
            'gcc-arm-none-eabi-5_4-2016q2/bin/arm-none-eabi-gcc'))
        qemu = shutil.which('qemu-arm') or shutil.which('qemu-arm-static')
        sdk = Path(os.environ.get('TC1_SDK_DIR', ROOT / 'build/tc1-sdk-3.6.4/mico-os'))
        json_dir = sdk / 'libraries/utilities/json_c'
        if not gcc.exists() or not qemu or not (json_dir / 'json_number_reent.h').exists():
            self.skipTest('built TC1 SDK, ARM GCC and qemu-arm are required')
        source = (json_dir / 'json_object.c').read_text()
        start = source.index('static int json_object_double_to_json_string(')
        converter = source[start:source.index('struct json_object* json_object_new_double(', start)]
        harness = r'''
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "json_number_reent.h"
static char arena[128 * 1024];
static size_t used;
void *_sbrk(ptrdiff_t n) {
    if (n < 0 || (size_t)n > sizeof(arena) - used) return (void *)-1;
    void *p = arena + used; used += n; return p;
}
void _exit(int code) {
    register int r0 __asm__("r0") = code;
    register int r7 __asm__("r7") = 1;
    __asm__ volatile("svc 0" :: "r"(r0), "r"(r7) : "memory");
    for (;;) {}
}
struct json_object { union { double c_double; } o; };
struct printbuf { char buf[32]; int failed; };
static int printbuf_memappend(struct printbuf *p, const char *s, int n) {
    if (n < 0 || n >= (int)sizeof(p->buf)) _exit(10);
    memcpy(p->buf, s, n); p->buf[n] = 0; return n;
}
'''
        harness += converter + r'''
void _start(void) {
    const double values[] = {0, 12.5, -0.125, 1000.25};
    const char *expected[] = {"0", "12.5", "-0.125", "1000.25"};
    /* The fixed arena also catches per-conversion allocations that were not
     * reclaimed: this many reentrant contexts must reuse the bounded heap. */
    for (unsigned cycle = 0; cycle < 10000; ++cycle) {
        for (unsigned i = 0; i < 4; ++i) {
            struct json_object o = {.o.c_double = values[i]};
            struct printbuf p; double parsed = 0;
            if (json_object_double_to_json_string(&o, &p) < 0 ||
                strcmp(p.buf, expected[i])) _exit(11);
            if (json_number_scan(p.buf, &parsed) != 1 || parsed != values[i]) _exit(12);
        }
    }
    _exit(0);
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            json_dir = copy_patched_json(json_dir, directory/'sdk')
            source = (json_dir/'json_object.c').read_text()
            start = source.index('static int json_object_double_to_json_string(')
            converter = source[start:source.index('struct json_object* json_object_new_double(', start)]
            # Replace the initially extracted baseline converter with the
            # checked callback from this disposable production overlay.
            old_start = harness.index('static int json_object_double_to_json_string(')
            old_end = harness.index('void _start(void)', old_start)
            harness = harness[:old_start] + converter + harness[old_end:]
            c, obj, elf = (directory / n for n in ('test.c', 'test.o', 'test.elf'))
            c.write_text(harness)
            compile_args = [str(gcc), '-mcpu=cortex-m4', '-mthumb', '-Os', '-fno-builtin',
                            '-I', str(json_dir), '-c', str(c), '-o', str(obj)]
            # A future link-only nano configuration must fail at compile time.
            bad = subprocess.run([str(gcc), '-mcpu=cortex-m4', '-mthumb',
                '-I'+str(json_dir), '-c', str(json_dir/'json_numeric/context.c'),
                '-o', str(directory/'bad-context.o')], capture_output=True, text=True)
            self.assertNotEqual(bad.returncode, 0)
            self.assertIn('require newlib-nano headers', bad.stderr)
            subprocess.run(compile_args + ['--specs=nano.specs'], check=True)
            subprocess.run([str(gcc), '-mcpu=cortex-m4', '-mthumb', '-nostartfiles',
                '--specs=nano.specs', '--specs=nosys.specs', '-u', '_printf_float',
                '-u', '_scanf_float', '-Wl,-e,_start,-Ttext=0x10000',
                '-I'+str(json_dir), str(obj), str(json_dir/'json_heap.c'),
                *map(str, (json_dir/'json_numeric').glob('*.c')), '-o', str(elf)],
                check=True)
            subprocess.run([qemu, str(elf)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
