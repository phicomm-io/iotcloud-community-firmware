"""Offline contract tests for the bounded HTTP scheduling comparison."""
import io
import json
import socket
import threading
import time
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[1]
import sys
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

from tests.device.readonly_http_scheduling import (
    AUTHORIZED_TARGETS,
    CASES,
    LIVE_BODY,
    MAX_REQUEST_BYTES,
    QUIET_GAP_SECONDS,
    SLOW_SETTLE_SECONDS,
    aggregate_probes,
    build_probe_request,
    build_slow_header,
    classify_response,
    main,
    observe_slow_socket,
    parse_probe_response,
    validate_targets,
)


class ReadonlyHTTPSchedulingTest(unittest.TestCase):
    def test_exact_authorized_target_set_and_identity_validation(self):
        self.assertEqual(set(AUTHORIZED_TARGETS), {
            '10.117.152.111', '10.117.152.102', '10.117.152.110'})
        for ip, ident in AUTHORIZED_TARGETS.items():
            self.assertEqual(validate_targets([f'{ip}={ident}'], []),
                             ({ip: ident}, {}))
        with self.assertRaises(ValueError):
            validate_targets(['10.117.152.101=unknown'], [])

    def test_manifest_and_request_bodies_are_fixed_read_only_status_queries(self):
        self.assertEqual(CASES, ('baseline', 'incomplete-status-header'))
        for ip in AUTHORIZED_TARGETS:
            probe = build_probe_request(ip)
            slow = build_slow_header(ip)
            self.assertLessEqual(len(probe), MAX_REQUEST_BYTES)
            self.assertLessEqual(len(slow), MAX_REQUEST_BYTES)
            self.assertIn(b'POST /api/v1/command HTTP/1.1\r\n', probe)
            self.assertEqual(probe.count(LIVE_BODY), 1)
            self.assertIn(b'GET /api/v1/status HTTP/1.1\r\n', slow)
            self.assertFalse(slow.endswith(b'\r\n\r\n'))
            self.assertNotIn(b'\r\n\r\n', slow)

    def test_raw_http_status_503_is_distinct_from_transport_failure(self):
        self.assertEqual(classify_response(503, b'', True)['category'], 'http_503')
        self.assertEqual(classify_response(None, b'', False)['category'], 'no_http')
        self.assertEqual(classify_response(429, b'', True)['category'], 'http_other')

    def test_http_parser_honors_content_length_and_rejects_incomplete_envelope(self):
        body = b'{"code":"ok","data":{"uptime_seconds":7}}'
        raw = (b'HTTP/1.1 200 OK\r\nContent-Length: ' + str(len(body)).encode() +
               b'\r\nConnection: close\r\n\r\n' + body + b'extra')
        parsed = parse_probe_response(raw, closed=True)
        self.assertEqual(parsed['status'], 200)
        self.assertTrue(parsed['complete'])
        self.assertEqual(parsed['body'], body)
        self.assertEqual(classify_response(200, body, True)['category'], 'success')
        body_start = raw.find(b'\r\n\r\n') + 4
        short = parse_probe_response(raw[:body_start + len(body) - 5], closed=True)
        self.assertFalse(short['complete'])

    def test_aggregation_keeps_503_and_no_http_as_separate_degradation(self):
        result = aggregate_probes([
            {'category': 'success', 'elapsed_ms': 10.0},
            {'category': 'http_503', 'elapsed_ms': 20.0},
            {'category': 'no_http', 'elapsed_ms': 30.0},
        ])
        self.assertEqual(result['success_rate'], 1 / 3)
        self.assertEqual(result['http_503'], 1)
        self.assertEqual(result['no_http'], 1)
        self.assertEqual(result['all_outcomes_p50_ms'], 20.0)
        self.assertEqual(result['latency_by_category']['success']['p50_ms'], 10.0)
        self.assertEqual(result['latency_by_category']['http_503']['p50_ms'], 20.0)
        self.assertEqual(result['latency_by_category']['no_http']['p50_ms'], 30.0)

    def test_cli_is_dry_run_without_execute_and_never_opens_socket(self):
        ip = '10.117.152.102'
        output = io.StringIO()
        with patch('tests.device.readonly_http_scheduling.socket.create_connection') as connect, \
                patch('tests.device.readonly_http_scheduling.SchedulingRunner') as runner, \
                redirect_stdout(output):
            self.assertEqual(main(['--target', f'{ip}={AUTHORIZED_TARGETS[ip]}',
                                   '--version', 'candidate-build', '--rounds', '3']), 0)
        connect.assert_not_called()
        runner.assert_not_called()
        manifest = json.loads(output.getvalue())
        self.assertEqual(manifest['mode'], 'dry-run')
        self.assertEqual(manifest['rounds'], 3)
        self.assertEqual(manifest['quiet_gap_seconds'], QUIET_GAP_SECONDS)

    def test_runner_executes_baseline_quiet_slow_recovery_quiet_per_round(self):
        import tempfile

        from tests.device.readonly_http_scheduling import SchedulingRunner

        ip = '10.117.152.102'
        with tempfile.TemporaryDirectory() as directory:
            runner = SchedulingRunner({ip: AUTHORIZED_TARGETS[ip]},
                                      'candidate-build', Path(directory) / 'out',
                                      rounds=2)
            runner.before[ip] = {'state': {'model': 'tc1'}}
            events = []
            with patch('tests.device.readonly_http_scheduling.run_case',
                       side_effect=lambda _ip, _runner, phase, **kwargs:
                       events.append((phase, kwargs.get('round_number')))), \
                    patch('tests.device.readonly_http_scheduling.recover_three_status_reads',
                          side_effect=lambda _ip, _runner, label:
                          events.append(('recovery', label)) or {'recovered': True}), \
                    patch('tests.device.readonly_http_scheduling.time.sleep',
                          side_effect=lambda seconds: events.append(('quiet', seconds))), \
                    patch.object(runner, 'save'), \
                    patch.object(runner, 'save_scheduling'):
                runner.run_comparison()
        self.assertEqual(events, [
            ('baseline', 1), ('quiet', QUIET_GAP_SECONDS),
            ('incomplete-status-header', 1),
            ('recovery', 'round-1-incomplete-status-header-recovery'),
            ('quiet', QUIET_GAP_SECONDS),
            ('baseline', 2), ('quiet', QUIET_GAP_SECONDS),
            ('incomplete-status-header', 2),
            ('recovery', 'round-2-incomplete-status-header-recovery'),
            ('quiet', QUIET_GAP_SECONDS),
        ])

    def test_execute_refuses_existing_output_before_any_network_or_runner(self):
        import tempfile

        ip = '10.117.152.102'
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'existing'
            output.mkdir()
            with patch('tests.device.readonly_http_scheduling.socket.create_connection') as connect, \
                    patch('tests.device.readonly_http_scheduling.SchedulingRunner') as runner, \
                    self.assertRaises(SystemExit):
                main(['--target', f'{ip}={AUTHORIZED_TARGETS[ip]}',
                      '--version', 'candidate-build', '--execute',
                      '--output', str(output)])
        connect.assert_not_called()
        runner.assert_not_called()

    def test_incomplete_slow_header_is_not_finished_by_builder(self):
        request = build_slow_header('10.117.152.102')
        self.assertTrue(request.endswith(b'X-Scheduling-Hold:'))
        self.assertNotIn(b'\r\n\r\n', request)

    def test_slow_socket_peek_identifies_early_503_and_empty_close(self):
        client, peer = socket.socketpair()
        try:
            peer.sendall(b'HTTP/1.1 503 Busy\r\nContent-Length: 0\r\n\r\n')
            observed = observe_slow_socket(client, .05)
            self.assertEqual(observed['state'], 'early_response')
            self.assertEqual(observed['http_status'], 503)
            self.assertGreater(observed['response_bytes'], 0)
            self.assertTrue(client.recv(8, socket.MSG_PEEK).startswith(b'HTTP/1.'))
        finally:
            client.close()
            peer.close()

        client, peer = socket.socketpair()
        try:
            peer.shutdown(socket.SHUT_WR)
            observed = observe_slow_socket(client, .05)
            self.assertEqual(observed['state'], 'closed_empty')
            self.assertIsNone(observed['http_status'])
        finally:
            client.close()
            peer.close()

    def test_slow_socket_without_response_is_only_pending(self):
        client, peer = socket.socketpair()
        try:
            observed = observe_slow_socket(client, .001)
            self.assertEqual(observed['state'], 'pending_no_response')
            self.assertIsNone(observed['http_status'])
        finally:
            client.close()
            peer.close()

    def test_early_rejection_marks_not_exercised_and_skips_probes(self):
        from tests.device.readonly_http_scheduling import run_case

        class FakeRunner:
            def __init__(self):
                self.stop = threading.Event()
                self.scheduling_cases = []
                self.blockers = []

            def block(self, ip, reason):
                self.blockers.append(reason)

        client, peer = socket.socketpair()
        peer.sendall(b'HTTP/1.1 503 Busy\r\nContent-Length: 0\r\n\r\n')
        runner = FakeRunner()
        try:
            with patch('tests.device.readonly_http_scheduling.socket.create_connection',
                       return_value=client), \
                    patch('tests.device.readonly_http_scheduling.time.sleep'), \
                    patch('tests.device.readonly_http_scheduling.run_probe_batch') as batch:
                result = run_case('10.117.152.102', runner,
                                  'incomplete-status-header', slow_connection=True,
                                  round_number=2)
            batch.assert_not_called()
            self.assertFalse(result['slow_admitted'])
            self.assertEqual(result['case_validity'], 'NOT_EXERCISED')
            self.assertEqual(result['round'], 2)
            self.assertEqual(result['probes'], [])
            self.assertEqual(result['slow_socket_observations'][0]['http_status'], 503)
        finally:
            peer.close()

    def test_probe_batch_never_exceeds_two_concurrent_clients(self):
        from tests.device.readonly_http_scheduling import run_probe_batch

        lock = threading.Lock()
        active = 0
        maximum = 0

        def fake_probe(ip, runner, deadline, index):
            nonlocal active, maximum
            with lock:
                active += 1
                maximum = max(maximum, active)
            time.sleep(.02)
            with lock:
                active -= 1
            return {'index': index, 'category': 'success', 'elapsed_ms': 20.0}

        with patch('tests.device.readonly_http_scheduling.probe_once', side_effect=fake_probe):
            rows = run_probe_batch('10.117.152.102', object(), 'baseline',
                                   time.monotonic() + 2)
        self.assertEqual(len(rows), 6)
        self.assertEqual(maximum, 2)
        self.assertTrue(all(row['phase'] == 'baseline' for row in rows))

    def test_slow_socket_is_closed_when_probe_batch_fails(self):
        from tests.device.readonly_http_scheduling import run_case

        class FakeRunner:
            def __init__(self):
                self.stop = threading.Event()
                self.scheduling_cases = []
                self.blockers = []

            def block(self, ip, reason):
                self.blockers.append(reason)

        sock, peer = socket.socketpair()
        runner = FakeRunner()
        try:
            with patch('tests.device.readonly_http_scheduling.socket.create_connection',
                       return_value=sock), \
                    patch('tests.device.readonly_http_scheduling.run_probe_batch',
                          side_effect=OSError('offline test')):
                result = run_case('10.117.152.102', runner,
                                  'incomplete-status-header', slow_connection=True)
            self.assertTrue(sock.fileno() == -1)
        finally:
            peer.close()
        self.assertEqual(result['error'], 'OSError')
        self.assertTrue(runner.blockers)

    def test_slow_socket_is_closed_when_initial_send_fails(self):
        from tests.device.readonly_http_scheduling import run_case

        class FakeRunner:
            def __init__(self):
                self.stop = threading.Event()
                self.scheduling_cases = []
                self.blockers = []

            def block(self, ip, reason):
                self.blockers.append(reason)

        sock = unittest.mock.MagicMock()
        sock.sendall.side_effect = OSError('offline test')
        runner = FakeRunner()
        with patch('tests.device.readonly_http_scheduling.socket.create_connection',
                   return_value=sock), \
                patch('tests.device.readonly_http_scheduling.run_probe_batch') as batch:
            result = run_case('10.117.152.102', runner,
                              'incomplete-status-header', slow_connection=True)
        sock.close.assert_called_once()
        batch.assert_not_called()
        self.assertEqual(result['slow_hold_seconds'], 0.0)
        self.assertEqual(result['error'], 'OSError')

    def test_slow_header_settles_briefly_before_probe_batch(self):
        from tests.device.readonly_http_scheduling import run_case

        class FakeRunner:
            def __init__(self):
                self.stop = threading.Event()
                self.scheduling_cases = []
                self.blockers = []

            def block(self, ip, reason):
                self.blockers.append(reason)

        sock, peer = socket.socketpair()
        runner = FakeRunner()
        events = []
        try:
            with patch('tests.device.readonly_http_scheduling.socket.create_connection',
                       return_value=sock), \
                    patch('tests.device.readonly_http_scheduling.run_probe_batch',
                          side_effect=lambda *args: events.append('probe') or []), \
                    patch('tests.device.readonly_http_scheduling.time.sleep',
                          side_effect=lambda _seconds: events.append('settle')) as sleep:
                result = run_case('10.117.152.102', runner,
                                  'incomplete-status-header', slow_connection=True)
            sleep.assert_called_once_with(SLOW_SETTLE_SECONDS)
            self.assertTrue(sock.fileno() == -1)
        finally:
            peer.close()
        self.assertEqual(events, ['settle', 'probe'])
        self.assertGreaterEqual(result['slow_settle_seconds'], 0.0)
        self.assertGreaterEqual(result['slow_hold_seconds'], 0.0)

    def test_transport_reset_is_measured_but_not_mistaken_for_device_reset(self):
        from tests.device.readonly_http_scheduling import probe_once

        class FakeRunner:
            def __init__(self):
                self.stop = threading.Event()
                self.blockers = []

            def block(self, ip, reason):
                self.blockers.append(reason)

        runner = FakeRunner()
        with patch('tests.device.readonly_http_scheduling.socket.create_connection',
                   side_effect=ConnectionResetError('offline reset')):
            row = probe_once('10.117.152.102', runner,
                             time.monotonic() + 5, 0)
        self.assertEqual(row['category'], 'no_http')
        self.assertEqual(row['error'], 'ConnectionResetError')
        self.assertFalse(runner.blockers)

    def test_probe_unexpected_http_blocks_but_503_and_transport_failure_degrade(self):
        from tests.device.readonly_http_scheduling import probe_once

        class FakeRunner:
            def __init__(self):
                self.stop = threading.Event()
                self.blockers = []

            def block(self, ip, reason):
                self.blockers.append(reason)

            def check(self, *_args):
                pass

        for status, category, should_block in (
                (401, 'http_other', True), (503, 'http_503', False)):
            with self.subTest(status=status):
                sock = unittest.mock.MagicMock()
                runner = FakeRunner()
                facts = {'status': status, 'body': b'', 'complete': True}
                with patch('tests.device.readonly_http_scheduling.socket.create_connection',
                           return_value=sock), \
                        patch('tests.device.readonly_http_scheduling._read_response',
                              return_value=(facts, 0)):
                    row = probe_once('10.117.152.102', runner,
                                     time.monotonic() + 5, 0)
                self.assertEqual(row['category'], category)
                self.assertEqual(bool(runner.blockers), should_block)

    def test_run_case_blocks_http_other_from_probe_batch(self):
        from tests.device.readonly_http_scheduling import run_case

        class FakeRunner:
            def __init__(self):
                self.stop = threading.Event()
                self.scheduling_cases = []
                self.blockers = []

            def block(self, ip, reason):
                self.blockers.append(reason)

        runner = FakeRunner()
        with patch('tests.device.readonly_http_scheduling.run_probe_batch',
                   return_value=[{'category': 'http_other', 'elapsed_ms': 8.0}]):
            result = run_case('10.117.152.102', runner, 'baseline')
        self.assertEqual(result['summary']['http_other'], 1)
        self.assertTrue(any('unexpected HTTP status' in reason
                            for reason in runner.blockers))


if __name__ == '__main__':
    unittest.main()
