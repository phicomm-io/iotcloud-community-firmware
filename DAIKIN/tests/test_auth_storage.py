"""Actual combined config/pair durable transaction: readback, reboot and revoke."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[2]
class Storage(unittest.TestCase):
 def test_actual_transaction(self):
  with tempfile.TemporaryDirectory() as d:
   temp=Path(d);(temp/'mbedtls').mkdir();(temp/'mbedtls/sha256.h').write_text('#include <openssl/sha.h>\nstatic int mbedtls_sha256_ret(const unsigned char*p,size_t n,unsigned char*out,int){return SHA256(p,n,out)?0:-1;}\n')
   source=(ROOT/'DAIKIN/src/main.cpp').read_text();start=source.index('static bool saveConfigTransaction(DynamicJsonDocument &candidate');end=source.index('static void startAccessPoint(',start);queued=source[source.index('  if(webPending){',source.index('void loop()')):source.index('  if(mqttQueued)',source.index('void loop()'))];(temp/'transaction.inc').write_text(source[start:end]+'\nstatic void dispatchQueued(){\n'+queued+'\n}\n')
   obj=temp/'auth.o';subprocess.run(['cc','-fsanitize=address,undefined','-fno-pie','-c',str(ROOT/'common/local_auth.c'),'-o',str(obj)],check=True)
   for variant in ('full','hub'):
    exe=temp/variant;subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-fno-pie','-no-pie',*(['-DIOTCLOUD_HUB=1'] if variant=='hub' else []),'-I'+str(temp),'-I'+str(ROOT/'common'),'-I'+str(ROOT/'DAIKIN/.pio/libdeps/bry88ab/ArduinoJson/src'),str(ROOT/'DAIKIN/tests/auth_storage.cpp'),str(obj),'-lcrypto','-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
