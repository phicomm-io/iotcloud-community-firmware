import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from compiler_profile import invocation
from prepare_toolchain import GCC_DIR, profile


class CompilerProfileTests(unittest.TestCase):
    def test_codegen_uses_gcc14_with_gcc5_newlib(self):
        actual, args, mode = invocation(Path('/cache'), 'arm-none-eabi-gcc', ['-c', 'input.c', '-o', 'input.o'])
        self.assertEqual((str(actual), mode), ('/cache/gcc14/bin/arm-none-eabi-gcc', 'compile'))
        self.assertIn('--sysroot=/cache/gcc5/arm-none-eabi', args)
        self.assertIn('/cache/gcc5/arm-none-eabi/include', args)

    def test_link_and_library_queries_keep_gcc5_runtime(self):
        for args in (['input.o', '-lc_nano', '-o', 'output.elf'], ['-print-libgcc-file-name'], ['-dumpmachine']):
            actual, forwarded, mode = invocation(Path('/cache'), 'arm-none-eabi-gcc', args)
            self.assertEqual(str(actual), '/cache/gcc5/bin/arm-none-eabi-gcc')
            self.assertEqual(forwarded, args)
            self.assertEqual(mode, 'link')

    def test_cpp_uses_matching_gcc5_headers(self):
        actual, args, mode = invocation(Path('/cache'), 'arm-none-eabi-g++', ['-E', 'input.cpp'])
        self.assertEqual(mode, 'compile')
        self.assertIn('-nostdinc++', args)
        self.assertIn('/cache/gcc5/arm-none-eabi/include/c++/5.4.1', args)

    def test_queries_and_assembly_selection(self):
        for flag in ('--version', '-dumpversion', '-dumpfullversion'):
            actual, args, mode = invocation(Path('/cache'), 'arm-none-eabi-gcc', [flag])
            self.assertEqual(str(actual), '/cache/gcc14/bin/arm-none-eabi-gcc')
            self.assertEqual(mode, 'query')
        self.assertEqual(invocation(Path('/cache'), 'arm-none-eabi-gcc', ['-c', 'start.S'])[2], 'compile')

    def test_shared_wrapper_survives_mico_path_and_repeated_preparation(self):
        with tempfile.TemporaryDirectory() as temporary:
            home = Path(temporary)
            (home / 'python2').mkdir()
            for version in ('gcc5', 'gcc14'):
                binaries = home / version / 'bin'
                binaries.mkdir(parents=True)
                for driver in ('arm-none-eabi-gcc', 'arm-none-eabi-g++', 'arm-none-eabi-nm'):
                    binary = binaries / driver
                    binary.write_text('#!' + str(Path(sys.executable).resolve()) + '\n'
                        'import json,sys\nprint(json.dumps(sys.argv))\n')
                    binary.chmod(0o755)
            selected = profile(home, 'gcc14')
            self.assertEqual(profile(home, 'gcc14'), selected)
            log = home / 'compiler.jsonl'
            environment = dict(os.environ, PATH='/no-python-on-mico-path', PHICOMM_COMPILER_LOG=str(log))
            for arguments, expected in [(['-dumpversion'], 'gcc14'),
                    (['@c_opts', 'main.c', '-o', 'main.o'], 'gcc14'),
                    (['main.o', '-o', 'main.elf'], 'gcc5')]:
                result = subprocess.check_output([str(selected / GCC_DIR / 'bin/arm-none-eabi-gcc'),
                    *arguments], env=environment, text=True)
                self.assertEqual(json.loads(result)[0], str(home / expected / 'bin/arm-none-eabi-gcc'))
            entries = [json.loads(line) for line in log.read_text().splitlines()]
            self.assertEqual([entry['mode'] for entry in entries], ['query', 'compile', 'link'])
            self.assertIn('-nostdinc', entries[1]['argv'])
