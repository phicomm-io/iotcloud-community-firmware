"""Execute the production integer converter with poisoned errno on host and ARM.

QEMU runs the pinned nano libc ABI, not the MiCO kernel or its network driver.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#define MC_DEBUG(...) ((void)0)
#ifdef ARM_TEST
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
#endif
'''
VECTORS = r'''
static int verify(void) {
    int64_t out = 0;
    errno = ERANGE;
    if (json_parse_int64("8", &out) || out != 8) return 41;
    const struct { const char *text; int64_t value; } valid[] = {
        {"0",0}, {"-0",0}, {"1",1}, {"7",7}, {"8",8},
        {"+8",8}, {"  -0012\t",-12},
        {"2147483647",INT32_MAX}, {"-2147483648",INT32_MIN},
        {"2147483648",INT32_MAX}, {"-2147483649",INT32_MIN},
        {"999999999999999999999999999999999999999",INT32_MAX},
        {"-999999999999999999999999999999999999999",INT32_MIN}
    };
    const char *invalid[] = {"", " ", "+", "--1", "1-2", "1x", "1.2", "1 2", "\x80"};
    const int poisons[] = {0, ERANGE, EINTR, EAGAIN};
    for (unsigned round = 0; round < 10000; ++round) {
        for (unsigned e = 0; e < sizeof(poisons)/sizeof(poisons[0]); ++e) {
            for (unsigned i = 0; i < sizeof(valid)/sizeof(valid[0]); ++i) {
                errno = poisons[e];
                if (json_parse_int64(valid[i].text, &out) || out != valid[i].value) return 42;
                if (errno != poisons[e]) return 43;
            }
            for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
                errno = poisons[e]; out = 123;
                if (!json_parse_int64(invalid[i], &out) || out != 123) return 44;
                if (errno != poisons[e]) return 45;
            }
        }
    }
    return 0;
}
#ifdef ARM_TEST
void _start(void) { _exit(verify()); }
#else
int main(void) { return verify(); }
#endif
'''


def production_function():
    sdk = Path(os.environ.get('TC1_SDK_DIR', ROOT / 'build/tc1-sdk-3.6.4/mico-os'))
    source = Path(os.environ.get('PHICOMM_TEST_JSON_DIR', sdk / 'libraries/utilities/json_c')) / 'json_util.c'
    if not source.exists():
        raise unittest.SkipTest('built pinned SDK required')
    text = source.read_text()
    start = text.index('int json_parse_int64(')
    return text[start:text.index('\n#if HAVE_REALLOC', start)]


class JsonIntegerParseTest(unittest.TestCase):
    def compile_and_run(self, arm):
        gcc = Path(os.environ.get('ARM_CC', ROOT / 'build/tc1-tools/'
            'gcc-arm-none-eabi-5_4-2016q2/bin/arm-none-eabi-gcc')) if arm else shutil.which('cc')
        qemu = shutil.which('qemu-arm') or shutil.which('qemu-arm-static')
        if not gcc or (arm and (not gcc.exists() or not qemu)):
            self.skipTest('C compiler and, for ARM, pinned toolchain/QEMU required')
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            c, binary = directory / 'test.c', directory / 'test'
            c.write_text(HARNESS + production_function() + VECTORS)
            flags = ['-std=gnu99', '-Wall', '-Wextra', '-Werror', '-O1']
            if arm:
                flags += ['-mcpu=cortex-m3', '-mthumb', '-DARM_TEST', '-fno-builtin',
                          '-nostartfiles', '--specs=nano.specs', '--specs=nosys.specs',
                          '-Wl,-e,_start,-Ttext=0x10000']
            else:
                flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie']
            subprocess.run([str(gcc), *flags, str(c), '-o', str(binary)], check=True)
            subprocess.run(([qemu] if arm else []) + [str(binary)], check=True, timeout=30)

    def test_integer_conversion_ignores_errno_host_sanitizers(self):
        self.compile_and_run(False)

    def test_integer_conversion_ignores_errno_arm_nano(self):
        self.compile_and_run(True)


if __name__ == '__main__':
    unittest.main()
