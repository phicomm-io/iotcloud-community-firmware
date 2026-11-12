"""SDK-free execution of actual shared HTTP product helpers, not route mocks."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
HERE = Path(__file__).resolve().parent
ROOT = Path(os.environ.get('NATIVE_MP_ROOT', HERE.parent))

class HTTPProductMP(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='http-product-mp-')
        cls.exe=Path(cls.temp.name)/'case'
        cmd=shlex.split(os.environ.get('CC','cc'))+['-std=gnu99','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-g','-I'+str(ROOT)]
        if os.environ.get('SANITIZE')=='1':cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']
        result=subprocess.run(cmd+[str(HERE/'http_product_mp/fixture.c'),str(ROOT/'common/httpd/product.c'),str(ROOT/'common/typed_wire.c'),str(ROOT/'common/wire_buffer.c'),'-lm','-o',str(cls.exe)],capture_output=True,text=True)
        if result.returncode:raise AssertionError(result.stdout+result.stderr)
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def run_case(self,name):
        result=subprocess.run([str(self.exe),name],capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
    def test_admission_and_busy_recovery(self):self.run_case('admission')
    def test_allocation_failures_release_claim_and_owned_output(self):self.run_case('failures')
    def test_synchronous_borrow_and_response_ownership(self):self.run_case('ownership')
    def test_fixed_busy_and_message_mime_without_heap(self):self.run_case('fixed')

if __name__=='__main__':unittest.main()
