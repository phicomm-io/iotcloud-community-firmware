#!/usr/bin/env python3
"""Production MQTT/runtime/source cache against an isolated local broker."""
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import time
import paho.mqtt.client as mqtt
from mqtt_observer import Observer, cleanup

def main(port):
    records=[]
    observer=Observer(port,lambda c,u,m:records.append((m.topic,m.payload))).start('#')
    worker=None
    account='u_'+b'external_co2'.hex()
    topic=f'phicomm/v2/{account}/sensor/123456abcdef/state'
    try:
        worker=subprocess.Popen([os.environ.get('PHICOMM_M1_SIMULATOR', str(Path('build/simulator/M1').resolve())),'aabbccddeeff','127.0.0.1','127.0.0.1',str(port),'external_co2'],stdin=subprocess.PIPE,stdout=subprocess.PIPE)
        def command(action,params=None):
            worker.stdin.write((json.dumps({'request_id':str(time.monotonic_ns()),'action':action,'params':params or {}})+'\n').encode());worker.stdin.flush()
            assert select.select([worker.stdout],[],[],5)[0],'simulator command timeout'
            reply=json.loads(worker.stdout.readline());assert reply['code'] in ('ok','accepted'),reply
            return reply.get('data',{})
        observer.wait(lambda:any(t.endswith('/availability') and p==b'online' for t,p in records),label='M1 online')
        command('co2_source.set',{'source':'mqtt','device_id':'123456abcdef','double_click':'switch_display','revision':0})
        body={'schema':2,'device_id':'123456abcdef','boot':123,'sequence':1,'sensors':{'co2':{'name':'Carbon dioxide','type':'number','valid':True,'value':1234,'unit':'ppm','age_ms':0}}}
        def send(value,wire_topic=topic,retain=False):observer.publish(wire_topic,json.dumps(value),retain=retain)
        def value():return command('co2_source.get')
        send(body);observer.wait(lambda:value().get('ppm')==1234,label='selected source')
        duplicate={**body,'sensors':{'co2':{**body['sensors']['co2'],'value':999}}}
        send(duplicate);time.sleep(.15);assert value()['ppm']==1234,'duplicate sample must not refresh data'
        # A different account and a different sensor cannot replace the source.
        send({**body,'sequence':2},topic.replace(account,'u_666f726569676e'))
        send({**duplicate,'sequence':2,'device_id':'abcdef123456'},topic.replace('123456abcdef','abcdef123456'))
        time.sleep(.15);assert value()['ppm']==1234
        bad_unit={**body,'sequence':2,'sensors':{'co2':{'name':'Carbon dioxide','type':'number','valid':True,'value':1800,'unit':'mg/m³','age_ms':0}}}
        send(bad_unit);observer.wait(lambda:not value()['valid'],label='invalid unit')
        send({**body,'sequence':3,'sensors':{'co2':{'name':'Carbon dioxide','type':'number','valid':True,'value':1500,'unit':'ppm','age_ms':19000}}})
        observer.wait(lambda:value().get('ppm')==1500,label='fresh recovery')
        observer.wait(lambda:not value()['valid'],seconds=4,label='expired source fallback')
        # Bad envelope types cannot replace the last valid sample.
        send({**body,'sequence':4})
        observer.wait(lambda:value().get('ppm')==1234,label='valid baseline')
        for key,invalid in [('schema',True),('schema',2.0),('boot',0),('boot',True),('sequence',1.5),('device_id','123456abcdeF')]:
            send({**body,'sequence':5,key:invalid})
        time.sleep(.15);assert value()['ppm']==1234
        # Live retained publishes may be fresh; replay after reconnect is not.
        send({**body,'sequence':6},retain=True)
        observer.wait(lambda:value().get('ppm')==1234,label='retained live sample')
        records.clear()
        takeover=mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,client_id=account+'_m1_aabbccddeeff',reconnect_on_failure=False)
        try:
            takeover.connect('127.0.0.1',port)
            assert takeover.loop(timeout=2)==mqtt.MQTT_ERR_SUCCESS
        finally:
            takeover.disconnect()
        observer.wait(lambda:any(t.endswith('/availability') and p==b'offline' for t,p in records),label='forced reconnect offline')
        observer.wait(lambda:any(t.endswith('/availability') and p==b'online' for t,p in records),seconds=20,label='forced reconnect online')
        time.sleep(.2);assert not value()['valid'],'retained replay cannot restore source freshness'
        send({**body,'sequence':7})
        observer.wait(lambda:value().get('ppm')==1234,label='live sample after reconnect')
        observer.publish(topic,b'',retain=True)
        print('PASS M1 external CO2: isolation, types, replay, units, expiry and reconnect freshness')
    finally:cleanup(observer,[worker],failing=sys.exc_info()[0] is not None)

if __name__=='__main__':main(int(sys.argv[1]))
