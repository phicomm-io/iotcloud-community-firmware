#!/usr/bin/env python3
import json
import math
import time
import urllib.request
from scenarios import call,wait,record
A='020000010002'
URL='http://127.0.0.1:32801'
MODES=['offline','delay','drop','duplicate','reorder','wrong_token','stale','abnormal']
FIXED_VALUES=[22,45,80,.05]
AGGREGATE_FIELDS=('temperature','humidity','pm25','formaldehyde')
MIN_FAULT_WINDOW_SECONDS=8
SAMPLE_PACKET_DEADLINE_SECONDS=12
def mode(index,value):
    with urllib.request.urlopen(urllib.request.Request(URL,data=json.dumps({'index':index,'mode':value}).encode()),timeout=5) as r:r.read()
def counts():return json.load(urllib.request.urlopen(URL))
def wait_for_injections(before,assignment,cycle):
    start=time.monotonic()
    deadline=start+SAMPLE_PACKET_DEADLINE_SECONDS
    sample_modes=('wrong_token','stale','abnormal')
    while True:
        current=counts()
        complete=True
        for i,m in assignment.items():
            proxy=current[str(i)]
            complete &= proxy['counts'].get(m,0)>before[str(i)]['counts'].get(m,0)
            if m in sample_modes:
                complete &= proxy['sample_counts'].get(m,0)>before[str(i)]['sample_counts'].get(m,0)
        elapsed=time.monotonic()-start
        if complete and elapsed>=MIN_FAULT_WINDOW_SECONDS:
            return current,elapsed
        if time.monotonic()>=deadline:
            raise AssertionError((cycle,'fault injection deadline',assignment,current))
        time.sleep(.2)
def check_aggregate(context):
    aggregate=call(A,'sensors.average.get')
    count=aggregate.get('sample_count')
    assert isinstance(count,int) and 1<=count<=7,(context,'sample_count',aggregate)
    assert aggregate.get('valid') is True,(context,'invalid aggregate',aggregate)
    for key,expected in zip(AGGREGATE_FIELDS,FIXED_VALUES):
        value=aggregate.get(key)
        assert isinstance(value,(int,float)) and math.isfinite(value),(context,key,'nonfinite',value)
        assert abs(value-expected)<.01,(context,key,'fault value entered aggregate',value,expected,count)
    return {'sample_count':count,'values':{k:aggregate[k] for k in AGGREGATE_FIELDS}}
def main():
    for i in range(1,8):call(f'02000002{i:04x}',raw={'sim_sensor':{'values':FIXED_VALUES}})
    b=call(A,'binding.get')
    for i,p in enumerate(b['peers'],1):p['ip']=f'127.30.2.{i}';p['force_mqtt']=False
    call(A,'binding.set',{'peers':b['peers']})
    wait(A,'binding.status.get',lambda s:len(s['peers'])==7 and all(p['fresh'] and p['transport']=='lan' for p in s['peers']),seconds=90)
    baseline=wait(A,'sensors.average.get',lambda s:s.get('sample_count')==7 and all(abs(s.get(k,999)-v)<.01 for k,v in zip(AGGREGATE_FIELDS,FIXED_VALUES)),seconds=90)
    record('fault-sensor-baseline',status=baseline)
    completed={m:0 for m in MODES}
    for cycle in range(23):
        assignment={i:MODES[(cycle*7+i-1)%8] for i in range(1,8)}
        before=counts()
        for i,m in assignment.items():mode(i,m)
        after,window_seconds=wait_for_injections(before,assignment,cycle)
        during=call(A,'binding.status.get')
        for i,m in assignment.items():
            assert after[str(i)]['counts'].get(m,0)>before[str(i)]['counts'].get(m,0),(cycle,i,m,'no packet traversed fault')
            if m in ('wrong_token','stale','abnormal'):
                assert after[str(i)]['sample_counts'].get(m,0)>before[str(i)]['sample_counts'].get(m,0),(cycle,i,m,'no sample packet traversed fault')
            completed[m]+=1
        aggregate=None
        if any(m in ('wrong_token','stale','abnormal') for m in assignment.values()):
            aggregate=check_aggregate((cycle,assignment))
        record('fault-window',cycle=cycle,modes=assignment,window_seconds=round(window_seconds,3),status=during,aggregate=aggregate)
        for i in assignment:mode(i,'normal')
        recovered=wait(A,'binding.status.get',lambda s:all(p['fresh'] and p['transport']=='lan' for p in s['peers']),seconds=30)
        recovered_aggregate=wait(A,'sensors.average.get',lambda s:s.get('sample_count')==7 and all(abs(s.get(k,999)-v)<.01 for k,v in zip(AGGREGATE_FIELDS,FIXED_VALUES)),seconds=30)
        record('fault-recovered',cycle=cycle,status=recovered,aggregate=recovered_aggregate,completed=completed.copy())
        print(cycle,completed,flush=True)
    assert all(n>=20 for n in completed.values())
    record('fault-matrix-pass',completed=completed)
if __name__=='__main__':
    try:main()
    except Exception as e:record('fault-failure',error=str(e));raise
    finally:
        for i in range(1,8):mode(i,'normal')
