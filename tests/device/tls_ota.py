#!/usr/bin/env python3
"""Live TLS OTA acceptance for one explicitly named, already bootstrapped DUT."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from collections import Counter
import json
from pathlib import Path
import threading
import time
import urllib.error
from client import Device,save

def main():
 p=argparse.ArgumentParser();p.add_argument('--ip',required=True);p.add_argument('--model',choices=['a1','m1','tc1'],required=True);p.add_argument('--id',required=True);p.add_argument('--ports',type=Path,required=True);p.add_argument('--host',required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--install',action='store_true');p.add_argument('--load-only',action='store_true');p.add_argument('--body-only',action='store_true');p.add_argument('--load-clients',type=int,default=6);a=p.parse_args()
 if not 0<=a.load_clients<=16:p.error('--load-clients must be between 0 and 16')
 d=Device(a.ip,timeout=3);before=d.get();assert before['device_id']==a.id and before['version'].endswith('dirty');save(a.output/'before.json',before)
 ports=json.loads(a.ports.read_text());results=[]
 def record(name,started,details):
  item=dict(case=name,elapsed=round(time.monotonic()-started,3),**details);results.append(item);save(a.output/'results.json',results);print(json.dumps(item),flush=True)
 def idle(seconds=35):
  state=d.wait(lambda s:not s['ota_active'] and s['ota_mode']=='idle',action='ota.status.get',seconds=seconds)
  assert state.get('binding_running',True);return state
 for source in (() if a.load_only or a.body_only else ('github','cn','global')):
  start=time.monotonic();reply=d.command('ota.check',{'source':source});state=d.wait(lambda s:not s['busy'],action='ota.check.get',seconds=35);mode=idle();assert state['state'] in ('up_to_date','available'),state
  record('latest_'+source,start,dict(reply=reply['code'],manifest=state,resources=mode))
  if source=='github':
   start=time.monotonic();d.command('ota.notes');notes=d.wait(lambda s:not s['busy'],action='ota.notes.get',seconds=35);mode=idle();assert notes['state']=='ready' and 'TLS OTA validation' in notes['text'];record('notes_https',start,dict(notes=notes,resources=mode))
 for fault,transport,cancel in ([] if a.load_only else [('body','http',False)] if a.body_only else [('stall','http',True),('stall','tls',True),('slow','tls',True),('stall','silence',True),('signature','tls',False),('wrongmodel','tls',False),('truncate','tls',False),('stall','tls',False)]):
  start=time.monotonic();url=f'{"http" if transport=="http" else "https"}://{a.host}:{ports[transport]}/{fault}/{a.model}';d.command('ota.start',{'url':url});active=d.wait(lambda s:s['ota_mode']=='transfer' or (not cancel and s['ota_state']=='failed'),action='ota.status.get',seconds=10)
  if cancel:
   time.sleep(.8);cstart=time.monotonic();cancel_retries=0
   while True:
    try:
     # Cancellation is idempotent; a lost response can be retried safely.
     reply=d.command('ota.cancel',expected=None)
     if reply['code']=='accepted':break
     state=d.get('ota.status.get')
     if not state['ota_active'] and state['ota_error']=='Cancelled':break
     raise AssertionError(reply)
    except OSError:
     cancel_retries+=1
     if time.monotonic()-cstart>=4:raise
     time.sleep(.1)
   after=idle();cancel_elapsed=time.monotonic()-cstart;assert cancel_elapsed<5,cancel_elapsed
  else:after=idle(seconds=40);cancel_elapsed=None
  assert after['ota_state']=='failed',after
  status=d.get();assert status['version']==before['version'];assert status.get('mqtt_connected',False),status.get('mqtt_status');assert status.get('uptime_ms',status.get('uptime_seconds',0)*1000)>=before.get('uptime_ms',before.get('uptime_seconds',0)*1000)
  if fault=='body':
   if a.model=='tc1':assert after['ota_progress']==100 and after['ota_error']=='Image validation failed',after
   else:assert status.get('ota_signature_verified') and not status.get('ota_image_verified'),'Signature/readback outcome not observed'
  record(f'{fault}_{transport}_{"cancel" if cancel else "failure"}',start,dict(active=active,after=after,cancel_elapsed=cancel_elapsed,free_memory=status.get('free_memory'),mqtt_status=status.get('mqtt_status')))
 if a.install:
  for check_attempt in range(3):
   d.command('ota.check',{'source':'github'});manifest=d.wait(lambda s:not s['busy'],action='ota.check.get',seconds=35);idle()
   if manifest['state'] in ('available','up_to_date'):break
   record('preload_check_retry',time.monotonic(),dict(attempt=check_attempt+1,manifest=manifest));time.sleep(1)
  assert manifest['state'] in ('available','up_to_date'),manifest
  stop=threading.Event();counts=Counter();samples=[];worker_samples=[];guard=threading.Lock()
  worker_transfer=threading.Event();worker_ready=threading.Event();start=time.monotonic()
  def load(worker):
   client=Device(a.ip,timeout=2)
   while not stop.is_set():
    try:
     result=client.command('config.get' if worker%2 else 'status.get',expected=None)
     with guard:
      counts[result.get('code','?')]+=1
      state=result.get('data',{})
      if 'ota_mode' in state:
       worker_samples.append(dict(at=round(time.monotonic()-start,2),observer='load_status',**state))
       if state['ota_mode']=='transfer':worker_transfer.set()
       if state['ota_state']=='ready':worker_ready.set()
    except Exception as e:
     with guard:counts[type(e).__name__]+=1
    stop.wait(.08)
  with ThreadPoolExecutor(max_workers=max(1,a.load_clients)) as pool:
   jobs=[pool.submit(load,i) for i in range(a.load_clients)];start=time.monotonic()
   try:
    time.sleep(3);start=time.monotonic()
    # An admission refusal means the request did not enter the device. Retry
    # those bounded failures; never blindly retry an ambiguous read timeout.
    admit_deadline=time.monotonic()+20;admit_retries=0
    while True:
     try:
      reply=d.command('ota.install');assert reply['code']=='accepted';break
     except urllib.error.URLError as e:
      if not isinstance(e.reason,ConnectionRefusedError):raise
     except AssertionError as e:
      info=e.args[0]
      if not (isinstance(info,tuple) and len(info)>2 and info[2]==503):raise
     admit_retries+=1
     if time.monotonic()>=admit_deadline:raise AssertionError('OTA control admission unavailable under load')
     time.sleep(.2)
    record('load_install_accepted',start,dict(load_clients=a.load_clients,counts=dict(counts),admission_retries=admit_retries))
    deadline=time.monotonic()+220;seen_transfer=False;seen_ready=False
    while time.monotonic()<deadline:
     try:
      s=d.get('ota.status.get');samples.append(dict(at=round(time.monotonic()-start,2),**s))
      if len(samples)%20==0:
       save(a.output/'load-samples.json',samples);save(a.output/'load-counts.json',dict(counts))
       print(json.dumps(dict(progress=s['ota_progress'],state=s['ota_state'],heap=s['free_memory'],elapsed=round(time.monotonic()-start,2))),flush=True)
      if s['ota_mode']=='transfer':seen_transfer=True
      if s['ota_state']=='failed':raise AssertionError(s)
      if s['ota_state']=='ready':seen_ready=True;break
     except (OSError,TimeoutError):pass
     except AssertionError as e:
      # The HTTP admission queue may reject a poll while load clients fill it.
      info=e.args[0]
      if not (isinstance(info,tuple) and len(info)>2 and info[2]==503):raise
     if worker_ready.is_set():seen_ready=True;break
     time.sleep(.5)
    assert (seen_transfer or worker_transfer.is_set()) and seen_ready,samples[-3:]
   finally:
    stop.set();save(a.output/'load-samples.json',samples);save(a.output/'load-counts.json',dict(counts))
    with guard:save(a.output/'load-status-samples.json',list(worker_samples))
  samples=sorted(samples+worker_samples,key=lambda s:s['at'])
  ready_samples=[s for s in samples if s['ota_state']=='ready']
  # Waiting for a lower uptime distinguishes the reboot from the old image's
  # ready state, even though a test reinstall preserves the version string.
  prior_uptime=before.get('uptime_ms',before.get('uptime_seconds',0)*1000)
  after=d.wait(lambda s:s.get('uptime_ms',s.get('uptime_seconds',1e9)*1000)<prior_uptime and s.get('mqtt_connected'),seconds=180)
  assert after['device_id']==a.id and after['version']==before['version'];mode=idle();save(a.output/'after-install.json',after)
  record('github_https_ota_under_load' if a.load_clients else 'github_https_ota',start,dict(load_clients=a.load_clients,counts=dict(counts),min_sampled_heap=min(s['free_memory'] for s in samples if s['ota_active']),last_mode=ready_samples[-1],after=mode))
 save(a.output/'results.json',results)
if __name__=='__main__':main()
