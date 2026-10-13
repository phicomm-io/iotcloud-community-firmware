#!/usr/bin/env python3
"""Isolated real platform/Mosquitto test of actual firmware workers + SDK.
Requires Docker, Go and paho-mqtt==2.1.0; touches no production service.
"""
import argparse, uuid, sys, contextlib, http.cookiejar, urllib.error, json, os, pathlib, socket, signal, subprocess, tempfile, time, urllib.request
import paho.mqtt.client as mqtt
ROOT=pathlib.Path(__file__).resolve().parents[1]
def run(*args, **kw): return subprocess.check_output(args, text=True, **kw).strip()
def wait(predicate, seconds=15):
    deadline=time.monotonic()+seconds
    while time.monotonic()<deadline:
        if predicate(): return
        time.sleep(.05)
    raise AssertionError('condition timed out')
def port():
    with socket.socket() as s: s.bind(('127.0.0.1',0)); return s.getsockname()[1]
def api(opener, base, path, data=None, method=None):
    req=urllib.request.Request(base+path,data=None if data is None else json.dumps(data).encode(),method=method,headers={'Content-Type':'application/json','Origin':base})
    with opener.open(req,timeout=5) as r: return json.load(r)
def client(user,password,broker_port,received):
    c=mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,client_id='observer_'+uuid.uuid4().hex)
    c.username_pw_set(user,password)
    connected=[]
    c.on_connect=lambda c,u,f,rc,p: connected.append(rc)
    c.on_message=lambda c,u,m: received.append((m.topic,m.payload.decode(),m.retain))
    c.on_disconnect=lambda c,u,f,rc,p: print('observer disconnected:',user,rc,flush=True)
    c.connect('127.0.0.1',broker_port);c.loop_start();wait(lambda:connected)
    assert connected[0]==0
    return c

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--platform',default=str(ROOT.parent/'phicomm-community-mqtt'));args=parser.parse_args()
    platform=pathlib.Path(args.platform).resolve();container='firmware-account-test-'+str(os.getpid())
    processes=[];clients=[]
    with tempfile.TemporaryDirectory(prefix='phicomm-firmware-test-') as tmp:
        tmp=pathlib.Path(tmp);backend=tmp/'backend';runtime=tmp/'runtime'
        subprocess.run(['go','build','-o',str(backend),'.'],cwd=platform/'backend',check=True)
        subprocess.run([str(backend),'init','--output',str(runtime)],check=True,stdout=subprocess.DEVNULL)
        backend_process=None
        try:
            run('docker','run','-d','--name',container,'--user',f'{os.getuid()}:{os.getgid()}', '-p','127.0.0.1::1883','-p','127.0.0.1::8883', '-v',f'{platform}/mosquitto/config/mosquitto.conf:/mosquitto/config/mosquitto.conf:ro','-v',f'{runtime}/broker:/mosquitto/security','-v',f'{runtime}/broker-data:/mosquitto/data','eclipse-mosquitto:2.0.22')
            broker_port=int(run('docker','port',container,'1883/tcp').rsplit(':',1)[1]);tls_port=int(run('docker','port',container,'8883/tcp').rsplit(':',1)[1]);http_port=port();base=f'http://127.0.0.1:{http_port}'
            config=tmp/'config.yaml';config.write_text(f'''db_path: {runtime}/app/app.db
broker_url: ssl://localhost:{tls_port}
broker_ca: {runtime}/broker/certs/ca.crt
broker_admin_username: __mqtt_manager
broker_admin_password_file: {runtime}/secrets/broker-password
admin_username: admin
admin_password_file: {runtime}/secrets/admin-password
public_url: {base}
listen_addr: 127.0.0.1:{http_port}
register_level: 1
cookie_secure: false
''')
            log=open(tmp/'backend.log','w');backend_process=subprocess.Popen([str(backend),str(config)],stdout=log,stderr=log)
            opener=urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
            def healthy():
                try:api(opener,base,'/api/register/status');return True
                except Exception:return False
            wait(healthy)
            password='p'*256
            users=['firmware_alice','firmware_bob']
            def register(user):
                deadline=time.monotonic()+15
                while True:
                    try:return api(opener,base,'/api/register',{'username':user,'password':password})
                    except urllib.error.HTTPError as e:
                        if e.code!=503 or time.monotonic()>deadline:raise
                        time.sleep(1)
            for user in users: register(user)
            received=[];alice=client(users[0],password,broker_port,received);clients.append(alice)
            key='u_'+users[0].encode().hex();bobkey='u_'+users[1].encode().hex();mac='aabbccddeeff'
            # Before discovery, only bootstrap permissions exist.
            grants=[]
            alice.on_subscribe=lambda c,u,mid,rc,p:grants.append(rc)
            alice.subscribe(f'phicomm/m1/{key}/{mac}/#',qos=1);wait(lambda:grants);assert int(grants[-1][0].value)==128
            print('PASS initial business SUBACK denied')
            alice.subscribe(f'phicomm/discovery/{key}/+',1);alice.subscribe(f'homeassistant/+/{key}/+/config',1)
            for model in ('M1','A1','TC1'):
                # All three use the same MAC with different accounts to avoid the
                # platform's one-model-per-account-device invariant.
                user='firmware_'+model.lower();register(user)
                records=[];observer=client(user,password,broker_port,records);clients.append(observer)
                modelkey='u_'+user.encode().hex();devicebase=f'phicomm/{model.lower()}/{modelkey}/{mac}'
                observer.subscribe(f'phicomm/discovery/{modelkey}/+',1);observer.subscribe(f'homeassistant/+/{modelkey}/+/config',1)
                output=open(tmp/f'{model}.log','w');env=dict(os.environ,HOST_MQTT_PASSWORD=password)
                if model=='M1': os.kill(backend_process.pid, signal.SIGSTOP)
                worker=subprocess.Popen([str(ROOT/'build/account-host'/model),'127.0.0.1',str(broker_port),user,'60'],env=env,stdout=output,stderr=output);processes.append(worker)
                wait(lambda:any(t.startswith('phicomm/discovery/') for t,b,r in records),10)
                if model=='M1':
                    time.sleep(3)
                    assert 'STATUS waiting_acl' in (tmp/f'{model}.log').read_text()
                    assert sum(t.startswith('phicomm/discovery/') for t,b,r in records)>=2
                    assert not any(t==devicebase+'/availability' and b=='online' for t,b,r in records)
                    os.kill(backend_process.pid, signal.SIGCONT)
                    print('PASS delayed platform authorization: repeated announcement, waiting_acl, no premature online')
                # Dynamic-security role changes disconnect existing clients.
                # Recreate the independent observer after discovery reconciliation.
                wait(lambda:'STATUS ready' in (tmp/f'{model}.log').read_text(),40)
                observer.disconnect();observer.loop_stop();time.sleep(.3)
                observer=client(user,password,broker_port,records);clients.append(observer)
                observer.subscribe(f'homeassistant/+/{modelkey}/+/config',1)
                grants=[];observer.on_subscribe=lambda c,u,mid,rc,p:grants.append(rc)
                for retry in range(20):
                    grants.clear();observer.subscribe(devicebase+'/#',1);wait(lambda:grants)
                    if grants[-1][0].value!=128:break
                    time.sleep(.1)
                assert grants[-1][0].value!=128
                wait(lambda:any(t==devicebase+'/availability' and b=='online' for t,b,r in records),10)
                ha=list({t:json.loads(b) for t,b,r in records if t.startswith('homeassistant/') and b}.items())
                assert len(ha)=={'M1':5,'A1':4,'TC1':16}[model],(model,len(ha))
                assert all(len(t.split('/'))==5 and modelkey in o.get('unique_id',o.get('uniq_id','')) for t,o in ha)
                assert all(not o.get('retain',True) for t,o in ha)
                command={'M1':('brightness/set','3'),'A1':('fan/set','ON'),'TC1':('set','set socket AABBCCDDEEFF 0 1')}[model]
                worker.kill();worker.wait();time.sleep(.1)
                start_records=len(records)
                observer.publish(devicebase+'/'+command[0],command[1],1,True).wait_for_publish()
                worker=subprocess.Popen([str(ROOT/'build/account-host'/model),'127.0.0.1',str(broker_port),user,'60'],env=env,stdout=output,stderr=output);processes.append(worker)
                wait(lambda:any(t==devicebase+'/availability' and b=='online' for t,b,r in records[start_records:]),15)
                time.sleep(.2);output.flush();assert 'CONTROL' not in (tmp/f'{model}.log').read_text()
                observer.publish(devicebase+'/'+command[0],command[1],1,False).wait_for_publish()
                wait(lambda:'CONTROL' in (tmp/f'{model}.log').read_text())
                if model=='M1':
                    wait(lambda:any(t==devicebase+'/sensor' and b for t,b,r in records),8)
                    sample=[json.loads(b) for t,b,r in records if t==devicebase+'/sensor' and b][-1]
                    assert sample=={'temperature':0.0,'humidity':0.0,'formaldehyde':0.0,'pm25':0},sample
                    freshness_mark=len(records)
                    wait(lambda:any(t==devicebase+'/sensor_availability' and b=='offline' for t,b,r in records[freshness_mark:]),20)
                    wait(lambda:any(t==devicebase+'/sensor' and not b for t,b,r in records[freshness_mark:]),3)
                    print('PASS M1 zero-valued fresh sample and 15-second expiration/retained-state removal')
                observer.publish('homeassistant/status','online',1,False) # must be denied: HA status is read-only
                # Cross-account business subscriptions must be rejected.
                cross=[];observer.on_subscribe=lambda c,u,mid,rc,p:cross.append(rc)
                observer.subscribe(f'phicomm/{model.lower()}/{bobkey}/{mac}/#',1);wait(lambda:cross);assert cross[-1][0].value==128
                # LWT is observed on real socket death, separate from normal exits.
                worker.kill();worker.wait();wait(lambda:any(t==devicebase+'/availability' and b=='offline' for t,b,r in records))
                output.close()
                assert all(modelkey in t or t=='homeassistant/status' for t,b,r in records)
                print(f'PASS {model}: real discovery/ACL, 256-byte password, scoped HA, retained rejection, command dispatch, LWT and cross-account denial')
            # Switch credentials while connected: tombstones must use the old session.
            for model in ('M1','A1','TC1'):
                olduser='firmware_'+model.lower();newuser='firmware_switch_'+model.lower();register(newuser)
                oldkey='u_'+olduser.encode().hex();newkey='u_'+newuser.encode().hex()
                oldrecords=[];newrecords=[]
                oldobserver=client(olduser,password,broker_port,oldrecords);clients.append(oldobserver)
                oldobserver.subscribe(f'homeassistant/+/{oldkey}/+/config',1)
                oldobserver.subscribe(f'phicomm/{model.lower()}/{oldkey}/{mac}/#',1)
                newobserver=client(newuser,password,broker_port,newrecords);clients.append(newobserver)
                newobserver.subscribe(f'phicomm/discovery/{newkey}/+',1)
                output=open(tmp/f'switch-{model}.log','w')
                worker=subprocess.Popen([str(ROOT/'build/account-host'/model),'127.0.0.1',str(broker_port),olduser,'45'],env=dict(os.environ,HOST_MQTT_PASSWORD=password,HOST_SWITCH_USER=newuser),stdout=output,stderr=output);processes.append(worker)
                wait(lambda:any(t.startswith('phicomm/discovery/') for t,b,r in newrecords),25)
                wait(lambda:len({t for t,b,r in oldrecords if t.startswith('homeassistant/') and not b})=={'M1':5,'A1':4,'TC1':16}[model],5)
                assert any(t==f'phicomm/{model.lower()}/{oldkey}/{mac}/availability' and b=='offline' for t,b,r in oldrecords)
                wait(lambda:(tmp/f'switch-{model}.log').read_text().count('STATUS ready')>=2,25)
                newobserver.disconnect();newobserver.loop_stop()
                newobserver=client(newuser,password,broker_port,newrecords);clients.append(newobserver)
                newobserver.subscribe(f'homeassistant/+/{newkey}/+/config',1)
                wait(lambda:sum(t.startswith('homeassistant/') and bool(b) for t,b,r in newrecords)=={'M1':5,'A1':4,'TC1':16}[model])
                worker.kill();worker.wait();output.close()
                print(f'PASS {model} credential switch: old HA tombstones/offline, new account discovery and isolated HA IDs')
            # Firmware must keep retrying denied subscriptions without legacy fallback.
            records=[];observer=client(users[0],password,broker_port,records);clients.append(observer)
            observer.subscribe(f'phicomm/discovery/{key}/+',1)
            output=open(tmp/'retry.log','w');worker=subprocess.Popen([str(ROOT/'build/account-host/M1'),'127.0.0.1',str(broker_port),users[0],'90'],env=dict(os.environ,HOST_MQTT_PASSWORD=password),stdout=output,stderr=output);processes.append(worker)
            # Platform discovery automatically grants; revoke by explicit unbind.
            api(opener,base,'/api/auth/login',{'username':users[0],'password':password})
            wait(lambda:any(t.startswith('phicomm/discovery/') for t,b,r in records));time.sleep(.5)
            devices=api(opener,base,'/api/devices');assert devices,devices
            print('PASS discovery creates device in the real platform API')
            wait(lambda:'STATUS ready' in (tmp/'retry.log').read_text())
            api(opener,base,'/api/devices/scoped/'+mac,method='DELETE')
            wait(lambda:'STATUS waiting_acl' in (tmp/'retry.log').read_text(),20)
            observer.disconnect();observer.loop_stop()
            records=[];observer=client(users[0],password,broker_port,records);clients.append(observer)
            observer.subscribe(f'homeassistant/+/{key}/+/config',1)
            wait(lambda:sum(t.startswith('homeassistant/') and bool(b) for t,b,r in records)==5)
            assert not any(t.startswith('homeassistant/') and not b for t,b,r in records)
            api(opener,base,'/api/devices/scoped/'+mac,{},method='PUT')
            time.sleep(.3);observer.disconnect();observer.loop_stop()
            records=[];observer=client(users[0],password,broker_port,records);clients.append(observer)
            observer.subscribe(f'phicomm/m1/{key}/{mac}/#',1)
            wait(lambda:any(t==f'phicomm/m1/{key}/{mac}/availability' and b=='online' for t,b,r in records),35)
            print('PASS real unbind/rebind: waiting_acl preserves HA config and restored authorization returns online')
            worker.kill();worker.wait();output.close()
            print('All isolated firmware/platform protocol scenarios passed; physical peripherals were stubbed.')
        finally:
            if sys.exc_info()[0]:
                for path in tmp.glob('*.log'): print(path.name+': '+path.read_text()[-4000:],flush=True)
            for p in processes:
                if p.poll() is None:p.kill();p.wait()
            for c in clients:c.disconnect();c.loop_stop()
            if backend_process:os.kill(backend_process.pid,signal.SIGCONT);backend_process.terminate();backend_process.wait(timeout=5)
            subprocess.run(['docker','rm','-f',container],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
if __name__=='__main__':main()
