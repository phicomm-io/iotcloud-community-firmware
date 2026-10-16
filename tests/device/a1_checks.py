"""Explicitly authorized A1 functional and concurrent HTTP evidence runner."""
import sys,os,time,json,pathlib,hashlib,threading,concurrent.futures,statistics,traceback,urllib.request,urllib.error,base64,argparse
from client import Device,save,redact
EVIDENCE_PARENT=pathlib.Path('build/evidence/a1-qa').resolve()
ROOT_ENV=os.environ.get('A1_QA_ROOT')
if not ROOT_ENV:raise RuntimeError('A1_QA_ROOT must select a candidate-specific directory under build/evidence/a1-qa')
OUT=pathlib.Path(ROOT_ENV).resolve()
if OUT==EVIDENCE_PARENT or not OUT.is_relative_to(EVIDENCE_PARENT):raise ValueError('A1_QA_ROOT must be a child of build/evidence/a1-qa')
MANIFEST_ENV=os.environ.get('A1_QA_MANIFEST')
UPLOAD_ENV=os.environ.get('A1_QA_UPLOAD')
if not MANIFEST_ENV or not UPLOAD_ENV:raise RuntimeError('A1_QA_MANIFEST and A1_QA_UPLOAD must identify the selected candidate and its upload evidence')
MANIFEST_PATH=pathlib.Path(MANIFEST_ENV).resolve()
SIGNED_PATH=pathlib.Path(os.environ.get('A1_QA_SIGNED',str(MANIFEST_PATH.parent/'phicomm_a1.ota.bin.signed'))).resolve()
UPLOAD_PATH=pathlib.Path(UPLOAD_ENV).resolve()
DEVICE_ID=os.environ.get('A1_DEVICE_ID','b0f893303134')
OUT.mkdir(parents=True,exist_ok=True)
if any(OUT.iterdir()):
 identity_path=OUT/'candidate-identity.json'
 if not identity_path.is_file():raise ValueError('A1_QA_ROOT is not empty and has no candidate identity; select a fresh evidence directory')
 prior=json.loads(identity_path.read_text())
 wanted=hashlib.sha256(MANIFEST_PATH.read_bytes()).hexdigest()
 if prior.get('manifest_sha256')!=wanted:raise ValueError('A1_QA_ROOT already contains evidence for a different candidate')
class EvidenceDevice(Device):
 def request(self,path,data=None):
  status,body=super().request(path,data)
  if path.startswith('/api/v1/'):
   try:json.loads(body)
   except Exception:
    directory=OUT if isinstance(data,dict) and data.get('action')=='binding.status.get' else OUT/'private'
    directory.mkdir(parents=True,exist_ok=True)
    target=directory/('invalid-response-'+str(time.time_ns())+'.bin');target.write_bytes(body);target.chmod(0o600)
    log('invalid-responses.jsonl',{'path':path,'action':data.get('action') if isinstance(data,dict) else None,'status':status,'body_path':str(target),'bytes':len(body)})
    raise
  return status,body
d=EvidenceDevice('10.117.152.111',5)
lock=threading.Lock()
def log(file,obj):
 with lock:
  with (OUT/file).open('a') as f:f.write(json.dumps(redact(json.loads(json.dumps({'utc':time.time(),**obj}))))+'\n')
def check(name,fn):
 try: result=fn();log('functional.jsonl',{'test':name,'passed':True,'result':result});return result
 except Exception as e:
  log('functional.jsonl',{'test':name,'passed':False,'error':str(e),'trace':traceback.format_exc()});raise
