#!/usr/bin/env python3
"""Run the real discovery worker against a local HTTP server, without Flash."""
import http.server,json,subprocess,sys,threading
class Server(http.server.HTTPServer):allow_reuse_address=True
class Handler(http.server.BaseHTTPRequestHandler):
 body={};status=200;chunked=False
 def log_message(self,*args):pass
 def do_GET(self):
  assert self.path=='/ota/m1/latest.json'
  body=json.dumps(self.body).encode();self.send_response(self.status)
  if self.chunked:self.send_header('Transfer-Encoding','chunked')
  else:self.send_header('Content-Length',str(len(body)))
  self.end_headers();self.wfile.write(body)
server=Server(('127.0.0.1',18766),Handler);threading.Thread(target=server.serve_forever,daemon=True).start()
base={'model':'m1','version':'9.10.0','path':'/releases/v9.10.0/fw.bin.signed','release_log':'/releases/v9.10.0/notes.md'}
try:
 for body,status,chunked,expected in [(base,200,False,'available'),({**base,'version':'0.0.0'},200,False,'up_to_date'),({**base,'model':'tc1'},200,False,'failed'),({**base,'path':'http://evil.invalid/fw'},200,False,'failed'),(base,302,False,'failed'),(base,200,True,'failed')]:
  Handler.body=body;Handler.status=status;Handler.chunked=chunked;subprocess.run([sys.argv[1],expected],check=True)
 print('OTA discovery: numeric versions, wrong model, hostile path, redirect, chunked, busy and explicit install passed')
finally:server.shutdown();server.server_close()
