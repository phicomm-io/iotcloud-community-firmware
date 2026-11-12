"""Compile actual product headers against independently frozen pre-auth layouts."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class AuthStorage(unittest.TestCase):
 def test_frozen_prefixes(self):
  for model in ('A1','M1','TC1'):
   with self.subTest(model=model), tempfile.TemporaryDirectory() as temp:
    exe=Path(temp)/'probe'
    shim=Path(temp)/'shim';shim.mkdir()
    generated=Path(temp)/'common/generated';generated.mkdir(parents=True)
    (generated/'build_version.h').write_text('#define FIRMWARE_VERSION "host-layout-test"\n')
    json_dir=Path(os.environ.get('PHICOMM_TEST_JSON_DIR',ROOT/'build'/f'{model.lower()}-sdk/mico-os/libraries/utilities/json_c'))
    subprocess.run(['cc','-std=gnu99','-DHOST_'+model,'-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I'+str(shim),'-I'+str(ROOT),'-I'+str(ROOT/'tests/host'),'-I'+str(ROOT/model),'-I'+str(json_dir),str(ROOT/'tests/storage_auth'/f'{model.lower()}.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
