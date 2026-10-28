"""Production HTTP emitter converts both JSON roots to MessagePack."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[2]
class HttpResponse(unittest.TestCase):
 def test_production_emitter_object_array_empty_and_error(self):
  with tempfile.TemporaryDirectory() as directory:
   p=Path(directory);source=(ROOT/'DAIKIN/src/http.inc').read_text();(p/'response.inc').write_text(source[:source.index('static bool authorized(')])
   exe=p/'test';subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie','-I'+str(p),'-I'+str(ROOT/'DAIKIN/include'),'-I'+str(ROOT/'DAIKIN/.pio/libdeps/bry88ab-hub/ArduinoJson/src'),str(ROOT/'DAIKIN/tests/hub/http_response.cpp'),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
