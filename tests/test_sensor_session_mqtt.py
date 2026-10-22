#!/usr/bin/env python3
"""Native workers: unilateral setup, quiet sessions and M1 reboot recovery."""
import json, os, select, subprocess, sys, time
from pathlib import Path
from mqtt_observer import Observer, cleanup
ROOT=Path(__file__).resolve().parents[1]
def main(port):
    nodes=[]; grants=[]; legacy=[]
    def received(client,userdata,msg):
        if '/link/' in msg.topic: legacy.append(msg.topic)
        if msg.topic.endswith('/command'):
            value=json.loads(msg.payload)
            if value.get('action')=='sensor.session.set': grants.append((time.monotonic(),value))
    observer=Observer(port,received).start('#')
    def launch(model,ident,ip):
        node=subprocess.Popen([str(ROOT/'build/simulator'/model),ident,ip,'127.0.0.1',str(port),'session'],env={**os.environ,'SIM_BIND_IP':ip},stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True,bufsize=1)
        nodes.append(node);return node
    def call(node,action,params=None,accepted=True):
        node.stdin.write(json.dumps({'action':action,'params':params or {}})+'\n');node.stdin.flush()
        assert select.select([node.stdout],[],[],8)[0],action
        value=json.loads(node.stdout.readline())
        if accepted: assert value.get('code') in ('ok','accepted'),(action,value)
        return value
    def wait(fn,seconds=30):
        deadline=time.monotonic()+seconds
        while time.monotonic()<deadline:
            observer.check()
            value=fn()
            if value:return value
            time.sleep(.2)
        raise AssertionError('session convergence timeout')
    try:
        a=launch('A1','020000ca0001','127.78.1.1');m=launch('M1','020000cb0001','127.78.1.2')
        wait(lambda:call(a,'status.get')['data'].get('mqtt_connected'))
        wait(lambda:call(m,'status.get')['data'].get('mqtt_connected'))
        assert call(m,'sensor.session.set',{'v':2,'a1':'020000ca0001','boot':1,'token':'ab'*16},False)['code']=='unsupported'
        call(a,'binding.set',{'peers':[{'mac':'020000cb0001','ip':'127.78.1.2'}]})
        avg=lambda:call(a,'sensors.average.get')['data']
        wait(lambda:avg().get('sample_count')==1)
        assert len(grants)==1 and not legacy
        assert call(m,'binding.status.get')['data']['peers'][0]['mac']=='020000ca0001'
        first=grants[0][1]['params']; assert first['v']==2 and len(first['token'])==32
        start=time.monotonic()
        while time.monotonic()-start<65:
            assert avg().get('sample_count')==1
            observer.check();time.sleep(1)
        assert len(grants)==1 and not legacy,'healthy reads must not reauthorize or use pair topics'
        # Established UDP reads remain usable while M1's broker is unreachable.
        call(m,'config.set',{'section':'mqtt','values':{'host':'127.0.0.2','port':1,'user':'session'}})
        wait(lambda:not call(m,'status.get')['data'].get('mqtt_connected'),15)
        offline=time.monotonic()
        while time.monotonic()-offline<20:
            assert avg().get('sample_count')==1
            observer.check();time.sleep(1)
        assert len(grants)==1
        call(m,'config.set',{'section':'mqtt','values':{'host':'127.0.0.1','port':port,'user':'session'}})
        wait(lambda:call(m,'status.get')['data'].get('mqtt_connected'),15)
        m.terminate();m.wait(timeout=5);nodes.remove(m)
        m=launch('M1','020000cb0001','127.78.1.2')
        wait(lambda:len(grants)==2,100)
        wait(lambda:call(m,'binding.status.get')['data'].get('peers'),15)
        wait(lambda:avg().get('sample_count')==1,20)
        assert grants[1][1]['params']==first,'recovery reuses the current RAM session'
        assert grants[1][0]-grants[0][0]>=60 and not legacy
        print('PASS unilateral setup, MQTT-only grant, 65s quiet UDP session, broker outage, M1 restart recovery; grants=2, pair messages=0',flush=True)
    except BaseException:
        print('DEBUG grants',len(grants),'legacy',len(legacy),flush=True)
        for node in nodes:
            for action in ('binding.status.get','mqtt.diagnostics.get'):
                try: print(action,call(node,action,accepted=False),flush=True)
                except Exception: pass
        raise
    finally:cleanup(observer,nodes,failing=sys.exc_info()[0] is not None)
if __name__=='__main__':main(int(sys.argv[1]))
