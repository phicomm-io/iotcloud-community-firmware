"""Startup protocol proof and bounded socket lifetime; no Docker or broker needed."""
import socket
import unittest
from unittest.mock import patch
from tests import mqtt_broker_ready as ready


class Connection:
    def __init__(self, parts):
        self.parts = iter(parts)
        self.closed = False
        self.sent = []
        self.timeouts = []

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.closed = True

    def settimeout(self, seconds):
        self.timeouts.append(seconds)

    def sendall(self, data):
        self.sent.append(data)

    def recv(self, length):
        value = next(self.parts)
        if isinstance(value, BaseException):
            raise value
        assert len(value) <= length
        return value


class BrokerReady(unittest.TestCase):
    def test_fragmented_connack_and_clean_disconnect(self):
        c = Connection([b'\x20', b'\x02\x00', b'\x00'])
        with patch.object(socket, 'create_connection', return_value=c) as connect:
            ready.probe(1234)
        self.assertTrue(c.closed)
        self.assertEqual(connect.call_args.args[0], ('127.0.0.1', 1234))
        self.assertEqual(c.sent[-1], b'\xe0\x00')
        self.assertEqual(c.sent[0][:2], bytes([0x10, len(c.sent[0]) - 2]))
        self.assertEqual(c.sent[0][2:12], b'\x00\x04MQTT\x04\x02\x00\x0a')
        self.assertEqual(len(c.sent), 2)  # No SUBSCRIBE/PUBLISH.

    def test_invalid_or_rejected_connack_fails_without_retry(self):
        for reply in (b'HTTP', b'\x20\x02\x01\x00', b'\x20\x02\x00\x06',
                      b'\x20\x02\x00\x05'):
            with self.subTest(reply=reply):
                c = Connection([reply])
                with patch.object(socket, 'create_connection', return_value=c) as connect:
                    with self.assertRaises(AssertionError):
                        ready.wait_for_broker(1234)
                self.assertEqual(connect.call_count, 1)
                self.assertTrue(c.closed)
                self.assertEqual(len(c.sent), 1)

    def test_proxy_close_is_startup_only_retry_and_every_socket_closes(self):
        connections = [Connection([b'']), Connection([ConnectionResetError()]),
                       Connection([b'\x20\x02\x00\x00'])]
        with patch.object(socket, 'create_connection', side_effect=connections), \
             patch.object(ready.time, 'sleep') as sleep:
            ready.wait_for_broker(1234, attempts=3)
        self.assertEqual(sleep.call_args_list[0].args, (.1,))
        self.assertEqual(sleep.call_count, 2)
        self.assertTrue(all(c.closed for c in connections))

    def test_exhaustion_is_failure_not_ready(self):
        c = Connection([socket.timeout()])
        with patch.object(socket, 'create_connection', return_value=c) as connect, \
             patch.object(ready.time, 'sleep') as sleep:
            with self.assertRaises(RuntimeError):
                ready.wait_for_broker(1234, attempts=1)
        self.assertTrue(c.closed)
        self.assertEqual(connect.call_count, 1)
        sleep.assert_not_called()

    def test_absolute_deadline_not_reset_by_fragmented_reads(self):
        c = Connection([b'\x20'])
        # Start, connect, send, first recv, then already past the single deadline.
        with patch.object(socket, 'create_connection', return_value=c), \
             patch.object(ready.time, 'monotonic', side_effect=[0, .1, .2, .3, 1.1]):
            with self.assertRaises(TimeoutError):
                ready.probe(1234, timeout=1)
        self.assertTrue(c.closed)
        self.assertEqual(c.timeouts, [.8, .7])

    def test_send_error_closes_socket(self):
        c = Connection([])
        with patch.object(socket, 'create_connection', return_value=c), \
             patch.object(c, 'sendall', side_effect=BrokenPipeError()):
            with self.assertRaises(BrokenPipeError):
                ready.probe(1234)
        self.assertTrue(c.closed)
