"""Exact production number conversion against pinned ARM 5.4 libc_nano.

Host glibc does not have this private newlib cache. Negative controls retain
the old reclaim-only call sites and reproduce their requested-byte leaks.
QEMU runs libc conversion and allocation ownership, not the MiCO kernel.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from tests.sdk_json import copy_patched_json

ROOT = Path(__file__).resolve().parents[1]


def selected_models():
    models = tuple(part.strip() for part in os.environ.get(
        'JSON_NUMBER_CACHE_MODELS', 'A1,M1,TC1').split(','))
    if any(model not in ('A1', 'M1', 'TC1') for model in models):
        raise ValueError('JSON_NUMBER_CACHE_MODELS must be a nonempty comma-separated '
                         'selection of A1,M1,TC1')
    return models


class JsonNumberCache(unittest.TestCase):
    def test_pinned_nano_private_cache_cleanup_and_negative_controls(self):
        models = selected_models()
        compiler = Path(os.environ.get('ARM_CC', ROOT/'build/tc1-tools/'
            'gcc-arm-none-eabi-5_4-2016q2/bin/arm-none-eabi-gcc'))
        qemu = shutil.which('qemu-arm') or shutil.which('qemu-arm-static')
        self.assertTrue(compiler.exists(), 'pinned ARM 5.4 compiler required')
        self.assertIsNotNone(qemu, 'qemu-arm or qemu-arm-static required')
        for model in models:
            with self.subTest(model=model):
                self.exercise_model(compiler, qemu, model)

    def sdk_source(self, model):
        sdk = ROOT/('build/tc1-sdk-3.6.4/mico-os' if model == 'TC1' else
                   'build/'+model.lower()+'-sdk/mico-os')
        return Path(os.environ.get(model+'_SDK_DIR', sdk))/'libraries/utilities/json_c'

    def exercise_model(self, compiler, qemu, model):
        sdk = self.sdk_source(model)
        with tempfile.TemporaryDirectory(prefix='json-number-cache-') as temporary:
            directory = Path(temporary)
            json = copy_patched_json(sdk, directory/'sdk')
            source = (json/'json_object.c').read_text()
            start = source.index('static int json_object_double_to_json_string(')
            converter = source[start:source.index('struct json_object* json_object_new_double(', start)]
            self.assertIn('json_number_reclaim(&context)', converter)
            header = (json/'json_number_reent.h').read_text()
            self.assertIn('json_number_reclaim(&context)', header)
            template = (ROOT/'tests/json_number_cache_harness.c').read_text()
            old = directory/'old'; old.mkdir()
            (old/'json_number_reent.h').write_text(header.replace(
                'json_number_reclaim(&context);', '_reclaim_reent(&context);'))
            flags = ['-mcpu=cortex-m4', '-mthumb', '-Os', '-g', '-std=gnu11',
                     '-fno-builtin', '-fstack-usage', '--specs=nano.specs']
            if model != 'TC1':
                flags += ['-mfpu=fpv4-sp-d16', '-mfloat-abi=hard']
            link = ['-nostartfiles', '--specs=nosys.specs', '-u', '_printf_float',
                    '-u', '_scanf_float', '-Wl,-e,_start,-Ttext=0x10000',
                    *['-Wl,--wrap='+name for name in ('malloc', 'calloc', 'free',
                      'realloc', '_malloc_r', '_calloc_r', '_free_r', '_realloc_r')]]
            for case in range(4):
                c, obj, elf = [directory/(str(case)+suffix) for suffix in ('.c', '.o', '.elf')]
                body = converter if not case else converter.replace(
                    'json_number_reclaim(&context);', '_reclaim_reent(&context);')
                c.write_text(template.replace('/* PRODUCTION_DOUBLE_CONVERTER */', body))
                includes = ['-I'+str(old if case else json)]
                defines = ['-DNEGATIVE_CASE='+str(case)] if case else []
                subprocess.run([str(compiler), *flags, *includes, *defines,
                    '-c', str(c), '-o', str(obj)], check=True, capture_output=True, text=True)
                subprocess.run([str(compiler), *flags, *link, str(obj), '-o', str(elf)],
                               check=True, capture_output=True, text=True)
                result = subprocess.run([qemu, str(elf)], capture_output=True,
                                        text=True, timeout=30)
                self.assertEqual(result.returncode, 73 if case else 0, result.stdout+result.stderr)
                if not case:
                    self.assertIn('33000 conversions, live bytes=0 blocks=0', result.stdout)
                else:
                    expected = {1: '84 blocks=3', 2: '172 blocks=5', 3: '28 blocks=1'}[case]
                    self.assertIn('residual bytes='+expected, result.stdout)
                print(model+': '+result.stdout.strip())
            # Record emitted helper/converter frames from this pinned compiler.
            stack = (directory/'0.su').read_text()
            print(model+' ARM cache helper/converter stack frames: '+', '.join(
                line for line in stack.splitlines()
                if 'json_number_reclaim' in line or 'json_object_double_to_json_string' in line))

    def test_glibc_preprocessed_production_unchanged(self):
        # Only __NEWLIB__ code changed. Compare complete object/tokener inputs
        # with the previous seven-overlay stack, not a handwritten host stub.
        for model in selected_models():
            with self.subTest(model=model), tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary)
                fixed = copy_patched_json(self.sdk_source(model), directory/'fixed')
                shutil.copytree(directory/'fixed', directory/'before')
                subprocess.run(['git', 'apply', '--reverse', '--ignore-whitespace',
                    str(ROOT/'common/json-number-cache-cleanup.patch')],
                    cwd=directory/'before', check=True, capture_output=True)
                before = directory/'before/libraries/utilities/json_c'
                for name in ('json_object.c', 'json_tokener.c'):
                    outputs = [subprocess.check_output([os.environ.get('CC','cc'),
                        '-E', '-P', '-I'+str(ROOT/'tests/stubs'), '-I'+str(json),
                        '-x', 'c', '-'], input=(json/name).read_bytes()) for json in (before, fixed)]
                    self.assertEqual(outputs[0], outputs[1], model+' '+name)
                    print(model+' glibc unchanged '+name+' sha256='+hashlib.sha256(outputs[1]).hexdigest())


if __name__ == '__main__':
    unittest.main()