def verify_candidate():
 manifest=json.loads(MANIFEST_PATH.read_text());upload=json.loads(UPLOAD_PATH.read_text())
 assert manifest['target'].startswith('A1@'),manifest.get('target')
 assert manifest['version']==f"{manifest['base_version']}-{manifest['git_commit'][:6]}"+('-dirty' if manifest['dirty'] else '')
 assert upload['http_status']==202,upload.get('http_status')
 assert upload['manifest']==manifest,'upload evidence manifest does not match selected candidate manifest'
 unsigned=os.environ.get('A1_QA_UNSIGNED')=='1'
 if unsigned:
  candidate=json.loads((UPLOAD_PATH.parent/'candidate.json').read_text())
  raw_path=pathlib.Path(os.environ.get('A1_QA_RAW',str(MANIFEST_PATH.parent/'phicomm_a1.ota.bin'))).resolve()
  image_hash=hashlib.sha256(raw_path.read_bytes()).hexdigest()
  assert upload.get('raw_sha256')==image_hash,(upload.get('raw_sha256'),image_hash)
  assert candidate.get('raw_sha256')==image_hash,(candidate.get('raw_sha256'),image_hash)
  assert candidate.get('manifest')==manifest
  assert candidate.get('endpoint')=='/api/v1/ota/upload-unsigned'
  assert candidate.get('signature_skipped_by_user_authorization') is True
 else:
  image_hash=hashlib.sha256(SIGNED_PATH.read_bytes()).hexdigest()
  assert upload['sha256']==image_hash,(upload.get('sha256'),image_hash)
 status=d.get()
 assert status['device_id']==DEVICE_ID,(status.get('device_id'),DEVICE_ID)
 assert status['version']==manifest['version'],(status.get('version'),manifest['version'])
 if unsigned:assert isinstance(status.get('last_reset_reason'),int),'R18 boot marker missing'
 evidence={'manifest_path':str(MANIFEST_PATH),'manifest_sha256':hashlib.sha256(MANIFEST_PATH.read_bytes()).hexdigest(),
  'unsigned':unsigned,'image_sha256':image_hash,'upload_path':str(UPLOAD_PATH),
  'upload_http_status':upload['http_status'],'upload_endpoint':candidate.get('endpoint') if unsigned else None,
  'version':manifest['version'],'git_commit':manifest['git_commit'],
  'device_id':status['device_id'],'device_status':status}
 save(OUT/'candidate-identity.json',evidence)
 log('candidate-identity.jsonl',evidence)
 return manifest,status
def restore_owned(label,actions):
 errors=[]
 for name,fn in actions:
  try:fn()
  except Exception as e:errors.append({'action':name,'error':str(e),'trace':traceback.format_exc()})
 if errors:
  log(label+'-restore-errors.jsonl',{'errors':errors})
  raise RuntimeError(label+' restoration failed; see restore-errors evidence')
def eq(a,b):
 assert a==b,(a,b);return a
_auth_cleanup=None
_auth_cleanup_state={'restored':True}
_functional_snapshot={}
IGNORED_READBACK_FIELDS={'revision','clock_valid','utc_epoch'}
OWNED_STATUS_FIELDS=('led_enabled','led_always_on','brightness','red','green','blue','child_lock','requested_speed','requested_fan_on')
def readback_value(value,domain=None):
 if isinstance(value,dict):
  ignored=IGNORED_READBACK_FIELDS|({'state','last'} if domain=='schedules' else set())
  return {k:readback_value(v,domain) for k,v in value.items() if k not in ignored}
 if isinstance(value,list):return [readback_value(v,domain) for v in value]
 return value
def paged_items(action):
 items=[];offset=0
 while True:
  page=d.command(action,{'offset':offset,'limit':8})['data'];items.extend(page['items'])
  if page['next']<=offset:break
  offset=page['next']
 return items
def functional_readback():
 current={name:d.get(name) for name in ('status.get','config.get','light_effect.get','night.get','time.get')}
 current['status.get']={k:current['status.get'][k] for k in OWNED_STATUS_FIELDS}
 current['schedules.get']={'items':paged_items('schedules.get')}
 current['buttons.get']={'items':paged_items('buttons.get')}
 mismatches={name:{'expected':readback_value(expected,'schedules' if name=='schedules.get' else None),
                   'actual':readback_value(current[name],'schedules' if name=='schedules.get' else None)}
             for name,expected in _functional_snapshot.items()
             if readback_value(current[name],'schedules' if name=='schedules.get' else None)!=
                readback_value(expected,'schedules' if name=='schedules.get' else None)}
 assert not mismatches,('owned functional state was not restored',mismatches)
 return {'matched':list(_functional_snapshot),'ignored_readback_fields':{
  'all_domains':['revision','clock_valid','utc_epoch'],'schedules':['state','last']}}
