"""Explicit M1-only reversible QA and timed read stress. Evidence is redacted."""
import argparse,os,base64,concurrent.futures,hashlib,json,pathlib,statistics,sys,threading,time,urllib.request,urllib.error
from client import Device,redact,save
IP='10.117.152.110'; ROOT=pathlib.Path(os.environ.get('M1_QA_ROOT','build/evidence/m1-qa'));
if not ROOT.resolve().is_relative_to(pathlib.Path('build/evidence/m1-qa').resolve()):raise ValueError('M1 evidence root must stay under m1-qa')
ROOT.mkdir(parents=True,exist_ok=True)
lock=threading.Lock()
def log(file,value):
 with lock:
  with (ROOT/file).open('a') as f:f.write(json.dumps(redact(dict(timestamp=time.time(),**value)))+'\n')
def record(name,data):log('functional.ndjson',{'check':name,'data':data})

def restore_functional(snap,task,auth_cleanup):
 actions=[]
 if auth_cleanup is not None:actions.append(('auth',auth_cleanup))
 if task is not None:actions.append(('temporary_schedule',lambda:d.set('schedules.delete',{'id':task})))
 actions.extend([
  ('config',lambda:d.set('config.set',{'section':'device','values':{'name':snap['config.get']['name']}})),
  ('time',lambda:d.set('time.set',snap['time.get'])),
  ('brightness',lambda:d.set('display.set',{'brightness':snap['status.get']['brightness_requested']*25})),
 ])
 errors=[]
 for name,action in actions:
  try:action()
  except Exception as e:
   errors.append(name);record('restore_error',{'field':name,'error':str(e)})
 actual={}
 for name in snap:
  try:actual[name]=d.get(name)
  except Exception as e:
   errors.append('readback_'+name);record('restore_error',{'field':'readback_'+name,'error':str(e)})
 checks={}
 if 'config.get' in actual:
  checks['config.get']= {k:v for k,v in actual['config.get'].items() if k!='revision'} == {k:v for k,v in snap['config.get'].items() if k!='revision'}
 if 'time.get' in actual:
  checks['time.get']= {k:v for k,v in actual['time.get'].items() if k!='revision'} == {k:v for k,v in snap['time.get'].items() if k!='revision'}
 if 'night.get' in actual:
  checks['night.get']= {k:v for k,v in actual['night.get'].items() if k!='revision'} == {k:v for k,v in snap['night.get'].items() if k!='revision'}
 if 'schedules.get' in actual:
  checks['schedules.get.items']=actual['schedules.get'].get('items') == snap['schedules.get'].get('items')
  original_ids={item.get('id') for item in snap['schedules.get'].get('items',[])}
  checks['schedules.get.id2']=([x for x in actual['schedules.get'].get('items',[]) if x.get('id')==2] ==
                               [x for x in snap['schedules.get'].get('items',[]) if x.get('id')==2])
  if task is not None and task in original_ids:checks['schedules.get.temporary_id']=False
 if 'binding.get' in actual:checks['binding.get']=actual['binding.get']==snap['binding.get']
 if 'status.get' in actual:checks['status.get.brightness_requested']=actual['status.get'].get('brightness_requested')==snap['status.get'].get('brightness_requested')
 record('restored',{'checks':checks,'data':actual})
 failed=[name for name,passed in checks.items() if not passed]
 if errors or failed:raise AssertionError({'restore_errors':errors,'restore_mismatches':failed})

def strict_json(raw):
 def pairs(items):
  result={}
  for key,value in items:
   if key in result:raise ValueError('duplicate JSON key: '+key)
   result[key]=value
  return result
 return json.loads(raw.decode('utf-8'),parse_constant=lambda value:(_ for _ in ()).throw(ValueError('invalid JSON constant: '+value)),object_pairs_hook=pairs)

def save_first_body(label,suffix,raw):
 evidence=ROOT/(label+'-first-invalid-json-'+suffix+'.body')
 with lock:
  if not evidence.exists():evidence.write_bytes(raw)
  evidence.chmod(0o600)

