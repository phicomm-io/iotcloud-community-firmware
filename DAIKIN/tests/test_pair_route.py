"""Owner pair retry pins the selected IP before starting auth; IO/auth machine are stubs."""
from pathlib import Path
import os,subprocess
from . import test_native_publish as cache_fixture
from .test_local_control_priority import function
ROOT=cache_fixture.ROOT
class PairRoute(cache_fixture.NativePublish):
 @classmethod
 def setUpClass(cls):
  super().setUpClass();temp=Path(cls.temp.name)
  (temp/'pair-route.inc').write_text(function(ROOT/'DAIKIN/src/lan_pairing.inc','static bool lanPairStart('))
  codec=ROOT/'DAIKIN/.pio/libdeps/bry88ab-hub/ArduinoJson/src';cls.binary=temp/'pair-route'
  subprocess.run(['c++','-std=c++11','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-return-type','-Wno-deprecated-copy','-Wno-sign-compare','-fsanitize=address,undefined','-fno-pie','-no-pie','-ffunction-sections','-fdata-sections','-DARDUINOJSON_ENABLE_ARDUINO_STRING=1','-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0','-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0','-DARDUINOJSON_ENABLE_PROGMEM=0','-DIOTCLOUD_HUB=1','-I'+str(temp),'-I'+str(ROOT/'DAIKIN/include'),'-I'+str(codec),str(ROOT/'DAIKIN/tests/hub/pair_route.cpp'),str(temp/'WString.cpp'),'-Wl,--gc-sections','-o',str(cls.binary)],check=True)
 def test_production_event_cache_and_numbered_diagnostics(self):
  subprocess.run([str(self.binary)],check=True,timeout=30,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
