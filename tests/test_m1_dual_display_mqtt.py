#!/usr/bin/env python3
"""Actual M1 session filters and runtime ingress against an isolated broker."""
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
    records = []
    observer = Observer(port, lambda c, u, m: records.append((m.topic, m.payload))).start('#')
    worker = None
    account = 'u_' + b'dual_display'.hex()
    prefix = f'phicomm/v2/{account}'
    co2_id, fan_id = '123456abcdef', 'abcdef123456'
    try:
        worker = subprocess.Popen([os.environ.get('PHICOMM_M1_SIMULATOR', str(Path('build/simulator/M1').resolve())),
                                   'aabbccddeeff', '127.0.0.1', '127.0.0.1', str(port), 'dual_display'],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE)

        def command(action, params=None):
            worker.stdin.write((json.dumps({'request_id': str(time.monotonic_ns()),
                                           'action': action, 'params': params or {}}) + '\n').encode())
            worker.stdin.flush()
            assert select.select([worker.stdout], [], [], 5)[0], 'M1 command timeout'
            reply = json.loads(worker.stdout.readline())
            assert reply['code'] in ('ok', 'accepted'), reply
            return reply.get('data', {})

        def lane(slot):
            return command('display_profile.get')['runtime']['lanes'][slot]

        def publish(topic, body, retained=False):
            observer.publish(topic, json.dumps(body) if isinstance(body, dict) else body, retain=retained)

        observer.wait(lambda: any(t.endswith('/availability') and p == b'online' for t, p in records), label='M1 online')
        lanes = [
            {'slot': 0, 'source': 'mqtt', 'device_id': co2_id, 'field_id': 'custom_sensor', 'type': 'number', 'unit': 'mSv/h',
             'display': {'mode': 'auto_kilo', 'show_label': False, 'show_unit': False}, 'ttl_seconds': 60,
             'color': {'mode': 'thresholds', 'low': 1000, 'high': 1500}},
            {'slot': 1, 'source': 'mqtt', 'device_id': fan_id, 'field_id': 'fan_speed_percent', 'type': 'number', 'unit': '%',
             'display': {'mode': 'integer', 'show_label': False, 'show_unit': False}, 'ttl_seconds': 60,
             'color': {'mode': 'fixed', 'lamps': [{'red': False, 'green': True, 'yellow': False} for _ in range(3)]}},
        ]
        reconnect_at = len(records)
        saved = command('display_profile.set', {'revision': 0, 'selected_source': 'user',
                                              'double_click': 'switch_display', 'lanes': lanes})
        assert saved['schema'] == 4 and saved['runtime']['profile'] == 'formatted'
        assert saved['runtime']['confirmation'] == 'requires_mcu_upgrade', 'simulator has no dual UART MCU'

        # Adding the first MQTT lane changes the subscription plan. A fresh
        # measurement must arrive after the new session acknowledges offline
        # delivery; the previous four-filter session cannot admit it safely.
        availability = f'{prefix}/m1/aabbccddeeff/availability'
        observer.wait(lambda: any(t == availability and p == b'offline'
                                 for t, p in records[reconnect_at:]), label='display source reconnect offline')
        observer.wait(lambda: any(t == availability and p == b'online'
                                 for t, p in records[reconnect_at:]), label='display source reconnect online')

        co2 = {'schema': 2, 'device_id': co2_id, 'model': 'unlisted_future_model', 'boot': 31, 'sequence': 1,
               'sensors': {'custom_sensor': {'name': 'Custom sensor', 'type': 'number', 'valid': True, 'value': 1234, 'unit': 'mSv/h', 'age_ms': 0}}}
        fan = {'schema': 2, 'device_id': fan_id, 'model': 'a1', 'boot': 4294967295, 'sequence': 4294967295,
               'sensors': {'fan_speed_percent': {'name': 'Fan percentage', 'type': 'number', 'valid': True, 'value': 60, 'unit': '%', 'age_ms': 0}}}
        co2_topic, fan_topic = f'{prefix}/sensor/{co2_id}/state', f'{prefix}/sensor/{fan_id}/state'
        publish(co2_topic, co2)
        publish(fan_topic, fan)
        observer.wait(lambda: lane(0)['value'] == 1234 and lane(1)['value'] == 60, label='both typed sources')
        assert lane(0)['number'] == 123 and lane(0)['format'] == 1
        assert not lane(0)['effective'], 'a fresh source does not imply UART confirmation'

        # Foreign accounts, unknown IDs and malformed envelopes cannot replace a selected source.
        for topic, body in [(fan_topic.replace(account, 'u_666f726569676e'), {**fan, 'sequence': 1}),
                            (fan_topic.replace(fan_id, '000000000001'), {**fan, 'device_id': '000000000001', 'sequence': 1}),
                            (fan_topic, {**fan, 'schema': True, 'sequence': 1}),
                            (fan_topic, {**fan, 'boot': 0, 'sequence': 1}),
                            (fan_topic, {**fan, 'boot': 4294967296, 'sequence': 1}),
                            (fan_topic, {**fan, 'schema': 1, 'sequence': 1}),
                            (fan_topic, {**fan, 'sequence': 4294967296}),
                            (fan_topic, {**fan, 'sequence': 9223372036854775808}),
                            (fan_topic, {**fan, 'sequence': 1.5})]:
            publish(topic, body)
        time.sleep(.15)
        assert lane(1)['value'] == 60

        # The fifth real session filter delivers offline without waiting for TTL.
        publish(f'{prefix}/future_model/000000000001/availability', b'offline')
        publish(f'{prefix.replace(account, "u_666f726569676e")}/a1/{fan_id}/availability', b'offline')
        time.sleep(.1)
        assert lane(1)['valid']
        publish(f'{prefix}/future_model/{fan_id}/availability', b'offline')
        observer.wait(lambda: lane(1)['reason'] == 'offline' and not lane(1)['valid'], label='selected generic device offline')
        assert lane(0)['valid'], 'offline fallback is independent per lane'
        publish(f'{prefix}/a1/{fan_id}/availability', b'online')
        publish(fan_topic, fan)  # Exact duplicate cannot revive a lease.
        time.sleep(.1)
        assert not lane(1)['valid']
        fan['sequence'] = 1  # uint32 wrap skips zero.
        publish(fan_topic, fan)
        observer.wait(lambda: lane(1)['value'] == 60, label='fresh sample after online')

        fan['sequence'] = 2
        fan['sensors']['fan_speed_percent']['type'] = 'future_type'
        fan['sensors']['fan_speed_percent']['value'] = {'future': 'payload'}
        publish(fan_topic, fan)
        observer.wait(lambda: not lane(1)['valid'], label='unknown field type revokes previous numeric snapshot')
        assert lane(0)['valid'], 'unknown field type is independent per lane'
        fan['sequence'] = 3
        fan['sensors']['fan_speed_percent']['type'] = 'number'
        fan['sensors']['fan_speed_percent']['value'] = 101
        publish(fan_topic, fan)
        observer.wait(lambda: lane(1)['value'] == 101, label='no per-model numeric range whitelist')
        fan['sequence'] = 4
        fan['sensors']['fan_speed_percent']['value'] = 75
        fan['sensors']['fan_speed_percent']['unit'] = 'W'
        publish(fan_topic, fan)
        observer.wait(lambda: not lane(1)['valid'], label='bound unit mismatch')
        fan['sequence'] = 5
        fan['sensors']['fan_speed_percent']['unit'] = '%'
        publish(fan_topic, fan)
        observer.wait(lambda: lane(1)['value'] == 75, label='generic numeric recovery')

        co2['sequence'] = 2
        co2['sensors']['custom_sensor']['age_ms'] = 20000
        publish(co2_topic, co2)
        observer.wait(lambda: not lane(0)['valid'], label='physical freshness cap despite 60-second setting')
        co2['sequence'] = 3
        co2['sensors']['custom_sensor']['age_ms'] = 19000
        publish(co2_topic, co2)
        observer.wait(lambda: lane(0)['valid'], label='near-expiry CO2 accepted')
        observer.wait(lambda: not lane(0)['valid'], seconds=3, label='CO2 lease expires independently')
        assert lane(1)['valid']
        co2['sequence'] = 4
        co2['sensors']['custom_sensor']['age_ms'] = 0
        publish(co2_topic, co2)
        observer.wait(lambda: lane(0)['valid'], label='CO2 fresh recovery')

        # A broker replay after reconnect cannot restore either lease, even
        # though a retained live publish is delivered as an ordinary sample.
        co2['sequence'] = fan['sequence'] = 6
        publish(co2_topic, co2, retained=True)
        publish(fan_topic, fan, retained=True)
        observer.wait(lambda: lane(0)['valid'] and lane(1)['valid'], label='live retained samples')
        records.clear()
        takeover = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                               client_id=account + '_m1_aabbccddeeff', reconnect_on_failure=False)
        try:
            takeover.connect('127.0.0.1', port)
            assert takeover.loop(timeout=2) == mqtt.MQTT_ERR_SUCCESS
        finally:
            takeover.disconnect()
        observer.wait(lambda: any(t.endswith('/availability') and p == b'offline' for t, p in records), label='forced reconnect offline')
        observer.wait(lambda: any(t.endswith('/availability') and p == b'online' for t, p in records), seconds=20, label='forced reconnect online')
        time.sleep(.2)
        assert not lane(0)['valid'] and not lane(1)['valid'], 'retained replay cannot restore leases'
        co2['sequence'] = fan['sequence'] = 7
        publish(co2_topic, co2)
        observer.wait(lambda: lane(0)['valid'], label='CO2 live after reconnect')
        assert not lane(1)['valid'], 'one source cannot refresh the other lane'
        publish(fan_topic, fan)
        observer.wait(lambda: lane(1)['valid'], label='fan live after reconnect')
        publish(co2_topic, b'', retained=True)
        publish(fan_topic, b'', retained=True)
        print('PASS M1 dual display: actual five session filters, schema4 arbitrary-field/type/unit/replay/TTL/offline guards and old-MCU capability')
    finally:
        cleanup(observer, [worker], failing=sys.exc_info()[0] is not None)


if __name__ == '__main__':
    main(int(sys.argv[1]))
