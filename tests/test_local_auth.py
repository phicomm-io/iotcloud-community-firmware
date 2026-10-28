"""Compile the production heap-free local auth core with sanitizer coverage."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LocalAuthTest(unittest.TestCase):
    def test_sessions_pairing_and_concurrent_admission(self):
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / 'local_auth'
            subprocess.run([
                os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g', '-pthread',
                str(ROOT / 'tests/local_auth/test_local_auth.c'),
                str(ROOT / 'common/local_auth.c'), '-o', str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)
