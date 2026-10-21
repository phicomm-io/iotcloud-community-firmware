"""Actual ARM/newlib full-JSON numeric parity and allocator-NULL coverage."""
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from tests.sdk_json import copy_patched_json

ROOT = Path(__file__).resolve().parents[1]


class JsonNumericChecked(unittest.TestCase):
    def test_pinned_arm_numeric_closure(self):
        compiler = Path(os.environ.get('ARM_CC', ROOT/'build/tc1-tools/'
            'gcc-arm-none-eabi-5_4-2016q2/bin/arm-none-eabi-gcc'))
        qemu = shutil.which('qemu-arm') or shutil.which('qemu-arm-static')
        self.assertTrue(compiler.exists(), 'pinned ARM compiler required')
        self.assertTrue(qemu, 'qemu-arm required')
        for model in os.environ.get('JSON_NUMERIC_MODELS', 'A1,M1,TC1').split(','):
            sdk = Path(os.environ.get(model+'_SDK_DIR', ROOT/('build/tc1-sdk-3.6.4/mico-os'
                if model == 'TC1' else 'build/'+model.lower()+'-sdk/mico-os')))
            with self.subTest(model=model), tempfile.TemporaryDirectory(prefix='json-checked-'+model+'-') as tmp:
                directory = Path(tmp)
                json = copy_patched_json(sdk/'libraries/utilities/json_c', directory/'sdk')
                flags = ['-mcpu=cortex-m4', '-mthumb', '-Os', '-g', '-std=gnu11',
                    '-fno-builtin', '-fstack-usage', '--specs=nano.specs',
                    '-DJSON_NUMERIC_TEST_TOKEN_BOUND', '-I'+str(ROOT/'common'),
                    '-I'+str(json), '-I'+str(ROOT/'tests/stubs')]
                if model != 'TC1':
                    flags += ['-mfpu=fpv4-sp-d16', '-mfloat-abi=hard']
                sources = [ROOT/'tests/json_numeric_checked_harness.c', ROOT/'common/json_owned.c',
                    *sorted(json.glob('*.c')), *sorted((json/'json_numeric').glob('*.c'))]
                objects = []
                for source in sources:
                    obj = directory/(source.stem+'.o')
                    subprocess.run([str(compiler), *flags, '-c', str(source), '-o', str(obj)],
                        check=True, capture_output=True, text=True)
                    objects.append(obj)
                # The oracle may link native libc. The private implementation
                # itself must never reach its unchecked allocating converters.
                nm = compiler.with_name('arm-none-eabi-nm')
                numeric_objects = [directory/(name+'.o') for name in
                    ('context', 'mprec', 'dtoa', 'strtod', 'match', 'scan', 'format')]
                undefined = subprocess.check_output([str(nm), '-u', *map(str, numeric_objects)], text=True)
                forbidden = re.findall(r'\b(?:_?(?:malloc|calloc|realloc|free)(?:_r)?|_?(?:dtoa|strtod)(?:_r)?|_?(?:s?scanf|s?n?printf)(?:_r)?)$', undefined, re.M)
                self.assertEqual(forbidden, [], undefined)
                elf = directory/'test.elf'
                wrapped = ('malloc', 'calloc', 'realloc', 'free',
                           '_malloc_r', '_calloc_r', '_realloc_r', '_free_r')
                subprocess.run([str(compiler), *flags, '-nostartfiles', '--specs=nosys.specs',
                    '-u', '_printf_float', '-u', '_scanf_float', '-Wl,-e,_start,-Ttext=0x10000',
                    *['-Wl,--wrap='+name for name in wrapped], *map(str, objects),
                    '-lm', '-o', str(elf)], check=True, text=True)
                if os.environ.get('JSON_NUMERIC_ARTIFACTS'):
                    shutil.copytree(directory, Path(os.environ['JSON_NUMERIC_ARTIFACTS'])/model, dirs_exist_ok=True)
                result = subprocess.run([qemu, str(elf)], capture_output=True, text=True, timeout=90)
                self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                self.assertIn('PASS numeric parity, fullJSON, fail-each allocation, cleanup;', result.stdout)
                print(model, result.stdout.strip())


if __name__ == '__main__':
    unittest.main()
