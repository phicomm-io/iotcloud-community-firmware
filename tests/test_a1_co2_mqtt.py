#!/usr/bin/env python3
"""Exercise optional CO2 linkage using production A1/M1 MQTT workers."""
import json, os, select, subprocess, sys, time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main(port):
    nodes=[]
    def launch(model, ident, ip):
        p=subprocess.Popen([str(ROOT/'build/simulator'/model),ident,ip,'127.0.0.1',str(port),'co2_linkage'],env={**os.environ,'SIM_BIND_IP':ip},stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True,bufsize=1)
        nodes.append(p);return p
    def call(node,action=None,params=None,raw=None):
        q=raw if raw is not None else {'action':action,'params':params or {}}
        node.stdin.write(json.dumps(q)+'\n');node.stdin.flush()
        assert select.select([node.stdout],[],[],8)[0],q
        r=json.loads(node.stdout.readline());assert r.get('code') in ('ok','accepted'),(q,r)
        return r.get('data',{})
    def wait(fn,seconds=45):
        end=time.monotonic()+seconds
        while time.monotonic()<end:
            value=fn()
            if value:return value
            time.sleep(.2)
        raise AssertionError('CO2 linkage convergence timeout')
    try:
        a=launch('A1','020000cc0001','127.77.1.1');m=launch('M1','020000dd0001','127.77.1.2')
        avg=lambda:call(a,'sensors.average.get')
        state=lambda:call(a,'status.get')
        wait(lambda:state().get('mqtt_connected'))
        call(m,'binding.set',{'response_ids':['020000cc0001:020000dd0001']})
        call(a,'binding.set',{'peers':[{'mac':'020000dd0001','ip':'127.77.1.2'}]})
        wait(lambda:avg().get('sample_count')==1)
        assert not avg()['co2_supported'] and not avg()['co2_valid'] and avg()['co2'] is None
        call(a,'linkage.set',{'enabled':1,'metrics':4,'mode':1,'min_speed':0,'max_speed':80,'fallback_speed':23,'low_co2':800,'high_co2':2000,'debounce_seconds':0,'min_switch_seconds':0})
        call(a,'fan.set',{'on':True});call(a,'fan.mode.set',{'mode':'auto'})
        wait(lambda:state()['percentage']==23)
        call(m,raw={'sim_co2':{'enabled':True,'ppm':1400}})
        wait(lambda:avg().get('co2')==1400)
        assert avg()['co2_sample_count']==1 and avg()['co2_valid']
        wait(lambda:state()['percentage']==40)
        call(m,raw={'sim_co2':{'enabled':True,'ppm':2200}})
        wait(lambda:state()['percentage']==80)
        call(a,'linkage.set',{'mode':0,'fixed_speed':55,'hysteresis_enabled':1,'on_co2':1500,'target_co2':1000})
        wait(lambda:state()['percentage']==55)
        call(m,raw={'sim_co2':{'enabled':True,'ppm':900}})
        wait(lambda:state()['percentage']==0)
        call(m,raw={'sim_co2':{'enabled':True,'ppm':2200}})
        wait(lambda:state()['percentage']==55)
        call(m,raw={'sim_sensor':{'enabled':False}})
        wait(lambda:not avg()['valid'],30)
        assert avg()['co2_valid'] and state()['percentage']==55, 'CO2 remains usable without UART samples'
        call(m,raw={'sim_sensor':{'enabled':True}})
        wait(lambda:avg()['valid'])
        call(m,raw={'sim_co2':{'enabled':False}})
        wait(lambda:not avg()['co2_valid'],35)
        assert avg()['co2_supported'] and avg()['co2'] is None and avg()['valid']
        wait(lambda:state()['percentage']==23)
        call(a,'linkage.set',{'metrics':1,'hysteresis_enabled':0,'mode':1,'low_pm':10,'high_pm':110})
        wait(lambda:state()['percentage']==32)
        call(m,raw={'sim_co2':{'enabled':True,'ppm':1400}})
        call(a,'linkage.set',{'metrics':7,'low_hcho':0,'high_hcho':.2})
        wait(lambda:avg().get('co2')==1400)
        wait(lambda:state()['percentage']==40)
        print('PASS optional CO2 wire data, legacy M1, linear/hysteresis, stale fallback, recovery and combined metrics',flush=True)
    finally:
        for p in nodes:p.terminate()
        for p in nodes:p.wait(timeout=5)
if __name__=='__main__':main(int(sys.argv[1]))
