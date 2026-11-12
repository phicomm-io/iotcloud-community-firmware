"""Execute the actual A1/fresh-air typed percent producer and session owner."""
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


class FanSensor(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='a1-fan-sensor-')
        cls.addClassCleanup(cls.temp.cleanup)
        build = Path(cls.temp.name)
        source = (ROOT / 'A1/mqtt.c').read_text()
        producer = source[source.index('/* sensor-values-v1'):source.index('static int discovery_build(', source.index('/* sensor-values-v1'))]
        (build / 'production.inc').write_text(function(source, 'static void primary_snapshot(') + '\n' + producer + '\n' + '\n'.join(
            function(source, signature) for signature in ('static int states_step(', 'static bool filter_priority(',
                                                         'static int session_initial(', 'static int session_cycle(')))
        cls.binaries = []
        for fresh in (False, True):
            binary = build / ('fresh-fan-sensor' if fresh else 'fan-sensor')
            subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu99', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation',
                            '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                            *(['-DA1_FRESH_AIR'] if fresh else []), '-I' + str(ROOT), '-I' + str(build),
                            str(ROOT / 'A1/tests/fan_sensor.c'), str(ROOT / 'common/typed_wire.c'), str(ROOT / 'common/wire_buffer.c'), '-lm', '-o', str(binary)], check=True)
            cls.binaries.append(binary)

    def scenario(self, name):
        results = []
        for binary in self.binaries:
            with self.subTest(build=binary.name):
                run = subprocess.run([str(binary), name], check=True, timeout=15, capture_output=True,
                                     text=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'})
                messages = []
                for line in run.stdout.splitlines():
                    marker, retained, topic, payload = line.split('\t', 3)
                    self.assertEqual((marker, retained), ('PUB', '0'))
                    body = json.loads(payload)
                    # Host observer joins separately validated retained metadata with MP state.
                    self.assertEqual((body['schema'], body['device_id']), (3, 'aabbccddeeff'))
                    for field in ('boot', 'sequence'):
                        self.assertIs(type(body[field]), int)
                        self.assertTrue(0 < body[field] <= 0xffffffff)
                    value = body['sensors']['fan_speed_percent']
                    self.assertEqual(value['type'], 'number')
                    self.assertEqual(value['name'], 'Fan speed')
                    self.assertEqual(value['unit'], '%')
                    self.assertIs(type(value['valid']), bool)
                    self.assertEqual(value['age_ms'], 0)
                    if value['valid']:
                        self.assertIs(type(value['value']), int)
                        self.assertTrue(0 <= value['value'] <= 100)
                    else:
                        self.assertIsNone(value['value'])
                    messages.append((topic, body))
                results.append(messages)
        return results

    def test_effective_percent_readiness_and_invalid_output(self):
        for messages in self.scenario('payload'):
            self.assertEqual([m[1]['sensors']['fan_speed_percent']['value'] for m in messages],
                             [37, 0, None, None, None, 100, None])

    def test_effective_off_and_fresh_primary_never_requested_speed_or_rpm(self):
        for messages in self.scenario('effective'):
            self.assertEqual([m[1]['sensors']['fan_speed_percent']['value'] for m in messages], [0, 37])

    def test_reconnect_and_account_switch_preserve_process_counters(self):
        for messages in self.scenario('reconnect'):
            self.assertEqual([m[1]['sequence'] for m in messages], [1, 2])
            self.assertEqual({m[1]['boot'] for m in messages}, {0xffffffff})
            self.assertEqual(messages[0][0], 'phicomm/v2/u_74657374/sensor/aabbccddeeff/state')
            self.assertEqual(messages[1][0], 'phicomm/v2/u_6e6578742f6f776e6572/sensor/aabbccddeeff/state')

    def test_uint32_wire_integers_wrap_without_zero(self):
        for messages in self.scenario('counters'):
            self.assertEqual([m[1]['sequence'] for m in messages], [0xffffffff, 1])

    def test_resource_retry_and_fatal_error_propagation(self):
        for messages in self.scenario('backoff'):
            self.assertEqual([m[1]['sequence'] for m in messages], [1, 2])

    def test_failed_or_zero_random_is_bounded_and_never_reuses_boot(self):
        self.scenario('failures')

    def test_real_owner_connection_ota_and_discovery_gates(self):
        self.scenario('gates')

    def test_slow_typed_publishes_leave_native_and_features_progress(self):
        self.scenario('fairness')

    def test_native_and_ha_have_independent_success_and_retry_cursors(self):
        self.scenario('independent')

    def test_maximum_account_and_clock_wrap_keep_bounded_cadence(self):
        for messages in self.scenario('bounds'):
            self.assertEqual(len(messages), 2)
            self.assertEqual(messages[0][0], 'phicomm/v2/u_' + '75' * 63 + '/sensor/aabbccddeeff/state')


if __name__ == '__main__':
    unittest.main()
