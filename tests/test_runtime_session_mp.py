"""Actual M1 runtime ingress/session wiring, independently of consumer internals."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tests.test_runtime_mqtt_mp import function
ROOT = Path(__file__).resolve().parents[1]

class RuntimeSessionMP(unittest.TestCase):
 def test_scoped_native_sensor_offline_and_reconnect_dispatch(self):
  runtime=(ROOT/'common/runtime.c').read_text()
  production='\n'.join(function(runtime,n) for n in (
   'static __attribute__((noinline)) bool display_availability_input(',
   'bool feature_mqtt_input(', 'void feature_mqtt_session('))
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);(d/'runtime_dispatch.inc').write_text(production)
   exe=d/'test'
   subprocess.run(['cc','-std=gnu99','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I'+str(ROOT),'-I'+str(d),'-I'+str(ROOT/'tests/host'),'-I'+str(ROOT/'common'),str(ROOT/'tests/test_runtime_session_mp.c'),str(ROOT/'common/typed_wire.c'),str(ROOT/'common/wire_buffer.c'),'-lm','-o',str(exe)],check=True)
   subprocess.run([exe],check=True)