def response_envelope(raw,label,suffix):
 try:reply=strict_json(raw)
 except (ValueError,UnicodeError):
  save_first_body(label,suffix,raw)
  raise
 if not isinstance(reply,dict) or not isinstance(reply.get('code'),str):
  save_first_body(label,suffix,raw)
  raise ValueError('invalid command response envelope')
 if reply['code']=='ok' and not isinstance(reply.get('data'),dict):
  save_first_body(label,suffix,raw)
  raise ValueError('successful command response has no object data')
 return reply

def controlled_refusal(status,reply):
 return status in (429,503) and reply.get('code') in ('busy','overloaded','rate_limited','unavailable')

class StrictDevice(Device):
 def command(self,action,params=None,expected=('ok','accepted'),request_id=None):
  payload={'action':action,'params':{} if params is None else params}
  if request_id is not None:payload['request_id']=request_id
  status,raw=self.request('/api/v1/command',payload)
  result=response_envelope(raw,'functional',action.replace('.','_'))
  if status!=200 or (expected is not None and result['code'] not in expected):
   raise AssertionError((self.ip,action,status,redact(result)))
  return result

d=StrictDevice(IP)

def verify_candidate():
 s=d.get();assert s['device_id']=='b0f8932864b6'
 upload=json.load(open(os.environ.get('M1_QA_UPLOAD','build/evidence/m1-upload.json')))
 manifest=upload.get('manifest') or {};expected_version=manifest.get('version')
 if not isinstance(expected_version,str) or not expected_version:raise AssertionError('candidate upload evidence has no manifest version')
 unsigned=os.environ.get('M1_QA_UNSIGNED')=='1'
 if unsigned:
  candidate=json.load(open(pathlib.Path(os.environ.get('M1_QA_UPLOAD','build/evidence/m1-upload.json')).parent/'candidate.json'))
  raw=pathlib.Path(os.environ.get('M1_QA_RAW','build/m1-release/phicomm_m1.ota.bin'))
  image_sha256=hashlib.sha256(raw.read_bytes()).hexdigest()
  assert image_sha256==upload.get('raw_sha256'),('raw image hash does not match candidate upload evidence',image_sha256,upload.get('raw_sha256'))
  assert candidate.get('raw_sha256')==image_sha256,('raw image hash does not match candidate record',image_sha256,candidate.get('raw_sha256'))
  assert candidate.get('manifest')==manifest
  assert candidate.get('endpoint')=='/api/v1/ota/upload-unsigned'
  assert candidate.get('signature_skipped_by_user_authorization') is True
  assert isinstance(s.get('last_reset_reason'),int),'R18 boot marker missing'
 else:
  signed=pathlib.Path(os.environ.get('M1_QA_SIGNED','build/m1-release/phicomm_m1.ota.bin.signed'))
  image_sha256=hashlib.sha256(signed.read_bytes()).hexdigest()
  assert image_sha256==upload.get('sha256'),('signed image hash does not match candidate upload evidence',image_sha256,upload.get('sha256'))
 assert s.get('version')==expected_version,('device firmware version does not match candidate manifest',s.get('version'),expected_version)
 record('image_identity',{'unsigned':unsigned,'image_sha256':image_sha256,'expected_version':expected_version,'status':s})
 return s

