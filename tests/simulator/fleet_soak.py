#!/usr/bin/env python3
"""Lightweight health sampling for an already-running 14-node host fleet."""
import argparse
import concurrent.futures
import datetime
import json
import math
import os
import pathlib
import time
import urllib.request

ROOT=pathlib.Path(__file__).resolve().parents[2]
DEFAULT_OUT=ROOT/'build/evidence/fleet/soak-health.ndjson'
BASE_URL='http://127.0.0.1:32800'
A1=[f'02000001{i:04x}' for i in range(1,8)]
M1=[f'02000002{i:04x}' for i in range(1,8)]
FIELDS=('temperature','humidity','pm25','formaldehyde')
LIMITS={'temperature':(-40,85),'humidity':(0,100),'pm25':(0,1000),'formaldehyde':(0,10)}
PHYSICAL_M1='b0f8932864b6'
NATIVE_M1={f'02000002{i:04x}' for i in range(1,8)}

def request(path, payload=None, timeout=4):
    data=None if payload is None else json.dumps(payload).encode()
    req=urllib.request.Request(BASE_URL+path,data=data,headers={'Content-Type':'application/json'})
    with urllib.request.urlopen(req,timeout=timeout) as response:
        return json.load(response)

def api_call(node, action):
    result=request('/',{'node':node,'request':{'action':action,'params':{}}})
    if result.get('code')!='ok':
        raise AssertionError('unexpected API envelope code '+str(result.get('code')))
    return result.get('data')

def check_process(node,pid):
    os.kill(pid,0)
    argv=pathlib.Path(f'/proc/{pid}/cmdline').read_bytes().decode(errors='replace').split('\0')
    model='A1' if node[6:8]=='01' else 'M1'
    if len(argv)<2 or argv[1]!=node or not argv[0].endswith('/'+model):
        raise AssertionError('fleet process identity mismatch')

def check_status(node,status):
    if status.get('id')!=node or status.get('mqtt_connected') is not True:
        raise AssertionError('runtime status or MQTT state invalid')
    uptime=status.get('uptime_ms')
    if not isinstance(uptime,int) or uptime<0:
        raise AssertionError('runtime uptime invalid')
    errors=status.get('mqtt_errors')
    if not isinstance(errors,int) or errors<0:
        raise AssertionError('runtime MQTT error counter invalid')
    return uptime,errors

def check_binding(binding,expected_peers):
    peers=binding.get('peers')
    if binding.get('available')!=expected_peers or not isinstance(peers,list) or len(peers)!=expected_peers:
        raise AssertionError('binding count does not match configured matrix')
    if expected_peers==7:
        if {p.get('mac') for p in peers}!=NATIVE_M1 or any(p.get('fresh') is not True or p.get('transport')!='lan' for p in peers):
            raise AssertionError('native seven-peer LAN matrix is not healthy')
    else:
        physical=[p for p in peers if p.get('mac')==PHYSICAL_M1]
        native=[p for p in peers if p.get('mac')!=PHYSICAL_M1]
        if len(physical)!=1 or {p.get('mac') for p in native}!=NATIVE_M1:
            raise AssertionError('physical eight-peer membership is incorrect')
        if physical[0].get('fresh') is not True or physical[0].get('transport')!='mqtt':
            raise AssertionError('physical M1 MQTT peer is not healthy')
        if any(p.get('fresh') is not True or p.get('transport')!='lan' for p in native):
            raise AssertionError('native M1 LAN peer is not healthy')
    if any(p.get('fresh') is not True for p in peers):
        raise AssertionError('binding peer is stale')

def check_average(average,expected_peers):
    if average.get('valid') is not True or average.get('sample_count')!=expected_peers:
        raise AssertionError('configured sensor aggregate is unavailable')
    values={}
    for key in FIELDS:
        value=average.get(key)
        if not isinstance(value,(int,float)) or not math.isfinite(value):
            raise AssertionError('aggregate contains a nonfinite value')
        low,high=LIMITS[key]
        if not low<=value<=high:
            raise AssertionError('aggregate is outside its valid range')
        values[key]=value
    return values

