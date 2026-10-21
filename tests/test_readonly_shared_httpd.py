"""Offline contract tests for the shared HTTP parser acceptance harness."""
import json
import io
import socket
import time
import unittest
from contextlib import redirect_stdout
from unittest.mock import patch

from tests.device.readonly_shared_httpd import (
    CHUNKED_BODY,
    LIVE_BODY,
    MAX_REQUEST_BYTES,
    MAX_RESPONSE_BYTES,
    AUTHORIZED_TARGETS,
    build_scenarios,
    evaluate_scenario,
    main,
    parse_response_facts,
    receive_response,
    validate_scenarios,
)


class ReadonlySharedHTTPDTest(unittest.TestCase):
    def test_manifest_contains_only_bounded_status_get_requests(self):
        expected = {
            'content-length-duplicate-equal', 'content-length-duplicate-different',
            'transfer-encoding-chunked', 'transfer-encoding-leading-ows',
            'transfer-encoding-trailing-ows', 'content-length-plus-transfer-encoding',
            'expect-100-continue', 'request-line-bare-lf', 'header-line-bare-lf',
            'header-embedded-nul', 'header-embedded-control', 'header-cr-garbage',
            'http-version-short', 'legal-tab-header', 'status-normal',
        }
        for ip in ('10.117.152.111', '10.117.152.102', '10.117.152.110'):
            cases = validate_scenarios(build_scenarios(ip))
            with self.subTest(ip=ip):
                self.assertEqual({case.name for case in cases}, expected)
                for case in cases:
                    self.assertLessEqual(len(case.wire), MAX_REQUEST_BYTES)
                    self.assertEqual(json.loads(case.entity_body), {
                        'action': 'status.get', 'params': {'view': 'live'}})
                    encoded_body = (CHUNKED_BODY if case.name in {
                        'transfer-encoding-chunked', 'transfer-encoding-leading-ows',
                        'transfer-encoding-trailing-ows',
                        'content-length-plus-transfer-encoding'} else LIVE_BODY)
                    self.assertTrue(case.wire.endswith(encoded_body))

    def test_exact_target_allowlist(self):
        self.assertEqual(set(AUTHORIZED_TARGETS), {
            '10.117.152.111', '10.117.152.102', '10.117.152.110'})

    def test_bad_wire_vectors_contain_real_control_bytes(self):
        cases = {case.name: case.wire for case in build_scenarios('10.117.152.102')}
        self.assertTrue(cases['request-line-bare-lf'].startswith(
            b'POST /api/v1/command HTTP/1.1\nHost:'))
        self.assertIn(b'X-Bare-LF: value\nContent-Length:',
                      cases['header-line-bare-lf'])
        self.assertIn(b'\x00', cases['header-embedded-nul'])
        self.assertIn(b'\x01', cases['header-embedded-control'])
        self.assertIn(b'value\rgarbage: x', cases['header-cr-garbage'])
        self.assertIn(b'HTTP/1\r\n', cases['http-version-short'])

    def test_chunked_cases_frame_only_the_live_status_entity(self):
        cases = {case.name: case for case in build_scenarios('10.117.152.102')}
        for name in ('transfer-encoding-chunked', 'transfer-encoding-leading-ows',
                     'transfer-encoding-trailing-ows',
                     'content-length-plus-transfer-encoding'):
            case = cases[name]
            self.assertTrue(case.wire.endswith(CHUNKED_BODY))
            self.assertEqual(CHUNKED_BODY.split(b'\r\n', 1)[1].split(b'\r\n0\r\n', 1)[0],
                             LIVE_BODY)

    def test_response_parser_honors_content_length_and_interim_100(self):
        body = b'{"code":"ok","data":{}}'
        raw = (b'HTTP/1.1 100 Continue\r\n\r\n'
               b'HTTP/1.1 200 OK\r\nContent-Length: ' + str(len(body)).encode() +
               b'\r\nConnection: close\r\n\r\n' + body + b'extra')
        facts = parse_response_facts(raw, closed=True)
        self.assertEqual(facts['interim_statuses'], [100])
        self.assertEqual(facts['status'], 200)
        self.assertEqual(facts['body'], body)
        self.assertTrue(facts['complete'])
        self.assertEqual(facts['content_length'], len(body))

    def test_response_parser_marks_truncated_content_length_incomplete(self):
        raw = b'HTTP/1.1 400 Bad Request\r\nContent-Length: 5\r\n\r\nno'
        facts = parse_response_facts(raw, closed=True)
        self.assertEqual(facts['status'], 400)
        self.assertFalse(facts['complete'])
        self.assertLessEqual(len(facts['raw']), MAX_RESPONSE_BYTES)

    def test_response_parser_rejects_invalid_or_ambiguous_content_length(self):
        for value in (b'-1', b'2, 2'):
            with self.subTest(value=value):
                raw = (b'HTTP/1.1 400 Bad Request\r\nContent-Length: ' + value +
                       b'\r\n\r\n')
                self.assertEqual(parse_response_facts(raw)['error'],
                                 'invalid_content_length')

    def test_acceptance_requires_4xx_or_response_less_close_for_malformed_cases(self):
        cases = {case.name: case for case in build_scenarios('10.117.152.102')}
        malformed = cases['content-length-duplicate-equal']
        self.assertTrue(evaluate_scenario(malformed, {
            'status': 400, 'complete': True, 'closed': True,
        })[0])
        self.assertFalse(evaluate_scenario(malformed, {
            'status': 400, 'complete': True, 'closed': False,
        })[0])
        self.assertTrue(evaluate_scenario(malformed, {
            'status': None, 'closed': True,
        })[0])
        self.assertFalse(evaluate_scenario(malformed, {
            'status': 200, 'complete': True, 'closed': True,
        })[0])
        self.assertFalse(evaluate_scenario(malformed, {
            'status': 500, 'complete': True, 'closed': True,
        })[0])
        self.assertFalse(evaluate_scenario(malformed, {
            'status': None, 'closed': True, 'response_bytes': 16,
        })[0])

    def test_complete_response_must_also_release_socket(self):
        raw = b'HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n'
        for peer_closes in (False, True):
            with self.subTest(peer_closes=peer_closes):
                reader, writer = socket.socketpair()
                try:
                    writer.sendall(raw)
                    if peer_closes:
                        writer.shutdown(socket.SHUT_WR)
                    result = receive_response(reader, time.monotonic() + .05)
                    self.assertTrue(result['complete'])
                    self.assertEqual(result['closed'], peer_closes)
                    self.assertEqual(result['timed_out'], not peer_closes)
                finally:
                    reader.close()
                    writer.close()

    def test_positive_case_requires_complete_content_length_status_envelope(self):
        scenario = next(case for case in build_scenarios('10.117.152.102')
                        if case.name == 'status-normal')
        body = b'{"code":"ok","data":{}}'
        result = {'status': 200, 'content_length': len(body), 'complete': True,
                  'body': body, 'closed': True}
        self.assertTrue(evaluate_scenario(scenario, result)[0])
        self.assertFalse(evaluate_scenario(scenario, {**result, 'content_length': None})[0])
        self.assertFalse(evaluate_scenario(scenario, {**result, 'complete': False})[0])
        self.assertFalse(evaluate_scenario(scenario, {**result, 'status': 204})[0])

    def test_cli_defaults_to_offline_manifest(self):
        ip = '10.117.152.102'
        output = io.StringIO()
        with patch('tests.device.readonly_shared_httpd.socket.create_connection') as connect, \
                patch('tests.device.readonly_shared_httpd.SharedHTTPRunner') as runner, \
                redirect_stdout(output):
            status = main(['--target', f'{ip}={AUTHORIZED_TARGETS[ip]}',
                           '--version', 'db113f'])
        self.assertEqual(status, 0)
        connect.assert_not_called()
        runner.assert_not_called()
        self.assertEqual(json.loads(output.getvalue())['mode'], 'dry-run')

    def test_parser_failure_still_runs_three_read_recoveries_before_stop(self):
        from pathlib import Path
        import tempfile
        from unittest.mock import patch

        from tests.device.readonly_shared_httpd import SharedHTTPRunner

        ip = '10.117.152.102'
        scenario = next(case for case in build_scenarios(ip)
                        if case.name == 'content-length-duplicate-equal')
        with tempfile.TemporaryDirectory() as directory:
            runner = SharedHTTPRunner({ip: 'D0BAE463E12C'}, 'db113f', Path(directory))
            with patch('tests.device.readonly_shared_httpd.send_scenario',
                          return_value={'name': scenario.name, 'status': 200,
                                        'complete': True, 'closed': True,
                                        'body': b'{"code":"ok","data":{}}',
                                        'content_length': 23}), \
                    patch.object(runner, 'recover') as recover, \
                    patch('builtins.print'), \
                    self.assertRaises(SystemExit):
                runner.run_scenarios()
            recover.assert_called_once_with(
                'content-length-duplicate-equal-recovery', seconds=30)
            self.assertTrue(runner.stop.is_set())


if __name__ == '__main__':
    unittest.main()