def authentication_check(config):
 global _auth_cleanup,_auth_cleanup_state
 if config.get('web_password_set'):
  _auth_cleanup=None;_auth_cleanup_state={'restored':True}
  log('functional.jsonl',{'test':'auth','status':'not executed','reason':'snapshot config.get.web_password_set was true'})
  return False
 password='A1-QA-temporary-'+str(int(time.time()))
 _auth_cleanup_state={'restored':False}
 def authreq(path,data=None):
  req=urllib.request.Request('http://'+d.ip+path,data=None if data is None else json.dumps(data).encode(),headers={'Content-Type':'application/json','Authorization':'Basic '+base64.b64encode(('admin:'+password).encode()).decode()})
  with urllib.request.urlopen(req,timeout=5) as r:return r.status,json.loads(r.read())
 def cleanup_auth():
  status,result=authreq('/api/v1/command',{'action':'config.set','params':{'section':'auth','values':{'password':''}}})
  assert status==200 and result.get('code') in ('ok','accepted'),('authentication cleanup rejected',status,result.get('code'))
  _auth_cleanup_state['restored']=True
 _auth_cleanup=cleanup_auth
 try:
  d.command('config.set',{'section':'auth','values':{'password':password}})
  check('auth-deny',lambda:eq(d.request('/api/v1/status')[0],401))
  check('auth-allow',lambda:eq(authreq('/api/v1/status')[0],200))
 finally:
  _auth_cleanup()
 check('auth-restored',lambda:eq(d.request('/api/v1/status')[0],200))
 check('auth-config-restored',lambda:eq(d.get('config.get').get('web_password_set'),False))
 return True
