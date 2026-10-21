#!/usr/bin/env python3
"""Compile actual shared production sources, not a parallel parser implementation."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class HTTP(unittest.TestCase):
 def test_production_core(self):
  with tempfile.TemporaryDirectory() as temp:
   exe=Path(temp)/'fault'
   subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I'+str(ROOT/'common/httpd'),str(ROOT/'tests/http_core/fault.c'),str(ROOT/'common/httpd/core.c'),str(ROOT/'common/httpd/parser.c'),'-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True)
 def test_tc1_auth(self):
  with tempfile.TemporaryDirectory() as temp:
   exe=Path(temp)/'auth'
   subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-no-pie','-I'+str(ROOT/'tests/http_core/auth_stubs'),'-I'+str(ROOT/'TC1/http_server'),str(ROOT/'tests/http_core/auth.c'),str(ROOT/'TC1/http_server/auth.c'),str(ROOT/'common/httpd/parser.c'),'-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True)
if __name__=='__main__':unittest.main()