def write_record(path,record):
    with path.open('a') as stream:
        stream.write(json.dumps(record,separators=(',',':'),allow_nan=False)+'\n')

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--duration-seconds',type=float,default=14400)
    parser.add_argument('--interval-seconds',type=float,default=5)
    parser.add_argument('--output',type=pathlib.Path,default=DEFAULT_OUT)
    parser.add_argument('--expected-peers',type=int,choices=(7,8),default=7)
    parser.add_argument('--physical-matrix',action='store_true')
    args=parser.parse_args()
    if args.duration_seconds<=0 or args.interval_seconds<=0:
        parser.error('duration and interval must be positive')
    if args.physical_matrix:args.expected_peers=8
    args.output.parent.mkdir(parents=True,exist_ok=True)
    started=time.monotonic();deadline=started+args.duration_seconds
    expected_pids={};last_uptime={};last_mqtt_errors={};samples=0;failures=0;overruns=0
    summary_path=args.output.with_name('soak-health-summary.json')
    pool=concurrent.futures.ThreadPoolExecutor(max_workers=28)
    try:
        while time.monotonic()<deadline:
            tick=time.monotonic();errors=[];metrics={}
            try:
                nodes=request('/')
                by_id={n['id']:n for n in nodes}
                if set(by_id)!=(set(A1)|set(M1)):
                    raise AssertionError('fleet node set changed')
                for node in sorted(by_id):
                    pid=by_id[node]['pid']
                    if node in expected_pids and expected_pids[node]!=pid:
                        raise AssertionError('fleet process restarted: '+node)
                    expected_pids[node]=pid
                    check_process(node,pid)
                jobs={}
                for node in A1:
                    jobs[(node,'status')]=pool.submit(api_call,node,'status.get')
                    jobs[(node,'binding')]=pool.submit(api_call,node,'binding.status.get')
                    jobs[(node,'average')]=pool.submit(api_call,node,'sensors.average.get')
                for node in M1:
                    jobs[(node,'status')]=pool.submit(api_call,node,'status.get')
                for (node,kind),future in jobs.items():
                    try:
                        result=future.result(timeout=6)
                        if kind=='status':
                            uptime,mqtt_errors=check_status(node,result)
                            if node in last_uptime and uptime<last_uptime[node]:
                                raise AssertionError('runtime uptime decreased')
                            if node in last_mqtt_errors and mqtt_errors!=last_mqtt_errors[node]:
                                raise AssertionError('runtime MQTT error counter increased')
                            last_uptime[node]=uptime
                            last_mqtt_errors[node]=mqtt_errors
                            metrics.setdefault(node,{})['uptime_ms']=uptime
                            metrics[node]['mqtt_errors']=mqtt_errors
                        elif kind=='binding':check_binding(result,args.expected_peers)
                        else:metrics.setdefault(node,{})['aggregate']=check_average(result,args.expected_peers)
                    except Exception as exc:
                        errors.append({'node':node,'check':kind,'error':str(exc)[:200]})
                samples+=1
            except Exception as exc:
                errors.append({'check':'fleet','error':str(exc)[:200]})
            if errors:failures+=len(errors)
            elapsed=time.monotonic()-started
            write_record(args.output,{
                'at':datetime.datetime.now(datetime.timezone.utc).isoformat(),
                'elapsed_seconds':round(elapsed,3),'sample':samples,'errors':errors,
                'nodes':metrics})
            next_tick=tick+args.interval_seconds
            if time.monotonic()>next_tick:
                overruns+=1
            else:
                time.sleep(max(0,next_tick-time.monotonic()))
    except KeyboardInterrupt:
        pass
    finally:
        pool.shutdown(wait=True,cancel_futures=True)
        summary={
            'started_at':datetime.datetime.fromtimestamp(time.time()-(time.monotonic()-started),datetime.timezone.utc).isoformat(),
            'elapsed_seconds':round(time.monotonic()-started,3),
            'requested_seconds':args.duration_seconds,'interval_seconds':args.interval_seconds,
            'expected_peers':args.expected_peers,
            'api_requests_per_sample':28,
            'pids':expected_pids,'final_uptime_ms':last_uptime,
            'final_mqtt_errors':last_mqtt_errors,
            'samples':samples,'failures':failures,'overruns':overruns,
            'complete':time.monotonic()-started>=args.duration_seconds,
            'host_runtime_only':True,'heap_or_stack_proof':False,
            'health_log':str(args.output)}
        summary_path.write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps(summary,sort_keys=True),flush=True)
    if not summary['complete'] or failures:
        raise SystemExit(1)

if __name__=='__main__':main()
