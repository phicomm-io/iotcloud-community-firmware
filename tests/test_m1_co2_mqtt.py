#!/usr/bin/env python3
"""Run production M1 MQTT worker against an isolated broker, with synthetic CO2."""
import json, subprocess, sys, time
from pathlib import Path
import paho.mqtt.client as mqtt
from jinja2 import Environment, StrictUndefined


def main(port):
    records = []
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    c.on_message = lambda c, u, m: records.append((m.topic, m.payload.decode(), m.retain))
    c.connect('127.0.0.1', port); c.subscribe('#', 1); c.loop_start()
    base = 'phicomm/v2/u_636f325f74657374/m1/aabbccddeeff'
    worker = subprocess.Popen([str(Path('build/simulator/M1').resolve()), 'aabbccddeeff', '127.0.0.1', '127.0.0.1', str(port), 'co2_test'], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    def wait(pred, timeout=15):
        deadline = time.monotonic()+timeout
        while time.monotonic()<deadline:
            if pred(): return
            assert worker.poll() is None
            time.sleep(.05)
        raise AssertionError('CO2 MQTT timeout')
    def configs():
        return [(t,json.loads(p)) for t,p,r in records if t.endswith('_co2/config') and '/u_636f325f74657374/' in t and p]
    def inject(enabled):
        worker.stdin.write(json.dumps({'sim_co2':{'enabled':enabled,'ppm':723}}).encode()+b'\n');worker.stdin.flush()
        assert json.loads(worker.stdout.readline())['code']=='ok'
    try:
        wait(lambda:any(t==base+'/sensor' for t,p,r in records))
        assert not configs(), 'No phantom CO2 discovery without sensor'
        inject(True)
        wait(lambda:bool(configs()) and any(t==base+'/co2' for t,p,r in records))
        topic,cfg=configs()[-1]
        assert cfg['device_class']=='carbon_dioxide' and cfg['unit_of_measurement']=='ppm'
        assert cfg['state_class']=='measurement' and cfg['expire_after']==20
        assert cfg['state_topic']==base+'/co2'
        assert {x['topic'] for x in cfg['availability']}=={base+'/availability',base+'/co2_availability'}
        assert Environment(undefined=StrictUndefined).from_string(cfg['value_template']).render(value_json={'co2':723})=='723'
        wait(lambda:any(t==base+'/co2_availability' and p=='online' for t,p,r in records))
        start=len(records);c.publish('homeassistant/status','online',qos=1).wait_for_publish()
        wait(lambda:any(t==topic and p for t,p,r in records[start:]))
        # Fresh samples are not retained and do not reappear as fresh on HA reconnect.
        replay=[];other=mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        other.on_message=lambda c,u,m:replay.append((m.topic,m.payload.decode(),m.retain))
        other.connect('127.0.0.1',port);other.subscribe(base+'/co2',1);other.loop_start()
        time.sleep(1.2);other.loop_stop();other.disconnect()
        assert not any(r for t,p,r in replay)
        inject(False);start=len(records)
        wait(lambda:any(t==base+'/co2_availability' and p=='offline' for t,p,r in records[start:]),25)
        assert any(t==base+'/sensor' and p for t,p,r in records[start:]), 'Original sensors continue'
        inject(True);start=len(records)
        wait(lambda:any(t==base+'/co2_availability' and p=='online' for t,p,r in records[start:]))
        print('PASS M1 CO2 discovery, units/template, independent availability, stale/recovery, HA birth and non-retained samples')
    finally:
        worker.terminate();worker.wait(timeout=5);c.loop_stop();c.disconnect()
if __name__=='__main__':main(int(sys.argv[1]))
