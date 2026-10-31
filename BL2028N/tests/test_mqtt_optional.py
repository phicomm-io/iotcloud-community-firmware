#!/usr/bin/env python3
"""Production sockets: exact optional ACL refusal, real 60 s retry and receipts."""
import argparse,json,os,socket,time
from pathlib import Path
from test_mqtt_heap import Trial
from test_network import Broker,Network,PACK,decode,lan_exchange

class PolicyBroker(Broker):
 def __init__(self,mode='refuse',required=None):
  self.mode=mode;self.required=required
  super().__init__()
 def suback(self,topic,identifier):
  required=(topic.endswith('/s30-v1b/aabbccddeeff/command'),topic.endswith('/ha/set'),topic=='homeassistant/status')
  if self.required is not None and required[self.required]:return b'\x90\x03'+identifier+b'\x80'
  if '/sensor/' in topic:
   if self.mode=='refuse':return b'\x90\x03'+identifier+b'\x80'
   if self.mode=='wrong-id':return b'\x90\x03'+(int.from_bytes(identifier,'big')+1).to_bytes(2,'big')+b'\x80'
   if self.mode=='timeout':return None
  return super().suback(topic,identifier)
 def sensor_events(self):
  with self.cv:return [e for e in self.subscription_events if '/sensor/' in e[1]]

def fatal(host,run,mode='allow',required=None):
 broker=PolicyBroker(mode,required)
 # Trial waits for READY; fatal subscriptions must never reach it.
 import subprocess
 from test_network import free_port
 run.mkdir(parents=True);err=(run/'stderr.log').open('w')
 process=subprocess.Popen([str(host),str(broker.port),str(free_port()),str(run/'storage')],stdout=subprocess.PIPE,stderr=err,text=True)
 try:
  assert process.stdout.readline().strip()=='READY'
  end=time.monotonic()+17
  while broker.connections<2 and time.monotonic()<end:
   if broker.error:raise broker.error
   time.sleep(.05)
  assert broker.connections>=2,(mode,required,broker.subscription_events)
  assert not any(t.endswith('/availability') and p==b'online' for t,p,_ in broker.messages)
  print('PASS strict fatal subscription',dict(optional=mode,required=required,connections=broker.connections),flush=True)
 finally:
  process.terminate();process.wait(5);broker.close();err.close()
  errors=(run/'stderr.log').read_text();assert 'Sanitizer' not in errors and 'runtime error:' not in errors,errors
  if broker.error:raise broker.error

def recover(host,run):
 broker=PolicyBroker();t=Trial(host,run,13144,broker=broker)
 try:
  first=broker.sensor_events();assert len(first)==1,first
  started=first[0][0]
  # Login and helpers exercise actual HTTP + MQTT + source-port UDP services.
  helper_class=type('OptionalNetwork',(Network,),dict(broker=broker,port=t.port))
  h=helper_class('test_01_auth_binary_and_json_rejection');h.token=helper_class.login('secret')
  assert t.http()['mqtt_connected'] and dict(t.http('diagnostics')['fields'])[35]==0
  assert t.command('optional-refused-control','outlet.set',{'on':False})['code']=='accepted'
  broker.publish('ha/set',b'ON',800);broker.ack(800)
  end=time.monotonic()+3
  while not t.http()['power'] and time.monotonic()<end:time.sleep(.02)
  assert t.http()['power'];broker.wait('/ha/state',lambda p:json.loads(p)['on']=='ON')
  broker.wait('/state',lambda p:p[:1]!=b'{' and decode(p).get('power') is True)
  while time.monotonic()<started+58:
   assert broker.connections==1 and len(broker.sensor_events())==1
   time.sleep(min(.2,max(0,started+58-time.monotonic())))
  # A receipt becomes pending across the next retry deadline. The optional
  # subscription must wait until that receipt has been transmitted.
  broker.mode='allow';t.configure(pressure=8000,ms=4200)
  rid='optional-pending-first';t.publish(dict(request_id=rid,action='power.get',params={}))
  while time.monotonic()<started+61:
   assert broker.connections==1 and len(broker.sensor_events())==1
   assert not any(topic.endswith('/result') and decode(p).get('request_id')==rid for topic,p,_ in broker.messages[t.cursor:])
   time.sleep(.05)
  assert t.result(rid,timeout=6)['code']=='ok'
  end=time.monotonic()+4
  while len(broker.sensor_events())<2 and time.monotonic()<end:time.sleep(.02)
  events=broker.sensor_events();assert len(events)==2,events
  gap=events[1][0]-events[0][0];assert gap>=60,gap
  # The successful subscription restores the existing SENSOR-only v4 path.
  peer='031122334455';boot=0xf1234567;token=b'\0'+os.urandom(15)
  h.sensor_grant(peer,boot,token,900)
  with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as s:
   s.bind(('127.0.0.2',0));s.settimeout(3)
   for id,action in enumerate(('power.get','status.get','info.get','capabilities.get','diagnostics.get'),1):
    assert h.lan_result(s,h.sensor_request(peer,boot,token,id,action))['code']=='ok'
   for id,action in enumerate(('outlet.set','indicator.set','child_lock.set','config.set'),10):
    expected='unsupported' if action=='config.set' else 'unauthorized'
    assert h.lan_result(s,h.sensor_request(peer,boot,token,id,action,{'on':False}))['code']==expected
  time.sleep(2)
  assert broker.connections==1 and len(broker.sensor_events())==2
  assert dict(t.http('diagnostics')['fields'])[35]==0
  print(f'PASS refusal stays READY; native+HA control; real retry {gap:.3f}s after pending receipt; SENSOR reads only; no reconnect/resubscribe',flush=True)
  return dict(retry_seconds=gap,connections=broker.connections,sensor_attempts=2,pending_receipt_priority=True)
 finally:t.close()

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('host',type=Path);p.add_argument('run',type=Path);a=p.parse_args();a.run.mkdir(parents=True,exist_ok=True)
 result=recover(a.host,a.run/'recover')
 for required in range(3):fatal(a.host,a.run/f'required-{required}',required=required)
 for mode in ('wrong-id','timeout'):fatal(a.host,a.run/('optional-'+mode),mode)
 (a.run/'result.json').write_text(json.dumps(dict(status='PASS',**result,required_fatal=3,optional_malformed_or_timeout_fatal=2),indent=2)+'\n')
 print('PASS optional subscription production sockets',flush=True)
if __name__=='__main__':main()
