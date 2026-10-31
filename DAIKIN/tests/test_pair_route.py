"""Authenticated route+key commit, fixed pins, imports and storage failures.

Production combined NVS serializer, with Preferences I/O modeled. UDP/password/
timeout cases live in test_lan_pairing; IDF NOR cuts in test_hub_config_storage.
"""
from pathlib import Path
import os, subprocess, tempfile, unittest
from .test_local_control_priority import function
from .arduinojson import codec_source
ROOT = Path(__file__).resolve().parents[2]
class PairRoute(unittest.TestCase):
 def test_authenticated_route_storage_and_import(self):
  with tempfile.TemporaryDirectory(prefix='daikin-pair-route-') as directory:
   temp=Path(directory);(temp/'mbedtls').mkdir()
   (temp/'mbedtls/sha256.h').write_text('#include <openssl/sha.h>\nstatic int mbedtls_sha256_ret(const unsigned char*p,size_t n,unsigned char*out,int){return SHA256(p,n,out)?0:-1;}\n')
   source=(ROOT/'DAIKIN/src/main.cpp').read_text();start=source.index('static bool saveConfigTransaction(DynamicJsonDocument &candidate');end=source.index('static void startAccessPoint(',start)
   queued=source[source.index('  if(webPending){',source.index('void loop()')):source.index('  if(mqttQueued)',source.index('void loop()'))]
   (temp/'transaction.inc').write_text(source[start:end]+'\nstatic void dispatchQueued(){\n'+queued+'\n}\n')
   (temp/'route-valid.inc').write_text(function(ROOT/'common/lan_client_core.inc','static bool lc_peer(')+'\n'+function(ROOT/'DAIKIN/src/commands.inc','static bool validConfig('))
   obj=temp/'auth.o';subprocess.run(['cc','-g','-fsanitize=address,undefined','-fno-pie','-c',str(ROOT/'common/local_auth.c'),'-o',str(obj)],check=True)
   for variant in ('full','hub'):
    binary=temp/variant
    subprocess.run(['c++','-std=c++11','-g','-O1','-fsanitize=address,undefined','-fno-pie','-no-pie',*(['-DIOTCLOUD_HUB=1'] if variant=='hub' else []),'-I'+str(temp),'-I'+str(ROOT/'common'),'-I'+str(codec_source()),str(ROOT/'DAIKIN/tests/hub/pair_route.cpp'),str(obj),'-lcrypto','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=30,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
