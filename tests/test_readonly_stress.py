"""Keep corrupt successful replies distinct from recoverable overload."""
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
                    self.assertFalse(runner.sample('127.0.0.1', 'probe', 'logs.get')['ok'])
                self.assertEqual(runner.stop.is_set(), blocked)
