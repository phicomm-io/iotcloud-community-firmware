"""Real UDP v4 plus production HomeKit model; fixture auth/business, no DUT."""
from pathlib import Path
import subprocess,tempfile,unittest
from DAIKIN.tests.arduinojson import codec_source
ROOT=Path(__file__).resolve().parents[2]
class LanHome(unittest.TestCase):
 def test_actual_udp_ack_then_measured_state_and_loss_no_replay(self):
  with tempfile.TemporaryDirectory() as directory:
   p=Path(directory);(p/'wire.c').write_text('#include "lan_wire_v4.inc"\n')
   flags=['-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie']
   objects=[]
   for index,source in enumerate([ROOT/'common/typed_wire.c',ROOT/'common/wire_buffer.c',p/'wire.c']):
    obj=p/(str(index)+'.o');subprocess.run(['cc','-std=c11',*flags,'-I'+str(ROOT/'common'),'-c',str(source),'-o',str(obj)],check=True);objects.append(str(obj))
   subprocess.run(['c++','-std=c++11',*flags,'-no-pie','-Wall','-Wextra','-Werror','-I'+str(ROOT/'DAIKIN/include'),'-I'+str(codec_source()),str(ROOT/'DAIKIN/tests/hub/lan_home.cpp'),*objects,'-lcrypto','-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True,timeout=15)
