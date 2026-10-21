"""Pinned SDK JSON growth avoids the kernel's unsafe unsized realloc.

The negative control deliberately reproduces that allocator's overread under
ASan. Host proofs establish memory boundaries, not the cause of DUT heap loss.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tests.sdk_json import copy_patched_json

ROOT = Path(__file__).resolve().parents[1]


class JsonSizedResize(unittest.TestCase):
    def test_actual_sdk_growth_and_unsafe_allocator_negative_control(self):
        source = Path(os.environ.get('PHICOMM_TEST_JSON_DIR',
            ROOT/'build/tc1-sdk-3.6.4/mico-os/libraries/utilities/json_c'))
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            json = copy_patched_json(source, temp/'sdk')
            # The production SDK is 32-bit; host-only format adaptation.
            for path in json.glob('*.c'):
                path.write_text(path.read_text().replace('%ld', '%d'))
            cc = os.environ.get('CC', 'cc')
            flags = ['-std=gnu99', '-g', '-O1', '-fsanitize=address,undefined',
                     '-fno-omit-frame-pointer', '-no-pie', '-fno-builtin',
                     '-I'+str(ROOT/'tests/stubs'), '-I'+str(ROOT/'common'), '-I'+str(json)]
            binary = temp/'resize'
            subprocess.run([cc, *flags, str(ROOT/'tests/test_json_sized_resize.c'),
                str(ROOT/'common/json_owned.c'), *map(str, json.glob('*.c')),
                *['-Wl,--wrap='+n for n in
                  ('malloc', 'calloc', 'free', 'strdup', 'realloc', 'json_heap_resize')],
                '-o', str(binary)], check=True, capture_output=True, text=True)
            safe = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(safe.returncode, 0, safe.stdout+safe.stderr)
            self.assertIn('native realloc=0, live=0', safe.stdout)
            unsafe = subprocess.run([str(binary), 'unsafe'], capture_output=True,
                                    text=True, timeout=30)
            self.assertNotEqual(unsafe.returncode, 0)
            self.assertIn('AddressSanitizer: heap-buffer-overflow', unsafe.stderr)
            # Inspect every compiled JSON translation unit, including conditional
            # code not reached by the runtime vectors. No native unsized calls.
            for path in json.glob('*.c'):
                obj = temp/(path.stem+'.o')
                subprocess.run([cc, *flags, '-c', str(path), '-o', str(obj)],
                               check=True, capture_output=True)
                symbols = subprocess.check_output(['nm', '-u', str(obj)], text=True)
                self.assertNotRegex(symbols, r'\b(?:realloc|json_heap_realloc|rpl_realloc)\b')
            # The ban is local to implementation files: normal application
            # include order remains legal even when stdlib follows json_heap.h.
            probe = temp/'application.c'
            probe.write_text('#include "json_heap.h"\n#include <stdlib.h>\n')
            subprocess.run([cc, *flags, '-c', str(probe), '-o', str(temp/'app.o')],
                           check=True, capture_output=True)
            # A future unsized addition must fail without depending on -Werror.
            for name in ('arraylist', 'json_object', 'json_tokener', 'json_util',
                         'linkhash', 'printbuf', 'json_heap'):
                probe.write_text((json/(name+'.c')).read_text()+
                    '\nvoid *forbidden(void *p) { return realloc(p, 16); }\n')
                result = subprocess.run([cc, *flags, '-c', str(probe),
                    '-o', str(temp/'bad.o')], capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0, name)
                self.assertIn('poisoned', result.stderr, name)
            # This alternate platform contract cannot silently restore rpl_realloc.
            config = json/'config.h'
            config.write_text(re.sub(r'(#define HAVE_REALLOC\s+)1', r'\g<1>0', config.read_text()))
            result = subprocess.run([cc, *flags, '-c', str(json/'json_util.c'),
                '-o', str(temp/'fallback.o')], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('unsized fallback is unsafe', result.stderr)
            print(safe.stdout.strip())
            print('JSON unsafe allocator negative control: ASan overread detected')


if __name__ == '__main__':
    unittest.main()
