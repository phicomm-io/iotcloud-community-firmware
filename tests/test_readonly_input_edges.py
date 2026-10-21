"""Offline contract tests for read-only HTTP input edge construction."""
import json
import io
from pathlib import Path
import stat
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest.mock import patch

from tests.device.readonly_input_edges import (
    AUTHORIZED_TARGETS,
    AUTHORIZED_MODELS,
    LIVE_BODY,
    MAX_REQUEST_BYTES,
    InputRunner,
    ProtocolError,
    build_scenarios,
    main,
    send_scenario,
    validate_targets,
)
from tests.device.readonly_shared_httpd import SharedHTTPRunner


class FakeClock:
    def __init__(self, close_at=None):
        self.now = 0.0
        self.close_at = close_at

    def monotonic(self):
        return self.now

    def select(self, readers, _writers, _errors, timeout):
        if self.close_at is not None and self.now + timeout >= self.close_at:
            self.now = self.close_at
            return readers, [], []
        self.now += timeout
        return [], [], []

    def wait(self, timeout):
        self.now += timeout
        return False


class FakeSocket:
    def __init__(self):
        self.sent = bytearray()

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        return False

    def settimeout(self, _timeout):
        pass

    def sendall(self, data):
        self.sent.extend(data)

    def recv(self, _size):
        return b''


