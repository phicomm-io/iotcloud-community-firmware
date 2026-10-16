#!/usr/bin/env python3
"""Production A1 service/MQTT workers, simulated hardware, isolated local broker.

Run via run_mqtt_v2.sh; never uses the device's configured broker.
"""
import json
from pathlib import Path
import subprocess
import sys
import time

import paho.mqtt.client as mqtt
from jinja2 import Environment, StrictUndefined


def main(port):
    root = Path(__file__).resolve().parents[1]
    user = 'filter_test'
    key = 'u_' + user.encode().hex()
    base = f'phicomm/v2/{key}/a1/aabbccddeeff'
    records = []
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_message = lambda c, u, m: records.append((m.topic, m.payload.decode(), m.retain))
    client.connect('127.0.0.1', port)
    client.subscribe('#', 1)
    client.loop_start()
    worker = None

    def wait(predicate, seconds=12):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if predicate():
                return
            if worker and worker.poll() is not None:
                raise AssertionError('simulator exited')
            time.sleep(.03)
        raise AssertionError('MQTT filter timeout')

    def publish(topic, payload):
        client.publish(topic, payload, qos=1, retain=False).wait_for_publish()

    def states(start):
        return [json.loads(p)['filter'] for t, p, r in records[start:]
                if t == base + '/state']

    try:
        worker = subprocess.Popen([str(root / 'build/simulator/A1'), 'aabbccddeeff',
                                   '127.0.0.1', '127.0.0.1', str(port), user],
                                  stdin=subprocess.PIPE, stdout=subprocess.DEVNULL)
        prefix = 'homeassistant/'
        def configs():
            return {t.rsplit('_filter_', 1)[-1].split('/')[0]: json.loads(p)
                    for t, p, r in records if t.startswith(prefix) and '/'+key+'/' in t
                    and '_filter_' in t and t.endswith('/config') and p}
        wait(lambda: len(configs()) == 5 and bool(states(0)))
        by = configs()
        assert set(by) == {'remaining', 'used', 'replacement', 'lifetime', 'reset'}
        env = Environment(undefined=StrictUndefined)
        sample = {'filter': {'remaining_percent': 7, 'used_seconds': 108000,
                             'replacement_due': True, 'lifetime_hours': 4320}}
        for name, value in [('remaining', '7'), ('used', '30.0'), ('replacement', 'ON'),
                            ('lifetime', '4320')]:
            obj = by[name]
            assert obj['state_topic'] == base+'/state'
            assert env.from_string(obj['value_template']).render(value_json=sample) == value
            assert obj['availability_topic'] == base+'/availability'
            assert obj['device']['identifiers'] == [key+'_a1_aabbccddeeff']
        sample['filter']['replacement_due'] = False
        assert env.from_string(by['replacement']['value_template']).render(value_json=sample) == 'OFF'
        assert by['lifetime']['min'] == 1 and by['lifetime']['max'] == 20000
        assert by['lifetime']['step'] == 1
        assert by['lifetime']['command_topic'] == by['reset']['command_topic'] == base+'/command'
        assert by['reset']['retain'] is False

        start = len(records)
        command = env.from_string(by['lifetime']['command_template']).render(value=6000)
        assert json.loads(command) == {'action': 'filter.set', 'params': {'lifetime_hours': 6000}}
        publish(base+'/command', command)
        wait(lambda: any(s['lifetime_hours'] == 6000 for s in states(start)), 5)
        publish(base+'/command', json.dumps({'action': 'fan.set', 'params': {'on': True, 'speed': 20}}))
        start = len(records)
        # No status polling or configuration edits: only the worker's minute timer can publish usage.
        wait(lambda: any(s['used_seconds'] >= 50 for s in states(start)), 75)
        print('PASS A1 autonomous minute MQTT usage report and immediate lifetime update', flush=True)

        start = len(records)
        reset = by['reset']['payload_press']
        assert json.loads(reset) == {'action': 'filter.reset', 'params': {}}
        publish(base+'/command', reset)
        wait(lambda: any(s['used_seconds'] == 0 and s['lifetime_hours'] == 6000
                         for s in states(start)), 5)
        retained = []
        late = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        late.on_message = lambda c, u, m: retained.append((m.topic, m.payload.decode(), m.retain))
        try:
            late.connect('127.0.0.1', port)
            late.subscribe(base+'/state', 1)
            late.loop_start()
            wait(lambda: bool(retained))
            assert retained[0][2] is True
            assert json.loads(retained[0][1])['filter']['used_seconds'] == 0
        finally:
            late.disconnect()
            late.loop_stop()
        start = len(records)
        publish('homeassistant/status', 'online')
        wait(lambda: len([t for t, p, r in records[start:] if '_filter_' in t
                          and t.startswith(prefix) and '/'+key+'/' in t and p]) == 5)
        print('PASS A1 five HA discovery templates, control commands, reset, retained replay and HA birth', flush=True)
    finally:
        if worker:
            worker.terminate()
            worker.wait(timeout=5)
        client.disconnect()
        client.loop_stop()


if __name__ == '__main__':
    main(int(sys.argv[1]))
