"""Mixed cloud codecs through direct application bus, not UDP wire acceptance."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from DAIKIN.tests.arduinojson import codec_source
ROOT=Path(__file__).resolve().parents[1]
class MixedHub(unittest.TestCase):
 def test_bus_business_roundtrips(self):
  codec=codec_source()
  with tempfile.TemporaryDirectory() as d:
   exe=Path(d)/'mixed'
   subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(ROOT/'include'),'-I'+str(codec),str(ROOT/'tests/hub/mixed_devices.cpp'),'-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True,timeout=30)
