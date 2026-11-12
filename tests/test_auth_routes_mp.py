"""Exercise real HTTP and MiCO auth adapters with the MessagePack codec."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class AuthRoutes(unittest.TestCase):
 def test_real_adapters(self):
  with tempfile.TemporaryDirectory() as d:
   exe=Path(d)/'auth'
   subprocess.run([os.environ.get('CC','cc'),'-std=gnu99','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I'+str(ROOT/'tests/host'),str(ROOT/'tests/auth_routes_mp/probe.c'),str(ROOT/'common/typed_wire.c'),str(ROOT/'common/wire_buffer.c'),'-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True)
