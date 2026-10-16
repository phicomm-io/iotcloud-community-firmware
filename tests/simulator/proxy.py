#!/usr/bin/env python3
"""Real UDP relay for reversible network faults; firmware packets stay native."""
import heapq
import http.server
import json
import random
import select
import socket
import threading
import time

class Proxy:
    def __init__(self,index):
        self.index=index;self.ip=f'127.30.2.{index}';self.target=(f'127.20.2.{index}',11288)
        self.sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);self.sock.bind((self.ip,11288))
        self.mode='normal';self.clients={};self.queue=[];self.serial=0;self.counts={};self.sample_counts={};self.last=None
    def send(self,b,to,delay=0):
        self.serial+=1;heapq.heappush(self.queue,(time.monotonic()+delay,self.serial,b,to))
    def receive(self):
        b,addr=self.sock.recvfrom(65535)
        try:o=json.loads(b)
        except ValueError:return
        reply=addr==self.target
        if not reply:self.clients[o.get('src')]=addr;to=self.target
        else:to=self.clients.get(o.get('dst'))
        if not to:return
        mode=self.mode;self.counts[mode]=self.counts.get(mode,0)+1
        if reply and o.get('op')=='sample':self.sample_counts[mode]=self.sample_counts.get(mode,0)+1
        if mode in ('offline','drop'):return
        if mode=='wrong_token' and reply:o['token']='00'*16;b=json.dumps(o).encode()
        if mode=='abnormal' and reply and o.get('op')=='sample':
            o['body']['humidity']=101;b=json.dumps(o).encode()
        if mode=='stale' and reply and o.get('op')=='sample':
            o['body']['age_ms']=20000;b=json.dumps(o).encode()
        if mode=='reorder':self.send(b,to,1.2 if self.serial%2 else .1)
        elif mode=='delay':self.send(b,to,1.3)
        else:self.send(b,to)
        if mode=='duplicate':self.send(b,to,.1)
    def flush(self):
        while self.queue and self.queue[0][0]<=time.monotonic():
            _,_,b,to=heapq.heappop(self.queue);self.sock.sendto(b,to)

def main():
    proxies={str(i):Proxy(i) for i in range(1,8)}
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self,*args):pass
        def do_GET(self):
            r={k:{'mode':p.mode,'counts':dict(p.counts),'sample_counts':dict(p.sample_counts)} for k,p in proxies.items()};self.send_response(200);self.end_headers();self.wfile.write(json.dumps(r).encode())
        def do_POST(self):
            q=json.loads(self.rfile.read(int(self.headers['Content-Length'])));p=proxies[str(q['index'])];p.mode=q['mode'];self.send_response(200);self.end_headers();self.wfile.write(b'{}')
    threading.Thread(target=http.server.ThreadingHTTPServer(('127.0.0.1',32801),Handler).serve_forever,daemon=True).start()
    sockets={p.sock:p for p in proxies.values()}
    while True:
        for s in select.select(list(sockets),[],[],.02)[0]:sockets[s].receive()
        for p in proxies.values():p.flush()
if __name__=='__main__':main()
