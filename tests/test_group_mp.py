"""Actual native LAN codec, owned snapshots and hostile packet boundaries."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class GroupMP(unittest.TestCase):
 def test_codec(self):
  core=Path(os.environ.get('PHICOMM_WIRE_CORE',ROOT/'common'))
  with tempfile.TemporaryDirectory() as temp:
   exe=Path(temp)/'probe'
   subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-error=misleading-indentation','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I'+str(ROOT),'-I'+str(core),str(ROOT/'tests/group_mp/probe.c'),str(core/'typed_wire.c'),str(core/'wire_buffer.c'),'-lm','-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True)
