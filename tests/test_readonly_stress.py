"""Keep corrupt successful replies distinct from recoverable overload."""
import base64
import json
import http.client
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

from tests.device.readonly_stress import InitializationError, ProtocolError, Runner, request


class ReadonlyStressTest(unittest.TestCase):
    def _assert_init_failure_evidence(self, runner, directory, ip, action, status,
                                      code, raw=None):
        evidence_path = Path(directory) / 'evidence.json'
        self.assertTrue(evidence_path.exists())
        self.assertEqual(evidence_path.stat().st_mode & 0o777, 0o600)
        evidence = json.loads(evidence_path.read_text(encoding='utf-8'))
        self.assertEqual(evidence['run']['status'], 'failed')
        self.assertFalse(evidence['run']['completed'])
        self.assertEqual(evidence['run']['failure_type'], 'InitializationError')
        self.assertEqual(evidence['rows'][-1]['ip'], ip)
        self.assertEqual(evidence['rows'][-1]['phase'], 'initialize')
        self.assertEqual(evidence['rows'][-1]['action'], action)
        self.assertEqual(evidence['rows'][-1]['http'], status)
        self.assertEqual(evidence['rows'][-1]['code'], code)
        if raw is not None:
            self.assertEqual(evidence['rows'][-1]['raw_base64'],
                             base64.b64encode(raw).decode())
        self.assertTrue(evidence['blockers'])
        self.assertFalse(evidence['run']['completed'])
        return evidence

    def test_initialize_503_fails_once_and_saves_before_baseline(self):
        ip = '127.0.0.1'
        raw = b'{"code":"busy"}'
        with tempfile.TemporaryDirectory() as directory:
            runner = Runner({ip: 'device'}, 'candidate', Path(directory))

            def request_busy(target, action='status.get', params=None, timeout=5,
                             capture=None):
                capture.update(http=503, raw=raw)
                return 503, {'code': 'busy'}, len(raw), 1.5, 12.0

            with patch('tests.device.readonly_stress.request', side_effect=request_busy) as mocked:
                with self.assertRaises(InitializationError):
                    runner.initialize()

            mocked.assert_called_once()
            evidence = self._assert_init_failure_evidence(
                runner, directory, ip, 'status.get', 503, 'busy', raw)
            self.assertEqual(evidence['before'], {})
            self.assertEqual(len(evidence['rows']), 1)

    def test_initialize_malformed_success_response_is_preserved_and_blocks(self):
        ip = '127.0.0.1'
        raw = b'{malformed'
        with tempfile.TemporaryDirectory() as directory:
            runner = Runner({ip: 'device'}, 'candidate', Path(directory))

            def malformed(target, action='status.get', params=None, timeout=5,
                          capture=None):
                capture.update(http=200, raw=raw)
                raise ProtocolError(200, raw)

            with patch('tests.device.readonly_stress.request', side_effect=malformed) as mocked:
                with self.assertRaises(InitializationError):
                    runner.initialize()

            mocked.assert_called_once()
            evidence = self._assert_init_failure_evidence(
                runner, directory, ip, 'status.get', 200, None, raw)
            self.assertEqual(evidence['before'], {})

    def test_initialize_transport_exception_is_saved_without_retry(self):
        ip = '127.0.0.1'
        with tempfile.TemporaryDirectory() as directory:
            runner = Runner({ip: 'device'}, 'candidate', Path(directory))
            with patch('tests.device.readonly_stress.request',
                       side_effect=http.client.RemoteDisconnected('closed')) as mocked:
                with self.assertRaises(InitializationError):
                    runner.initialize()

            mocked.assert_called_once()
            evidence = self._assert_init_failure_evidence(
                runner, directory, ip, 'status.get', None, None)
            self.assertIn('RemoteDisconnected', evidence['rows'][-1]['error'])
            self.assertNotIn('raw_base64', evidence['rows'][-1])

    def test_initialize_partial_targets_records_later_target_failure(self):
        targets = {'127.0.0.1': 'device-a', '127.0.0.2': 'device-b'}
        raw_busy = b'{"code":"busy"}'
        state = {
            'model': 'tc1', 'device_id': 'device-a', 'version': 'candidate',
            'uptime_seconds': 20, 'socket_states': [0] * 6,
            'child_lock': False, 'led_enabled': True,
        }
        with tempfile.TemporaryDirectory() as directory:
            runner = Runner(targets, 'candidate', Path(directory))
            calls = []

            def request_partial(ip, action='status.get', params=None, timeout=5,
                                capture=None):
                calls.append((ip, action))
                if ip == '127.0.0.1' and action == 'status.get':
                    body, reply = json.dumps({'code': 'ok', 'data': state}).encode(), {
                        'code': 'ok', 'data': state}
                    capture.update(http=200, raw=body)
                    return 200, reply, len(body), 1.0, 100.0
                if ip == '127.0.0.1' and action == 'config.get':
                    body, reply = b'{"code":"ok","data":{}}', {'code': 'ok', 'data': {}}
                    capture.update(http=200, raw=body)
                    return 200, reply, len(body), 1.0, 101.0
                capture.update(http=503, raw=raw_busy)
                return 503, {'code': 'busy'}, len(raw_busy), 1.0, 102.0

            with patch('tests.device.readonly_stress.request', side_effect=request_partial) as mocked:
                with self.assertRaises(InitializationError):
                    runner.initialize()

            self.assertEqual(calls, [
                ('127.0.0.1', 'status.get'), ('127.0.0.1', 'config.get'),
                ('127.0.0.2', 'status.get'),
            ])
            self.assertEqual(mocked.call_count, 3)
            evidence = self._assert_init_failure_evidence(
                runner, directory, '127.0.0.2', 'status.get', 503, 'busy', raw_busy)
            self.assertEqual(set(evidence['before']), {'127.0.0.1'})

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
            zero = {'json_live_blocks': 0, 'memory_active': 0, 'memory_reserved': 0,
                    'memory_waiting': 0}
            nonzero = dict(zero, json_live_blocks=1)
            runner.memory = [row(1, zero), row(2, zero), row(3, zero),
                             row(4, nonzero)]
            self.assertFalse(runner.idle_zero_tail('127.0.0.1', 'final'))
            runner.memory.extend([row(5, zero), row(6, zero), row(7, zero)])
            self.assertTrue(runner.idle_zero_tail('127.0.0.1', 'final'))
            runner.memory.append(row(8, dict(zero, memory_waiting=1)))
            self.assertFalse(runner.idle_zero_tail('127.0.0.1', 'final'))

    def test_early_uptime_reset_is_not_hidden_by_boot_origin_tolerance(self):
        ip = '127.0.0.1'
        state = {'model': 'tc1', 'device_id': 'test', 'version': 'test',
                 'uptime_seconds': 2, 'socket_states': [0] * 6,
                 'child_lock': False, 'led_enabled': True}
        with tempfile.TemporaryDirectory() as directory:
            runner = Runner({ip: 'test'}, 'test', Path(directory))
            runner.before[ip] = {'id': 'test', 'state': state, 'boot_origin': 98}
            runner.check(ip, state, 100)
            runner.check(ip, dict(state, uptime_seconds=1), 103)
            self.assertTrue(runner.stop.is_set())
            self.assertIn('uptime decreased', runner.blockers[0]['reason'])

    def test_active_baseline_samples_cannot_hide_idle_heap_loss(self):
        ip = '127.0.0.1'
        zero = {'json_live_blocks': 0, 'memory_active': 0,
                'memory_reserved': 0, 'memory_waiting': 0}
        with tempfile.TemporaryDirectory() as directory:
            runner = Runner({ip: 'test'}, 'test', Path(directory))
            runner.before[ip] = {'config': {}}
            for index in range(10):
                data = dict(zero, free_memory=30000)
                if index < 7:
                    data.update(json_live_blocks=27, memory_active=1,
                                memory_reserved=512, free_memory=25000)
                runner.memory.append({'ip': ip, 'phase': 'baseline', 'ok': True,
                                      'http': 200, 'data': data})
                runner.memory.append({'ip': ip, 'phase': 'final', 'ok': True,
                                      'http': 200, 'data': dict(zero, free_memory=28000)})
            with patch.object(runner, 'restore_connectivity', return_value=True), \
                 patch.object(runner, 'memory_window'), \
                 patch.object(runner, 'wait_idle_quiescence', return_value=True), \
                 patch('tests.device.readonly_stress.request',
                       return_value=(200, {'code': 'ok', 'data': {}})):
                with self.assertRaises(SystemExit):
                    runner.finish()
            self.assertTrue(any('persistent heap loss' in row['reason']
                                for row in runner.blockers))

    def test_overlapping_status_replies_do_not_report_a_false_reset(self):
        ip = '127.0.0.1'
        state = {'model': 'tc1', 'device_id': 'test', 'version': 'test',
                 'uptime_seconds': 5, 'socket_states': [0] * 6,
                 'child_lock': False, 'led_enabled': True}
        with tempfile.TemporaryDirectory() as directory:
            runner = Runner({ip: 'test'}, 'test', Path(directory))
            runner.before[ip] = {'id': 'test', 'state': state, 'boot_origin': 95}
            runner.check(ip, state, 100, end=110)
            runner.check(ip, dict(state, uptime_seconds=4), 99, end=105)
            runner.check(ip, dict(state, uptime_seconds=16), 111, end=112)
            self.assertFalse(runner.stop.is_set())

    def test_memory_uptime_reset_blocks_without_a_status_probe(self):
        for uptimes, blocked in (((2000, 1000), True), ((2000, 3000), False)):
            with self.subTest(uptimes=uptimes), tempfile.TemporaryDirectory() as directory:
                runner = Runner({'127.0.0.1': 'test'}, 'test', Path(directory))
                connection = Mock()
                response = connection.getresponse.return_value
                response.status = 200
                response.read.side_effect = [json.dumps({
                    'code': 'ok', 'data': {'uptime_ms': uptime}}).encode()
                    for uptime in uptimes]
                with patch('tests.device.readonly_stress.http.client.HTTPConnection',
                           return_value=connection), \
                     patch('tests.device.readonly_stress.time.monotonic',
                           side_effect=(100, 100.1, 101, 101.1)):
                    runner.memory_sample('127.0.0.1', 'idle')
                    runner.memory_sample('127.0.0.1', 'idle')
                self.assertEqual(runner.stop.is_set(), blocked)
                if blocked:
                    self.assertIn('uptime decreased', runner.blockers[0]['reason'])
                self.assertTrue(all('request_ended_monotonic' in row for row in runner.memory))

    def test_initial_status_seeds_the_first_memory_uptime_comparison(self):
        ip = '127.0.0.1'
        state = {'model': 'tc1', 'device_id': 'test', 'version': 'test',
                 'uptime_seconds': 2, 'socket_states': [0] * 6,
                 'child_lock': False, 'led_enabled': True}
        replies = [(200, {'code': 'ok', 'data': state}, 1, 5, 100),
                   (200, {'code': 'ok', 'data': {}}, 1, 5, 100.1)]
        with tempfile.TemporaryDirectory() as directory:
            runner = Runner({ip: 'test'}, 'test', Path(directory))
            with patch('tests.device.readonly_stress.request', side_effect=replies), \
                 patch.object(runner, 'memory_window'), \
                 patch.object(runner, 'wait_idle_quiescence', return_value=True):
                runner.initialize()
            connection = Mock()
            response = connection.getresponse.return_value
            response.status = 200
            response.read.return_value = b'{"code":"ok","data":{"uptime_ms":1000}}'
            with patch('tests.device.readonly_stress.http.client.HTTPConnection',
                       return_value=connection), \
                 patch('tests.device.readonly_stress.time.monotonic',
                       side_effect=(101, 101.1)):
                runner.memory_sample(ip, 'idle')
            self.assertTrue(runner.stop.is_set())
            self.assertIn('memory uptime decreased', runner.blockers[0]['reason'])

    def test_shared_core_requires_healthy_owner_and_actual_admission(self):
        healthy = {'http_core_owner_state': 1, 'http_core_close_failures': 0,
                   'http_core_restarts': 0, 'http_core_tcp_limit': 3,
                   'http_core_tcp_rc': 0, 'http_core_occupied': 1,
                   'http_core_server_bytes': 4636, 'http_tcp_sample_ms': 1000}
        for changed, blocked in (({}, False),
                                 ({'http_core_owner_state': 2}, True),
                                 ({'http_core_owner_state': 3}, True),
                                 ({'http_core_close_failures': 1}, True),
                                 ({'http_core_restarts': 1}, True),
                                 ({'http_core_tcp_limit': 5}, True),
                                 ({'http_core_tcp_rc': -1}, True),
                                 ({'http_core_occupied': 4}, True),
                                 ({'http_core_server_bytes': None}, True)):
            with self.subTest(changed=changed), tempfile.TemporaryDirectory() as directory:
                runner = Runner({'127.0.0.1': 'test'}, 'test', Path(directory),
                                require_shared_core=True)
                runner.before = {'127.0.0.1': {'state': {'model': 'tc1'}}}
                runner.check_shared_core('127.0.0.1', dict(healthy, **changed))
                self.assertEqual(runner.stop.is_set(), blocked)

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

    def test_anomaly_scenarios_recover_each_target_exactly_once(self):
        ips = ('127.0.0.1', '127.0.0.2', '127.0.0.3')
        scenarios = (
            ('slow_clients', 'incomplete-headers-recovery'),
            ('aborted_clients', 'aborted-connections-recovery'),
            ('buffered_read_clients', 'buffered-read-input-recovery'),
            ('missing_read_clients', 'missing-read-eof-recovery'),
        )
        for method, recovery in scenarios:
            with self.subTest(method=method), tempfile.TemporaryDirectory() as directory:
                plan = {'steps': [method], 'phases': [], 'recovery_steps': [
                    {'name': recovery, 'targets': list(ips)}]}
                runner = Runner({ip: ip for ip in ips}, 'test', Path(directory), plan=plan)
                # Exercise the real scenario/recovery wiring without physical
                # sockets, delays, status parsing or filesystem churn.
                with patch('tests.device.readonly_stress.socket.create_connection'), \
                     patch.object(runner.stop, 'wait'), \
                     patch.object(runner, 'save'), \
                     patch.object(runner, 'memory_sample'), \
                     patch.object(runner, 'sample', return_value={'ok': True}) as sample:
                    runner.scenario(method, getattr(runner, method))
                self.assertEqual(sample.call_count, 3 * len(ips))
                self.assertEqual([list(record['targets']) for record in runner.recoveries],
                                 [[ip] for ip in ips])
                self.assertTrue(runner._planned_steps_complete())

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
