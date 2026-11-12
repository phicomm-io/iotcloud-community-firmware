#!/usr/bin/env python3
"""Real broker, pinned SDK and complete product MQTT workers; fake peripherals.

Native traffic uses independently decoded MP. Only HA topics contain JSON.
This is host protocol evidence, not physical device/closed network heap QA.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import uuid

import msgpack
from jinja2 import Environment, StrictUndefined
from host_models import selected_models
from mqtt_observer import Observer

ROOT = Path(__file__).resolve().parents[1]
port = int(sys.argv[1])
records = []
observer = Observer(port, lambda c, u, m: records.append((m.topic, bytes(m.payload), m.retain)))


def unpack(payload):
    return msgpack.unpackb(payload, raw=False, strict_map_key=False)


def publish(topic, value, retain=False, *, ha=False):
    if isinstance(value, dict):
        value = json.dumps(value) if ha else msgpack.packb(value, use_bin_type=True)
    observer.publish(topic, value, retain)


def barrier(base):
    marker = base + '/_test_barrier/' + uuid.uuid4().hex
    publish(marker, b'barrier')
    observer.wait(lambda: any(t == marker for t, p, r in records), label='publication barrier')


def finish_worker(worker, failing, start):
    exit_before = worker.poll()
    try:
        worker.terminate()
        try:
            out = worker.communicate(timeout=5)[0]
        except subprocess.TimeoutExpired:
            worker.kill()
            out = worker.communicate(timeout=5)[0]
    except Exception as exc:
        if not failing:
            raise
        print('MQTT worker cleanup failed:', type(exc).__name__, flush=True)
        return ''
    if failing:
        print('MQTT worker failure diagnostics:', 'exit_before_cleanup=', exit_before,
              'exit_after_cleanup=', worker.returncode,
              'received_messages=', len(records) - start, flush=True)
        print('MQTT worker stdout tail:\n' + out[-16384:], flush=True)
    return out


def scenario(model, legacy=False):
    user = 'native_' + model.lower() + ('_legacy' if legacy else '')
    key = 'u_' + user.encode().hex()
    base = ('device/ztc1/AABBCCDDEEFF' if legacy else
            f'phicomm/v2/{key}/{model.lower()}/aabbccddeeff')
    start = len(records)
    action, params = (('display.set', {'brightness': 0}) if model == 'M1' else
                      ('fan.set', {'on': True, 'speed': 0}) if model == 'A1' else
                      ('outlet.set', {'index': 0, 'on': True}))
    command = {'request_id': 'retained', 'action': action, 'params': params}
    publish(base + '/command', command, True)
    publish(base + '/ha/command', command, True, ha=True)
    environment = dict(os.environ)
    if legacy:
        environment['TC1_TEST_LEGACY'] = '1'
    worker = subprocess.Popen([str(ROOT / 'build/account-host' / model),
                               '127.0.0.1', str(port), user, '45'],
                              env=environment, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True)

    def messages(leaf):
        return [p for t, p, r in records[start:] if t == base + '/' + leaf]

    def results(request_id=None, code=None):
        return [row for p in messages('result') for row in [unpack(p)]
                if (request_id is None or row.get('request_id') == request_id)
                and (code is None or row['code'] == code)]

    def configs():
        identity = '/AABBCCDDEEFF/' if legacy else '/' + key + '/'
        return [json.loads(p) for t, p, r in records[start:]
                if t.startswith('homeassistant/') and identity in t
                and t.endswith('/config') and p]

    try:
        observer.wait(lambda: b'online' in messages('availability'), label=model + ' online')
        minimum = {'M1': 5, 'A1': 4, 'TC1': 17}[model]
        observer.wait(lambda: len(configs()) >= minimum, label=model + ' HA discovery')
        assert worker.poll() is None
        if not legacy:
            discovery = f'phicomm/v2/{key}/discovery/aabbccddeeff'
            observer.wait(lambda: any(t == discovery for t, p, r in records[start:]), label='MP discovery')
            announcement = next(unpack(p) for t, p, r in records[start:] if t == discovery)
            assert announcement['protocol'] == 'tmcp' and announcement['protocol_version'] == 2
            assert announcement['wire_formats'] == ['msgpack']
            assert announcement['default_wire_format'] == 'msgpack'
        for obj in configs():
            assert obj.get('unique_id') or obj.get('uniq_id')
            assert obj.get('json_attributes_topic') == base + '/ha/state', (model, obj)
            assert obj.get('retain') is False
            assert obj.get('device', {}).get('identifiers')
            for field in ('command_topic', 'cmd_t', 'percentage_command_topic', 'preset_mode_command_topic'):
                if field in obj:
                    assert obj[field] == base + '/ha/command', (model, field, obj[field])
            for field in ('payload_on', 'payload_off', 'pl_on', 'pl_off', 'pl_prs'):
                if field in obj and obj[field] not in ('ON', 'OFF'):
                    assert 'action' in json.loads(obj[field])
            env = Environment(undefined=StrictUndefined)
            for field in ('command_template', 'percentage_command_template', 'command_on_template', 'command_off_template'):
                if field in obj:
                    values = dict(value='ON' if field == 'command_template' and model == 'A1' else '50',
                                  brightness=128, red=1, green=2, blue=3)
                    assert 'action' in json.loads(env.from_string(obj[field]).render(**values))
        if model == 'TC1':
            energy = next(o for o in configs() if 'energySession' in o.get('uniq_id', '')
                          or 'energy_session' in o.get('uniq_id', ''))
            assert energy['state_class'] == 'total_increasing'
            assert all(o.get('device_class') != 'outlet' for o in configs()
                       if o.get('entity_category') == 'config')
        assert not messages('result'), 'retained command executed'
        command['request_id'] = 'same'
        assert b'\0' in msgpack.packb(command, use_bin_type=True)
        publish(base + '/command', command)
        observer.wait(lambda: len(results('same', 'accepted')) == 1, label='binary control including NUL')
        publish(base + '/command', command)
        observer.wait(lambda: len(results('same', 'accepted')) == 2, label='duplicate receipt')
        changed = {'brightness': 75} if model == 'M1' else {**params, 'on': False}
        publish(base + '/command', {**command, 'params': changed})
        observer.wait(lambda: results('same', 'conflict'), label='dedup conflict')
        publish(base + '/command', {'request_id': 'read', 'action': 'status.get', 'params': {}})
        observer.wait(lambda: results('read', 'ok'), label='native full status')
        status = results('read', 'ok')[-1]['data']
        assert status['model'] == model.lower()
        if model == 'M1':
            assert status['brightness'] == 0
        elif model == 'A1':
            assert status['on'] is True and status['speed'] == 0
        else:
            assert status['outlets'][0] is True
        before_invalid = len(results(code='invalid_params'))
        publish(base + '/command', command, ha=True)
        observer.wait(lambda: len(results(code='invalid_params')) > before_invalid,
                      label='JSON rejected on native command topic')
        # Native bytes must not fall through to the HA compatibility parser.
        publish(base + '/ha/command', command)
        # Isolated HA JSON remains functional, independent of the native path.
        publish(base + '/ha/command', {'action': action, 'params': changed}, ha=True)
        observer.wait(lambda: messages('ha/state'), label='HA JSON state')
        for p in messages('ha/state'):
            assert isinstance(json.loads(p), dict)
        if not legacy:
            observer.wait(lambda: messages('state') and messages('capabilities'), label='MP retained snapshots')
            for leaf in ('state', 'capabilities'):
                assert unpack(messages(leaf)[-1])['code'] == 'ok'
            retained = []
            snapshot = Observer(port, lambda c, u, m: retained.append((m.topic, m.retain)))
            try:
                snapshot.start(base + '/#')
                marker = base + '/_test_snapshot/' + uuid.uuid4().hex
                snapshot.publish(marker, 'barrier')
                snapshot.wait(lambda: any(t == marker for t, r in retained))
            finally:
                snapshot.close(failing=sys.exc_info()[0] is not None)
            assert (base + '/state', True) in retained and (base + '/capabilities', True) in retained
            assert not any(t == base + '/result' for t, r in retained)
        before_birth = len(configs())
        publish('homeassistant/status', 'online')
        observer.wait(lambda: len(configs()) > before_birth, label='HA birth rediscovery')
        barrier(base)
        print(model, 'legacy addressing' if legacy else 'account addressing',
              'PASS MP discovery/control/NUL, retained rejection, dedup/conflict, JSON isolation, HA templates/state/birth',
              flush=True)
    finally:
        failing = sys.exc_info()[0] is not None
        try:
            out = finish_worker(worker, failing, start)
            if not failing:
                # One native mutation and one HA mutation; replay/conflict/JSON
                # and retained/binary-HA traffic must produce no side effects.
                assert len([line for line in out.splitlines() if line.startswith('CONTROL ')]) == 2, out
        finally:
            for leaf in ('command', 'ha/command'):
                try:
                    publish(base + '/' + leaf, b'', True)
                except Exception:
                    if not failing:
                        raise


try:
    observer.start('#')
    for model in selected_models():
        scenario(model)
    if 'TC1' in selected_models():
        scenario('TC1', legacy=True)
finally:
    observer.close(failing=sys.exc_info()[0] is not None)
