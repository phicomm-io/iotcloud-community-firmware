#!/usr/bin/env python3
"""Actual shared MQTT sockets with allocation-accounted BK low heap."""
import argparse,base64,http.client,json,os,shutil,socket,struct,subprocess,time
from pathlib import Path
from test_network import Broker,PACK,UNPACK,decode,free_port,packet,string,lan_pair_resume

class ResetBroker(Broker):
 reset_next_puback=False
 def send(self,data):
  if self.reset_next_puback and data[:2]==b'\x40\x02':
   self.reset_next_puback=False
   # Close from the broker's receive thread, so its own descriptor is the last
   # reference and SO_LINGER emits a real TCP reset rather than an idle FIN.
   self.sock.setsockopt(socket.SOL_SOCKET,socket.SO_LINGER,struct.pack('ii',1,0))
   raise ConnectionResetError()
  super().send(data)

class Trial:
 def __init__(self,host,run,idle,static_credit=0):
  self.run=run;run.mkdir(parents=True,exist_ok=True);self.generation=0;self.idle=idle;self.control=run/'heap-control';self.stats=run/'heap-stats';self.configure()
  self.broker=ResetBroker();self.port=free_port();self.err=(run/'stderr.log').open('w')
  env=dict(os.environ,PH_HOST_HEAP_CONTROL=str(self.control),PH_HOST_HEAP_STATS=str(self.stats),PH_HOST_STATIC_HEAP_CREDIT=str(static_credit))
  self.process=subprocess.Popen([str(host),str(self.broker.port),str(self.port),str(run/'storage')],stdout=subprocess.PIPE,stderr=self.err,env=env,text=True)
  assert self.process.stdout.readline().strip()=='READY'
  self.broker.wait('/availability',lambda p:p==b'online');self.identifier=100
 def configure(self,fail_min=0,fail_max=0,fail_count=0,pressure=0,ms=0):
  self.generation+=1;temporary=self.control.with_suffix('.tmp');temporary.write_text(f'{self.generation} {self.idle} {fail_min} {fail_max} {fail_count} {pressure} {ms}\n');temporary.replace(self.control)
 def http(self,path='status'):
  c=http.client.HTTPConnection('127.0.0.1',self.port,timeout=8);c.request('GET','/api/v1/'+path,headers={'Authorization':'Basic '+base64.b64encode(b'admin:secret').decode()});r=c.getresponse();b=r.read();c.close();assert r.status==200,(r.status,b);return UNPACK[1](b)['data']
 def publish(self,body,f=1):
  self.cursor=len(self.broker.messages);self.identifier+=1;self.broker.publish('command',PACK[f](body),self.identifier);return self.identifier
 def result(self,id,timeout=8):
  end=time.monotonic()+timeout
  with self.broker.cv:
   while time.monotonic()<end:
    if self.broker.error:raise self.broker.error
    for topic,p,retained in self.broker.messages[self.cursor:]:
     if topic.endswith('/result') and decode(p).get('request_id')==id:
      assert not retained;return decode(p)
    self.broker.cv.wait(.05)
  raise AssertionError('MQTT result timeout '+id)
 def command(self,id,action='power.get',params=None,f=1):
  seq=self.publish({'request_id':id,'action':action,'params':params or {}},f);result=self.result(id);self.broker.ack(seq);return result
 def online(self,since,timeout=12):
  end=time.monotonic()+timeout
  with self.broker.cv:
   while time.monotonic()<end:
    if self.broker.error:raise self.broker.error
    if any(topic.endswith('/availability') and payload==b'online' for topic,payload,_ in self.broker.messages[since:]):return
    self.broker.cv.wait(.05)
  raise AssertionError('reconnected without fresh online availability')
 def snapshot(self):
  end=time.monotonic()+2
  while time.monotonic()<end:
   try:
    fields=list(map(int,self.stats.read_text().split()))
    if len(fields)==10 and fields[4]==self.generation:return fields
   except (FileNotFoundError,ValueError):pass
   time.sleep(.01)
  raise AssertionError('no allocator snapshot')
 def close(self):
  self.process.terminate();self.process.wait(5);self.broker.close();self.err.close();errors=(self.run/'stderr.log').read_text();assert 'Sanitizer' not in errors and 'runtime error:' not in errors,errors
  if self.broker.error:raise self.broker.error

