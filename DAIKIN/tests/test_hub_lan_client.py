"""Real localhost UDP between production hub client and shared device server."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[2]
class LanClient(unittest.TestCase):
 def test_actual_udp_chunk_retry_and_no_replay(self):
  with tempfile.TemporaryDirectory() as directory:
   p=Path(directory);(p/'wire.c').write_text('#include "lan_wire_v4.inc"\n')
   flags=['-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie']
   objects=[]
   for index,source in enumerate([ROOT/'common/typed_wire.c',ROOT/'common/wire_buffer.c',p/'wire.c']):
    obj=p/(str(index)+'.o');subprocess.run(['cc','-std=c11',*flags,'-I'+str(ROOT/'common'),'-c',str(source),'-o',str(obj)],check=True);objects.append(str(obj))
   subprocess.run(['c++','-std=c++11',*flags,'-no-pie','-Wall','-Wextra','-Werror','-I'+str(ROOT/'DAIKIN/include'),str(ROOT/'DAIKIN/tests/hub/lan_client.cpp'),*objects,'-lcrypto','-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True,timeout=15)
