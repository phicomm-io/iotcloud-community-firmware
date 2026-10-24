"""Exercise the production SDK transport through POSIX shims and real TLS."""
import os
from pathlib import Path
import re
import socketserver
import ssl
import subprocess
import tempfile
import threading
import time
import unittest

ROOT=Path(__file__).resolve().parents[1]

class Fetch(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();cls.directory=Path(cls.temp.name)
        override=os.environ.get('OTA_FETCH_CLIENT')
        cls.client=Path(override) if override else cls.directory/'fetch-client'
        if not override:
            subprocess.run(['cc','-Os','-g','-DPHICOMM_DEVICE_OTA=1','-DOTA_FETCH_HANDSHAKE_MS=600u','-DOTA_FETCH_TOTAL_MS=1200u','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie','-I'+str(ROOT/'tests/ota_fetch'),'-I'+str(ROOT/'build/a1-sdk/mico-os/libraries/utilities/json_c'),str(ROOT/'tests/ota_fetch/client.c'),str(ROOT/'tests/ota_fetch/kernel_tls.c'),str(ROOT/'common/ota/fetch.c'),'-lssl','-lcrypto','-lpthread','-o',str(cls.client)],check=True)

        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(cls.directory/'key'),'-out',str(cls.directory/'cert'),'-days','1','-subj','/CN=wrong.invalid'],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        cls.context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);cls.context.minimum_version=cls.context.maximum_version=ssl.TLSVersion.TLSv1_2
        cls.names=[];cls.context.set_servername_callback(lambda sock,name,context:cls.names.append(name));
        cls.context.set_ciphers('AES128-GCM-SHA256');cls.context.load_cert_chain(cls.directory/'cert',cls.directory/'key')
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def case(self,fault='',tls=True,cancel=0,payload=b'abcd'*800,limit=16384,shim_fault='',attempts=1):
        context=self.context
        class Handler(socketserver.BaseRequestHandler):
            def handle(self):
                stream=self.request
                try:
                    if fault=='handshake_stall':time.sleep(3);return
                    if tls:stream=context.wrap_socket(stream,server_side=True)
                    with stream:
                        reader=stream.makefile('rb')
                        while reader.readline(2048):
                            headers={}
                            while True:
                                line=reader.readline(2048)
                                if line==b'\r\n':break
                                if not line:return
                                key,value=line.decode().strip().split(':',1);headers[key.lower()]=value.strip()
                            if fault=='stall':time.sleep(3);return
                            if fault=='slow':time.sleep(.45)
                            start,stop=map(int,re.fullmatch(r'bytes=(\d+)-(\d+)',headers['range']).groups()) if tls else (0,len(payload)-1)
                            stop=min(stop,len(payload)-1);body=payload[start:stop+1]
                            status='206 Partial Content' if tls else '200 OK'
                            if fault=='ignored_range':status='200 OK'
                            lines=['HTTP/1.1 '+status,f'Content-Length: {len(body)}']
                            if tls:lines.append(f'Content-Range: bytes {start+(fault=="wrong_offset")}-{stop}/{len(payload)+(fault=="changed_total" and start>0)}')
                            if fault=='duplicate_length':lines.append(f'Content-Length: {len(body)}')
                            if fault=='chunked':lines.append('Transfer-Encoding: chunked')
                            if fault=='close':lines.append('Connection: close')
                            stream.sendall(('\r\n'.join(lines)+'\r\n\r\n').encode())
                            if fault=='truncated':stream.sendall(body[:-2]);return
                            stream.sendall(body)
                            if fault=='close' or not tls:return
                except (OSError,ValueError,ssl.SSLError):pass
        class Server(socketserver.ThreadingTCPServer):allow_reuse_address=True;daemon_threads=True
        with Server(('127.0.0.1',0),Handler) as server:
            t=threading.Thread(target=server.serve_forever,kwargs={'poll_interval':.01},daemon=True);t.start()
            output=self.directory/'body'
            p=subprocess.run([str(self.client),f'{"https" if tls else "http"}://127.0.0.1:{server.server_address[1]}/test',str(limit),str(cancel),str(output),str(attempts)],env={**os.environ,'OTA_TEST_FAULT':shim_fault},text=True,capture_output=True,timeout=20)
            server.shutdown();t.join()
        self.assertNotIn('Sanitizer',p.stderr,p.stderr)
        self.assertNotIn('Assertion',p.stderr,p.stderr)
        if p.returncode==0:self.assertEqual(output.read_bytes(),payload)
        return p
    def test_real_tls_wrong_certificate_and_http(self):
        for tls in (False,True):self.assertEqual(self.case(tls=tls).returncode,0)
        self.assertIn('127.0.0.1',self.names)
    def test_reconnect(self):self.assertEqual(self.case('close').returncode,0)
    def test_nonblocking_returns(self):
        for fault in ('want_write','want_read','read_wants_write'):
            with self.subTest(fault=fault):self.assertEqual(self.case(shim_fault=fault).returncode,0)
    def test_cleanup_and_retry_in_one_process(self):
        for fault in ('sem1','sem2','thread','tls_alloc'):
            with self.subTest(fault=fault):
                p=self.case(shim_fault=fault,attempts=4)
                self.assertEqual(p.returncode,0,p.stderr)
                self.assertEqual(len(re.findall('ok=0',p.stdout)),1)
                self.assertEqual(len(re.findall('ok=1',p.stdout)),3)
        for fault in ('handshake_stall','truncated','wrong_offset'):
            with self.subTest(fault=fault):self.assertNotEqual(self.case(fault,attempts=4).returncode,0)
        self.assertNotEqual(self.case('handshake_stall',cancel=100,attempts=4).returncode,0)
    def test_faults(self):
        for fault in ('ignored_range','wrong_offset','changed_total','duplicate_length','chunked','truncated'):
            with self.subTest(fault=fault):self.assertNotEqual(self.case(fault).returncode,0)
    def test_handshake_and_total_deadline(self):
        p=self.case('handshake_stall');self.assertNotEqual(p.returncode,0)
        self.assertGreaterEqual(int(re.search(r'elapsed=(\d+)',p.stdout)[1]),500)
        self.assertLess(int(re.search(r'elapsed=(\d+)',p.stdout)[1]),1000)
        p=self.case('slow');self.assertNotEqual(p.returncode,0);self.assertIn('error=Download timed out',p.stdout)
        self.assertLess(int(re.search(r'elapsed=(\d+)',p.stdout)[1]),1600)
    def test_limit(self):self.assertNotEqual(self.case(limit=100).returncode,0)
    def test_cancel_stalled_http_tls_and_handshake(self):
        for tls,fault in ((False,'stall'),(True,'stall'),(True,'handshake_stall')):
            with self.subTest(tls=tls,fault=fault):
                p=self.case(fault,tls=tls,cancel=300);self.assertNotEqual(p.returncode,0);self.assertIn('error=Cancelled',p.stdout);self.assertLess(int(re.search(r'elapsed=(\d+)',p.stdout)[1]),1000)

if __name__=='__main__':unittest.main()
