"""Production v4 server+DAIKIN adapter; auth/UDP IO/execute are explicit host stubs."""
import os,subprocess
from pathlib import Path
from . import test_native_publish as cache_fixture
ROOT=cache_fixture.ROOT
class LanBusiness(cache_fixture.NativePublish):
 @classmethod
 def setUpClass(cls):
  super().setUpClass()
  temp=Path(cls.temp.name);core=ROOT/'common'
  (temp/'lan-business.inc').write_text((ROOT/'DAIKIN/src/lan_business.inc').read_text().replace('#include "../../common/lan_wire_v4.h"','#include "lan_wire_v4.h"'))
  (temp/'mbedtls').mkdir();(temp/'mbedtls/sha256.h').write_text('#include <openssl/sha.h>\nstatic int mbedtls_sha256_ret(const unsigned char*p,size_t n,unsigned char*out,int){return SHA256(p,n,out)?0:-1;}\n')
  (temp/'v4.c').write_text('#include "lan_wire_v4.inc"\n')
  objects=[]
  for name,path in [('typed',core/'typed_wire.c'),('buffer',core/'wire_buffer.c'),('v4',temp/'v4.c')]:
   obj=temp/(name+'.o');subprocess.run(['cc','-std=c99','-g','-fsanitize=address,undefined','-fno-pie','-I'+str(core),'-c',str(path),'-o',str(obj)],check=True);objects.append(str(obj))
  codec=ROOT/'DAIKIN/.pio/libdeps/bry88ab-hub/ArduinoJson/src'
  cls.binary=temp/'lan-business'
  subprocess.run(['c++','-std=c++11','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-return-type','-Wno-deprecated-copy','-Wno-sign-compare','-fsanitize=address,undefined','-fno-pie','-no-pie','-ffunction-sections','-fdata-sections','-DARDUINOJSON_ENABLE_ARDUINO_STRING=1','-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0','-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0','-DARDUINOJSON_ENABLE_PROGMEM=0','-DIOTCLOUD_HUB=1','-I'+str(temp),'-I'+str(core),'-I'+str(ROOT/'DAIKIN/include'),'-I'+str(codec),str(ROOT/'DAIKIN/tests/hub/lan_business.cpp'),str(temp/'WString.cpp'),*objects,'-Wl,--gc-sections','-lcrypto','-o',str(cls.binary)],check=True)
 def test_production_event_cache_and_numbered_diagnostics(self):
  subprocess.run([str(self.binary)],check=True,timeout=30,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
