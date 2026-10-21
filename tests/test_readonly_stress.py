"""Keep corrupt successful replies distinct from recoverable overload."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

from tests.device.readonly_stress import ProtocolError, Runner, request


class ReadonlyStressTest(unittest.TestCase):
    def test_response_classification_and_connection_cleanup(self):
        for status, body, corrupt in (
            (200, b'not JSON', True),
            (200, b'{"code":"ok","data":null}', True),
            (503, b'busy', False),
            (200, b'{"code":"ok","data":{}}', False),
        ):
            with self.subTest(status=status, body=body):
                connection = Mock()
                response = connection.getresponse.return_value
                response.status, response.read.return_value = status, body
                with patch('tests.device.readonly_stress.http.client.HTTPConnection', return_value=connection):
                    if corrupt:
                        with self.assertRaises(ProtocolError) as caught:
                            request('127.0.0.1')
                        self.assertEqual(caught.exception.raw, body)
                    else:
                        self.assertEqual(request('127.0.0.1')[0], status)
                connection.close.assert_called_once()

    def test_corrupt_success_stops_run_and_preserves_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            runner = Runner({'127.0.0.1': 'test'}, 'test', Path(directory))
            with patch('tests.device.readonly_stress.request', side_effect=ProtocolError(200, b'broken')):
                row = runner.sample('127.0.0.1', 'probe')
            self.assertTrue(runner.stop.is_set())
            self.assertEqual(len(runner.blockers), 1)
            self.assertEqual(row['raw_base64'], 'YnJva2Vu')

    def test_overload_is_allowed_but_unexpected_command_result_blocks(self):
        for code, blocked in (('busy', False), ('unsupported', True)):
            with self.subTest(code=code), tempfile.TemporaryDirectory() as directory:
                runner = Runner({'127.0.0.1': 'test'}, 'test', Path(directory))
                reply = (200, {'code': code}, 20, 1, 0)
                with patch('tests.device.readonly_stress.request', return_value=reply):
                    self.assertFalse(runner.sample(
                        '127.0.0.1', 'probe', 'logs.get', {'limit': 8})['ok'])
                self.assertEqual(runner.stop.is_set(), blocked)

    def test_non_503_http_errors_block_but_503_is_an_allowed_degradation(self):
        for status, blocked in ((503, False), (500, True), (404, True)):
            with self.subTest(status=status), tempfile.TemporaryDirectory() as directory:
                runner = Runner({'127.0.0.1': 'test'}, 'test', Path(directory))
                with patch('tests.device.readonly_stress.request',
                           return_value=(status, {'code': 'busy'}, 20, 1, 0)):
                    row = runner.sample('127.0.0.1', 'probe', 'logs.get')
                self.assertFalse(row['ok'])
                self.assertEqual(runner.stop.is_set(), blocked)

    def test_missing_baseline_or_live_control_field_blocks(self):
        ip = '127.0.0.1'
        baseline = {
            'model': 'tc1', 'device_id': 'test', 'version': 'test',
            'uptime_seconds': 10, 'socket_states': [0] * 6,
            'child_lock': False, 'led_enabled': True,
        }
        for missing_side in ('baseline', 'live'):
            with self.subTest(missing_side=missing_side), tempfile.TemporaryDirectory() as directory:
                before = dict(baseline)
                state = dict(baseline)
                if missing_side == 'baseline':
                    before.pop('child_lock')
                else:
                    state.pop('child_lock')
                runner = Runner({ip: 'test'}, 'test', Path(directory))
                runner.before = {ip: {
                    'id': 'test', 'state': before, 'boot_origin': 0,
                    'config': {},
                }}
                runner.check(ip, state, 10)
                self.assertTrue(runner.stop.is_set())
                self.assertIn('missing control state field', runner.blockers[0]['reason'])

    def test_idle_gate_requires_terminal_consecutive_zero_samples(self):
        with tempfile.TemporaryDirectory() as directory:
            runner = Runner({'127.0.0.1': 'test'}, 'test', Path(directory))
            def row(at, values, ok=True):
                return {'ip': '127.0.0.1', 'phase': 'final', 'at': at,
                        'ok': ok, 'data': values}
            zero = {'json_live_blocks': 0, 'memory_active': 0, 'memory_reserved': 0}
            nonzero = {'json_live_blocks': 1, 'memory_active': 0, 'memory_reserved': 0}
            runner.memory = [row(1, zero), row(2, zero), row(3, zero),
                             row(4, nonzero)]
            self.assertFalse(runner.idle_zero_tail('127.0.0.1', 'final'))
            runner.memory.extend([row(5, zero), row(6, zero), row(7, zero)])
            self.assertTrue(runner.idle_zero_tail('127.0.0.1', 'final'))

    def test_planned_completion_rejects_missing_or_short_phases_and_recovery(self):
        plan = {
            'steps': ['concurrency-1'],
            'phases': [{'name': 'concurrency-1', 'requested_seconds': 60,
                        'clients_per_device': 1}],
            'recovery_steps': [{'name': 'concurrency-1-recovery',
                                'targets': ['127.0.0.1']}],
        }
        with tempfile.TemporaryDirectory() as directory:
            runner = Runner({'127.0.0.1': 'test'}, 'test', Path(directory), plan=plan)
            runner.scenarios = [{'name': 'concurrency-1', 'completed': True}]
            self.assertFalse(runner._planned_steps_complete())
            runner.phases = [{'name': 'concurrency-1', 'completed': True,
                              'clients_per_device': 1, 'requested_seconds': 60,
                              'elapsed_seconds': 59.9}]
            self.assertFalse(runner._planned_steps_complete())
            runner.phases[0]['elapsed_seconds'] = 60
            self.assertFalse(runner._planned_steps_complete())
            runner.recoveries = [{'name': 'concurrency-1-recovery',
                                  'completed': True,
                                  'targets': {'127.0.0.1': {'completed': True}}}]
            self.assertTrue(runner._planned_steps_complete())

    def test_planned_recovery_accepts_one_record_per_target_and_requires_full_coverage(self):
        ips = ('127.0.0.1', '127.0.0.2')
        plan = {
            'steps': ['slow-clients'],
            'phases': [],
            'recovery_steps': [{'name': 'slow-recovery', 'targets': list(ips)}],
        }
        with tempfile.TemporaryDirectory() as directory:
            runner = Runner({ip: ip for ip in ips}, 'test', Path(directory), plan=plan)
            runner.scenarios = [{'name': 'slow-clients', 'completed': True}]
            runner.recoveries = [
                {'name': 'slow-recovery', 'completed': True,
                 'targets': {ip: {'completed': True}}}
                for ip in ips
            ]
            self.assertTrue(runner._planned_steps_complete())
            runner.recoveries.pop()
            self.assertFalse(runner._planned_steps_complete())
            runner.recoveries.append({
                'name': 'slow-recovery', 'completed': True,
                'targets': {ips[0]: {'completed': True}},
            })
            self.assertFalse(runner._planned_steps_complete())

    def test_unplanned_short_runner_does_not_require_full_stress_plan(self):
        with tempfile.TemporaryDirectory() as directory:
            runner = Runner({'127.0.0.1': 'test'}, 'test', Path(directory))
        self.assertFalse(runner.run['plan_completion_required'])
        self.assertTrue(runner._planned_steps_complete())

    def test_interruption_snapshot_is_marked_incomplete(self):
        with tempfile.TemporaryDirectory() as directory:
            runner = Runner({'127.0.0.1': 'test'}, 'test', Path(directory),
                            plan={'mode': 'full', 'steps': ['soak']})
            runner.save_interrupted(KeyboardInterrupt())
            evidence = json.loads(
                (Path(directory) / 'evidence.json').read_text(encoding='utf-8'))
        self.assertEqual(evidence['run']['status'], 'interrupted')
        self.assertFalse(evidence['run']['completed'])
        self.assertEqual(evidence['run']['failure_type'], 'KeyboardInterrupt')

    def test_network_recovery_gate(self):
        for recovered in (True, False):
            with self.subTest(recovered=recovered), tempfile.TemporaryDirectory() as directory:
                runner = Runner({'127.0.0.1': 'test'}, 'test', Path(directory))
                runner.before = {'127.0.0.1': {'state': {'wifi_connected': True, 'mqtt_connected': True}}}
                row = {'ok': True, 'wifi_connected': True, 'mqtt_connected': recovered}
                with patch.object(runner, 'sample', return_value=row), patch.object(runner.stop, 'wait'), patch('tests.device.readonly_stress.time.monotonic', side_effect=(0, 1, 91)):
                    runner.restore_connectivity()
                self.assertEqual(runner.stop.is_set(), not recovered)
