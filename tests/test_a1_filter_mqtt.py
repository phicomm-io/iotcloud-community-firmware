#!/usr/bin/env python3
"""Production A1 service/MQTT workers, simulated hardware, isolated local broker.

Run via run_mqtt_v2.sh; never uses the device's configured broker.
"""
import json
import select
from pathlib import Path
import subprocess
import sys
import time

from mqtt_observer import Observer, cleanup
from jinja2 import Environment, StrictUndefined


def main(port):
    root = Path(__file__).resolve().parents[1]
    user = 'filter_test'
    key = 'u_' + user.encode().hex()
    base = f'phicomm/v2/{key}/a1/aabbccddeeff'
    records = []
    reconnect_birth = {'parent': None, 'sent': False}
    def receive(c, u, message):
        payload = message.payload.decode()
        records.append((message.topic, payload, message.retain))
        if reconnect_birth['parent'] and not reconnect_birth['sent'] and message.topic.endswith('_filter_remaining/config') and payload:
            if json.loads(payload)['device']['identifiers'] == [reconnect_birth['parent']]:
                reconnect_birth['sent'] = True
                info = c.publish('homeassistant/status', 'online', retain=False)
                assert info.rc == 0, 'callback birth publish failed'
    client = Observer(port, receive).start('#')
    worker = None

    def wait(predicate, seconds=12):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            client.check()
            if predicate():
                client.check()
                return
            if worker and worker.poll() is not None:
                raise AssertionError('simulator exited')
            time.sleep(.03)
        raise AssertionError('MQTT filter timeout')

    def publish(topic, payload):
        client.publish(topic, payload)

    def bind(params):
        # Changing subscription roots reconnects MQTT; install via the service
        # input and require its acknowledgement before checking rediscovery.
        worker.stdin.write(json.dumps({'action': 'binding.set', 'params': params}).encode()+b'\n')
        worker.stdin.flush()
        assert select.select([worker.stdout], [], [], 12)[0], 'binding setup timeout'
        assert json.loads(worker.stdout.readline())['code'] == 'accepted'

    def states(start):
        return [json.loads(p)['filter'] for t, p, r in records[start:]
                if t == base + '/state']

    try:
        worker = subprocess.Popen([str(root / 'build/simulator/A1'), 'aabbccddeeff',
                                   '127.0.0.1', '127.0.0.1', str(port), user],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        prefix = 'homeassistant/'
        def configs():
            latest = {t: p for t, p, r in records if t.startswith(prefix)
                      and '/'+key+'/' in t and '_filter_' in t and t.endswith('/config')}
            return {t.rsplit('_filter_', 1)[-1].split('/')[0]: json.loads(p)
                    for t, p in latest.items() if p}
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
        late = Observer(port, lambda c, u, m: retained.append((m.topic, m.payload.decode(), m.retain)))
        try:
            late.start(base+'/state')
            late.wait(lambda: bool(retained))
            client.check()
            assert retained[0][2] is True
            assert json.loads(retained[0][1])['filter']['used_seconds'] == 0
        finally:
            late.close(failing=sys.exc_info()[0] is not None)
        start = len(records)
        publish('homeassistant/status', 'online')
        wait(lambda: {t.rsplit('_filter_', 1)[-1].split('/')[0]
                      for t, p, r in records[start:] if '_filter_' in t
                      and t.startswith(prefix) and '/'+key+'/' in t and p} == set(by))
        print('PASS A1 five HA discovery templates, control commands, reset, retained replay and HA birth', flush=True)

        original_ids = {k: v['unique_id'] for k, v in by.items()}
        group = '客厅净化组'
        parent = f'v2_{key}_group_{("g_" + group.encode().hex()).encode().hex()}'
        group_start = len(records)
        reconnect_birth['parent'] = parent
        bind({'group': group})
        wait(lambda: len(configs()) == 5 and all(
            v['device']['identifiers'] == [parent] for v in configs().values()))
        controls = {}
        def virtual_configs():
            for t, p, r in records:
                if t.startswith(prefix) and '_bound_' in t:
                    if p: controls[t] = json.loads(p)
                    else: controls.pop(t, None)
            return controls
        wait(lambda: len(virtual_configs()) == 4)
        retired_sensors = {'temperature', 'humidity', 'pm25', 'formaldehyde',
                           'co2', 'scd40_temperature', 'scd40_humidity'}
        # Proxy telemetry was removed; discovery must delete its retained
        # entities, including optional SCD40 entities from older firmware.
        wait(lambda: all(any(t.startswith(prefix) and '/'+key+'/' in t
                             and t.endswith('_average_'+name+'/config') and not p
                             for t, p, r in records[group_start:])
                         for name in retired_sensors))
        # Birth injected during initial discovery must survive startup and
        # produce a complete second set of all five physical filter configs.
        wait(lambda: reconnect_birth['sent'] and all(
            sum(t.endswith('_filter_'+k+'/config') and bool(p)
                for t, p, r in records[group_start:]) >= 2 for k in by))
        names = {o['name'] for o in controls.values()}
        assert names == {'空气净化器', '灯环', '童锁', '灯环常亮'}
        for obj in controls.values():
            assert obj['device']['identifiers'] == [parent]
            assert obj['device']['model'] == 'A1 Air Purifier (EMW3080B)'
            if obj['name'] == '空气净化器': assert obj['icon'] == 'mdi:air-purifier'
        for k, obj in configs().items():
            assert obj['unique_id'] == original_ids[k]
            assert obj['name'].startswith('Runtime simulator ')
            assert obj['device']['name'] == group
            assert obj['availability_topic'] == base+'/availability'
            if k != 'reset': assert obj['state_topic'] == base+'/state'
            if k in ('lifetime', 'reset'): assert obj['command_topic'] == base+'/command'
        start = len(records)
        publish('homeassistant/status', 'online')
        wait(lambda: {t.rsplit('_filter_', 1)[-1].split('/')[0]
                      for t, p, r in records[start:] if '_filter_' in t
                      and t.startswith(prefix) and '/'+key+'/' in t and p} == set(by))
        assert len(configs()) == 5  # bound HA birth must not delete filter entities
        # A grouped fan shares controls; reset still targets this physical filter.
        start = len(records)
        publish(base+'/command', command)
        publish(base+'/command', reset)
        wait(lambda: any(s['used_seconds'] == 0 and s['lifetime_hours'] == 6000
                         for s in states(start)), 5)
        bind({'group': '', 'members': [], 'peers': []})
        wait(lambda: len(configs()) == 5 and all(v['device']['identifiers'] ==
            [key+'_a1_aabbccddeeff'] for v in configs().values()))
        assert {k: v['unique_id'] for k, v in configs().items()} == original_ids
        print('PASS bound/unbound filters retain identity, share the purifier parent and keep per-A1 commands', flush=True)
    finally:
        cleanup(client, [worker], failing=sys.exc_info()[0] is not None)


if __name__ == '__main__':
    main(int(sys.argv[1]))
