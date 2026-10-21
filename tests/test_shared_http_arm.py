"""Run the production HTTP fault harness with the pinned ARM nano libc ABI.

QEMU user mode exercises the core and integer/pointer ABI. It does not emulate
MiCO, Wi-Fi, socket ownership in its closed kernel, or physical reset behavior.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
STARTUP = r'''
#include <stddef.h>
static unsigned char arena[128 * 1024];
static size_t used;
void *_sbrk(ptrdiff_t n) {
    if (n < 0 || (size_t)n > sizeof(arena) - used) return (void *)-1;
    void *p = arena + used; used += (size_t)n; return p;
}
void _exit(int code) {
    register int r0 __asm__("r0") = code;
    register int r7 __asm__("r7") = 1;
    __asm__ volatile("svc 0" :: "r"(r0), "r"(r7) : "memory");
    for (;;) {}
}
int ph_http_fault_main(void);
void _start(void) { _exit(ph_http_fault_main()); }
'''


class SharedHTTPArmTest(unittest.TestCase):
    def test_production_faults_arm_nano(self):
        compiler = Path(os.environ.get('ARM_CC', ROOT / 'build/tc1-tools/'
            'gcc-arm-none-eabi-5_4-2016q2/bin/arm-none-eabi-gcc'))
        qemu = shutil.which('qemu-arm') or shutil.which('qemu-arm-static')
        if not compiler.exists() or not qemu:
            self.skipTest('Pinned ARM compiler and QEMU required')
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            startup = directory / 'startup.c'
            startup.write_text(STARTUP)
            for cpu in ('cortex-m3', 'cortex-m4'):
                with self.subTest(cpu=cpu):
                    binary = directory / cpu
                    subprocess.run([
                        str(compiler), '-std=gnu99', '-Wall', '-Wextra', '-Werror',
                        '-O1', '-mcpu='+cpu, '-mthumb', '-fno-builtin',
                        '-Dmain=ph_http_fault_main', '-nostartfiles',
                        '--specs=nano.specs', '--specs=nosys.specs',
                        '-Wl,-e,_start,-Ttext=0x10000',
                        '-I'+str(ROOT / 'common/httpd'), str(startup),
                        str(ROOT / 'tests/http_core/fault.c'),
                        str(ROOT / 'common/httpd/core.c'),
                        str(ROOT / 'common/httpd/parser.c'), '-o', str(binary),
                    ], check=True, timeout=30)
                    subprocess.run([qemu, str(binary)], check=True, timeout=30)


if __name__ == '__main__':
    unittest.main()
