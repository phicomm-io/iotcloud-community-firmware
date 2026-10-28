"""Production v4/auth/service functions, separate translation units, no DUT."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class LanBusiness(unittest.TestCase):
 def build_run(self,name,service=False):
  with tempfile.TemporaryDirectory(prefix='lan-business-') as temp:
   t=Path(temp);(t/'generated').mkdir();(t/'generated/build_ota.h').write_text('')
   cmd=[os.environ.get('CC','cc'),'-std=gnu99','-DMOC=1','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-deprecated-declarations','-Wno-unused-function','-Wno-unused-variable','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-pthread','-I'+str(ROOT/'tests/service_mp'),'-I'+str(t),'-I'+str(ROOT),str(ROOT/'tests/lan_business'/name),str(ROOT/'common/local_auth.c')]
   if service:cmd += [str(ROOT/'common/service.c'),str(ROOT/'tests/service_mp/hooks.c'),str(ROOT/'common/typed_wire.c'),str(ROOT/'common/wire_buffer.c'),'-Wl,--wrap=malloc','-Wl,--wrap=free','-Wl,--wrap=ph_auth_context_valid','-lcrypto','-lm']
   cmd += ['-o',str(t/'case')]
   built=subprocess.run(cmd,capture_output=True,text=True,timeout=45);self.assertEqual(built.returncode,0,built.stderr)
   ran=subprocess.run([str(t/'case')],capture_output=True,text=True,timeout=30);self.assertEqual(ran.returncode,0,ran.stderr);print(ran.stdout.strip())
 def test_command_gate_initialization(self):
  with tempfile.TemporaryDirectory() as d:
   exe=Path(d)/'gate';subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-I'+str(ROOT),str(ROOT/'tests/lan_business/command_gate.c'),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
 def test_actual_server(self):self.build_run('server.c',True)
 def test_boot_resume(self):self.build_run('auth_boot.c')
 def test_actual_dispatcher(self):self.build_run('service.c',True)
if __name__=='__main__':unittest.main()
