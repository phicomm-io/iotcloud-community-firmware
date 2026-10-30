#!/usr/bin/env python3
"""Real sockets through the production shared HTTP/Paho MQTT/UDP owners."""
import argparse,base64,gzip,http.client,json,os,socket,subprocess,threading,time,unittest
from pathlib import Path
from test_protocols import PACK,UNPACK,DIAGNOSTICS

def receive(s,n):
 d=b''
 while len(d)<n:
  p=s.recv(n-len(d))
  if not p:raise EOFError()
  d+=p
 return d

def packet(h,p):
 n=len(p);v=bytearray()
 while True:
  b=n%128;n//=128;v.append(b|(128 if n else 0))
  if not n:return bytes([h])+v+p

def string(s):
 b=s.encode();return len(b).to_bytes(2,'big')+b

def decode(p):
 try:return UNPACK[1](p)
 except Exception:return UNPACK[2](p)

def free_port():
 with socket.socket() as s:s.bind(('127.0.0.1',0));return s.getsockname()[1]

def lan_exchange(sock,value):
 sock.sendto(PACK[1](value),('127.0.0.1',11288));reply,source=sock.recvfrom(1200)
 assert source==('127.0.0.1',11288),source
 return UNPACK[1](reply)

def lan_pair_resume(sock,password,peer='001122334455',boot=888):
 nonce=os.urandom(16);reply=lan_exchange(sock,dict(op='auth_pair',src=peer,password=password,scopes=1,nonce=nonce))
 assert reply['code']=='ok' and reply['nonce']==nonce and isinstance(reply['key'],bytes),reply
 nonce=os.urandom(16);reply=lan_exchange(sock,dict(op='auth_resume',src=peer,key=reply['key'],nonce=nonce,boot=boot))
 assert reply['code']=='ok' and reply['nonce']==nonce and isinstance(reply['token'],bytes),reply
 return reply['token']

class Broker:
 def __init__(self):
  self.listener=socket.socket();self.listener.bind(('127.0.0.1',0));self.listener.listen(3)
  self.port=self.listener.getsockname()[1];self.messages=[];self.acks=set();self.subscriptions=set();self.cv=threading.Condition();self.lock=threading.Lock();self.error=None;self.sock=None;self.stopping=False;self.root='';self.connections=0
  self.thread=threading.Thread(target=self.run,daemon=True);self.thread.start()
 def send(self,data):
  with self.lock:self.sock.sendall(data)
 def run(self):
  try:
   while not self.stopping:
    self.sock,_=self.listener.accept();self.connections+=1;self.sock.settimeout(40)
    try:
     while not self.stopping:
      h=receive(self.sock,1)[0];n=0;mul=1
      while True:
       b=receive(self.sock,1)[0];n+=(b&127)*mul;mul*=128
       if not b&128:break
      p=receive(self.sock,n);kind=h>>4
      if kind==1:
       assert p[:7]==b'\x00\x04MQTT\x04' and p[7]==0xee
       assert b'offline' in p and b'test-pass' in p
       for b in b'\x20\x02\0\0':self.send(bytes([b]));time.sleep(.002)
      elif kind==8:
       size=int.from_bytes(p[2:4],'big');topic=p[4:4+size].decode()
       self.subscriptions.add(topic)
       parts=topic.split('/')
       if len(parts)==6 and parts[:2]==['phicomm','v2'] and parts[3]=='s30-v1b' and parts[5]=='command':self.root=topic[:-8]
       self.send(b'\x90\x03'+p[:2]+b'\x01')
      elif kind==3:
       size=int.from_bytes(p[:2],'big');topic=p[2:2+size].decode();at=2+size
       if (h>>1)&3:self.send(b'\x40\x02'+p[at:at+2]);at+=2
       with self.cv:self.messages.append((topic,p[at:],bool(h&1)));self.cv.notify_all()
      elif kind==4:
       with self.cv:self.acks.add(int.from_bytes(p,'big'));self.cv.notify_all()
      elif kind==12:self.send(b'\xd0\0')
      elif kind==14:break
      else:raise AssertionError('unexpected MQTT packet '+str(kind))
    except (EOFError,ConnectionResetError):pass
    finally:self.sock.close()
  except (EOFError,OSError) as e:
   if not self.stopping:self.error=e
  except BaseException as e:self.error=e
 def publish(self,suffix,payload,identifier,retained=False,fragment=False):
  self.publish_topic(self.root+'/'+suffix,payload,identifier,retained,fragment)
 def publish_topic(self,topic,payload,identifier,retained=False,fragment=False):
  data=packet(0x32|int(retained),string(topic)+identifier.to_bytes(2,'big')+payload)
  if fragment:
   # Fragment TCP writes, while keeping this complete MQTT packet ordered
   # before PUBACKs sent concurrently by the broker's receive thread.
   with self.lock:
    for at in range(0,len(data),3):self.sock.sendall(data[at:at+3]);time.sleep(.001)
  else:self.send(data)
 def wait(self,suffix,predicate=lambda p:True,timeout=12):
  end=time.monotonic()+timeout
  with self.cv:
   while time.monotonic()<end:
    if self.error:raise self.error
    for t,p,r in reversed(self.messages):
     if t.endswith(suffix) and predicate(p):return t,p,r
    self.cv.wait(min(.2,max(0,end-time.monotonic())))
  raise AssertionError('MQTT timeout '+suffix)
 def ack(self,id):
  end=time.monotonic()+5
  with self.cv:
   while id not in self.acks and time.monotonic()<end:self.cv.wait(.05)
  assert id in self.acks
 def close(self):
  self.stopping=True
  if self.sock:
   try:self.sock.shutdown(socket.SHUT_RDWR)
   except OSError:pass
   self.sock.close()
  self.listener.close();self.thread.join(3)