def basic(host,run,idle):
 t=Trial(host,run,idle)
 try:
  before=t.http()['sequence'];codes={};total=0
  for f in (1,2):
   for n in range(20):
    action=('power.get','status.get','diagnostics.get','info.get')[n%4];id=f'heap-{idle}-{f}-{n}'
    result=t.command(id,action,f=f);assert result['code'] in ('ok','busy'),result;codes[result['code']]=codes.get(result['code'],0)+1;total+=1
  assert t.http()['sequence']==before
  for f in (1,2):
   id=f'nul-repeat-{f}';body={'on':True,'index':0};assert b'\0' in PACK[f]({'request_id':id,'action':'outlet.set','params':body})
   first=t.command(id,'outlet.set',body,f);assert first['code']=='accepted',first
   sequence=t.http()['sequence'];second=t.command(id,'outlet.set',body,3-f);assert second['code']=='accepted';assert t.http()['sequence']==sequence
   result=t.command(id,'outlet.set',{'on':False,'index':0},f);assert result['code']=='conflict',result
  t.broker.publish('ha/set',b'OFF',900);t.broker.ack(900)
  end=time.monotonic()+3
  while time.monotonic()<end and t.http()['power']:time.sleep(.03)
  assert not t.http()['power']
  _,payload,_=t.broker.wait('/ha/state',lambda p:json.loads(p)['on']=='OFF');assert json.loads(payload)['on']=='OFF'
  with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as s:
   s.bind(('127.0.0.2',0));s.settimeout(3);assert len(lan_pair_resume(s,'secret'))==16
  # Fail one real command-queue allocation. The common emergency reply must
  # retain its request ID and must not execute the requested mutation.
  t.configure(100,400,1);result=t.command('ingress-oom','outlet.set',{'on':True,'index':0});assert result['code']=='busy',result;assert not t.http()['power'];assert t.snapshot()[5]>=1
  t.configure();assert t.command('after-oom','outlet.set',{'on':True})['code']=='accepted'
  # Force a deferred receipt after the port's precheck, while HTTP/status and
  # HA input are active. Its owner-stack bytes must survive intact.
  t.configure(pressure=1800,ms=2200);id='deferred-read';t.publish({'request_id':id,'action':'power.get','params':{}})
  for _ in range(10):assert t.http()['model']=='s30-v1b';time.sleep(.03)
  for f in (1,2):t.broker.publish('command',PACK[f]({'request_id':f'pending-{f}','action':'outlet.set','params':{'on':False}}),910+f)
  result=t.result(id,6);assert result['code']=='ok' and result['data']['on'] is True,result
  for f in (1,2):assert t.result(f'pending-{f}')['code']=='busy';t.broker.ack(910+f)
  assert t.http()['power']
  t.configure();connections=t.broker.connections;since=len(t.broker.messages);t.broker.reset_next_puback=True
  t.publish({'request_id':'reset-probe','action':'power.get','params':{}})
  end=time.monotonic()+12
  while time.monotonic()<end and t.broker.connections==connections:time.sleep(.05)
  assert t.broker.connections>connections;t.online(since);assert t.command('after-disconnect')['code']=='ok'
  time.sleep(.1);stats=t.snapshot();assert stats[8]==0,stats
  print(f'PASS idle_with_cache={idle} reads={total} codes={codes} repeat/NUL/HA/HTTP/UDP/OOM/deferred/reconnect stats={stats}',flush=True)
 finally:t.close()

def deadline(host,run):
 t=Trial(host,run,8072)
 try:
  connections=t.broker.connections;since=len(t.broker.messages);t.configure(pressure=1800,ms=32000)
  t.publish({'request_id':'bounded-pending','action':'outlet.set','params':{'on':True}})
  start=time.monotonic()
  while time.monotonic()-start<40 and t.broker.connections==connections:
   assert t.http()['model']=='s30-v1b';time.sleep(.25)
  elapsed=time.monotonic()-start;assert t.broker.connections>connections and 29<=elapsed<40,elapsed
  t.configure();t.online(since);before=t.http()['sequence'];result=t.command('bounded-pending','outlet.set',{'on':True});assert result['code']=='accepted';assert t.http()['sequence']==before
  print(f'PASS pending resource deadline elapsed={elapsed:.2f}s; reconnected and mutation retry reused receipt',flush=True)
 finally:t.close()

def cache_reclaim(host,run):
 t=Trial(host,run,16000)
 try:
  end=time.monotonic()+3
  while time.monotonic()<end:
   t.http()
   if t.snapshot()[6]:break
   time.sleep(.05)
  assert t.snapshot()[6]>=1
  t.idle=7360;t.configure();assert t.command('cached-to-low')['code']=='ok'
  stats=t.snapshot();assert stats[7]>=1 and stats[0]>=7360+2064,stats
  print(f'PASS cached-to-low releases real 2KiB block before command admission stats={stats}',flush=True)
 finally:t.close()

if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('host',type=Path);p.add_argument('run',type=Path);a=p.parse_args();host=a.host.resolve();run=a.run.resolve();shutil.rmtree(run,ignore_errors=True)
 for idle in (7360,7680,8072):basic(host,run/str(idle),idle)
 deadline(host,run/'deadline')
 cache_reclaim(host,run/'cache-reclaim')
