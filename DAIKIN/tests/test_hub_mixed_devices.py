"""Mixed cloud codecs through direct application bus, not UDP wire acceptance."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class MixedHub(unittest.TestCase):
 def test_bus_business_roundtrips(self):
  codec=Path(os.environ.get('DAIKIN_ARDUINOJSON_SOURCE',str(ROOT/'.pio/libdeps/bry88ab-hub/ArduinoJson/src')))
  with tempfile.TemporaryDirectory() as d:
   exe=Path(d)/'mixed'
   subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(ROOT/'include'),'-I'+str(codec),str(ROOT/'tests/hub/mixed_devices.cpp'),'-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True,timeout=30)