def functional():
 s=verify_candidate()
 snap={'status.get':s};snap.update({a:d.get(a) for a in ['config.get','time.get','night.get','schedules.get','binding.get']});save(ROOT/'owned-snapshot.json',snap)
 task=None; auth_cleanup=None
 try:
  for n in range(3):
   s=d.get();assert s['sensor_valid'] and s['uart_errors']==0 and s['age_ms']<15000
   assert all(isinstance(s[k],(int,float)) for k in ['temperature','humidity','formaldehyde','pm25']);record('sensor',s);time.sleep(5)
  for level in [0,25,50,75,100]:
   d.set('display.set',{'brightness':level});s=d.wait(lambda x:x['brightness_requested']==level//25);record('display_requested',{'percent':level,'level':s['brightness_requested']})
  for p in [{'brightness':26},{'brightness':-1},{'brightness':101},{'brightness':'25'},{'brightness':25,'unexpected':1}]:
   r=d.command('display.set',p,expected=None);assert r['code']=='invalid_params';record('invalid_display',r)
  rid='m1qa-'+str(int(time.time())); r=d.command('display.set',{'brightness':50},request_id=rid);rr=d.command('display.set',{'brightness':50},request_id=rid);assert r==rr
  conflict=d.command('display.set',{'brightness':75},request_id=rid,expected=None);assert conflict['code']=='conflict';record('dedup',{'first':r,'replay':rr,'conflict':conflict})
  before=d.get()['ntp_last_success'];d.set('time.set',{'ntp_host':'ntp1.aliyun.com','timezone_minutes':0});s=d.wait(lambda x:x['ntp_last_success']>before and x['timezone_minutes']==0 and x['ntp_screen_queued'],seconds=90);record('ntp_primary_timezone',s)
  before=s['ntp_last_success'];d.set('time.set',{'ntp_host':'127.0.0.1','timezone_minutes':60});s=d.wait(lambda x:x['ntp_last_success']>before and x['ntp_server']!='127.0.0.1' and x['ntp_screen_queued'],seconds=120);record('ntp_fallback',s)
  d.set('time.set',snap['time.get']);d.wait(lambda x:x['timezone_minutes']==snap['time.get']['timezone_minutes'])
  when=int(time.time())+15;r=d.set('schedules.put',{'enabled':True,'trigger':'once','at':when,'action':'display.set','params':{'brightness':25}});task=r['data']['id'];record('schedule_created',r)
  d.wait(lambda x:any(t['id']==task and t['state']=='executed' and not t['enabled'] for t in x['items']),action='schedules.get',seconds=45);assert d.get()['brightness_requested']==1;record('schedule_executed',d.get('schedules.get'))
  d.set('config.set',{'section':'device','values':{'name':'M1-QA-persistence'}});u=d.get()['uptime_ms'];d.command('system.reboot');time.sleep(3)
  s=d.wait(lambda x:x['uptime_ms']<u,seconds=60);assert d.get('config.get')['name']=='M1-QA-persistence';assert any(t['id']==task for t in d.get('schedules.get')['items']);record('reboot_persistence',s)
  d.command('wifi.scan');scan=d.wait(lambda x:not x['scanning'],action='wifi.scan.get',seconds=45);record('wifi_scan',{'network_count':len(scan['networks']),'scanning':scan['scanning']})
  if snap['config.get'].get('web_password_set'):
   record('http_auth',{'executed':False,'reason':'preexisting_auth_enabled'})
  else:
   password='M1qaTemporary42'
   def authorized(action,params=None):
    req=urllib.request.Request('http://'+IP+'/api/v1/command',data=json.dumps({'action':action,'params':{} if params is None else params}).encode(),headers={'Content-Type':'application/json','Authorization':'Basic '+base64.b64encode(('admin:'+password).encode()).decode()})
    with urllib.request.urlopen(req,timeout=5) as r:return json.load(r)
   auth_cleanup=lambda:authorized('config.set',{'section':'auth','values':{'password':''}})
   d.set('config.set',{'section':'auth','values':{'password':password}});assert d.request('/api/v1/status')[0]==401
   assert authorized('status.get')['code']=='ok';record('http_auth',{'unauthorized':401,'authorized':200,'restored_after_cleanup':True})
 finally:
  restore_functional(snap,task,auth_cleanup)

def stress(label,clients,seconds,pace,start_stagger=0,fail_fast=False):
 stop=threading.Event();results=[];health=[];health_errors=[];health_refusals=[]
 idle_before=[]
 if label=='soak':
  for _ in range(7):idle_before.append(d.get());time.sleep(5)
 run_started=time.monotonic();end=run_started+seconds
 def worker(index):
  if start_stagger:stop.wait(start_stagger*index)
  local=Device(IP);paths=['/api/v1/status','/api/v1/config','/api/v1/schedules','/api/v1/logs','/','binding.status.get'];n=0
  while not stop.is_set() and time.monotonic()<end:
   started=time.monotonic();path=paths[(n+index)%len(paths)];ok=False;error=None;controlled=False;status=None;body_bytes=None
   try:
    if path[0]=='/':
     status,body=local.request(path);body_bytes=len(body);ok=status==200 and len(body)>0
     if path.startswith('/api/') or status in (429,503):
      parsed=response_envelope(body,label,path.rsplit('/',1)[-1])
      controlled=clients>=8 and controlled_refusal(status,parsed)
      ok=controlled or (status==200 and parsed['code']=='ok')
      if not ok:raise AssertionError('unexpected HTTP/API response '+str(status)+'/'+parsed['code'])
    else:
     status,raw=local.request('/api/v1/command',{'action':path,'params':{}});body_bytes=len(raw)
     body=response_envelope(raw,label,path.replace('.','_'))
     controlled=clients>=8 and controlled_refusal(status,body)
     ok=controlled or (status==200 and body['code']=='ok')
     if not ok:raise AssertionError('unexpected HTTP/API response '+str(status)+'/'+body['code'])
   except Exception as e:error=str(e)
   elapsed=time.monotonic()-started;entry={'client':index,'path':path,'ok':ok,'controlled_refusal':bool(locals().get('controlled',False)),'elapsed':elapsed,'error':error,'http_status':status,'body_bytes':body_bytes,'source_port':getattr(local,'last_source_port',None)};controlled=False;results.append(entry);log(label+'-requests.ndjson',entry)
   if not ok:
    failure_path=ROOT/(label+'-first-failure.json')
    with lock:
     if not failure_path.exists():save(failure_path,entry)
     if fail_fast:stop.set()
   n+=1
   if pace:stop.wait(max(0,pace-elapsed))
 def monitor():
  while not stop.is_set():
   try:
    status,raw=Device(IP).request('/api/v1/command',{'action':'status.get','params':{}})
    reply=response_envelope(raw,label,'health')
    if status!=200 or reply['code']!='ok':
     if clients>=8 and controlled_refusal(status,reply):
      health_refusals.append(status);log(label+'-health.ndjson',{'controlled_refusal':True,'http_status':status,'code':reply['code']});stop.wait(5);continue
     raise AssertionError('health API failed '+str(status)+'/'+reply['code'])
    s=reply['data'];health.append(s);log(label+'-health.ndjson',{'status':s})
   except Exception as e:
    health_errors.append(str(e));log(label+'-health.ndjson',{'error':str(e)})
    if fail_fast:stop.set()
   stop.wait(5)
 t=threading.Thread(target=monitor);t.start()
 with concurrent.futures.ThreadPoolExecutor(max_workers=clients) as pool:list(pool.map(worker,range(clients)))
 stop.set();t.join();run_seconds=time.monotonic()-run_started;recovery_start=time.monotonic();recovery=None
 recovery_end=recovery_start+30
 while time.monotonic()<recovery_end:
  try:
   api_status,api_raw=Device(IP).request('/api/v1/command',{'action':'status.get','params':{}})
   recovery_reply=response_envelope(api_raw,label,'recovery')
   if api_status==200 and recovery_reply['code']=='ok':
    recovery=recovery_reply['data']
    if recovery.get('sensor_valid') and recovery.get('age_ms',15001)<15000:break
   else:raise AssertionError('recovery API failed '+str(api_status)+'/'+recovery_reply['code'])
  except OSError as e:log(label+'-recovery.ndjson',{'error':str(e)})
  time.sleep(1)
 if recovery is None or not recovery.get('sensor_valid') or recovery.get('age_ms',15001)>=15000:
  save(ROOT/(label+'-summary.json'),{'clients':clients,'requested_seconds':seconds,'run_seconds':run_seconds,'count':len(results),'failures':sum(not r['ok'] for r in results),'health_errors':len(health_errors),'health_controlled_refusals':len(health_refusals),'recovery_api_pass':recovery is not None,'recovery_sensor_pass':False,'incomplete_reason':'API/sensor recovery failed'})
  raise AssertionError(label+' did not recover API and sensor within 30 seconds')
 recovery_api=recovery;assert recovery_api['device_id']=='b0f8932864b6'
 times=sorted(r['elapsed'] for r in results)
 failures=sum(not r['ok'] for r in results);controlled_count=sum(r['controlled_refusal'] for r in results)
 uptimes=[s['uptime_ms'] for s in health]
 uptime_continuous=bool(uptimes) and all(b>=a for a,b in zip(uptimes,uptimes[1:])) and recovery['uptime_ms']>=uptimes[-1]

 summary={'clients':clients,'requested_seconds':seconds,'run_seconds':run_seconds,'duration_pass':run_seconds>=seconds,'count':len(results),'failures':failures,'controlled_refusals':controlled_count,'p95':times[int(.95*(len(times)-1))] if times else None,'recovery_seconds':time.monotonic()-recovery_start,'recovery_api_pass':recovery_api['device_id']=='b0f8932864b6','recovery_sensor_pass':recovery['sensor_valid'] and recovery['age_ms']<15000,'uptime_continuous':uptime_continuous,'uptime_first':uptimes[0] if uptimes else None,'uptime_last':recovery['uptime_ms'],'sensor_invalid_samples':sum(not s['sensor_valid'] or s['age_ms']>=15000 for s in health),'heap_first':health[0]['free_memory'] if health else None,'heap_last':health[-1]['free_memory'] if health else None,'heap_min':min(s['free_memory'] for s in health) if health else None,'heap_max':max(s['free_memory'] for s in health) if health else None,'heap_samples':len(health),'health_errors':len(health_errors),'health_controlled_refusals':len(health_refusals)}
 # Preserve measured results even when an acceptance assertion below fails.
 save(ROOT/(label+'-summary.json'),summary)
 if run_seconds<seconds:raise AssertionError(label+' ran shorter than requested')
 if not uptime_continuous:raise AssertionError(label+' detected uptime discontinuity or missing health samples')
 if health_errors:raise AssertionError(label+' had uncontrolled health probe failures')
 if clients<=4 and (failures or summary['p95']>2):raise AssertionError(label+' requires zero failures and P95 <= 2 seconds')
 if clients>=8 and failures:raise AssertionError(label+' had uncontrolled failures')
 if label=='soak':
  idle_after=[]
  for _ in range(7):idle_after.append(d.get());time.sleep(5)
  save(ROOT/'soak-idle-before.json',idle_before);save(ROOT/'soak-idle-after.json',idle_after)
  before=statistics.median(x['free_memory'] for x in idle_before);after=statistics.median(x['free_memory'] for x in idle_after)
  summary.update(idle_before_median=before,idle_after_median=after,idle_decline_bytes=before-after,heap_decline_pass=before-after<=1024,reboot_detected=any(b['uptime_ms']<a['uptime_ms'] for a,b in zip(health,health[1:])))
  save(ROOT/(label+'-summary.json'),summary)
  if not summary['heap_decline_pass'] or summary['reboot_detected']:raise AssertionError('soak idle heap or uptime gate failed')
 save(ROOT/(label+'-summary.json'),summary);print(label,json.dumps(summary),flush=True);return summary
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('mode',choices=['functional','load','soak']);p.add_argument('--clients',type=int,choices=[1,4,8,16]);p.add_argument('--seconds',type=int,default=120);a=p.parse_args()
 if a.mode=='functional':functional()
 elif a.mode=='load':
  verify_candidate()
  stages=[(a.clients,a.seconds)] if a.clients else [(1,600),(4,600),(8,120),(16,120)]
  for c,seconds in stages:stress('load-'+str(c),c,seconds,1,fail_fast=True)
 else:verify_candidate();stress('soak',4,14400,4,start_stagger=1)
