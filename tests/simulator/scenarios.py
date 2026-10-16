#!/usr/bin/env python3
"""Assertions against actual production workers in separate host processes."""
import json
import os
import pathlib
import time
import urllib.request

URL='http://127.0.0.1:32800'
OUT=pathlib.Path(os.environ.get('SIM_EVIDENCE_ROOT','build/evidence/fleet'))/'scenarios.ndjson'
def record(event,**fields):
    with OUT.open('a') as f:f.write(json.dumps({'at':time.time(),'event':event,**fields})+'\n')
def call(node,action=None,params=None,raw=None):
    payload={'node':node,'request':raw or {'action':action,'params':params or {}}}
    with urllib.request.urlopen(urllib.request.Request(URL,data=json.dumps(payload).encode()),timeout=12) as r:result=json.load(r)
    if result.get('code') not in ('ok','accepted'):raise AssertionError((node,action,result))
    return result.get('data')
def wait(node,action,predicate,seconds=100):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        last=call(node,action)
        if predicate(last):return last
        time.sleep(1)
    raise AssertionError((node,action,last))
def main():
    ns=json.load(urllib.request.urlopen(URL));aa=[n for n in ns if n['id'][6:8]=='01'];mm=[n for n in ns if n['id'][6:8]=='02']
    for m in mm:call(m['id'],'binding.set',{'response_ids':[a['id']+':'+m['id'] for a in aa]})
    for a in aa:call(a['id'],'binding.set',{'peers':[{'mac':m['id'],'ip':m['ip'],'force_mqtt':False,'timeout_ms':1000,'recovery_seconds':5} for m in mm]})
    for a in aa:
        s=wait(a['id'],'binding.status.get',lambda x:x.get('available')==7)
        record('seven-peer-lan-ready',node=a['id'],status=s)
    record('matrix-ready',a1=7,m1=7,bindings=49)
    print('matrix ready',flush=True)
if __name__=='__main__':
    try:main()
    except Exception as e:record('failure',error=str(e));raise
