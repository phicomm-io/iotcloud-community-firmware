#!/usr/bin/env python3
"""Real broker + SDK workers; fake hardware. No production platform/ACL changes.
Run after build_host.sh with paho-mqtt and Jinja2, passing isolated broker port.
"""
import json,os,pathlib,subprocess,sys,time
import paho.mqtt.client as mqtt
from jinja2 import Environment,StrictUndefined
ROOT=pathlib.Path(__file__).resolve().parents[1]
port=int(sys.argv[1]);records=[]
c=mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
c.on_message=lambda c,u,m:records.append((m.topic,m.payload.decode(),m.retain))
c.connect('127.0.0.1',port);c.subscribe('#',1);c.loop_start()
def wait(f,seconds=12):
 end=time.monotonic()+seconds
 while time.monotonic()<end:
  if f():return
  time.sleep(.03)
 raise AssertionError('timeout')
def publish(t,o,retain=False):c.publish(t,json.dumps(o) if isinstance(o,dict) else o,qos=1,retain=retain).wait_for_publish()
def finish_worker(worker, failing, start):
 exit_before=worker.poll()
 try:
  worker.terminate()
  try:out=worker.communicate(timeout=5)[0]
  except subprocess.TimeoutExpired:
   worker.kill();out=worker.communicate(timeout=5)[0]
 except Exception as exc:
  if not failing:raise
  # Diagnostic cleanup must not replace the original scenario failure.
  print('MQTT worker cleanup failed:',type(exc).__name__,
        'exit_before_cleanup=',exit_before,flush=True)
  return ''
 if failing:
  print('MQTT worker failure diagnostics:',
        'exit_before_cleanup=',exit_before,'exit_after_cleanup=',worker.returncode,
        'received_messages=',len(records)-start,'stdout_chars=',len(out),flush=True)
  print('MQTT worker stdout tail (max 16384 chars):\n'+out[-16384:],flush=True)
 return out

try:
 for model in ('M1','A1','TC1'):
  user='v2_'+model.lower();key='u_'+user.encode().hex();base=f'phicomm/v2/{key}/{model.lower()}/aabbccddeeff';start=len(records)
  action,params=('display.set',{'brightness':50}) if model=='M1' else ('fan.set',{'on':True,'speed':40}) if model=='A1' else ('outlet.set',{'index':0,'on':True})
  command={'request_id':'retained','action':action,'params':params};publish(base+'/command',command,True)
  worker=subprocess.Popen([str(ROOT/'build/account-host'/model),'127.0.0.1',str(port),user,'12'],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
  try:
   wait(lambda:any(t==base+'/availability' and p=='online' for t,p,r in records[start:]));assert worker.poll() is None
   announcement=next(json.loads(p) for t,p,r in records[start:] if t==f'phicomm/v2/{key}/discovery/aabbccddeeff')
   assert announcement['protocol']=='tmcp' and announcement['protocol_version']==2 and announcement['version']=='v2'
   configs=[json.loads(p) for t,p,r in records[start:] if t.startswith('homeassistant/') and '/'+key+'/' in t and t.endswith('/config') and p]
   assert len(configs)>=({'M1':5,'A1':4,'TC1':17}[model]),len(configs)
   for obj in configs:
    assert obj.get('unique_id') or obj.get('uniq_id');assert obj.get('json_attributes_topic')==base+'/state';assert obj.get('retain') is False
    assert obj.get('device',{}).get('identifiers')
    topic=obj.get('command_topic',obj.get('cmd_t'))
    if topic:assert topic==base+'/command',(model,topic)
    for key2 in ('payload_on','payload_off','pl_on','pl_off','pl_prs'):
     if key2 in obj and obj[key2] not in ('ON','OFF'):assert 'action' in json.loads(obj[key2])
    env=Environment(undefined=StrictUndefined)
    for key2 in ('command_template','percentage_command_template','command_on_template','command_off_template'):
     if key2 in obj:
      values={'value':'ON' if key2=='command_template' and model=='A1' else '50','brightness':128,'red':1,'green':2,'blue':3}
      assert 'action' in json.loads(env.from_string(obj[key2]).render(**values))
   if model=='TC1':
    by={o['object_id'].rsplit('_',1)[-1]:o for o in configs if 'object_id' in o}
    energy=next(o for o in configs if 'energySession' in o.get('uniq_id','') or 'energy_session' in o.get('uniq_id',''));assert energy['state_class']=='total_increasing'
    for o in configs:
     if o.get('entity_category')=='config':assert o.get('device_class')!='outlet'
   assert not any(t==base+'/result' for t,p,r in records[start:]),'retained command executed'
   command['request_id']='same';publish(base+'/command',command);wait(lambda:any(t==base+'/result' and json.loads(p).get('request_id')=='same' for t,p,r in records[start:]));publish(base+'/command',command)
   command['params']={**params,**({'brightness':75} if model=='M1' else {'on':False})};publish(base+'/command',command)
   wait(lambda:any(t==base+'/result' and json.loads(p)['code']=='conflict' for t,p,r in records[start:]))
   snapshot=mqtt.Client(mqtt.CallbackAPIVersion.VERSION2);retained=[];snapshot.on_message=lambda c,u,m:retained.append((m.topic,m.retain));snapshot.connect('127.0.0.1',port);snapshot.subscribe(base+'/#',1);snapshot.loop_start();time.sleep(.2);snapshot.disconnect();snapshot.loop_stop()
   assert (base+'/state',True) in retained and (base+'/capabilities',True) in retained;assert not any(t==base+'/result' for t,r in retained)
   publish('homeassistant/status','online');before=len([1 for t,p,r in records[start:] if t.startswith('homeassistant/') and t.endswith('/config')]);wait(lambda:len([1 for t,p,r in records[start:] if t.startswith('homeassistant/') and t.endswith('/config')])>before)
   print(model,'PASS discovery commands/templates/attributes, retained state, command replay rejection, request dedup/conflict and HA birth recovery',flush=True)
  finally:
   failing=sys.exc_info()[0] is not None
   out=finish_worker(worker,failing,start)
   if not failing:assert 'CONTROL' in out,out
   publish(base+'/command','',True)
 # TC1 Legacy keeps its original command topic and HA identifiers.
 start=len(records);legacy='device/ztc1/AABBCCDDEEFF'
 worker=subprocess.Popen([str(ROOT/'build/account-host/TC1'),'127.0.0.1',str(port),'legacy_test','12'],env={**os.environ,'TC1_TEST_LEGACY':'1'},stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
 try:
  wait(lambda:any(t==legacy+'/availability' and p=='online' for t,p,r in records[start:]))
  configs=[json.loads(p) for t,p,r in records[start:] if t.startswith('homeassistant/') and '/AABBCCDDEEFF/' in t and t.endswith('/config') and p]
  assert configs and any(o.get('cmd_t')==legacy+'/set' for o in configs)
  publish(legacy+'/set','set socket AABBCCDDEEFF 0 1')
  wait(lambda:any(t=='homeassistant/switch/AABBCCDDEEFF/socket_0/state' and '1' in p for t,p,r in records[start:]))
  assert not any(t==legacy+'/capabilities' for t,p,r in records[start:])
  print('TC1 Legacy PASS original subscription, HA identity and text control',flush=True)
 finally:
  failing=sys.exc_info()[0] is not None
  out=finish_worker(worker,failing,start)
  if not failing:assert 'CONTROL socket=0 on=1' in out,out
finally:c.disconnect();c.loop_stop()
