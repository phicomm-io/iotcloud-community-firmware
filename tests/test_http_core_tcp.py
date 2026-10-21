"""Actual common parser/scheduler over real host TCP. MiCO remains a DUT gate."""
import concurrent.futures
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest
ROOT=Path(__file__).resolve().parents[1]
class TCP(unittest.TestCase):
 def test_common_tcp(self):
  with tempfile.TemporaryDirectory() as temp:
   exe=Path(temp)/'server'
   subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-no-pie','-I'+str(ROOT/'common/httpd'),str(ROOT/'tests/http_core/loopback.c'),str(ROOT/'common/httpd/core.c'),str(ROOT/'common/httpd/parser.c'),'-o',str(exe)],check=True)
   p=subprocess.Popen([str(exe)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
   port=int(p.stdout.readline())
   def connect():
    s=socket.create_connection(('127.0.0.1',port),timeout=3);s.settimeout(6);return s
   def request(wire):
    with connect() as s:
     s.sendall(wire);out=b''
     while True:
      try:part=s.recv(65536)
      except ConnectionResetError:break
      if not part:break
      out+=part
     return out
   try:
    self.assertTrue(request(b'GET / HTTP/1.1\r\n\r\n').endswith(b'\r\n\r\nok'))
    head=request(b'HEAD / HTTP/1.1\r\n\r\n');self.assertIn(b'Content-Length: 2\r\n',head);self.assertTrue(head.endswith(b'\r\n\r\n'))
    self.assertTrue(request(b'GET /304 HTTP/1.1\r\n\r\n').endswith(b'\r\n\r\n'))
    self.assertTrue(request(b'GET /large HTTP/1.1\r\n\r\n').endswith(b'a'*32768))
    with connect() as slow:
     slow.sendall(b'GET / HTTP/1.1\r\nX:');time.sleep(.1)
     start=time.monotonic();self.assertTrue(request(b'GET / HTTP/1.1\r\n\r\n').endswith(b'ok'));self.assertLess(time.monotonic()-start,1)
    for malformed in (b'GET / HTTP/1.1\n\n',b'GET / HTTP/1.1\r\nX:a\0b\r\n\r\n',b'POST /body HTTP/1.1\r\nContent-Length:4294967296\r\n\r\n',b'GET / HTTP/1.1\r\nTransfer-Encoding:chunked\r\n\r\n'):
     self.assertIn(b'400 ',request(malformed))
    for _ in range(10):
     with connect() as s:
      s.sendall(b'POST /body HTTP/1.1\r\nContent-Length:8192\r\n\r\nabc');s.shutdown(socket.SHUT_WR)
     self.assertTrue(request(b'GET / HTTP/1.1\r\n\r\n').endswith(b'ok'))
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
     results=list(pool.map(lambda _:request(b'GET / HTTP/1.1\r\n\r\n'),range(40)))
    self.assertTrue(all(b'200 ' in r or b'503 ' in r for r in results))
   finally:
    p.stdin.write('q');p.stdin.flush();out,err=p.communicate(timeout=5)
    self.assertEqual(p.returncode,0,err);self.assertNotIn('Sanitizer',err)
if __name__=='__main__':unittest.main()
