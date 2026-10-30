"""Production reload/restore retire UDP work and both MQTT source generations."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[2]
class HubLifecycle(unittest.TestCase):
 def test_actual_restore_and_reload(self):
  with tempfile.TemporaryDirectory() as directory:
   p=Path(directory);source=(ROOT/'DAIKIN/src/hub.inc').read_text()
   def section(start,end): return source[source.index(start):source.index(end,source.index(start))]
   (p/'globals.inc').write_text(section('static bool hubReload','static String hubHex('))
   (p/'capture.inc').write_text(section('static bool hubCapture(','static void hubConfigureUpstream('))
   (p/'lifecycle.inc').write_text(section('static void hubResetInbox()','static bool hubReady()'))
   (p/'reload.inc').write_text(section('static void pollHub(uint32_t now) {','  hubRelay.tick(now);')+'\n(void)now;\n}\n')
   (p/'wire.c').write_text('#include "lan_wire_v4.inc"\n')
   flags=['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie'];objects=[]
   for i,f in enumerate([ROOT/'common/typed_wire.c',ROOT/'common/wire_buffer.c',p/'wire.c']):
    o=p/(str(i)+'.o');subprocess.run(['cc','-std=c11',*flags,'-I'+str(ROOT/'common'),'-c',str(f),'-o',str(o)],check=True);objects.append(str(o))
   exe=p/'test';subprocess.run(['c++','-std=c++11',*flags,'-no-pie','-Wall','-Wextra','-Werror','-I'+str(p),'-I'+str(ROOT/'DAIKIN/include'),str(ROOT/'DAIKIN/tests/hub/lifecycle.cpp'),*objects,'-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True,timeout=15)
