"""ASan/UBSan regression of the production HTTP JSON ownership adapter."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from tests.sdk_json import copy_patched_json

ROOT = Path(__file__).resolve().parents[1]

class OwnedJson(unittest.TestCase):
    def test_owned_json(self):
        sdk = Path(os.environ.get('PHICOMM_TEST_JSON_DIR', ROOT / 'build/a1-sdk/mico-os/libraries/utilities/json_c'))
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp)
            json = copy_patched_json(sdk, temp/'sdk')
            for source in json.glob('*.c'):
                source.write_text(source.read_text().replace('%ld', '%d'))
            binary = temp / 'owned-json'
            subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu99', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                '-I'+str(ROOT), '-I'+str(ROOT/'tests/stubs'), '-I'+str(json),
                str(ROOT/'tests/http_core/json_owned.c'), str(ROOT/'common/httpd/product.c'),str(ROOT/'common/json_owned.c'),
                *map(str, json.glob('*.c')), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=30)

if __name__ == '__main__':
    unittest.main()
