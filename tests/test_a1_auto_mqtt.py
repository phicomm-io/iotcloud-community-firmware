#!/usr/bin/env python3
"""Exercise production A1/M1 workers and HA discovery on an isolated broker."""
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import time
import paho.mqtt.client as mqtt
from jinja2 import Environment, StrictUndefined

ROOT = Path(__file__).resolve().parents[1]


def main(port):
    records=[]
    client=mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_message=lambda c,u,m:records.append((m.topic,m.payload.decode(),m.retain))
    client.connect('127.0.0.1',port);client.subscribe('#',1);client.loop_start()
    nodes=[]
    def launch(model,ident,ip):
        env={**os.environ,'SIM_BIND_IP':ip}
        p=subprocess.Popen([str(ROOT/'build/simulator'/model),ident,ip,'127.0.0.1',str(port),'auto_test'],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True,bufsize=1)
        nodes.append(p);return p
    def call(node,action=None,params=None,raw=None):
        q=raw if raw is not None else {'action':action,'params':params or {}}
        node.stdin.write(json.dumps(q)+'\n');node.stdin.flush()
        if not select.select([node.stdout],[],[],8)[0]:raise AssertionError(('command timeout',q))
        r=json.loads(node.stdout.readline())
        assert r.get('code') in ('ok','accepted'),(q,r)
        return r.get('data',{})
    def wait(fn,seconds=25):
        end=time.monotonic()+seconds
        while time.monotonic()<end:
            value=fn()
            if value:return value
            time.sleep(.15)
        raise AssertionError('automatic control timeout')
    def publish(topic,payload):client.publish(topic,payload,qos=1).wait_for_publish()
    try:
        a=launch('A1','020000aa0001','127.75.1.1')
        m=launch('M1','020000bb0001','127.75.1.2')
        state=lambda:call(a,'status.get')
        wait(lambda:state().get('mqtt_connected'))
        call(a,'fan.set',{'speed':64})
        call(a,'fan.mode.set',{'mode':'auto'})
        assert state()['percentage']==30
        call(a,'fan.set',{'speed':45})
        assert state()['fan_mode']=='auto' and state()['requested_speed']==64
        call(a,'fan.mode.set',{'mode':'manual'})
        assert state()['percentage']==64
        call(a,'fan.set',{'on':False})
        call(a,'fan.mode.set',{'mode':'auto'})
        assert not state()['fan_on'] and state()['fan_mode']=='auto'
        call(a,'fan.mode.toggle');assert state()['fan_mode']=='manual' and not state()['fan_on']
        call(a,raw={'sim_button':{'key':0,'clicks':2,'seconds':0}})
        assert state()['fan_mode']=='auto' and not state()['fan_on']
        call(a,raw={'sim_panel':-5})
        assert state()['fan_on'] and state()['auto_max_speed']==40 and state()['requested_speed']==64
        call(a,'child_lock.set',{'enabled':True})
        call(a,raw={'sim_panel':10});assert state()['auto_max_speed']==40
        call(a,'child_lock.set',{'enabled':False})
        call(a,'fan.set',{'on':False,'speed':70});assert not state()['fan_on']
        call(a,'fan.set',{'speed':0});assert not state()['fan_on']
        call(a,'linkage.set',{'min_speed':0,'max_speed':80,'fallback_speed':90,'mode':1,'debounce_seconds':0,'min_switch_seconds':0})
        call(a,'fan.set',{'on':True});assert state()['percentage']==80
        call(a,'linkage.set',{'fallback_speed':0});assert state()['fan_state']=='idle' and state()['fan_on']
        print('PASS independent mode/power, manual restore, cap, default double click, ring, child lock and bounded fallback',flush=True)
        key='u_'+b'auto_test'.hex();base=f'phicomm/v2/{key}/a1/020000aa0001'
        def fan_config():
            configs=[json.loads(p) for t,p,r in records if t.startswith('homeassistant/fan/') and p and json.loads(p).get('percentage_command_topic')==base+'/command']
            return configs[-1] if configs else None
        config=wait(fan_config)
        assert config['preset_modes']==['auto','manual'] and config['preset_mode_state_topic']==base+'/fan/mode/state'
        # HA also exposes a normal select entity, visible without opening fan details.
        select_config=wait(lambda:next((json.loads(p) for t,p,r in reversed(records)
            if t.startswith('homeassistant/select/') and p and json.loads(p).get('command_topic')==base+'/command'),None))
        assert select_config['options']==['自动','手动']
        assert 'entity_category' not in select_config
        call(a,'fan.set',{'on':False})
        env=Environment(undefined=StrictUndefined)
        for label,mode in [('手动','manual'),('自动','auto')]:
            wire=env.from_string(select_config['command_template']).render(value=label)
            assert json.loads(wire)=={'action':'fan.mode.set','params':{'mode':mode}}
            publish(base+'/command',wire);wait(lambda:state()['fan_mode']==mode)
            assert not state()['fan_on']
            assert env.from_string(select_config['value_template']).render(value_json=state())==label
        for mode in ('manual','auto'):
            wire=env.from_string(config['preset_mode_command_template']).render(value=mode)
            assert json.loads(wire)=={'action':'fan.mode.set','params':{'mode':mode}}
            start=len(records);publish(base+'/command',wire)
            wait(lambda:state()['fan_mode']==mode)
            wait(lambda:any(t==base+'/fan/mode/state' and p==mode for t,p,r in records[start:]))
            assert not state()['fan_on'], 'mode-only HA command must preserve off'
        wire=env.from_string(config['percentage_command_template']).render(value=47)
        assert json.loads(wire)=={'action':'fan.set','params':{'mode':'manual','speed':47}}
        start=len(records);publish(base+'/command',wire);wait(lambda:state()['fan_mode']=='manual' and state()['percentage']==47)
        wait(lambda:any(t==base+'/fan/mode/state' and p=='manual' for t,p,r in records[start:]))
        late=[];reader=mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        reader.on_message=lambda c,u,m:late.append((m.payload.decode(),m.retain))
        reader.connect('127.0.0.1',port);reader.subscribe(base+'/fan/mode/state',1);reader.loop_start()
        try:wait(lambda:('manual',True) in late)
        finally:reader.loop_stop();reader.disconnect()
        print('PASS HA preset discovery, rendered mode/speed commands, state convergence and retained late subscription',flush=True)
        call(m,'binding.set',{'response_ids':['020000aa0001:020000bb0001']})
        call(a,'binding.set',{'peers':[{'mac':'020000bb0001','ip':'127.75.1.2'}]})
        call(a,'fan.mode.set',{'mode':'auto'})
        call(a,'linkage.set',{'mode':1,'metrics':1,'low_pm':10,'high_pm':110,'min_speed':0,'max_speed':80,'fallback_speed':23})
        wait(lambda:call(a,'sensors.average.get').get('sample_count')==1,45)
        wait(lambda:state()['percentage']==32)
        call(m,raw={'sim_sensor':{'values':[22,45,0,.01]}})
        wait(lambda:state()['fan_state']=='idle')
        call(m,raw={'sim_sensor':{'values':[22,45,200,.01]}})
        wait(lambda:state()['percentage']==80)
        call(a,'fan.set',{'on':False});time.sleep(6)
        assert not state()['fan_on'] and state()['percentage']==0
        call(a,'fan.set',{'on':True})
        call(a,'linkage.set',{'mode':0,'fixed_speed':57});assert state()['percentage']==57
        call(a,'linkage.set',{'mode':1,'metrics':2,'low_hcho':0,'high_hcho':.2})
        wait(lambda:state()['percentage']==4)
        call(a,'linkage.set',{'metrics':3});assert state()['percentage']==80
        call(a,'linkage.set',{'mode':2,'target_pm':35,'kp':20,'ki':.1,'kd':0})
        for _ in range(3):
            assert 0<=state()['percentage']<=80;time.sleep(2)
        call(m,raw={'sim_sensor':{'enabled':False}})
        wait(lambda:state()['percentage']==23,35)
        call(m,raw={'sim_sensor':{'enabled':True}})
        wait(lambda:call(a,'binding.status.get').get('data_valid'),35)
        # Optional hysteresis wraps the production speed algorithms. Positive
        # minimum must not prevent idle, and cap edits must not reset the latch.
        call(m,raw={'sim_sensor':{'values':[22,45,40,.09]}})
        wait(lambda:call(a,'sensors.average.get').get('pm25')==40)
        call(a,'linkage.set',{'hysteresis_enabled':1,'mode':0,'metrics':1,'min_speed':20,'max_speed':80,'fixed_speed':57,'on_pm':50,'target_pm':35,'debounce_seconds':2,'min_switch_seconds':10})
        wait(lambda:state()['fan_state']=='idle')
        def sensor(pm,hcho=.09):
            call(m,raw={'sim_sensor':{'values':[22,45,pm,hcho]}})
            wait(lambda:call(a,'sensors.average.get').get('pm25')==pm)
        sensor(50);wait(lambda:state()['percentage']==57)
        sensor(40);time.sleep(6);assert state()['percentage']==57
        call(a,'fan.set',{'speed':45});assert state()['percentage']==45
        sensor(35);wait(lambda:state()['fan_state']=='idle')
        sensor(40);time.sleep(6);assert state()['fan_state']=='idle'
        sensor(50);wait(lambda:state()['percentage']==45)
        call(a,'linkage.set',{'mode':1,'low_pm':10,'high_pm':110})
        assert state()['percentage']==30
        call(a,'linkage.set',{'mode':2});assert 20<=state()['percentage']<=45
        call(a,'fan.set',{'on':False});time.sleep(6);assert not state()['fan_on']
        call(a,'fan.set',{'on':True});wait(lambda:state()['percentage']>=20)
        call(a,'linkage.set',{'metrics':3,'mode':0,'fixed_speed':40,'on_hcho':.1,'target_hcho':.08})
        sensor(35,.09);time.sleep(6);assert state()['percentage']==40
        sensor(35,.08);wait(lambda:state()['fan_state']=='idle')
        call(m,raw={'sim_sensor':{'enabled':False}})
        wait(lambda:state()['percentage']==23,35)
        call(m,raw={'sim_sensor':{'enabled':True,'values':[22,45,40,.09]}})
        wait(lambda:state()['fan_state']=='idle',35)
        call(a,'linkage.set',{'hysteresis_enabled':0})
        assert state()['percentage']==40
        print('PASS production hysteresis start/target/deadband, positive-minimum idle, cap preservation, algorithms, combined stop, off latch and sensor fallback',flush=True)
        call(a,'fan.mode.set',{'mode':'manual'});assert state()['percentage']==47
        print('PASS fresh sensor fixed/linear/PM/HCHO/combined/PID, idle/resume, off latch, stale fallback and recovery',flush=True)
    finally:
        for node in nodes:
            node.terminate()
        for node in nodes:
            try:node.wait(5)
            except subprocess.TimeoutExpired:node.kill();node.wait()
        client.loop_stop();client.disconnect()


if __name__=='__main__':main(int(sys.argv[1]))
