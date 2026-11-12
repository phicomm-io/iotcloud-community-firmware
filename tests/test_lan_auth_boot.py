"""Production auth/client shared ABI, including TC1's historical MAC casing."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class LanAuthBoot(unittest.TestCase):
 def test_uppercase_product_pair_resume_actual_sender(self):
  with tempfile.TemporaryDirectory() as d:
   exe=Path(d)/'pair';cmd=[os.environ.get('CC','cc'),'-std=gnu99','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-misleading-indentation','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I'+str(ROOT/'tests/host'),'-I'+str(ROOT),str(ROOT/'tests/lan_business/pair_sender.c'),str(ROOT/'common/typed_wire.c'),str(ROOT/'common/wire_buffer.c'),'-o',str(exe)]
   built=subprocess.run(cmd,capture_output=True,text=True,timeout=30);self.assertEqual(built.returncode,0,built.stderr)
   ran=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30);self.assertEqual(ran.returncode,0,ran.stderr);print(ran.stdout.strip())
