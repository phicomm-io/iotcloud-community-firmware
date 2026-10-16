#!/usr/bin/env python3
"""Launch 14 real-runtime nodes. Loopback control API is test-only, never firmware."""
import concurrent.futures
import http.server
import json
import os
import pathlib
import select
import signal
import subprocess
import threading
import time
import uuid

ROOT=pathlib.Path(__file__).resolve().parents[2]
EVIDENCE=pathlib.Path(os.environ.get('SIM_EVIDENCE_ROOT',ROOT/'build/evidence/fleet')).resolve()
class Node:
    def __init__(self,model,index,user,broker,port):
        self.id=f'020000{1 if model=="A1" else 2:02x}{index:04x}'
        self.ip=f'127.20.{1 if model=="A1" else 2}.{index}'
        self.lock=threading.Lock()
        self.buffer=b''
        self.stderr=open(EVIDENCE/f'{self.id}.stderr','a')
        self.p=subprocess.Popen([str(ROOT/f'build/simulator/{model}'),self.id,self.ip,broker,str(port),user],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=self.stderr,bufsize=0,env={**os.environ,'SIM_BIND_IP':self.ip})
    def call(self,q):
        # Correlate native replies so a delayed command after SIGCONT cannot be
        # mistaken for the next query. Use raw pipes, not buffered readline/select.
        with self.lock:
            q=dict(q)
            expected=None
            if 'action' in q:
                expected=q.setdefault('request_id','sim-'+uuid.uuid4().hex)
            self.p.stdin.write((json.dumps(q)+'\n').encode())
            deadline=time.monotonic()+10
            while True:
                while b'\n' not in self.buffer:
                    remaining=deadline-time.monotonic()
                    if remaining<=0 or not select.select([self.p.stdout],[],[],remaining)[0]:
                        raise TimeoutError(self.id)
                    chunk=os.read(self.p.stdout.fileno(),65536)
                    if not chunk:raise RuntimeError((self.id,self.p.poll()))
                    self.buffer+=chunk
                line,self.buffer=self.buffer.split(b'\n',1)
                result=json.loads(line)
                if expected is None or result.get('request_id')==expected:
                    return result

def main():
    import argparse
    ap=argparse.ArgumentParser();ap.add_argument('--user-file',required=True);ap.add_argument('--broker',default='10.96.210.226');ap.add_argument('--port',type=int,default=32768);args=ap.parse_args()
    EVIDENCE.mkdir(parents=True,exist_ok=True)
    user=json.loads(pathlib.Path(args.user_file).read_text())['config.get']['mqtt_user']
    nodes={}
    for model in ('A1','M1'):
        for i in range(1,8):
            n=Node(model,i,user,args.broker,args.port);nodes[n.id]=n
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self,*args):pass
        def do_GET(self):
            body=json.dumps([{'id':n.id,'ip':n.ip,'pid':n.p.pid} for n in nodes.values()]).encode();self.send_response(200);self.end_headers();self.wfile.write(body)
        def do_POST(self):
            try:
                q=json.loads(self.rfile.read(int(self.headers['Content-Length'])));r=nodes[q['node']].call(q['request']);code=200
            except Exception as e:r={'error':str(e)};code=500
            body=json.dumps(r).encode();self.send_response(code);self.end_headers();self.wfile.write(body)
    signal.signal(signal.SIGTERM,lambda *_: (_ for _ in ()).throw(SystemExit(0)))
    try:http.server.ThreadingHTTPServer(('127.0.0.1',32800),Handler).serve_forever()
    finally:
        for n in nodes.values():n.p.terminate()
if __name__=='__main__':main()
