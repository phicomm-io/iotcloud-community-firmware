"""Actual SDK MQTTSubscribe/serializer: short-enum object bounds and QoS wire."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from tests import test_mqtt_buffer_storage as storage
ROOT=Path(__file__).resolve().parents[1]

class MQTTSubscribeQoS(unittest.TestCase):
 def run_sdk(self,fixed,short_enum):
  with tempfile.TemporaryDirectory() as temp:
   path=Path(temp);protocol=storage.MQTTBufferStorage().protocol(path,with_fix=True,with_qos=fixed)
   binary=path/'qos';cmd=[os.environ.get('CC','cc'),'-std=gnu99','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie']
   if short_enum:cmd+=['-fshort-enums']
   for include in (ROOT/'tests/mqtt_core/stubs',protocol,protocol/'mico',protocol/'MQTTPacket'):cmd+=['-I'+str(include)]
   cmd+=[str(ROOT/'tests/mqtt_subscribe_qos_harness.c'),str(protocol/'MQTTClient.c')]+[str(p) for p in sorted((protocol/'MQTTPacket').glob('*.c'))]+['-o',str(binary)]
   subprocess.run(cmd,check=True,timeout=60)
   env=os.environ.copy();env['ASAN_OPTIONS']='detect_leaks=1:halt_on_error=1';env['UBSAN_OPTIONS']='halt_on_error=1'
   return subprocess.run([str(binary)],capture_output=True,text=True,env=env,timeout=30)
 def test_original_arm_short_enum_overread(self):
  result=self.run_sdk(False,True)
  self.assertNotEqual(result.returncode,0)
  self.assertIn('stack-buffer-overflow',result.stderr)
  self.assertIn('MQTTSerialize_subscribe',result.stderr)
 def test_fixed_short_enum_qos_0_1_2_and_suback(self):
  result=self.run_sdk(True,True);self.assertEqual(result.returncode,0,result.stderr)
 def test_fixed_default_enum_qos_0_1_2_and_suback(self):
  result=self.run_sdk(True,False);self.assertEqual(result.returncode,0,result.stderr)
