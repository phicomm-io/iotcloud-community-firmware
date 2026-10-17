#!/usr/bin/env python3
"""Explicitly selected physical A1 plus a production M1 simulator on a test broker.
Caller supplies an isolated reachable MQTT listener; settings are restored.
"""
import argparse,json,os,pathlib,select,subprocess,time
from client import Device,save,redact

class BusyDevice(Device):
    def command(self,*args,**kwargs):
        for _ in range(12):
            try:return super().command(*args,**kwargs)
            except AssertionError as e:
                if '503' not in str(e) or 'busy' not in str(e):raise
                time.sleep(.5)
        raise AssertionError('persistent HTTP busy')

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    for key in ('ip','device-id','broker','port','evidence'):ap.add_argument('--'+key,required=True)
    a=ap.parse_args();d=BusyDevice(a.ip);out=pathlib.Path(a.evidence)
    baseline=d.get();assert baseline['device_id']==a.device_id
    cfg=d.get('config.get');binding=d.get('binding.get');link=d.get('linkage.get');night=d.get('night.get')
    assert 'hysteresis_enabled' in link
    results=[];ident='020000bb0043'
    save(out/'before.json',{'status':baseline,'binding':binding,'linkage':link,'night':night})
    p=subprocess.Popen(['build/simulator/M1',ident,a.broker,a.broker,a.port,'auto_test'],env={**os.environ,'SIM_BIND_IP':a.broker},stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True,bufsize=1)
    def sim(action=None,params=None,raw=None):
        q=raw if raw is not None else {'action':action,'params':params or {}}
        p.stdin.write(json.dumps(q)+'\n');p.stdin.flush();assert select.select([p.stdout],[],[],8)[0]
        r=json.loads(p.stdout.readline());assert r['code'] in ('ok','accepted'),r
    def check(label,predicate,seconds=45):
        s=d.wait(predicate,seconds=seconds);results.append({'case':label,'status':s});save(out/'progress.json',results);print('PASS',label,flush=True);return s
    def sensor(pm,hcho=.09):
        sim(raw={'sim_sensor':{'values':[22,45,pm,hcho]}})
        d.wait(lambda s:s.get('pm25')==pm and abs(s.get('formaldehyde',-1)-hcho)<.0001,action='sensors.average.get',seconds=35)
    try:
        d.set('child_lock.set',{'enabled':True})
        sim('binding.set',{'response_ids':[a.device_id+':'+ident]})
        sim(raw={'sim_sensor':{'values':[22,45,40,.09]}})
        d.set('config.set',{'section':'mqtt','values':{'host':a.broker,'port':int(a.port),'user':'auto_test'}})
        check('connect isolated broker',lambda s:s['mqtt_connected'] and s['mqtt_host']==a.broker)
        d.set('binding.set',{'peers':[{'mac':ident,'ip':a.broker,'force_mqtt':True,'recovery_seconds':5}]})
        d.wait(lambda s:s['data_valid'],action='binding.status.get',seconds=60)
        d.set('night.set',{'windows':[]})
        d.set('fan.set',{'mode':'manual','speed':64,'on':False})
        d.set('linkage.set',{'enabled':1,'hysteresis_enabled':1,'mode':0,'metrics':1,'min_speed':20,'max_speed':80,'fixed_speed':57,'fallback_speed':23,'on_pm':50,'target_pm':35,'on_hcho':.1,'target_hcho':.08,'debounce_seconds':2,'min_switch_seconds':10})
        d.set('fan.set',{'on':True})
        check('boot in band idles despite minimum20',lambda s:s['fan_on'] and s['fan_state']=='idle' and s['applied_speed']==0)
        sensor(50);check('start boundary uses fixed57',lambda s:s['percentage']==57 and s['applied_speed']==57)
        sensor(40);time.sleep(6);check('running latch survives deadband',lambda s:s['percentage']==57)
        d.set('fan.set',{'speed':45});check('cap edit retains running latch',lambda s:s['percentage']==45 and s['requested_speed']==64)
        sensor(35);check('target boundary idles with positive minimum',lambda s:s['fan_on'] and s['percentage']==0 and s['applied_speed']==0)
        sensor(40);time.sleep(6);check('idle latch survives deadband',lambda s:s['fan_state']=='idle')
        sensor(49);time.sleep(6);check('below start threshold remains idle',lambda s:s['fan_state']=='idle')
        sensor(50);check('start threshold resumes',lambda s:s['percentage']==45)
        d.set('linkage.set',{'mode':1,'low_pm':10,'high_pm':110});check('linear inner algorithm',lambda s:s['percentage']==30 and s['applied_speed']==30)
        d.set('linkage.set',{'mode':2,'kp':20,'ki':.1,'kd':0});check('PID inner algorithm bounded',lambda s:20<=s['percentage']<=45)
        d.set('linkage.set',{'mode':0,'fixed_speed':40,'metrics':3})
        sensor(35,.09);time.sleep(6);check('combined metrics wait for all targets',lambda s:s['percentage']==40)
        sensor(35,.08);check('combined metrics all clean idle',lambda s:s['fan_state']=='idle')
        sensor(35,.1);check('formaldehyde alone starts',lambda s:s['percentage']==40)
        d.set('fan.set',{'on':False});time.sleep(8);check('manual off prevents pollution restart',lambda s:not s['fan_on'] and s['applied_speed']==0)
        d.set('fan.mode.set',{'mode':'manual'});d.set('fan.set',{'on':True});check('manual speed memory restored',lambda s:s['percentage']==64)
        d.set('fan.mode.set',{'mode':'auto'})
        sensor(35,.08);check('auto clean idle restored',lambda s:s['fan_state']=='idle')
        sim(raw={'sim_sensor':{'enabled':False}})
        check('stale sensor uses bounded fallback23',lambda s:s['percentage']==23 and s['applied_speed']==23,50)
        sim(raw={'sim_sensor':{'enabled':True,'values':[22,45,40,.09]}})
        check('sensor recovery in band returns idle',lambda s:s['fan_state']=='idle',50)
        d.set('fan.set',{'on':False});uptime=d.get()['uptime_ms'];d.set('system.reboot',{});time.sleep(4)
        check('reboot retains auto off and settings',lambda s:s['uptime_ms']<uptime and s['fan_mode']=='auto' and not s['fan_on'] and s['panel_ready'],80)
        persisted=d.get('linkage.get');assert persisted['hysteresis_enabled']==1 and persisted['on_pm']==50 and persisted['target_pm']==35
    finally:
        try:
            d.set('fan.set',{'mode':'manual','speed':baseline['requested_speed'],'on':False})
            d.set('night.set',night);d.set('linkage.set',link);d.set('binding.set',binding)
            d.set('config.set',{'section':'mqtt','values':{'host':cfg['mqtt_host'],'port':cfg['mqtt_port'],'user':cfg['mqtt_user']}})
            d.set('fan.set',{'mode':baseline['fan_mode'],'on':baseline['fan_on']})
            d.set('child_lock.set',{'enabled':baseline['child_lock']})
            final=d.wait(lambda s:s['mqtt_connected'] and s['mqtt_host']==cfg['mqtt_host'],seconds=60)
            assert final['fan_on']==baseline['fan_on'] and final['requested_speed']==baseline['requested_speed']
            for action,expected in [('binding.get',binding),('linkage.get',link),('night.get',night)]:
                actual=redact(d.get(action));expected=redact({k:v for k,v in expected.items() if k!='revision'});actual.pop('revision',None);assert actual==expected,action
            save(out/'result.json',{'cases':results,'restored':True,'final':final})
            print('RESTORED fresh baseline configuration',flush=True)
        finally:p.terminate();p.wait(10)

if __name__=='__main__':main()
