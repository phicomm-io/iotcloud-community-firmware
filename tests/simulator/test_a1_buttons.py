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
        raw({'sim_button': dict(key=key, clicks=clicks, low_memory=True)})

    try:
        # Factory power key: wake while off, inspect while on; no mode/speed/Flash changes.
        command('fan.mode.set', {'mode': 'auto'}, 'accepted')
        command('fan.set', {'on': False}, 'accepted')
        press(0)
        baseline = command('status.get')
        assert baseline['requested_fan_on'] and baseline['fan_mode'] == 'auto'
        feedback = raw({'sim_panel_feedback': True})
        assert feedback['until'] > 0 and feedback['revision'] > 0
        press(0)
        assert raw({'sim_panel_feedback': True})['config_revision'] == feedback['config_revision']
        inspected = command('status.get')
        assert all(inspected[k] == baseline[k] for k in ('requested_fan_on','fan_mode','requested_speed'))
        command('fan.panel', {'on': False}, 'invalid_params')
        command('child_lock.set', {'enabled': True}, 'accepted')
        feedback = raw({'sim_panel_feedback': True})
        press(0)
        assert raw({'sim_panel_feedback': True}) == feedback
        command('child_lock.set', {'enabled': False}, 'accepted')
        for seconds in range(1,5):
            raw({'sim_button': dict(key=0,seconds=seconds)})
            assert not command('status.get')['requested_fan_on']
            assert raw({'sim_panel_feedback': True})['until'] == 0
            # Double-click while off changes mode without turning power on.
            mode = command('status.get')['fan_mode']
            press(0,2)
            off = command('status.get')
            assert not off['requested_fan_on'] and off['fan_mode'] != mode
            press(0)
        command('fan.set', {'on': False}, 'accepted')
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
