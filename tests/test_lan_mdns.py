"""Pinned SDK add/resume functions plus the actual bounded identity builder."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
from tests.test_native_udp_runtime import function
ROOT=Path(__file__).resolve().parents[1]
SDK=Path(os.environ.get('PHICOMM_LAN_SDK',os.environ.get('PHICOMM_TC1_SDK',os.environ.get('TC1_SDK_DIR',ROOT/'build/tc1-sdk-3.6.4/mico-os'))))
REL='MiCO/system/mdns/mico_mdns.c'
class LanMDNS(unittest.TestCase):
 def test_actual_sdk_failures_and_identity(self):
  old=subprocess.check_output(['git','-C',str(SDK),'show','HEAD:'+REL],text=True)
  with tempfile.TemporaryDirectory(prefix='lan-mdns-') as d:
   t=Path(d);target=t/REL;target.parent.mkdir(parents=True);target.write_text(old)
   for _ in range(2):subprocess.run(['python3',str(ROOT/'tools/apply_sdk_patch.py'),str(t),str(ROOT/'common/bonjour-lan-mdns-oom.patch'),str(ROOT/'common/bonjour-lan-mdns-compact.patch')],check=True,capture_output=True)
   source=target.read_text();(t/'mdns_production.inc').write_text('\n'.join(function(source,s) for s in ('static void _clean_record_resource(','OSStatus mdns_add_record(','void mdns_resume_record(')))
   cmd=['cc','-std=gnu99','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I'+str(t),'-I'+str(ROOT),str(ROOT/'tests/lan_business/mdns_fixture.c'),'-o',str(t/'case')]
   built=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(built.returncode,0,built.stderr)
   ran=subprocess.run([str(t/'case')],capture_output=True,text=True);self.assertEqual(ran.returncode,0,ran.stderr);print(ran.stdout.strip())
 def test_all_products_enforce_overlay_and_actual_lan_stack(self):
  for model in ('A1','M1','TC1'):
   s=(ROOT/model/'build.sh').read_text();self.assertIn('common/bonjour-lan-mdns-oom.patch',s);self.assertIn('common/bonjour-lan-mdns-compact.patch',s);self.assertIn('tools/check_lan_stack.py',s)
