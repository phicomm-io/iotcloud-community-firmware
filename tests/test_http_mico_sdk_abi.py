"""Compile the production port with each real SDK, then execute its fcntl path.

No host mico.h/socket constants are used. QEMU proves the application ABI and
failure handling only; it does not execute the closed MiCO socket kernel.
Set PHICOMM_HTTP_ABI_ARTIFACT_DIR to retain ARM objects, ELF and disassembly.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <stdarg.h>
#include "mico_port.c"
static unsigned calls, failure, bad;
int fcntl(int fd, int cmd, ...) {
    va_list ap; va_start(ap, cmd); int value = va_arg(ap, int); va_end(ap);
    ++calls;
    /* Numeric expectations are the audited kernel ABI, not header aliases. */
    if (fd != 3 || cmd != (calls == 2 ? 4 : 3) ||
        value != (calls == 2 ? 1 : 0)) bad = 1;
    if (calls == failure) return -1;
    return calls == 3 && failure != 4 ? 1 : 0;
}
static int check_abi(void) {
    for (failure = 0; failure <= 4; ++failure) {
        calls = bad = 0;
        int rc = nonblock(3);
        if (bad || (failure ? rc != -1 : rc != 0)) return 10 + failure;
        unsigned expected = failure && failure < 3 ? failure : 3;
        if (calls != expected) return 20 + failure;
    }
    return 0;
}
void _start(void) {
    register int r0 __asm__("r0") = check_abi();
    register int r7 __asm__("r7") = 1;
    __asm__ volatile("svc 0" :: "r"(r0), "r"(r7) : "memory");
    for (;;) {}
}
'''


class MicoSocketABI(unittest.TestCase):
    def check_model(self, model):
        tc1 = model == 'TC1'
        sdk_name = 'tc1-sdk-3.6.4' if tc1 else model.lower() + '-sdk'
        sdk = Path(os.environ.get('PHICOMM_' + model + '_SDK',
                                  ROOT / 'build' / sdk_name / 'mico-os'))
        cc = Path(os.environ.get('ARM_CC', ROOT / 'build/tc1-tools/'
            'gcc-arm-none-eabi-5_4-2016q2/bin/arm-none-eabi-gcc'))
        if not (sdk / 'include/mico.h').exists() or not cc.exists():
            self.skipTest(model + ' pinned SDK and ARM compiler required')
        mcu, board = ('MW3xx', 'MK3031') if tc1 else ('MX1290', 'MK3080B')
        includes = ['include', 'platform', 'platform/include',
                    'platform/Cortex-M4', 'platform/Cortex-M4/CMSIS',
                    'platform/MCU/' + mcu, 'platform/MCU/' + mcu + '/peripherals',
                    'platform/MCU/' + mcu + '/interface', 'board/' + board,
                    'MiCO/system', 'MiCO/system/include', 'libraries/utilities',
                    'MiCO/security', 'MiCO/security/TLS/mocSSL']
        if tc1:
            includes += ['platform/MCU/MW3xx/peripherals/sdk/src/incl/' + p
                         for p in ('sdk', 'platform/os/micortos', 'sdk/drivers',
                                   'sdk/drivers/mw300', 'sdk/drivers/mw300/regs')]
        flags = ['-std=gnu11', '-Os', '-mthumb', '-mcpu=cortex-m3' if tc1
                 else '-mcpu=cortex-m4', '-DMOC=1', '-ffunction-sections',
                 '-fdata-sections', '-fno-inline', '-I' + str(ROOT / model),
                 '-I' + str(ROOT / 'common/httpd'),
                 '-DPH_HTTP_CLIENT_SLOTS=' + ('4' if model == 'M1' else '3')]
        flags += ['-DMW310', '-DCONFIG_VARIANT_MW310'] if tc1 else [
            '-DCONFIG_CPU_MX1290', '-mfpu=fpv4-sp-d16', '-mfloat-abi=hard']
        flags += ['-I' + str(sdk / p) for p in includes]
        with tempfile.TemporaryDirectory() as temp:
            retained = os.environ.get('PHICOMM_HTTP_ABI_ARTIFACT_DIR')
            directory = Path(retained).resolve() / model if retained else Path(temp)
            directory.mkdir(parents=True, exist_ok=True)
            production = ROOT / 'common/httpd/mico_port.c'
            obj = directory / 'mico_port.o'
            # Compile the unmodified complete production translation unit.
            subprocess.run([str(cc), *flags, '-c', str(production), '-o', str(obj)],
                           check=True, timeout=30)
            harness = directory / 'fcntl_abi.c'
            harness.write_text(HARNESS)
            binary = directory / 'fcntl_abi.elf'
            subprocess.run([str(cc), *flags, '-nostdlib', '-Wl,--gc-sections',
                            '-Wl,-e,_start,-Ttext=0x10000', str(harness),
                            '-o', str(binary)], check=True, timeout=30)
            dis = subprocess.check_output([str(cc.with_name('arm-none-eabi-objdump')),
                                           '-d', str(binary)], text=True)
            (directory / 'fcntl_abi.dis').write_text(dis)
            self.assertIn('<nonblock>', dis)
            qemu = shutil.which('qemu-arm') or shutil.which('qemu-arm-static')
            if not qemu:
                self.skipTest('ARM compilation passed; QEMU runtime required')
            subprocess.run([qemu, str(binary)], check=True, timeout=30)
            # Reintroducing the actual offending include must fail production
            # compilation, even if a host-stub regression would still pass.
            polluted = directory / 'polluted.c'
            polluted.write_text(production.read_text().replace(
                '#include "http_trace_hooks.h"',
                '#include "http_trace_hooks.h"\n#include <fcntl.h>'))
            rejected = subprocess.run([str(cc), *flags, '-c', str(polluted),
                                      '-o', str(directory / 'polluted.o')],
                                     capture_output=True, text=True, timeout=30)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn('HTTP port requires the MiCO socket fcntl ABI', rejected.stderr)

    def test_a1(self):
        self.check_model('A1')

    def test_m1(self):
        self.check_model('M1')

    def test_tc1(self):
        self.check_model('TC1')


if __name__ == '__main__':
    unittest.main()
