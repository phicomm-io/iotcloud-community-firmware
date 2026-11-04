"""Exercise stock-compatible TLS replies and the HTTP artifact over real sockets."""
import importlib.util
import json
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import unittest
import urllib.request

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('stock',ROOT/'tools/s30_stock_ota.py')
stock=importlib.util.module_from_spec(spec);spec.loader.exec_module(stock)


class StockTransportTests(unittest.TestCase):
    def test_three_stock_requests_receive_complete_json_in_one_tls_record(self):
        with tempfile.TemporaryDirectory() as tmp:
            key=Path(tmp)/'key.pem';cert=Path(tmp)/'cert.pem'
            subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','1',
                '-keyout',str(key),'-out',str(cert),'-subj','/CN=localhost'],
                check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            reply=json.dumps({'respCode':1,'respCont':'SUCCESS','mqttUrl':'ssl://127.0.0.1:18894'}).encode()
            server=stock.Server(('127.0.0.1',0),stock.gateway_handler('127.0.0.1',reply))
            ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);ctx.load_cert_chain(cert,key)
            server.socket=ctx.wrap_socket(server.socket,server_side=True)
            thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
            client_ctx=ssl._create_unverified_context()
            try:
                with client_ctx.wrap_socket(socket.create_connection(server.server_address),server_hostname='localhost') as client:
                    client.settimeout(3)
                    for path in ('/device/inform/bootstrap','/device/inform/boot','/device-manage/device/inform/dmReport'):
                        client.sendall(f'POST {path} HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\n\r\n{{}}'.encode())
                        response=client.recv(4096)
                        headers,body=response.split(b'\r\n\r\n',1)
                        self.assertIn(b'HTTP/1.1 200 OK',headers)
                        self.assertIn(b'Content-Length: '+str(len(reply)).encode(),headers)
                        self.assertEqual(body,reply)
            finally:
                server.shutdown();server.server_close();thread.join()

    def test_http_delivery_bytes_and_completion(self):
        body=b'RBL-test-payload'*2048;done=threading.Event()
        server=stock.Server(('127.0.0.1',0),stock.artifact_handler('127.0.0.1',body,done))
        thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        try:
            with urllib.request.urlopen('http://127.0.0.1:'+str(server.server_port)+'/s30.rbl',timeout=3) as response:
                self.assertEqual(response.read(),body)
            self.assertTrue(done.wait(1))
        finally:
            server.shutdown();server.server_close();thread.join()

    def test_nonstandard_port_rejected_before_network_or_file_access(self):
        result=subprocess.run([sys.executable,str(ROOT/'tools/s30_stock_ota.py'),
            '--device','127.0.0.1','--host','127.0.0.1','--device-id','0c79556b0001',
            '--wifi-file','absent','--mqtt-file','absent','--mqtt-ca','absent',
            '--cert','absent','--key','absent','--rbl','absent','--sha256','absent',
            '--http-port','8080'],capture_output=True,text=True)
        self.assertEqual(result.returncode,2)
        self.assertIn('invalid choice: 8080',result.stderr)


if __name__=='__main__':unittest.main()
