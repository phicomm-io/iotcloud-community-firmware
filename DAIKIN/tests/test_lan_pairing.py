"""Compile actual DAIKIN UDP adapters and shared MiCO MP encoder/decoder/core."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[2]
class LanPairing(unittest.TestCase):
 def test_production_interop(self):
  with tempfile.TemporaryDirectory() as directory:
   p=Path(directory);objects=[]
   for name in ('local_auth','typed_wire','wire_buffer'):
    obj=p/(name+'.o');subprocess.run(['cc','-g','-fsanitize=address,undefined','-fno-pie','-I'+str(ROOT/'common'),'-c',str(ROOT/'common'/f'{name}.c'),'-o',str(obj)],check=True);objects.append(str(obj))
   (p/'lan.c').write_text('#include "lan_wire.inc"\n');obj=p/'lan.o';subprocess.run(['cc','-g','-fsanitize=address,undefined','-fno-pie','-I'+str(ROOT/'common'),'-c',str(p/'lan.c'),'-o',str(obj)],check=True);objects.append(str(obj))
   exe=p/'test';subprocess.run(['c++','-std=c++11','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(ROOT/'common'),'-I'+str(ROOT/'DAIKIN/include'),'-I'+str(ROOT/'DAIKIN/.pio/libdeps/bry88ab/ArduinoJson/src'),str(ROOT/'DAIKIN/tests/sessions.cpp'),*objects,'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
