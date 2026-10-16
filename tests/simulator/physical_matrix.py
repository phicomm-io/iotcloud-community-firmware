#!/usr/bin/env python3
"""Explicitly authorized two-DUT/eight-per-model matrix on the dedicated broker.

The parent restores original bindings after validation. Never use production
brokers: broker restarts are opt-in and limited to this named test container.
"""
import argparse
import json
import pathlib
import subprocess
import sys
import time
import urllib.request
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'device'))
from client import Device,save
from scenarios import call,wait,record,URL
A_ID='b0f893303134'
M_ID='b0f8932864b6'
A=Device('10.117.152.111')
M=Device('10.117.152.110')
CONTAINER='phicomm-stability-20261015'

def setup():
    assert A.get()['device_id']==A_ID and M.get()['device_id']==M_ID
    for d in (A,M):
        c=d.get('config.get');assert c['mqtt_host']=='10.96.210.226' and c['mqtt_port']==32768
    nodes=json.load(urllib.request.urlopen(URL))
    aa=[n for n in nodes if n['id'][6:8]=='01']
    mm=[n for n in nodes if n['id'][6:8]=='02']
    A.set('linkage.set',{'enabled':0})
    for a in aa:
        call(a['id'],'linkage.set',{'enabled':0})
        call(a['id'],'binding.set',{'group':'','members':[]})
    all_a=[a['id'] for a in aa]+[A_ID]
    for m in mm:call(m['id'],'binding.set',{'response_ids':[a+':'+m['id'] for a in all_a]})
    M.set('binding.set',{'response_ids':[a+':'+M_ID for a in all_a]})
    for a in aa:
        peers=[{'mac':m['id'],'ip':m['ip'],'force_mqtt':False,'timeout_ms':1000,'recovery_seconds':5} for m in mm]
        peers.append({'mac':M_ID,'ip':M.ip,'force_mqtt':True,'timeout_ms':1000,'recovery_seconds':5})
        call(a['id'],'binding.set',{'peers':peers})
    peers=[{'mac':m['id'],'ip':m['ip'],'force_mqtt':True,'timeout_ms':1000,'recovery_seconds':5} for m in mm]
    peers.append({'mac':M_ID,'ip':M.ip,'force_mqtt':False,'timeout_ms':1000,'recovery_seconds':5})
    A.set('binding.set',{'peers':peers})
    for a in aa:
        s=wait(a['id'],'binding.status.get',lambda s:s['available']==8,seconds=150)
        assert s['peers'][-1]['transport']=='mqtt'
        record('eight-peer-ready',node=a['id'],status=s)
    s=A.wait(lambda s:s['available']==8,'binding.status.get',seconds=150)
    assert all(p['transport']=='mqtt' for p in s['peers'][:7]) and s['peers'][-1]['transport']=='lan'
    record('physical-eight-peer-ready',node=A_ID,status=s)
    save('build/evidence/physical-matrix.json',{'a1_count':8,'m1_count':8,'physical_a1_binding':s,'physical_m1_binding':M.get('binding.status.get')})
    print('16-device matrix ready: 8 A1, 8 M1, 64 bindings',flush=True)

def broker_faults():
    # Runtime/config APIs stay available while this isolated broker is stopped.
    details=json.loads(subprocess.check_output(['docker','inspect',CONTAINER]))[0]
    assert details['Config']['Image']=='eclipse-mosquitto:2.0.22'
    for d in (A,M):
        c=d.get('config.get');assert c['mqtt_host']=='10.96.210.226' and c['mqtt_port']==32768
    baseline={d.ip:d.get()['uptime_ms'] for d in (A,M)}
    for cycle in range(20):
        try:
            subprocess.run(['docker','stop','-t','2',CONTAINER],check=True,stdout=subprocess.DEVNULL)
            time.sleep(4)
            for d in (A,M):assert d.get()['uptime_ms']>=baseline[d.ip]
            record('broker-offline',cycle=cycle)
        finally:subprocess.run(['docker','start',CONTAINER],check=True,stdout=subprocess.DEVNULL)
        for d in (A,M):
            s=d.wait(lambda s:s['mqtt_connected'] and s['mqtt_status']=='ready',seconds=100)
            assert s['uptime_ms']>=baseline[d.ip]
            baseline[d.ip]=s['uptime_ms']
        A.wait(lambda s:s['available']==8,'binding.status.get',seconds=100)
        record('broker-recovered',cycle=cycle)
        print('broker recovery',cycle,flush=True)
    record('broker-twenty-recoveries-pass')

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('action',choices=['setup','broker-faults']);a=ap.parse_args()
    (setup if a.action=='setup' else broker_faults)()