class Network(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.broker=Broker();cls.port=free_port();cls.err=(RUN/'network-stderr.log').open('w')
  cls.process=subprocess.Popen([str(HOST),str(cls.broker.port),str(cls.port),str(RUN/'host-storage')],stdout=subprocess.PIPE,stderr=cls.err,text=True)
  assert cls.process.stdout.readline().strip()=='READY'
  try:
   cls.broker.wait('/availability',lambda p:p==b'online')
   cls.token=cls.login('secret')
  except BaseException:
   cls.process.terminate();cls.process.wait(5);cls.broker.close();cls.err.close();raise
 @classmethod
 def tearDownClass(cls):
  cls.process.terminate();cls.process.wait(5);cls.broker.close();cls.err.close()
  errors=(RUN/'network-stderr.log').read_text()
  assert 'Sanitizer' not in errors and 'runtime error:' not in errors,errors
  if cls.broker.error:raise cls.broker.error
 @classmethod
 def http(cls,method,path,body=None,headers=None):
  c=http.client.HTTPConnection('127.0.0.1',cls.port,timeout=8);c.request(method,path,body,headers or {});r=c.getresponse();result=r.status,dict(r.getheaders()),r.read();c.close();return result
 @classmethod
 def login(cls,password):
  s,_,b=cls.http('POST','/api/v1/auth/login',PACK[1]({'password':password}),{'Content-Type':'application/msgpack'});assert s==200,(s,b);return UNPACK[1](b)['data']['token']
 def api(self,path='status',f=1,body=None):
  mime='application/msgpack' if f==1 else 'application/cbor';h={'Accept':mime,'Authorization':'Bearer '+self.token}
  if body is not None:h['Content-Type']=mime
  s,_,b=self.http('POST' if body is not None else 'GET','/api/v1/'+path,PACK[f](body) if body is not None else None,h);self.assertEqual(s,200,(path,b));reply=UNPACK[f](b);return reply if body is not None else reply['data']
 def command(self,action,params={},id=None,f=1):
  b={'action':action,'params':params}
  if id:b['request_id']=id
  return self.api('command',f,b)
 def sensor_grant(self,peer,boot,token,identifier,topic=None,retained=False,f=1,params=None):
  if topic is None:topic=self.broker.root.rsplit('/',2)[0]+'/sensor/aabbccddeeff/command'
  body=dict(action='sensor.session.set',params=params if params is not None else dict(v=3,a1=peer,boot=boot,token=token))
  self.broker.publish_topic(topic,PACK[f](body),identifier,retained=retained,fragment=True);self.broker.ack(identifier)
  # PUBACK precedes queued application dispatch. A following command result
  # is the FIFO barrier, so a negative UDP assertion cannot race the grant.
  rid='grant-sync-'+str(identifier);sequence=identifier+2000
  self.broker.publish('command',PACK[1](dict(request_id=rid,action='power.get',params={})),sequence)
  self.broker.wait('/result',lambda p:decode(p).get('request_id')==rid);self.broker.ack(sequence)
 def sensor_request(self,peer,boot,token,id=1,action='power.get',params=None):
  return dict(v=4,src=peer,dst='aabbccddeeff',boot=boot,id=id,op='command',token=token,
              body=PACK[1](dict(request_id='sensor-'+str(id),action=action,params=params or {})))
 def lan_result(self,s,request):
  first=lan_exchange(s,request);self.assertEqual(first['code'],'ok',first);parts=[first['body']]
  base={k:v for k,v in request.items() if k not in ('op','body')}
  for part in range(1,first['total']):parts.append(lan_exchange(s,{**base,'op':'pull','part':part})['body'])
  self.assertEqual(len(b''.join(parts)),first['size']);self.assertEqual(lan_exchange(s,{**base,'op':'ack'})['code'],'acked')
  return UNPACK[1](b''.join(parts))
 def test_01_auth_binary_and_json_rejection(self):
  s,_,b=self.http('GET','/api/v1/info',headers={'Accept':'application/msgpack, application/json'});self.assertEqual(s,200);self.assertEqual(UNPACK[1](b)['data']['default_wire_format'],'msgpack')
  s,_,b=self.http('GET','/api/v1/auth/status');self.assertEqual(UNPACK[1](b),{'password_required':True,'authenticated':False})
  basic='Basic '+base64.b64encode(b'admin:secret').decode()
  s,_,b=self.http('GET','/api/v1/status',headers={'Authorization':basic});self.assertEqual(s,200);self.assertEqual(UNPACK[1](b)['data']['model'],'s30-v1b')
  s,_,_=self.http('GET','/api/v1/status');self.assertEqual(s,401)
  s,_,_=self.http('GET','/api/v1/status',headers={'Accept':'application/json','Authorization':basic});self.assertEqual(s,406)
  s,_,_=self.http('POST','/api/v1/auth/login',b'{"password":"secret"}',{'Content-Type':'application/json'});self.assertEqual(s,415)
  for f in (1,2):self.assertEqual(self.api('status?cache=off',f)['protocol'],'openiot')
 def test_02_idle_connections_and_fragmented_body(self):
  idle=[socket.create_connection(('127.0.0.1',self.port)) for _ in range(2)]
  try:
   start=time.monotonic();self.assertEqual(self.api()['model'],'s30-v1b');self.assertLess(time.monotonic()-start,1.5)
   body=PACK[1]({'request_id':'http-fragment','action':'outlet.set','params':{'on':True}})
   with socket.create_connection(('127.0.0.1',self.port),timeout=8) as s:
    h=f'POST /api/v1/command HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/msgpack\r\nAccept: application/cbor\r\nAuthorization: Bearer {self.token}\r\nContent-Length: {len(body)}\r\n\r\n'.encode();s.sendall(h+body[:2]);time.sleep(.03)
    for at in range(2,len(body),5):s.sendall(body[at:at+5]);time.sleep(.002)
    r=http.client.HTTPResponse(s);r.begin();self.assertEqual(r.status,200);self.assertEqual(UNPACK[2](r.read())['code'],'accepted')
  finally:
   for s in idle:s.close()
  self.assertEqual(self.api()['socket_states'],[True])
 def test_03_shared_web_and_disabled_legacy(self):
  for p in ('/api/cmnd','/api/state','/cfg_pins','/cfg_wifi_set?ssid=test','/cm?cmnd=Power%20ON'):
   self.assertEqual(self.http('GET',p)[0],404,p)
  self.assertEqual(self.http('GET','/')[0],200)
  s,h,b=self.http('GET','/',headers={'Authorization':'Bearer '+self.token});self.assertEqual(s,200);self.assertEqual(h['Content-Encoding'],'gzip')
  html=gzip.decompress(b).decode();self.assertIn('data-settings-page="device"',html);self.assertIn('function mqttPage(',html);self.assertIn('UI.start({',html);self.assertNotIn('href="/cfg_',html)
  self.assertEqual(self.api('capabilities')['wire_formats'],['msgpack','cbor'])
 def test_03_two_waiting_requests_and_binary_overload(self):
  body=PACK[1]({'action':'status.get','params':{}})
  active=socket.create_connection(('127.0.0.1',self.port),timeout=3)
  queued=[]
  try:
   active.sendall((f'POST /api/v1/command HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/msgpack\r\nAuthorization: Bearer {self.token}\r\nContent-Length: {len(body)}\r\n\r\n').encode()+body[:2])
   time.sleep(.04)
   for _ in range(3):
    s=socket.create_connection(('127.0.0.1',self.port),timeout=3);queued.append(s)
    s.sendall((f'GET /api/v1/status HTTP/1.1\r\nHost: localhost\r\nAccept: application/cbor\r\nAuthorization: Bearer {self.token}\r\n\r\n').encode());time.sleep(.04)
   r=http.client.HTTPResponse(queued[2]);r.begin();self.assertEqual(r.status,503)
   self.assertEqual(r.getheader('Content-Type'),'application/cbor');self.assertEqual(UNPACK[2](r.read())['code'],'busy')
   active.sendall(body[2:]);r=http.client.HTTPResponse(active);r.begin();self.assertEqual(r.status,200);r.read()
   for s in queued[:2]:
    r=http.client.HTTPResponse(s);r.begin();self.assertEqual(r.status,200);self.assertEqual(UNPACK[2](r.read())['code'],'ok')
  finally:
   active.close()
   for s in queued:s.close()
  d=dict(self.api('diagnostics')['fields'])
  self.assertEqual(d[DIAGNOSTICS['http_accept_queue_capacity']],2)
  self.assertGreaterEqual(d[DIAGNOSTICS['http_accept_queue_ok']],2)
 def test_04_mqtt_ha_retained_and_commands(self):
  self.assertEqual(self.broker.root,'phicomm/v2/u_746573742d75736572/s30-v1b/aabbccddeeff')
  self.assertIn('phicomm/v2/u_746573742d75736572/sensor/aabbccddeeff/command',self.broker.subscriptions)
  _,payload,retained=self.broker.wait('/discovery/aabbccddeeff');self.assertTrue(retained)
  announcement=UNPACK[1](payload)
  self.assertEqual(announcement,{'schema':2,'device_id':'aabbccddeeff','model':'s30-v1b','variant':'full','firmware_version':self.api('info')['firmware_version'],'protocol':'openiot','command_protocol':'tmcp','protocol_version':2,'version':'v2','wire_version':1,'wire_formats':['msgpack'],'default_wire_format':'msgpack'})
  (RUN/'mqtt-announcement.msgpack').write_bytes(payload)
  for key in ('outlet_0','voltage','current','power','energy','meter_valid'):
   _,p,r=self.broker.wait('_'+key+'/config');self.assertTrue(r);cfg=json.loads(p);self.assertEqual(cfg['device']['model'],'S30-V1B');self.assertIn('u_746573742d75736572',cfg['unique_id'])
  _,p,_=self.broker.wait('/ha/state');self.assertIsNone(json.loads(p)['voltage'])
  _,p,_=self.broker.wait('/state',lambda p:p[:1] not in (b'{',));self.assertEqual(UNPACK[1](p)['protocol'],'openiot')
  self.broker.publish('command',PACK[2]({'request_id':'mqtt-off','action':'outlet.set','params':{'on':False}}),42,fragment=True)
  _,p,r=self.broker.wait('/result',lambda p:UNPACK[2](p).get('request_id')=='mqtt-off');self.assertFalse(r);self.assertEqual(UNPACK[2](p)['code'],'accepted');self.broker.ack(42);self.assertFalse(self.api()['power'])
  self.broker.publish('command',PACK[1]({'request_id':'retained-on','action':'outlet.set','params':{'on':True}}),43,retained=True);self.broker.ack(43);self.assertFalse(self.api()['power'])
  self.broker.publish('command',b'{"action":"outlet.set","params":{"on":true}}',44);self.broker.ack(44);self.assertFalse(self.api()['power'])
  self.broker.publish('ha/set',b'ON',45);self.broker.ack(45)
  end=time.monotonic()+5
  while time.monotonic()<end and not self.api()['power']:time.sleep(.02)
  self.assertTrue(self.api()['power'])
 def test_04_sensor_grant_scopes_and_credential_revoke(self):
  peer='031122334455';boot=0xf1234567;token=b'\0'+os.urandom(15)
  topic=self.broker.root.rsplit('/',2)[0]+'/sensor/aabbccddeeff/command'
  with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as s:
   s.bind(('127.0.0.2',0));s.settimeout(2)
   probe=self.sensor_request(peer,boot,token)
   self.assertEqual(lan_exchange(s,probe)['code'],'auth_required')
   self.sensor_grant(peer,boot,token,1001,topic=topic.replace('/u_746573742d75736572/','/u_6f74686572/'))
   self.sensor_grant(peer,boot,token,1002,topic=topic.replace('aabbccddeeff','aabbccddee00'))
   self.sensor_grant(peer,boot,token,1003,retained=True)
   self.sensor_grant(peer,boot,token,1004,f=2)
   invalid=[dict(v=3,a1=peer,boot=boot,token=b'\0'*16),dict(v=3,a1=peer,boot=boot,token=b'a'*15),
            dict(v=3,a1=peer,boot=boot,token=token.hex()),dict(v=3,a1=peer,boot=0,token=token),
            dict(v=3,a1=peer.upper(),boot=boot,token=token),dict(v=4,a1=peer,boot=boot,token=token),
            dict(v=3,a1=peer,boot=boot,token=token,scopes=1)]
   # Include alphabetic peer characters so uppercase is a distinct invalid ID.
   invalid[4]['a1']='03112233AABB'
   for n,params in enumerate(invalid,1010):self.sensor_grant(peer,boot,token,n,params=params)
   self.assertEqual(lan_exchange(s,probe)['code'],'auth_required')
   self.assertEqual(self.command('sensor.session.set',dict(v=3,a1=peer,boot=boot,token=token),'http-grant')['code'],'unsupported')
   self.sensor_grant(peer,boot,token,1020)
   # Wrong boot must be refused before it can claim the first v4 session.
   self.assertEqual(lan_exchange(s,{**probe,'boot':boot-1})['code'],'auth_required')
   self.assertEqual(lan_exchange(s,{**probe,'token':bytes([token[0]^1])+token[1:]})['code'],'auth_required')
   end=time.monotonic()+3
   while True:
    reply=lan_exchange(s,probe)
    if reply['code']=='ok':break
    self.assertLess(time.monotonic(),end,reply);time.sleep(.02)
   self.assertEqual(UNPACK[1](reply['body'])['code'],'ok')
   self.assertEqual(lan_exchange(s,{**{k:v for k,v in probe.items() if k not in ('op','body')},'op':'ack'})['code'],'acked')
   before=self.api();number=2
   for action in ('status.get','info.get','capabilities.get','diagnostics.get','power.get'):
    self.assertEqual(self.lan_result(s,self.sensor_request(peer,boot,token,number,action))['code'],'ok');number+=1
   for action,params in (('outlet.set',{'on':not before['power']}),('indicator.set',{'enabled':False}),('child_lock.set',{'enabled':True})):
    self.assertEqual(self.lan_result(s,self.sensor_request(peer,boot,token,number,action,params))['code'],'unauthorized');number+=1
   for action in ('config.set','ota.start','sensor.session.set'):
    self.assertEqual(self.lan_result(s,self.sensor_request(peer,boot,token,number,action))['code'],'unsupported');number+=1
   after=self.api();self.assertEqual((after['sequence'],after['power'],after['indicator'],after['child_lock']),
                                    (before['sequence'],before['power'],before['indicator'],before['child_lock']))
   self.assertEqual(self.command('outlet.set',{'on':False},'sensor-live-update')['code'],'accepted')
   self.assertFalse(self.lan_result(s,self.sensor_request(peer,boot,token,number))['data']['on']);number+=1
   self.assertEqual(self.command('config.set',{'section':'auth','values':{'password':'sensor-rotate'}},'sensor-pwd')['code'],'accepted')
   self.assertEqual(lan_exchange(s,self.sensor_request(peer,boot,token,number))['code'],'auth_required')
   self.__class__.token=self.login('sensor-rotate')
   self.assertEqual(self.command('config.set',{'section':'auth','values':{'password':'secret-1'}},'sensor-pwd-back')['code'],'accepted')
   self.__class__.token=self.login('secret-1')
   token=b'\0'+os.urandom(15);self.sensor_grant(peer,boot,token,1021)
   self.assertEqual(self.lan_result(s,self.sensor_request(peer,boot,token))['code'],'ok')
   self.__class__.sensor_probe=self.sensor_request(peer,boot,token,2)
 def test_05_settings_and_captured_mqtt_receipt(self):
  self.assertEqual(self.command('config.set',{'section':'report','values':{'seconds':7}},'report-save')['code'],'accepted')
  self.assertEqual(self.api()['report_seconds'],7)
  previous=self.broker.root
  body={'request_id':'mqtt-rename','action':'config.set','params':{'section':'mqtt','values':{'host':'127.0.0.1','port':self.broker.port,'user':'next-user','protocol':'tmcp'}}}
  self.broker.publish('command',PACK[1](body),46)
  topic,p,_=self.broker.wait('/result',lambda p:decode(p).get('request_id')=='mqtt-rename');self.assertEqual(topic,previous+'/result');self.assertEqual(UNPACK[1](p)['code'],'accepted')
  new='phicomm/v2/u_6e6578742d75736572/s30-v1b/aabbccddeeff'
  self.broker.wait(new+'/availability',lambda p:p==b'online');self.assertGreaterEqual(self.broker.connections,2);self.assertEqual(self.api()['mqtt_user'],'next-user');self.assertTrue(self.api()['mqtt_password_set'])
  with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as s:
   s.bind(('127.0.0.2',0));s.settimeout(2);old=self.sensor_probe
   self.assertEqual(lan_exchange(s,old)['code'],'auth_required')
   self.sensor_grant(old['src'],old['boot'],old['token'],1030,topic=previous.rsplit('/',2)[0]+'/sensor/aabbccddeeff/command')
   self.assertEqual(lan_exchange(s,old)['code'],'auth_required')
   self.sensor_grant(old['src'],old['boot'],old['token'],1031)
   self.assertEqual(self.lan_result(s,old)['code'],'ok')
 def test_06_password_change_and_empty_password(self):
  self.assertEqual(self.command('config.set',{'section':'auth','values':{'password':'new-password'}},'password-new')['code'],'accepted')
  self.assertEqual(self.http('GET','/api/v1/status',headers={'Authorization':'Bearer '+self.token})[0],401)
  self.__class__.token=self.login('new-password')
  self.assertEqual(self.command('config.set',{'section':'auth','values':{'password':''}},'password-empty')['code'],'accepted')
  for _ in range(8):
   self.assertEqual(self.http('GET','/')[0],200)
   s,_,b=self.http('GET','/api/v1/auth/status');self.assertEqual(UNPACK[1](b),{'password_required':False,'authenticated':True})
  self.assertEqual(self.login(''),'')
 def test_07_udp_v4_ephemeral_port_and_owned_response(self):
  with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as s:
   s.bind(('127.0.0.2',0));s.settimeout(2);token=lan_pair_resume(s,'')
   def request(id,action,params):
    return dict(v=4,src='001122334455',dst='aabbccddeeff',boot=888,id=id,op='command',token=token,body=PACK[1](dict(request_id='udp-'+str(id),action=action,params=params)))
   def control(request,op,**extra):
    return {**{k:v for k,v in request.items() if k not in ('op','body')},'op':op,**extra}
   mutation=request(3,'outlet.set',{'on':False});first=lan_exchange(s,mutation)
   self.assertEqual(first['code'],'ok');self.assertEqual(UNPACK[1](first['body'])['code'],'accepted');self.assertFalse(self.api()['power'])
   self.assertEqual(lan_exchange(s,mutation),first)
   self.assertEqual(lan_exchange(s,request(3,'outlet.set',{'on':True}))['code'],'conflict')
   self.assertEqual(lan_exchange(s,control(mutation,'ack'))['code'],'acked')
   self.assertEqual(lan_exchange(s,mutation)['code'],'expired')
   status=request(4,'status.get',{});first=lan_exchange(s,status);self.assertEqual(first['code'],'ok');pieces=[first['body']]
   for part in range(1,first['total']):pieces.append(lan_exchange(s,control(status,'pull',part=part))['body'])
   self.assertEqual(len(b''.join(pieces)),first['size']);self.assertEqual(UNPACK[1](b''.join(pieces))['data']['socket_states'],[False])
   self.assertEqual(lan_exchange(s,control(status,'ack'))['code'],'acked')
   self.assertNotEqual(s.getsockname()[1],11288)
 def test_08_ota_drains_receipt_pauses_and_restores_services(self):
  previous=self.broker.root;connections=self.broker.connections
  body={'request_id':'mqtt-ota','action':'ota.start','params':{'url':'http://127.0.0.1/test.rbl'}}
  self.broker.publish('command',PACK[1](body),47)
  topic,p,_=self.broker.wait('/result',lambda p:decode(p).get('request_id')=='mqtt-ota')
  self.assertEqual(topic,previous+'/result');self.assertEqual(UNPACK[1](p)['code'],'accepted')
  self.assertTrue(self.api()['ota_active'])
  power=self.api()['power']
  self.assertEqual(self.command('outlet.set',{'on':not power},'during-ota')['code'],'busy')
  self.assertEqual(self.api()['power'],power)
  with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as s:
   s.bind(('127.0.0.2',0));s.settimeout(.4);time.sleep(.2)
   discovery=dict(op='auth_pair',src='001122334466',password='',scopes=1,nonce=os.urandom(16))
   s.sendto(PACK[1](discovery),('127.0.0.1',11288))
   with self.assertRaises(socket.timeout):s.recv(4096)
   end=time.monotonic()+12
   while time.monotonic()<end:
    state=self.api()
    if not state['ota_active'] and self.broker.connections>connections:break
    time.sleep(.1)
   self.assertFalse(state['ota_active']);self.assertEqual(state['ota_state'],'failed')
   self.assertGreater(self.broker.connections,connections)
   s.settimeout(2);s.sendto(PACK[1](discovery),('127.0.0.1',11288))
   self.assertEqual(UNPACK[1](s.recv(4096))['code'],'ok')
  self.assertEqual(self.command('outlet.set',{'on':power},'after-ota')['code'],'accepted')

if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('host',type=Path);p.add_argument('run',type=Path);a,rest=p.parse_known_args();HOST=a.host.resolve();RUN=a.run.resolve();unittest.main(argv=[__file__]+rest,verbosity=2)
