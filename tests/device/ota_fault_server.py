#!/usr/bin/env python3
"""Local HTTP/TLS OTA fault fixtures. Serve only signed test images for selected DUTs."""
import argparse,tempfile
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--images',required=True,help='Directory with signed OTA fixtures')
parser.add_argument('--ports',required=True,help='Write ephemeral port numbers as JSON')
a=parser.parse_args()
import ssl,socket,socketserver,threading,time,json,re
from pathlib import Path
root=Path(a.images)
import subprocess
cert_temp=tempfile.TemporaryDirectory(prefix='ota-fault-cert-');cert=Path(cert_temp.name)
subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(cert/'key'),'-out',str(cert/'cert'),'-days','1','-subj','/CN=wrong.invalid'],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.minimum_version=context.maximum_version=ssl.TLSVersion.TLSv1_2;context.set_ciphers('AES128-GCM-SHA256');context.load_cert_chain(cert/'cert',cert/'key')
class Handler(socketserver.BaseRequestHandler):
 def handle(self):
  stream=self.request
  try:
   stream.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
   if self.server.kind=="silence":time.sleep(30);return
   tls=self.server.kind=="tls"
   if tls:stream=context.wrap_socket(stream,server_side=True)
   with stream:
    reader=stream.makefile('rb')
    while True:
     first=reader.readline(2048)
     if not first:return
     path=first.decode().split()[1];headers={}
     while True:
      line=reader.readline(2048)
      if not line:return
      if line==b'\r\n':break
      k,v=line.decode().split(':',1);headers[k.lower()]=v.strip()
     fault,model=path.strip('/').split('/')[:2]
     print(json.dumps(dict(path=path,range=headers.get('range'),port=self.server.server_address[1],at=time.time())),flush=True)
     if fault=='stall':time.sleep(30);return
     name='phicomm_'+model+('_a1' if model=='tc1' else '')+'.ota.bin.signed'
     payload=bytearray((root/name).read_bytes())
     if fault=='signature':payload[128]^=1
     if fault=='body':payload[-1]^=1
     if fault=='wrongmodel':payload=bytearray((root/('phicomm_a1.ota.bin.signed' if model=='m1' else 'phicomm_m1.ota.bin.signed')).read_bytes())
     start,stop=map(int,re.fullmatch(r'bytes=(\d+)-(\d+)',headers['range']).groups()) if tls else (0,len(payload)-1)
     stop=min(stop,len(payload)-1);body=bytes(payload[start:stop+1])
     status='206 Partial Content' if tls else '200 OK';lines=['HTTP/1.1 '+status,f'Content-Length: {len(body)}']
     if tls:lines.append(f'Content-Range: bytes {start}-{stop}/{len(payload)}')
     stream.sendall(('\r\n'.join(lines)+'\r\n\r\n').encode())
     if fault=='slow':stream.sendall(body[:256]);time.sleep(30);return
     if fault=='truncate':stream.sendall(body[:512]);return
     stream.sendall(body)
     if not tls:return
  except (OSError,ValueError,ssl.SSLError) as e:print(type(e).__name__,str(e),flush=True)
class Server(socketserver.ThreadingTCPServer):allow_reuse_address=True;daemon_threads=True
ports={}
for kind in ('http','tls','silence'):
 server=Server(('0.0.0.0',0),Handler);server.kind=kind;ports[kind]=server.server_address[1];threading.Thread(target=server.serve_forever,daemon=True).start()
Path(a.ports).write_text(json.dumps(ports))
print('READY',ports,flush=True)
threading.Event().wait()
