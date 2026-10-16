#!/usr/bin/env python3
"""Exercise A1 button actions through production service/runtime/control code.

Run tests/simulator/build.sh first. Peripheral state and Flash use RAM shims.
"""
import json
import os
from pathlib import Path
import select
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main():
    node = subprocess.Popen(
        [str(ROOT / 'build/simulator/A1'), '020000010099', '127.39.1.99', '127.0.0.1', '9', ''],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        text=True, env={**os.environ, 'SIM_BIND_IP': '127.39.1.99'})

    def raw(q, code='ok'):
        node.stdin.write(json.dumps(q) + '\n')
        node.stdin.flush()
        assert select.select([node.stdout], [], [], 5)[0], ('timeout', q)
        r = json.loads(node.stdout.readline())
        assert r['code'] == code, (q, r)
        return r.get('data')

    def command(action, params=None, code='ok'):
        return raw({'action': action, 'params': params or {}}, code)

    def mapping(action, params=None, key=2, clicks=1):
        command('buttons.set', dict(key=key, clicks=clicks, action=action, params=params or {}), 'accepted')

    def press(key=2, clicks=1):
        raw({'sim_button': dict(key=key, clicks=clicks)})

    try:
        caps = command('capabilities.get')
        assert caps['button_keys'] == ['power', 'wifi', 'combination']
        command('light_effect.set', {'effect': 4, 'segments': 3}, 'accepted')
        mapping('light_effect.next')
        press()
        light = command('light_effect.get')
        assert light['effect'] == 0 and light['segments'] == 3
        mapping('light_effect.select', {'effect': 3}, key=1, clicks=2)
        press(1, 2)
        assert command('light_effect.get')['effect'] == 3
        mapping('light.set', {'toggle': True}, key=0)
        press(0)
        s = command('status.get')
        assert not s['led_enabled'] and s['brightness'] == 255
        press(0)
        assert command('status.get')['led_enabled']
        mapping('light_always.set', {'toggle': True})
        press()
        assert command('status.get')['led_always_on']
        mapping('child_lock.set', {'toggle': True}, clicks=2)
        press(clicks=2)
        assert command('status.get')['child_lock']
        press()  # Locked buttons cannot change normal settings.
        assert command('status.get')['led_always_on']
        press(clicks=2)  # The configured unlock gesture still works.
        assert not command('status.get')['child_lock']
        press()
        assert not command('status.get')['led_always_on']
        mapping('fan.set', {'toggle': True, 'speed': 42}, key=1)
        press(1)
        assert command('status.get')['requested_fan_on']
        press(1)
        assert not command('status.get')['requested_fan_on']
        raw({'sim_filter_seconds': 7200})
        command('filter.set', {'lifetime_hours': 100}, 'accepted')
        f = command('filter.get')
        assert f['used_seconds'] == 7200 and f['remaining_percent'] == 98
        mapping('filter.reset')
        press()
        f = command('filter.get')
        assert f['used_seconds'] == 0 and f['lifetime_hours'] == 100 and f['remaining_percent'] == 100
        command('filter.set', {'lifetime_hours': 0}, 'invalid_params')
        command('light_effect.select', {'effect': 5}, 'invalid_params')
        print('PASS A1 production button actions: effect wrap/preservation, light toggles, lock/unlock, fan and filter reset')
    finally:
        node.terminate()
        node.communicate(timeout=5)


if __name__ == '__main__':
    main()