def functions():
 snap={a:d.get(a) for a in ['status.get','config.get','light_effect.get','night.get','schedules.get','time.get']}
 snap['schedules.get']={'items':paged_items('schedules.get')};snap['buttons.get']={'items':paged_items('buttons.get')}
 global _functional_snapshot
 _functional_snapshot={k:v for k,v in snap.items() if k!='status.get'}
 _functional_snapshot['status.get']={k:snap['status.get'][k] for k in OWNED_STATUS_FIELDS}
 save(OUT/'snapshot.json',snap)
 s=snap['status.get'];task=None
 try:
  for n in [0,1,30,60,100]:
   check('fan-'+str(n),lambda n=n:(d.command('fan.set',{'speed':n}),d.wait(lambda s:s['applied_speed']==n and s['fan_on']==bool(n))))
  for b,c in [(0,(255,0,0)),(1,(0,255,0)),(30,(0,0,255)),(60,(127,33,220)),(100,(255,255,255))]:
   def light(b=b,c=c):
    d.command('light.set',{'enabled':True,'always_on':True,'brightness':b,'color':dict(zip('rgb',c))});r=d.get();eq(r['brightness'],(b*255+50)//100);eq(tuple(r[k] for k in ['red','green','blue']),c);return r
   check('light-'+str(b),light)
  rev=d.get('light_effect.get')['revision']
  for effect in range(5):
   def eff(effect=effect):
    nonlocal rev
    p=d.get('light_effect.get');p['effect']=effect;r=d.command('light_effect.set',p);now=d.get('light_effect.get');eq(now['effect'],effect);assert now['revision']>rev;rev=now['revision'];return now
   check('effect-'+str(effect),eff)
  for enabled in [True,False]:check('child-lock-'+str(enabled),lambda enabled=enabled:(d.command('child_lock.set',{'enabled':enabled}),eq(d.get()['child_lock'],enabled)))
  for action,params in [('fan.set',{'speed':101}),('fan.set',{'speed':-1}),('light.set',{'brightness':101}),('child_lock.set',{'enabled':1})]:check('invalid-'+action+str(params),lambda a=action,p=params:eq(d.command(a,p,expected=None)['code'],'invalid_params'))
  rid='a1-qa-dedup-'+str(int(time.time()));r=d.command('fan.set',{'speed':30},request_id=rid)
  check('dedup',lambda:eq(d.command('fan.set',{'speed':30},request_id=rid),r));check('dedup-conflict',lambda:eq(d.command('fan.set',{'speed':60},request_id=rid,expected=None)['code'],'conflict'))
  for p in ['/debug','/api/debug','/api/v1/debug','/api/v1/diagnostics','/api/v1/light/test']:check('404-'+p,lambda p=p:eq(d.request(p)[0],404))
  check('404-POST-light-test',lambda:eq(d.request('/api/v1/light/test',{})[0],404))
  check('wifi-scan',lambda:d.command('wifi.scan'));check('wifi-scan-finish',lambda:d.wait(lambda x:not x['scanning'],'wifi.scan.get',45))
  # Night caps require a valid device wall clock; this temporary NTP value is restored.
  if not d.get('binding.status.get')['clock_valid']:
   p=d.get('time.get');p['ntp_host']='ntp1.aliyun.com';d.set('time.set',p)
   check('night-clock-ready',lambda:d.wait(lambda x:x['clock_valid'],'binding.status.get',150))
  d.command('child_lock.set',{'enabled':True})
  tz=d.get('time.get')['timezone_minutes'];minute=int((time.time()+tz*60)//60)%1440
  window={'days':127,'fan':1,'light':1,'max_speed':30,'brightness':25,'block_start':0,'start':(minute-2)%1440,'end':(minute+4)%1440}
  check('night-caps',lambda:(d.command('night.set',{'windows':[window]}),d.command('fan.set',{'speed':100}),d.command('light.set',{'brightness':100}),d.wait(lambda x:x['applied_speed']==30)))
  d.command('night.set',{'windows':snap['night.get']['windows']})
  d.command('fan.set',{'speed':30});at=int(time.time())+15
  r=d.command('schedules.put',{'enabled':True,'trigger':'once','at':at,'action':'fan.set','params':{'on':True,'speed':60}});task=r['data']['id']
  check('scheduled-real-execution',lambda:d.wait(lambda x:x['applied_speed']==60,seconds=40));log('functional.jsonl',{'test':'schedule-state','result':d.get('schedules.get')})
  d.command('schedules.delete',{'id':task});task=None
  # Never overwrite a password that was already present in the snapshot.
  authentication_check(snap['config.get'])
  # Name persistence uses the same original value, so avoids unrelated config changes.
  d.command('config.set',{'section':'device','values':{'name':snap['config.get']['name']}})
  before=d.get()['uptime_ms'];d.command('system.reboot');time.sleep(4)
  check('reboot-persistence',lambda:d.wait(lambda x:x['uptime_ms']<before and x['requested_speed']==60,seconds=60))
 finally:
  actions=[]
  if task:actions.append(('delete temporary schedule',lambda:d.command('schedules.delete',{'id':task})))
  if snap['config.get'].get('web_password_set') is False and _auth_cleanup:
   actions.append(('authentication cleanup retry',lambda:_auth_cleanup() if not _auth_cleanup_state['restored'] else True))
  actions.append(('device name',lambda:d.command('config.set',{'section':'device','values':{'name':snap['config.get']['name']}})))
  if snap['config.get'].get('web_password_set') is False:
   actions.append(('authentication',lambda:eq(d.get('config.get').get('web_password_set'),False)))
  actions.extend([
   ('light effect',lambda:d.set('light_effect.set',snap['light_effect.get'])),
   ('night settings',lambda:d.set('night.set',snap['night.get'])),
   ('time settings',lambda:d.set('time.set',snap['time.get'])),
   ('fan state',lambda:d.command('fan.set',{'speed':s['requested_speed'],'on':s['requested_fan_on']})),
   ('light state',lambda:d.command('light.set',{'enabled':s['led_enabled'],'always_on':s['led_always_on'],'brightness':round(s['brightness']*100/255),'color':{'r':s['red'],'g':s['green'],'b':s['blue']}})),
   ('child lock',lambda:d.command('child_lock.set',{'enabled':s['child_lock']})),
   ('owned state readback',lambda:check('owned-state-restored',functional_readback)),
   ('post-functional snapshot',lambda:save(OUT/'after-functional.json',d.get()))])
  restore_owned('functional',actions)
 log('functional.jsonl',{'physical_confirmation_pending':['panel button child lock','visual RGB/effects/brightness','physical fan speed']})
 return snap
def extended_smoke(snapshot=None):
 # This subtest owns only one temporary button mapping and light configuration.
 original=d.get('light_effect.get');saved_button=None
 offset=0
 while offset<68:
  page=d.command('buttons.get',{'offset':offset,'limit':8})['data'];log('extended.jsonl',{'buttons_page':page})
  for item in page['items']:
   if item['key']==1 and item['clicks']==29:saved_button=item
  if page['next']<=offset:break
  offset=page['next']
 try:
  p={'key':1,'clicks':29,'action':'child_lock.set','params':{'enabled':True}}
  check('button-mapping-save',lambda:d.command('buttons.set',p))
  page=d.command('buttons.get',{'offset':60,'limit':8})['data']
  check('button-mapping-read',lambda:eq(any(x['key']==1 and x['clicks']==29 and x['action']=='child_lock.set' for x in page['items']),True))
  first=d.command('logs.get',{'after':0,'limit':2})['data'];second=d.command('logs.get',{'after':first['next'],'limit':2})['data']
  check('logs-pagination',lambda:eq(bool(set(x['sequence'] for x in first['records']) & set(x['sequence'] for x in second['records'])),False))
  log('extended.jsonl',{'logs_pages':[first,second],'sensors_average':d.get('sensors.average.get'),'linkage_readonly':d.get('linkage.get')})
  for action,params in [('logs.get',{'limit':0}),('logs.get',{'after':-1}),('buttons.get',{'offset':69}),('schedules.put',{'enabled':True,'trigger':'once','at':0,'action':'fan.set','params':{'on':True}})]:
   check('extended-invalid-'+action+str(params),lambda a=action,p=params:eq(d.command(a,p,expected=None)['code'],'invalid_params'))
  effect=d.get('light_effect.get');effect['segment'][0]['colors']=[[11,22,33],[44,55,66],[77,88,99]]
  old=dict(effect);check('segment-save',lambda:d.command('light_effect.set',effect));check('segment-readback',lambda:eq(d.get('light_effect.get')['segment'][0]['colors'],effect['segment'][0]['colors']))
  check('stale-effect-revision',lambda:eq(d.command('light_effect.set',old,expected=None)['code'],'busy'))
  task=d.command('schedules.put',{'enabled':False,'trigger':'weekly','days':127,'minute':100,'action':'fan.set','params':{'on':True,'speed':30}})['data']
  try:
   check('schedule-enable',lambda:d.command('schedules.enable',{'id':task['id'],'enabled':True}))
   check('schedule-disable',lambda:d.command('schedules.enable',{'id':task['id'],'enabled':False}))
  finally:d.command('schedules.delete',{'id':task['id']})
 finally:
  restore={'key':1,'clicks':29,'action':'none','params':{}} if saved_button is None else {k:v for k,v in saved_button.items() if k in ('key','clicks','seconds','action','params')}
  restore_owned('extended',[('light effect',lambda:d.set('light_effect.set',original)),
   ('button mapping',lambda:d.command('buttons.set',restore)),
   ('button mapping readback',lambda:check('button-mappings-restored',lambda:eq(readback_value({'items':paged_items('buttons.get')}),readback_value(snapshot['buttons.get'] if snapshot else {'items':[saved_button] if saved_button else []}))))])
 save(OUT/'after-functional.json',d.get())
def p95(values):
 values=sorted(values)
 return values[int((len(values)-1)*.95)] if values else None
def recovery(label,baseline_uptime,timeout=30):
 start=time.monotonic();deadline=start+timeout;previous=baseline_uptime;samples=[];last_error=None;uptime_continuous=True
 while time.monotonic()<deadline:
  try:
   status=d.get();binding=d.get('binding.status.get')
   sample={'elapsed_seconds':round(time.monotonic()-start,3),'status':status,'binding':binding,
    'uptime_continuous':status['uptime_ms']>=previous,
    'ready':bool(status['panel_ready'] and status['fan_ready'] and status['wifi_connected'])}
   if not sample['uptime_continuous']:
    uptime_continuous=False;sample['error']='uptime regressed during recovery'

   previous=status['uptime_ms'];samples.append(sample);log(label+'-recovery.jsonl',sample)
   if sample['ready']:
    summary={'passed':uptime_continuous,'ready':True,'recovery_seconds':sample['elapsed_seconds'],'samples':len(samples),
     'uptime_continuous':uptime_continuous,'panel_ready':status['panel_ready'],'fan_ready':status['fan_ready'],
     'wifi_connected':status['wifi_connected'],'mqtt_connected':status.get('mqtt_connected'),
     'last_uptime_ms':status['uptime_ms'],'last_free_memory':status.get('free_memory'),
     'minimum_free_memory':min((x['status'].get('free_memory') for x in samples if x.get('status')),default=None)}
    save(OUT/(label+'-recovery-summary.json'),summary);return summary
  except Exception as e:
   last_error=str(e);log(label+'-recovery.jsonl',{'elapsed_seconds':round(time.monotonic()-start,3),'error':last_error})
  time.sleep(min(5,max(0,deadline-time.monotonic())))
 summary={'passed':False,'recovery_seconds':round(time.monotonic()-start,3),'samples':len(samples),
  'ready':False,'uptime_continuous':uptime_continuous,
  'reason':'not ready within timeout','last_error':last_error,
  'last_uptime_ms':samples[-1]['status'].get('uptime_ms') if samples and samples[-1].get('status') else None,
  'last_free_memory':samples[-1]['status'].get('free_memory') if samples and samples[-1].get('status') else None,
  'minimum_free_memory':min((x['status'].get('free_memory') for x in samples if x.get('status')),default=None)}
 save(OUT/(label+'-recovery-summary.json'),summary);return summary
def controlled_refusal(status,reply):
 return status==503 and isinstance(reply,dict) and reply.get('code')=='busy'
def sample_binding(device,clients,label,controlled_health):
 code,raw=device.request('/api/v1/command',{'action':'binding.status.get','params':{}})
 reply=json.loads(raw)
 assert isinstance(reply,dict) and isinstance(reply.get('code'),str),('invalid binding health response envelope',reply)
 if clients>=8 and controlled_refusal(code,reply):
  entry={'probe':'binding.status.get','http_status':code,'code':reply['code']}
  controlled_health.append(entry);log(label+'-health.jsonl',{'controlled_refusal':True,**entry})
  return None,True
 assert code==200 and reply['code']=='ok',('binding health API failure',code,reply.get('code'))
 data=reply.get('data')
 assert isinstance(data,dict),('invalid binding health data',reply)
 return data,False
def load(clients,seconds,label,rate=0,save_summary=True):
 baseline_status=d.get();baseline=baseline_status['uptime_ms'];started=time.monotonic();stop=started+seconds
 results=[];health=[];controlled_health=[];fatal=threading.Event();barrier=threading.Barrier(clients+1)
 paths=['/api/v1/status','/api/v1/config','/api/v1/schedules','/api/v1/logs','/','binding']
 def worker(i):
  barrier.wait()
  seq=0
  while time.monotonic()<stop and not fatal.is_set():
   start=time.monotonic();p=paths[(seq+i)%len(paths)];ok=False;error=None;error_type=None
   code=None;reply_code=None;controlled=False;root_503=False;body=None
   try:
    if p=='binding':
     code,body=d.request('/api/v1/command',{'action':'binding.status.get','params':{}})
     try:body=json.loads(body)
     except Exception:
      log(label+'-invalid-binding-json.jsonl',{'client':i,'http_status':code,'bytes':len(body)});raise
    else:
     code,body=d.request(p)
     if p=='/' and code==200:
      assert b'</html>' in body.lower(),('incomplete html',code,len(body))
     elif p=='/' and code!=200:
      try:body=json.loads(body)
      except Exception:pass
     elif p!='/':
      try:body=json.loads(body)
      except Exception:
       private=OUT/'private';private.mkdir(exist_ok=True)
       target=private/(label+'-invalid-'+str(i)+'-'+str(time.time_ns())+'.bin');target.write_bytes(body);target.chmod(0o600)
       log(label+'-invalid-json.jsonl',{'client':i,'path':p,'private_body_path':str(target),'http_status':code});raise
    if isinstance(body,dict):reply_code=body.get('code')
    root_503=code==503 and isinstance(body,dict) and reply_code=='busy'
    controlled=clients>=8 and controlled_refusal(code,body)
    if controlled:ok=True
    elif p=='/':ok=code==200 and isinstance(body,(bytes,bytearray)) and b'</html>' in body.lower()
    else:
     assert isinstance(body,dict) and isinstance(reply_code,str),('invalid response envelope',body)
     ok=code==200 and reply_code=='ok'
     assert ok,(code,reply_code)
   except Exception as e:error=str(e);error_type=type(e).__name__
   elapsed=time.monotonic()-start;row={'client':i,'path':p,'ok':ok,'http_status':code,
    'reply_code':reply_code,'root_503':root_503,'controlled_refusal':controlled,
    'seconds':elapsed,'error_type':error_type,'error':error};log(label+'-requests.jsonl',row);results.append(row);seq+=1
   if not ok and clients<=4:fatal.set()
   if rate:time.sleep(max(0,1/rate-elapsed))
 def sampler():
  barrier.wait()
  last=baseline
  while time.monotonic()<stop and not fatal.is_set():
   try:
    code,raw=d.request('/api/v1/command',{'action':'status.get','params':{}});reply=json.loads(raw)
    assert isinstance(reply,dict) and isinstance(reply.get('code'),str),('invalid health response envelope',reply)
    if clients>=8 and controlled_refusal(code,reply):
     entry={'probe':'status.get','http_status':code,'code':reply['code']}
     controlled_health.append(entry)
     log(label+'-health.jsonl',{'controlled_refusal':True,'root_503':code==503 and reply.get('code')=='busy',**entry})
     time.sleep(5);continue
    assert code==200 and reply['code']=='ok',('health API failure',code,reply.get('code'))
    s=reply['data'];assert isinstance(s,dict),('invalid health data',reply)
    binding,refused=sample_binding(d,clients,label,controlled_health)
    if refused:
     time.sleep(5);continue
    log(label+'-health.jsonl',{'status':s,'binding':binding});health.append(s)
    if s['uptime_ms']<last:log(label+'-fatal.jsonl',{'reason':'unexpected reboot','status':s});fatal.set()
    last=s['uptime_ms']
   except Exception as e:
    log(label+'-health.jsonl',{'error':str(e)})
    fatal.set()
   time.sleep(5)
 with concurrent.futures.ThreadPoolExecutor(max_workers=clients+1) as pool:
  jobs=[pool.submit(worker,i) for i in range(clients)]+[pool.submit(sampler)]
  for j in jobs:j.result()
 elapsed=time.monotonic()-started;failures=[r for r in results if not r['ok']]
 duration_ok=elapsed>=seconds-5
 p95_value=p95([r['seconds'] for r in results])
 normal=clients<=4
 per_client={str(i):sum(1 for r in results if r['client']==i) for i in range(clients)}
 min_per_client=100 if clients==1 else (25 if normal else max(5,seconds-10) if rate else 5)
 sample_ok=len(results)>=max(100 if normal else clients*5,clients*min_per_client) and all(n>=min_per_client for n in per_client.values())
 semantic_failures=[r for r in failures if 'AssertionError' in (r['error'] or '')]
 controlled_count=sum(r.get('controlled_refusal',False) for r in results)+len(controlled_health)
 reset_failures=[r for r in failures if r['error_type'] in ('ConnectionResetError','RemoteDisconnected') or
  'connection reset' in (r['error'] or '').lower() or 'remote end closed' in (r['error'] or '').lower()]
 http_anomalies=[r for r in results if r['http_status'] not in (None,200) and not r['controlled_refusal']]
 root_503_count=sum(r['root_503'] for r in results)+sum(x['http_status']==503 and x['code']=='busy' for x in controlled_health)
 summary={'clients':clients,'requested_seconds':seconds,'elapsed_seconds':round(elapsed,3),
  'requests':len(results),'requests_per_client':per_client,'minimum_per_client_sample':min_per_client,
  'failed':len(failures),'controlled_refusals':controlled_count,
  'root_503_controlled':root_503_count,'http_anomalies':len(http_anomalies),
  'connection_reset_failures':len(reset_failures),
  'uncontrolled_failures':sum(not r['ok'] and not r.get('controlled_refusal',False) for r in results),
  'semantic_failures':len(semantic_failures),
  'p95_seconds':p95_value,'duration_ok':duration_ok,'sample_ok':sample_ok,
  'initial_uptime_ms':baseline_status.get('uptime_ms'),'initial_free_memory':baseline_status.get('free_memory'),
  'minimum_free_memory':min((x.get('free_memory') for x in health),default=None),
  'last_free_memory':health[-1].get('free_memory') if health else None,
  'normal_p95_ok':(p95_value is not None and p95_value<=2) if normal else None,
  'four_client_zero_failures':(len(failures)==0) if clients==4 else None,
  'fatal':fatal.is_set(),'last_uptime_ms':health[-1]['uptime_ms'] if health else None}
 summary['passed']=duration_ok and sample_ok and (not failures if normal else summary['uncontrolled_failures']==0 and not fatal.is_set()) and (summary['normal_p95_ok'] is not False)
 if save_summary:save(OUT/(label+'-summary.json'),summary);print(json.dumps(summary),flush=True)
 return summary
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('mode',choices=['functional','load','soak']);p.add_argument('--clients',type=int,choices=[1,4,8,16]);p.add_argument('--seconds',type=int,default=120);args=p.parse_args()
 verify_candidate()
 if args.mode=='functional':
  snap=functions();extended_smoke(snap);check('owned-state-final-readback',functional_readback)
  save(OUT/'after-functional.json',d.get())
 elif args.mode=='load':
  clients=args.clients or 16
  r=load(clients,args.seconds,'load-'+str(clients),rate=1,save_summary=False)
  recovered=recovery('load-'+str(clients),r['last_uptime_ms'] or r['initial_uptime_ms'],timeout=30)
  r['recovery']=recovered
  r['passed']=r['passed'] and recovered['passed']
  save(OUT/('load-'+str(clients)+'-summary.json'),r)
  print(json.dumps(r),flush=True)
  if not r['passed'] or r['fatal']:raise SystemExit(2)
 else:
  def idle(name):
   samples=[];started=time.monotonic()
   for i in range(12):
    s=d.get();row={'sample':i,'elapsed_seconds':round(time.monotonic()-started,3),'free_memory':s['free_memory'],'uptime_ms':s['uptime_ms'],'panel_ready':s['panel_ready'],'fan_ready':s['fan_ready'],'wifi_connected':s['wifi_connected']};samples.append(row);log(name+'-idle.jsonl',row);time.sleep(5)
   values=[x['free_memory'] for x in samples]
   return {'median_free_memory':statistics.median(values),'min_free_memory':min(values),'max_free_memory':max(values),
    'first_free_memory':values[0],'last_free_memory':values[-1],'samples':len(values)}
  warm=idle('pre-soak');r=load(4,14400,'soak',rate=.25)
  if not r['passed'] or r['fatal']:raise SystemExit(2)
  recovered=recovery('soak',r['last_uptime_ms'] or d.get()['uptime_ms'])
  check('soak-recovery',lambda:eq(recovered['passed'],True))
  end=idle('post-soak');drop=warm['median_free_memory']-end['median_free_memory']
  soak_summary={'pre_idle':warm,'post_idle':end,'idle_median_drop':drop,'within_1kib':drop<=1024,
   'idle_heap_pass':drop<=1024,'load_pass':r['passed'],'recovery_pass':recovered['passed']}
  save(OUT/'soak-idle-summary.json',soak_summary)
  log('soak-idle-trend.jsonl',{'phase':'pre','summary':warm});log('soak-idle-trend.jsonl',{'phase':'post','summary':end,'median_drop':drop})
  if not soak_summary['idle_heap_pass']:raise SystemExit(3)
