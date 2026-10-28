"""Migrate retained UDP/session timing scenarios onto actual native runtime bodies."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
def function(source,signature):
 start=source.index(signature);brace=source.index('{',start);depth=0
 for token in re.finditer(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]',source[brace:]):
  if token.group()=='{':depth+=1
  elif token.group()=='}':
   depth-=1
   if not depth:return source[start:brace+token.end()]
 raise AssertionError(signature)
class NativeUDPRuntime(unittest.TestCase):
 def test_timing_and_passive_sessions(self):
  runtime=(ROOT/'common/runtime.c').read_text();lan=(ROOT/'common/lan_sensor.inc').read_text();engine=(ROOT/'common/engine.c').read_text()
  session=function(runtime,'void feature_mqtt_session(')
  peer_start=runtime.index('typedef struct {\n  uint32_t request, sent')
  peers=runtime[peer_start:runtime.index('\n#else',peer_start)]
  sources={
   'pacing':peers+'\n'+'\n'.join(function(runtime,s) for s in ['static bool peer_current(','static bool sensor_grant(','static void sensor_poll('])+ '\n'+session,
   'passive':'\n'.join(function(engine,s) for s in ['unsigned feature_sample_fresh_fields(','bool feature_co2_fresh('])+'\n'+'\n'.join(function(runtime,s) for s in ['bool feature_sensor_session_mp(','bool feature_bound('])+ '\n'+function(lan,'static bool lan_send(')+'\n'+function(lan,'static void lan_sample_capture(')+'\n'+function(lan,'static void lan_sensor_input(')+'\n'+session,
  }
  status=(ROOT/'common/feature_mp.inc').read_text();status_start=status.index('  for(unsigned i=0;i<FEATURE_MAX_PEERS;i++){\n   sensor_session_t *s=')
  status_body=status[status_start:status.index('\n#endif',status_start)]
  sources['passive']+='\nstatic void status_peers(ph_writer *w){uint32_t now=now_ms();phw_begin(w,NULL,true);\n'+status_body+'\nphw_end(w);}'
  for model,production in sources.items():
   with self.subTest(model=model),tempfile.TemporaryDirectory() as directory:
    directory=Path(directory);(directory/'runtime.inc').write_text(production);exe=directory/'probe'
    subprocess.run(['cc','-std=gnu99','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I'+str(ROOT),'-I'+str(directory),str(ROOT/'tests/native_udp_runtime'/f'{model}.c'),str(ROOT/'common/typed_wire.c'),str(ROOT/'common/wire_buffer.c'),'-lm','-o',str(exe)],check=True)
    subprocess.run([exe],check=True)
