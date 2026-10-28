"""Compile actual DAIKIN UDP adapters and shared MiCO MP encoder/decoder/core."""
from pathlib import Path
import os,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[2]
class LanPairing(unittest.TestCase):
 def test_production_interop(self):
  candidates=[Path(os.environ['DAIKIN_ARDUINOJSON_SOURCE'])] if os.environ.get('DAIKIN_ARDUINOJSON_SOURCE') else [ROOT/'DAIKIN/.pio/libdeps'/name/'ArduinoJson/src' for name in ('bry88ab','bry88ab-hub')]
  codec=next((path for path in candidates if (path/'ArduinoJson.h').is_file()),None)
  if codec is None:raise RuntimeError('Build a DAIKIN target or set DAIKIN_ARDUINOJSON_SOURCE to the pinned ArduinoJson source')
  with tempfile.TemporaryDirectory() as directory:
   p=Path(directory);objects=[]
   for name in ('local_auth','typed_wire','wire_buffer'):
    obj=p/(name+'.o');subprocess.run(['cc','-g','-fsanitize=address,undefined','-fno-pie','-I'+str(ROOT/'common'),'-c',str(ROOT/'common'/f'{name}.c'),'-o',str(obj)],check=True);objects.append(str(obj))
   (p/'lan.c').write_text('#include "lan_wire.inc"\n');obj=p/'lan.o';subprocess.run(['cc','-g','-fsanitize=address,undefined','-fno-pie','-I'+str(ROOT/'common'),'-c',str(p/'lan.c'),'-o',str(obj)],check=True);objects.append(str(obj))
   exe=p/'test';subprocess.run(['c++','-std=c++11','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(ROOT/'common'),'-I'+str(ROOT/'DAIKIN/include'),'-I'+str(codec),str(ROOT/'DAIKIN/tests/sessions.cpp'),*objects,'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
