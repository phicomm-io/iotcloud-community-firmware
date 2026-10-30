"""Real socket coverage for MQTT wills across TCP and WebSocket termination."""
import base64
import hashlib
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def port():
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        return listener.getsockname()[1]


def text(value):
    value = value.encode() if isinstance(value, str) else value
    return struct.pack('!H', len(value)) + value


def packet(flags, body=b''):
    length = bytearray()
    remaining = len(body)
    while True:
        length.append(remaining % 128 | (128 if remaining >= 128 else 0))
        remaining //= 128
        if not remaining:
            return bytes([flags]) + length + body


def frame(opcode, body=b'', masked=True):
    assert len(body) < 126
    mask = b'1234'
    header = bytes([0x80 | opcode, len(body) | (0x80 if masked else 0)])
    return header + (mask + bytes(b ^ mask[i % 4] for i, b in enumerate(body))
                     if masked else body)


@unittest.skip("Obsolete embedded-MQTT topology; not UDP acceptance")
class TransportWill(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='daikin-transport-will-')
        cls.binary = str(Path(cls.tmp.name) / 'server')
        subprocess.run(['c++', '-std=c++11', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                        '-I' + str(ROOT / 'include'), str(ROOT / 'tests/hub/server.cpp'),
                        '-lcrypto', '-pthread', '-o', cls.binary], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.tcp, self.ws = port(), port()
        while self.tcp == self.ws:
            self.ws = port()
        self.errors = tempfile.TemporaryFile()
        self.server = subprocess.Popen([self.binary, str(self.tcp), str(self.ws)],
                                       stdout=subprocess.PIPE, stderr=self.errors,
                                       env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'})
        self.assertEqual(self.server.stdout.readline(), b'ready\n')
        self.sockets = []
        self.observer = self.connect(False, 'observer', will=False)
        self.observer.sendall(packet(0x82, b'\0\1' + text('will/#') + b'\0'))
        self.assertEqual(self.mqtt(self.observer), (0x90, b'\0\1\0'))

    def tearDown(self):
        for connection in self.sockets:
            connection.close()
        self.server.terminate()
        self.server.wait(timeout=5)
        self.server.stdout.close()
        self.errors.seek(0)
        errors = self.errors.read().decode()
        self.errors.close()
        self.assertEqual(self.server.returncode, 0, errors)
        self.assertNotIn('runtime error:', errors)

    def read(self, connection, length):
        result = b''
        while len(result) < length:
            chunk = connection.recv(length - len(result))
            self.assertTrue(chunk, 'Unexpected transport EOF')
            result += chunk
        return result

    def mqtt(self, connection):
        flags = self.read(connection, 1)[0]
        remaining, multiplier = 0, 1
        for _ in range(4):
            byte = self.read(connection, 1)[0]
            remaining += (byte & 127) * multiplier
            if not byte & 128:
                return flags, self.read(connection, remaining)
            multiplier *= 128
        self.fail('Invalid MQTT remaining length')

    def connect(self, websocket, name, will=True):
        connection = socket.create_connection(('127.0.0.1', self.ws if websocket else self.tcp), timeout=3)
        self.sockets.append(connection)
        if websocket:
            key = base64.b64encode(b'0123456789abcdef').decode()
            connection.sendall((f'GET /mqtt HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n'
                                f'Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n'
                                f'Sec-WebSocket-Protocol: mqtt\r\nSec-WebSocket-Key: {key}\r\n\r\n').encode())
            response = b''
            while not response.endswith(b'\r\n\r\n'):
                response += self.read(connection, 1)
            self.assertIn(b'101 Switching Protocols', response)
            self.assertIn(base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()), response)
        body = text('MQTT') + bytes([4, 0xc6 if will else 0xc2, 0, 30]) + text(name)
        if will:
            body += text('will/status') + text('offline')
        body += text('test') + text('test-password')
        connect = packet(0x10, body)
        connection.sendall(frame(2, connect) if websocket else connect)
        if websocket:
            self.assertEqual(self.read(connection, 2), b'\x82\x04')
        self.assertEqual(self.mqtt(connection), (0x20, b'\0\0'))
        return connection

    def assert_will_count(self, expected):
        if expected:
            self.assertEqual(self.mqtt(self.observer), (0x30, text('will/status') + b'offline'))
        # Keep the observer alive while close echo, the closing timer and socket
        # EOF finish. A second disconnect must not publish the same will again.
        self.observer.settimeout(0.4)
        with self.assertRaises(socket.timeout, msg='Unexpected or duplicate will'):
            self.observer.recv(1)

    def test_mqtt_disconnect_over_tcp_suppresses_will(self):
        publisher = self.connect(False, 'tcp-graceful')
        publisher.sendall(packet(0xe0))
        self.assertEqual(publisher.recv(1), b'')
        publisher.close()
        self.assert_will_count(0)

    def test_mqtt_disconnect_over_websocket_then_close_suppresses_will(self):
        publisher = self.connect(True, 'ws-graceful')
        publisher.sendall(frame(2, packet(0xe0)) + frame(8, struct.pack('!H', 1000)))
        self.assertEqual(publisher.recv(1), b'')
        publisher.close()
        self.assert_will_count(0)

    def close_without_disconnect(self, body):
        publisher = self.connect(True, 'ws-close')
        publisher.sendall(frame(8, body))
        self.assertEqual(self.read(publisher, len(body) + 2), bytes([0x88, len(body)]) + body)
        self.assertEqual(publisher.recv(1), b'')
        publisher.close()
        self.assert_will_count(1)

    def test_empty_websocket_close_without_mqtt_disconnect_publishes_one_will(self):
        self.close_without_disconnect(b'')

    def test_normal_websocket_close_without_mqtt_disconnect_publishes_one_will(self):
        self.close_without_disconnect(struct.pack('!H', 1000) + b'normal closure')

    def test_websocket_tcp_eof_without_disconnect_publishes_one_will(self):
        publisher = self.connect(True, 'ws-eof')
        publisher.shutdown(socket.SHUT_RDWR)
        publisher.close()
        self.assert_will_count(1)

    def test_websocket_tcp_reset_without_disconnect_publishes_one_will(self):
        publisher = self.connect(True, 'ws-reset')
        publisher.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        publisher.close()
        self.assert_will_count(1)

    def test_websocket_protocol_error_publishes_one_will(self):
        publisher = self.connect(True, 'ws-error')
        publisher.sendall(frame(1, b'not-mqtt-binary'))
        self.assertEqual(publisher.recv(1), b'')
        publisher.close()
        self.assert_will_count(1)


if __name__ == '__main__':
    unittest.main()
