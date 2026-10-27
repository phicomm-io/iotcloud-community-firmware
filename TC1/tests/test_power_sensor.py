"""Actual TC1 pulse sampler + typed MQTT publisher + session owner, under sanitizers."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class PowerSensor(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='tc1-power-sensor-')
        cls.addClassCleanup(cls.temp.cleanup)
        build = Path(cls.temp.name)
        source = (ROOT / 'TC1/mqtt_server/user_mqtt_client.c').read_text()
        # Keep the complete production measurement translation unit; only SDK
        # includes are replaced with fixture declarations. No pulse algorithm copy.
        cls.power = '\n'.join(line for line in (ROOT / 'TC1/user_power.c').read_text().splitlines()
                              if not line.startswith('#include'))
        publisher = source[source.index('/* sensor-values-v1'):source.index('static bool power_requested(')]
        (build / 'production.inc').write_text(cls.power + '\n' + publisher + '\n' +
            function(source, 'static bool power_requested(') + '\n' +
            function(source, 'static int session_initial(') + '\n' +
            function(source, 'static int session_cycle('))
        cls.binary = build / 'power-sensor'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu99', '-g', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                        '-I' + str(ROOT), '-I' + str(build), str(ROOT / 'TC1/tests/power_sensor.c'),
                        '-pthread', '-lm', '-o', str(cls.binary)], check=True)

    def scenario(self, name):
        result = subprocess.run([str(self.binary), name], check=True, timeout=15, capture_output=True,
                                text=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'})
        messages = []
        for line in result.stdout.splitlines():
            marker, retained, topic, payload = line.split('\t', 3)
            self.assertEqual(marker, 'PUB')
            self.assertEqual(retained, '0')
            body = json.loads(payload)
            self.assertEqual(body['schema'], 2)
            self.assertEqual(body['model'], 'tc1')
            self.assertEqual(body['device_id'], 'aabbccddeeff')
            for field in ('boot', 'sequence'):
                self.assertIs(type(body[field]), int)
                self.assertTrue(0 < body[field] <= 0xffffffff)
            power = body['sensors']['power']
            self.assertEqual(power['type'], 'number')
            self.assertEqual(power['name'], 'Power')
            self.assertEqual(power['unit'], 'W')
            self.assertIs(type(power['valid']), bool)
            self.assertIs(type(power['age_ms']), int)
            self.assertGreaterEqual(power['age_ms'], 0)
            if not power['valid']:
                self.assertIsNone(power['value'])
            messages.append((topic, body))
        return messages

    def test_actual_irq_sample_age_zero_and_invalid_boundaries(self):
        self.scenario('reader')

    def test_interrupt_snapshot_never_mixes_value_and_timestamp(self):
        self.scenario('concurrent')

    def test_actual_sdk_clock_is_not_reentered_from_the_typed_reader(self):
        build = Path(self.temp.name)
        sdk = Path(os.environ.get('TC1_SDK_DIR', ROOT / 'build/tc1-sdk-3.6.4/mico-os'))
        clock = (sdk / 'platform/MCU/platform_nsclock.c').read_text()
        (build / 'sdk-clock.inc').write_text(function(clock, 'uint64_t platform_get_nanosecond_clock_value('))
        production = build / 'clock-power.inc'
        production.write_text(self.power)
        binary = build / 'power-clock'
        flags = [os.environ.get('CC', 'cc'), '-std=gnu99', '-g', '-Wall', '-Wextra', '-Werror',
                 '-Wno-unused-parameter',
                 '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                 '-I' + str(ROOT), '-I' + str(build), str(ROOT / 'TC1/tests/power_clock.c'),
                 '-lm', '-o', str(binary)]
        subprocess.run(flags, check=True)
        subprocess.run([str(binary)], check=True, timeout=15,
                       env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'})
        # Reintroduce only the unsafe SDK read side effect, while still using
        # the correct RTOS age domain. The actual shared accumulator must fail
        # when an older SDK reader was suspended, independently of age math.
        old = self.power.replace('uint32_t age=mico_rtos_get_time()-at;',
                                'uint32_t age=(mico_nanosecond_clock_value(),mico_rtos_get_time())-at;')
        self.assertNotEqual(old, self.power)
        production.write_text(old)
        subprocess.run(flags, check=True)
        run = subprocess.run([str(binary)], capture_output=True, timeout=15,
                             env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'})
        self.assertNotEqual(run.returncode, 0, 'unguarded mutable SDK clock must fail')

    def test_real_owner_payload_age_account_change_and_process_counters(self):
        messages = self.scenario('payload')
        self.assertEqual(len(messages), 5)
        self.assertEqual(messages[0][0], 'phicomm/v2/u_74657374/sensor/aabbccddeeff/state')
        self.assertEqual(messages[0][1]['sensors']['power'],
                         dict(type='number', name='Power', value=3.109, unit='W', valid=True, age_ms=100))
        self.assertEqual(messages[1][1]['sensors']['power']['age_ms'], 5100)
        self.assertFalse(messages[2][1]['sensors']['power']['valid'])
        self.assertEqual(messages[2][1]['sensors']['power']['age_ms'], 20000)
        self.assertEqual(messages[3][0], 'phicomm/v2/u_6e6578742f6f776e6572/sensor/aabbccddeeff/state')
        self.assertEqual(messages[3][1]['sensors']['power'],
                         dict(type='number', name='Power', value=None, unit='W', valid=False, age_ms=0xffffffff))
        self.assertEqual(messages[4][1]['sensors']['power'],
                         dict(type='number', name='Power', value=12.5, unit='W', valid=True, age_ms=0))
        self.assertEqual([m[1]['sequence'] for m in messages], [1, 2, 3, 4, 5])
        self.assertEqual({m[1]['boot'] for m in messages}, {0xffffffff})

    def test_uint32_positive_integer_wrap_skips_zero(self):
        messages = self.scenario('counters')
        self.assertEqual([m[1]['sequence'] for m in messages], [0xffffffff, 1])

    def test_resource_retry_is_bounded_and_network_error_propagates(self):
        messages = self.scenario('backoff')
        self.assertEqual([m[1]['sequence'] for m in messages], [2, 3])

    def test_random_zero_random_failure_and_numeric_failure_back_off(self):
        self.scenario('failures')

    def test_no_feed_for_legacy_ota_stale_session_or_initial_discovery(self):
        self.scenario('gates')

    def test_slow_sends_cannot_starve_original_status_or_power(self):
        self.scenario('fairness')

    def test_maximum_account_key_and_initial_zero_are_bounded(self):
        messages = self.scenario('bounds')
        self.assertEqual(messages[0][0], 'phicomm/v2/u_' + '75' * 63 + '/sensor/aabbccddeeff/state')
        self.assertFalse(messages[0][1]['sensors']['power']['valid'])
        self.assertEqual(messages[1][1]['sensors']['power'],
                             dict(type='number', name='Power', value=0, unit='W', valid=True, age_ms=0))


if __name__ == '__main__':
    unittest.main()
