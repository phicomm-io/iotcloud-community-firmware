#!/usr/bin/env python3
"""Run the real discovery worker against a local HTTP server, without Flash."""
import http.server,json,os,subprocess,sys,threading
class Server(http.server.HTTPServer):allow_reuse_address=True
class Handler(http.server.BaseHTTPRequestHandler):
 body={};status=200;chunked=False
 def log_message(self,*args):pass
 def do_GET(self):
  assert self.path=='/ota/v1/m1/full/latest.json'
  body=json.dumps(self.body).encode();self.send_response(self.status)
  if self.chunked:self.send_header('Transfer-Encoding','chunked')
  else:self.send_header('Content-Length',str(len(body)))
  self.end_headers();self.wfile.write(body)
server=Server(('127.0.0.1',int(os.environ['PHICOMM_TEST_OTA_PORT'])),Handler);threading.Thread(target=server.serve_forever,daemon=True).start()
base={'model':'m1','version':'9.10.0','path':'/releases/v9.10.0/fw.bin.signed','release_log':'/releases/v9.10.0/notes.md'}
try:
 cases=[(base,200,False,'available',None),({**base,'version':'0.0.0'},200,False,'up_to_date',None),({**base,'model':'tc1'},200,False,'failed',None),({**base,'path':'http://evil.invalid/fw'},200,False,'failed',None),(base,302,False,'failed',None),(base,200,True,'failed',None)]
 for local,remote,expected in [
  ('1.0.0-rc-abcdef','1.0.0-123456','available'),
  ('1.0.0-abcdef','1.0.0-rc-123456','up_to_date'),
  ('1.0.0-rc.2-abcdef','1.0.0-rc.10-123456','available'),
  ('1.0.0-rc.10-abcdef','1.0.0-rc.2-123456','up_to_date'),
  ('1.0.0-rc-abcdef','1.0.0-rc.0','available'),
  ('1.0.0-abcdef-dirty','1.0.0+metadata.1','up_to_date'),
  ('1.0.0-rc.2+old','1.0.0-rc.2+new','up_to_date'),
  ('1.0.0-rc-abcdef','1.0.0-rc.01','failed'),
  ('1.0.0-rc-abcdef','9.0.0-rc-garbage','failed'),
  ('1.0.0-rc-abcdef','9.0.0-beta','failed'),
  ('1.0.0-rc-abcdef','01.0.0','failed'),
  ('1.0.0-rc-abcdef','4294967296.0.0','failed'),
  ('1.0.0-unknown','2.0.0','failed'),
 ]:cases.append(({**base,'version':remote},200,False,expected,local))
 for body,status,chunked,expected,local in cases:
  Handler.body=body;Handler.status=status;Handler.chunked=chunked
  subprocess.run([sys.argv[1],expected]+([local] if local else []),check=True)
 print('OTA discovery: real worker RC promotion/order, strict versions, metadata, wrong model, hostile path, redirect, chunked, busy and explicit install passed')
finally:server.shutdown();server.server_close()