class ReadonlyInputEdgesTest(unittest.TestCase):
    def prime_runner(self, runner, ip):
        state = {
            'device_id': AUTHORIZED_TARGETS[ip], 'version': 'db113f',
            'model': AUTHORIZED_MODELS[ip], 'uptime_seconds': 500,
            'watchdog_state': 'healthy', 'wifi_connected': True,
            'mqtt_connected': True, 'socket_states': [0, 0, 0, 0],
            'requested_fan_on': False, 'requested_speed': 0,
            'child_lock': False, 'led_enabled': True,
            'brightness_requested': 30,
        }
        runner.before[ip] = {'id': AUTHORIZED_TARGETS[ip], 'state': state,
                             'boot_origin': 995}
        runner.uptime_observations[ip, 'status'] = (999.0, 999.1, 500)
        return state

    def run_recovery_with_states(self, runner, ip, states, *, repeat_last=False):
        clock = FakeClock()
        clock.now = 1000.0
        iterator = iter(states)
        last = states[-1]

        def fake_request(_ip, action, timeout):
            self.assertEqual(action, 'status.get')
            self.assertEqual(timeout, 2.0)
            try:
                state = next(iterator)
            except StopIteration:
                if not repeat_last:
                    raise AssertionError('unexpected extra recovery sample')
                state = last
            started = clock.monotonic()
            clock.now += .01
            return 200, {'code': 'ok', 'data': state}, 512, 10, started

        with patch('tests.device.readonly_input_edges.request', side_effect=fake_request), \
                patch('tests.device.readonly_stress.time.monotonic',
                      side_effect=clock.monotonic), \
                patch.object(runner.stop, 'wait', side_effect=clock.wait):
            runner.recover('mock-connectivity-recovery', seconds=30, targets=(ip,))
        return clock

    def test_allowlisted_scenarios_are_bounded_and_only_post_live_status(self):
        expected = {
            'user-agent-empty', 'user-agent-compatible',
            'user-agent-compatible-paren', 'user-agent-compatible-msie',
            'user-agent-msie',
            'user-agent-invalid-octet', 'content-type-long',
            'content-length-negative', 'content-length-overflow',
            'request-line-long', 'http-version-token-long',
            'headers-many', 'header-value-long', 'header-slow-drip',
        }
        for ip in AUTHORIZED_TARGETS:
            cases = build_scenarios(ip)
            with self.subTest(ip=ip):
                self.assertEqual({case.name for case in cases}, expected)
                for case in cases:
                    self.assertLessEqual(len(case.wire), MAX_REQUEST_BYTES)
                    self.assertTrue(case.wire.startswith(b'POST '))
                    if case.name == 'header-slow-drip':
                        self.assertNotIn(b'\r\n\r\n', case.wire)
                        self.assertNotIn(LIVE_BODY, case.wire)
                    else:
                        self.assertIn(b'\r\n\r\n', case.wire)
                        body = case.wire.split(b'\r\n\r\n', 1)[1]
                        self.assertEqual(body, LIVE_BODY)
                        envelope = json.loads(body)
                        self.assertEqual(envelope, {
                            'action': 'status.get', 'params': {'view': 'live'}})

    def test_required_parser_edge_vectors_are_present(self):
        cases = {case.name: case.wire for case in build_scenarios('10.117.152.102')}
        self.assertIn(b'User-Agent:\r\n', cases['user-agent-empty'])
        self.assertIn(b'Mozilla/5.0 compatible', cases['user-agent-compatible'])
        self.assertIn(b'Mozilla/5.0 (compatible;)', cases['user-agent-compatible-paren'])
        self.assertIn(b'(compatible; MSIE', cases['user-agent-compatible-msie'])
        self.assertIn(b'MSIE ', cases['user-agent-msie'])
        self.assertIn(b'Content-Type: ' + b'a' * 5000,
                      cases['content-type-long'])
        self.assertIn(b'Content-Length: -1\r\n', cases['content-length-negative'])
        self.assertIn(b'18446744073709551615', cases['content-length-overflow'])
        self.assertGreater(cases['headers-many'].count(b'X-Edge-'), 90)
        self.assertTrue(all(case.drip_start >= 0 for case in build_scenarios('10.117.152.102')
                            if case.name == 'header-slow-drip'))

    def test_only_exact_authorized_ip_identity_pairs_are_accepted(self):
        self.assertEqual(set(AUTHORIZED_TARGETS), {
            '10.117.152.111', '10.117.152.102', '10.117.152.110'})
        for ip, ident in AUTHORIZED_TARGETS.items():
            self.assertEqual(validate_targets([f'{ip}={ident}'], []), ({ip: ident}, {}))
        with self.assertRaises(ValueError):
            validate_targets(['10.117.152.111=wrong-id'], [])
        with self.assertRaises(ValueError):
            validate_targets(['10.117.152.101=unknown'], [])
        with self.assertRaises(ValueError):
            validate_targets(['10.117.152.111=b0f893303134',
                              '10.117.152.111=b0f893303134'], [])

    def test_version_overrides_must_match_explicit_unique_targets(self):
        ip = '10.117.152.102'
        targets, versions = validate_targets(
            [f'{ip}={AUTHORIZED_TARGETS[ip]}'], [f'{ip}=db113f'])
        self.assertEqual(targets, {ip: AUTHORIZED_TARGETS[ip]})
        self.assertEqual(versions, {ip: 'db113f'})
        with self.assertRaises(ValueError):
            validate_targets([f'{ip}={AUTHORIZED_TARGETS[ip]}'],
                             ['10.117.152.111=db113f'])

    def test_cli_is_dry_run_without_execute_flag(self):
        ip = '10.117.152.102'
        output = io.StringIO()
        with patch('tests.device.readonly_input_edges.socket.create_connection') as connect, \
                patch('tests.device.readonly_input_edges.InputRunner') as runner, \
                redirect_stdout(output):
            result = main(['--target', f'{ip}={AUTHORIZED_TARGETS[ip]}',
                           '--version', 'db113f'])
        self.assertEqual(result, 0)
        connect.assert_not_called()
        runner.assert_not_called()
        self.assertEqual(json.loads(output.getvalue())['mode'], 'dry-run')

    def test_evidence_file_is_private_and_excludes_configuration(self):
        ip = '10.117.152.102'
        with tempfile.TemporaryDirectory() as directory:
            runner = InputRunner({ip: AUTHORIZED_TARGETS[ip]}, 'db113f',
                                 Path(directory))
            runner.save()
            path = Path(directory) / 'evidence.json'
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
            evidence = json.loads(path.read_text())
            self.assertNotIn('config', evidence)
            self.assertNotIn('memory', evidence)

    def test_initialize_and_recovery_use_complete_status_state(self):
        ip = '10.117.152.102'
        state = {
            'device_id': AUTHORIZED_TARGETS[ip], 'version': 'db113f',
            'model': AUTHORIZED_MODELS[ip], 'uptime_seconds': 500,
            'watchdog_state': 'healthy',
            'model': 'tc1', 'wifi_connected': True, 'mqtt_connected': True,
            'socket_states': [0, 0, 0, 0], 'requested_fan_on': False,
            'requested_speed': 0, 'child_lock': False,
            'led_enabled': True, 'brightness_requested': 30,
        }
        response = (200, {'code': 'ok', 'data': state}, 100, 5, 1000)
        with tempfile.TemporaryDirectory() as directory:
            runner = InputRunner({ip: AUTHORIZED_TARGETS[ip]}, 'db113f',
                                 Path(directory))
            with patch('tests.device.readonly_input_edges.request',
                       return_value=response) as request_mock:
                runner.initialize()
                request_mock.assert_called_once_with(
                    ip, 'status.get', timeout=2.0)
                request_mock.reset_mock()
                row = runner.sample(ip, 'recovery')
                request_mock.assert_called_once_with(
                    ip, 'status.get', timeout=2.0)
            self.assertTrue(row['ok'])
            self.assertTrue(runner.before[ip]['state']['wifi_connected'])
            self.assertEqual(row['model'], AUTHORIZED_MODELS[ip])

    def test_malformed_success_is_blocking_even_if_later_recovery_succeeds(self):
        ip = '10.117.152.102'
        with tempfile.TemporaryDirectory() as directory:
            runner = InputRunner({ip: AUTHORIZED_TARGETS[ip]}, 'db113f',
                                 Path(directory))
            state = self.prime_runner(runner, ip)
            healthy = (200, {'code': 'ok', 'data': state}, 100, 5, 1000)
            with patch('tests.device.readonly_input_edges.request',
                       side_effect=[ProtocolError(200, b'{'), healthy]):
                malformed = runner.sample(ip, 'malformed')
                recovered = runner.sample(ip, 'later-recovery')
            self.assertFalse(malformed['ok'])
            self.assertEqual(malformed['http'], 200)
            self.assertFalse(recovered.get('error'))
            self.assertTrue(recovered['network_ready'])
            self.assertFalse(recovered['ok'])
            self.assertTrue(runner.stop.is_set())
            self.assertTrue(any('malformed successful HTTP response' in item['reason']
                                for item in runner.blockers))

    def test_sample_classifies_success_busy_503_and_transport_degradation(self):
        ip = '10.117.152.102'
        with tempfile.TemporaryDirectory() as directory:
            runner = InputRunner({ip: AUTHORIZED_TARGETS[ip]}, 'db113f',
                                 Path(directory))
            state = self.prime_runner(runner, ip)
            good = (200, {'code': 'ok', 'data': state}, 100, 5, 1000)
            busy = (200, {'code': 'busy'}, 4, 2, 1000)
            overloaded = (503, {}, 0, 2, 1000)
            with patch('tests.device.readonly_input_edges.request',
                       side_effect=[good, busy, overloaded, OSError('reset')]):
                success = runner.sample(ip, 'normal')
                busy_row = runner.sample(ip, 'busy')
                overload_row = runner.sample(ip, 'overload')
                transport_row = runner.sample(ip, 'transport')
            self.assertTrue(success['ok'])
            self.assertFalse(busy_row['ok'])
            self.assertEqual(busy_row['degraded'], 'busy')
            self.assertFalse(overload_row['ok'])
            self.assertEqual(overload_row['degraded'], 'http_503')
            self.assertEqual(transport_row['error'], 'OSError')
            self.assertFalse(runner.blockers)
            self.assertFalse(runner.stop.is_set())

    def test_transient_disconnect_recovers_only_after_three_consecutive_ready_samples(self):
        ip = '10.117.152.102'
        with tempfile.TemporaryDirectory() as directory:
            runner = InputRunner({ip: AUTHORIZED_TARGETS[ip]}, 'db113f',
                                 Path(directory))
            baseline = self.prime_runner(runner, ip)
            states = []
            for connected in (False, True, False, True, True, True):
                state = dict(baseline, uptime_seconds=501,
                             wifi_connected=True, mqtt_connected=connected,
                             errors=2, last_error='mqtt reconnect pending')
                states.append(state)
            self.run_recovery_with_states(runner, ip, states)

            recovery = runner.recoveries[-1]
            self.assertTrue(recovery['completed'])
            self.assertEqual(recovery['targets'][ip]['attempts'], 6)
            self.assertEqual(recovery['targets'][ip]['consecutive_successes'], 3)
            rejected = [row for row in runner.rows if row.get('degraded') == 'network_not_ready']
            self.assertEqual(len(rejected), 2)
            self.assertTrue(all(row['ok'] is False and row['network_ready'] is False
                                for row in rejected))
            self.assertTrue(all(row['mqtt_connected'] is False and row['errors'] == 2 and
                                row['last_error'] == 'mqtt reconnect pending'
                                for row in rejected))
            self.assertTrue(all(row['uptime'] == row['uptime_seconds']
                                for row in rejected))
            saved = json.loads((Path(directory) / 'evidence.json').read_text())
            self.assertEqual(saved['recoveries'], runner.recoveries)
            self.assertEqual(len(saved['status_checks']), 6)

    def test_persistent_disconnect_exhausts_existing_recovery_window(self):
        ip = '10.117.152.102'
        with tempfile.TemporaryDirectory() as directory:
            runner = InputRunner({ip: AUTHORIZED_TARGETS[ip]}, 'db113f',
                                 Path(directory))
            baseline = self.prime_runner(runner, ip)
            disconnected = dict(baseline, uptime_seconds=501,
                                wifi_connected=True, mqtt_connected=False)
            self.run_recovery_with_states(runner, ip, [disconnected], repeat_last=True)

            recovery = runner.recoveries[-1]
            self.assertFalse(recovery['completed'])
            self.assertEqual(recovery['targets'][ip]['attempts'], 30)
            self.assertEqual(recovery['targets'][ip]['consecutive_successes'], 0)
            self.assertTrue(runner.stop.is_set())
            self.assertTrue(any('no sustained recovery' in item['reason']
                                for item in runner.blockers))

    def test_control_change_and_uptime_reset_block_recovery_immediately(self):
        ip = '10.117.152.102'
        cases = (
            ('control', {'child_lock': True}, 'control state changed: child_lock'),
            ('uptime', {'uptime_seconds': 1}, 'uptime decreased'),
        )
        for name, mutation, reason in cases:
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                runner = InputRunner({ip: AUTHORIZED_TARGETS[ip]}, 'db113f',
                                     Path(directory))
                baseline = self.prime_runner(runner, ip)
                state = dict(baseline)
                state['uptime_seconds'] = 501
                state.update(mutation)
                self.run_recovery_with_states(runner, ip, [state])
                self.assertTrue(runner.stop.is_set())
                self.assertEqual(runner.recoveries[-1]['targets'][ip]['attempts'], 1)
                self.assertTrue(any(reason in item['reason'] for item in runner.blockers))

    def test_missing_connectivity_field_is_protocol_blocker(self):
        ip = '10.117.152.102'
        with tempfile.TemporaryDirectory() as directory:
            runner = InputRunner({ip: AUTHORIZED_TARGETS[ip]}, 'db113f',
                                 Path(directory))
            state = self.prime_runner(runner, ip)
            del state['mqtt_connected']
            with patch('tests.device.readonly_input_edges.request',
                       return_value=(200, {'code': 'ok', 'data': state}, 100, 5, 1000)):
                row = runner.sample(ip, 'recovery')
            self.assertFalse(row['ok'])
            self.assertEqual(row.get('degraded'), None)
            self.assertTrue(runner.stop.is_set())
            self.assertTrue(any('connectivity field missing or invalid: mqtt_connected'
                                in item['reason'] for item in runner.blockers))

    def test_shared_http_runner_save_preserves_parent_recovery_records(self):
        ip = '10.117.152.102'
        with tempfile.TemporaryDirectory() as directory:
            runner = SharedHTTPRunner({ip: AUTHORIZED_TARGETS[ip]}, 'db113f',
                                      Path(directory))
            runner.recoveries.append({'name': 'fixture', 'completed': True,
                                      'targets': {ip: {'completed': True}}})
            runner.save()
            saved = json.loads((Path(directory) / 'shared-httpd-evidence.json').read_text())
            self.assertEqual(saved['recoveries'], runner.recoveries)

    def test_unexpected_http_and_command_results_are_blocking(self):
        ip = '10.117.152.102'
        responses = [
            (401, {}, 0, 2, 1000),
            (500, {}, 0, 2, 1000),
            (200, {'code': 'error'}, 10, 2, 1000),
        ]
        for response in responses:
            with self.subTest(response=response[0:2]), \
                    tempfile.TemporaryDirectory() as directory:
                runner = InputRunner({ip: AUTHORIZED_TARGETS[ip]}, 'db113f',
                                     Path(directory))
                self.prime_runner(runner, ip)
                with patch('tests.device.readonly_input_edges.request',
                           return_value=response):
                    row = runner.sample(ip, 'unexpected')
                self.assertFalse(row['ok'])
                self.assertTrue(runner.stop.is_set())
                self.assertEqual(len(runner.blockers), 1)

    def test_slow_drip_requires_server_close_and_never_finishes_request(self):
        ip = '10.117.152.102'
        scenario = next(case for case in build_scenarios(ip)
                        if case.name == 'header-slow-drip')

        for close_at, expected_closed in ((5.5, True), (None, False)):
            with self.subTest(close_at=close_at):
                clock = FakeClock(close_at)
                fake_socket = FakeSocket()
                with patch('tests.device.readonly_input_edges.time.monotonic',
                           side_effect=clock.monotonic), \
                        patch('tests.device.readonly_input_edges.select.select',
                              side_effect=clock.select), \
                        patch('tests.device.readonly_input_edges.socket.create_connection',
                              return_value=fake_socket):
                    result = send_scenario(ip, scenario)
                self.assertEqual(result['closed'], expected_closed)
                self.assertNotIn(b'\r\n\r\n', fake_socket.sent)
                self.assertNotIn(LIVE_BODY, fake_socket.sent)
                if expected_closed:
                    self.assertLessEqual(result['close_seconds'], 12.0)
                else:
                    self.assertGreaterEqual(result['sent_seconds'], 10.0)
                    self.assertIsNone(result['close_seconds'])

    def test_slow_drip_deadline_stops_the_run(self):
        ip = '10.117.152.102'
        scenario = next(case for case in build_scenarios(ip)
                        if case.name == 'header-slow-drip')
        with tempfile.TemporaryDirectory() as directory:
            runner = InputRunner({ip: AUTHORIZED_TARGETS[ip]}, 'db113f',
                                 Path(directory))
            with patch('tests.device.readonly_input_edges.build_scenarios',
                       return_value=(scenario,)), \
                    patch('tests.device.readonly_input_edges.send_scenario',
                          return_value={'name': scenario.name, 'closed': False,
                                        'sent_seconds': 10.5,
                                        'close_seconds': None}), \
                    redirect_stdout(io.StringIO()):
                with self.assertRaises(SystemExit):
                    runner.run_scenarios()
            self.assertTrue(runner.stop.is_set())
            self.assertTrue(any('did not close' in item['reason']
                                for item in runner.blockers))


if __name__ == '__main__':
    unittest.main()
