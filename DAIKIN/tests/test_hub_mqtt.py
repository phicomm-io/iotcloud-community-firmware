"""Real TCP/WebSocket MQTT clients against the exact embedded broker/transport.

Requires a C++ compiler, OpenSSL headers and paho-mqtt (no device or broker service).
"""
import base64
import hashlib
import json
import os
from pathlib import Path
import queue
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest

try:
    import paho.mqtt.client as mqtt
except ImportError:
    mqtt = None  # Obsolete tests do not require installing the old MQTT harness.

ROOT = Path(__file__).resolve().parents[1]


def port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def text(value):
    value = value.encode() if isinstance(value, str) else value
    return struct.pack('!H', len(value)) + value


def packet(flags, body=b''):
    n = len(body)
    length = bytearray()
    while True:
        length.append(n % 128 | (128 if n >= 128 else 0))
        n //= 128
        if not n:
            return bytes([flags]) + length + body


@unittest.skip("Obsolete embedded-MQTT topology; not UDP acceptance")
class HubTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='iotcloud-hub-')
        cls.binary = str(Path(cls.tmp.name) / 'server')
        flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if os.getenv('SANITIZE') == '1' else []
        subprocess.run(['c++', '-std=c++11', '-g', '-Wall', '-Wextra', '-Werror', *flags,
                        '-I', str(ROOT / 'include'), str(ROOT / 'tests/hub/server.cpp'),
                        '-lcrypto', '-pthread', '-o', cls.binary], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.tcp, self.ws = port(), port()
        while self.ws == self.tcp:
            self.ws = port()
        self.errors = tempfile.TemporaryFile()
        self.server = subprocess.Popen([self.binary, str(self.tcp), str(self.ws)], stdout=subprocess.PIPE, stderr=self.errors)
        self.assertEqual(self.server.stdout.readline(), b'ready\n')
        self.clients = []

    def tearDown(self):
        for c in self.clients:
            c.disconnect()
            c.loop_stop()
        self.server.terminate()
        self.server.wait(timeout=5)
        self.server.stdout.close()
        self.errors.seek(0)
        errors = self.errors.read().decode()
        self.errors.close()
        self.assertEqual(self.server.returncode, 0, errors)
        self.assertNotIn('runtime error:', errors)

    def client(self, name, ws=False, password='test-password', will=None, expected=0):
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=name, transport='websockets' if ws else 'tcp')
        c.username_pw_set('test', password)
        if will:
            c.will_set(*will)
        c.messages = queue.Queue()
        c.acks = queue.Queue()
        ready = threading.Event()
        result = []
        c.on_connect = lambda client, userdata, flags, reason, props: (result.append(reason.value), ready.set())
        c.on_message = lambda client, userdata, msg: c.messages.put((msg.topic, msg.payload, msg.retain))
        c.on_subscribe = lambda client, userdata, mid, codes, props: c.acks.put(('sub', mid))
        c.on_unsubscribe = lambda client, userdata, mid, codes, props: c.acks.put(('unsub', mid))
        c.connect('127.0.0.1', self.ws if ws else self.tcp, 15)
        c.loop_start()
        self.clients.append(c)
        self.assertTrue(ready.wait(3), 'CONNACK missing')
        # Paho converts the MQTT 3 refusal to the MQTT 5 reason code.
        self.assertEqual(result[0], expected)
        return c

    def subscribe(self, c, topic, qos=1):
        _, mid = c.subscribe(topic, qos)
        self.assertEqual(c.acks.get(timeout=3), ('sub', mid))

    def publish(self, c, topic, body, retain=False, qos=1):
        info = c.publish(topic, body, qos=qos, retain=retain)
        info.wait_for_publish(3)
        self.assertTrue(info.is_published())

    def test_tcp_websocket_qos_retain_and_delete(self):
        tcp = self.client('sensor')
        ws = self.client('webui', ws=True)
        self.subscribe(ws, 'home/+/state', qos=2)
        self.publish(tcp, 'home/air/state', '123', retain=True)
        self.assertEqual(ws.messages.get(timeout=3), ('home/air/state', b'123', False))
        late = self.client('ha', ws=True)
        self.subscribe(late, 'home/#')
        self.assertEqual(late.messages.get(timeout=3), ('home/air/state', b'123', True))
        self.publish(tcp, 'home/air/state', b'', retain=True)
        self.assertEqual(late.messages.get(timeout=3), ('home/air/state', b'', False))
        fresh = self.client('after-delete')
        self.subscribe(fresh, 'home/#')
        with self.assertRaises(queue.Empty):
            fresh.messages.get(timeout=.15)

    def test_account_isolation_and_credentials(self):
        self.client('bad', password='wrong', expected=134)
        observer = self.client('observer')
        self.subscribe(observer, '#')
        attacker = self.client('attacker')
        attacker.publish('phicomm/v2/u_6f74686572/m1/aabbccddeeff/state', 'secret')
        with self.assertRaises(queue.Empty):
            observer.messages.get(timeout=.2)
        own = self.client('own')
        topic = 'phicomm/v2/u_74657374/m1/aabbccddeeff/state'
        self.publish(own, topic, 'allowed')
        self.assertEqual(observer.messages.get(timeout=3)[:2], (topic, b'allowed'))

    def test_will_duplicate_identifier_and_graceful_disconnect(self):
        observer = self.client('observer')
        self.subscribe(observer, 'device/#')
        original = self.client('same', will=('device/status', 'offline', 1, True))
        replacement = self.client('same')
        self.assertEqual(observer.messages.get(timeout=3), ('device/status', b'offline', False))
        original.disconnect()
        original.loop_stop()
        replacement.disconnect()
        replacement.loop_stop()
        graceful = self.client('graceful', will=('device/status', 'bad', 1, True))
        graceful.disconnect()
        graceful.loop_stop()
        with self.assertRaises(queue.Empty):
            observer.messages.get(timeout=.15)
        late = self.client('late')
        self.subscribe(late, 'device/status')
        self.assertEqual(late.messages.get(timeout=3), ('device/status', b'offline', True))

    def test_unsubscribe_wildcards_and_dollar_topics(self):
        observer = self.client('observer')
        publisher = self.client('publisher')
        self.subscribe(observer, '#')
        self.publish(publisher, '$SYS/state', 'hidden')
        with self.assertRaises(queue.Empty):
            observer.messages.get(timeout=.1)
        _, mid = observer.unsubscribe('#')
        self.assertEqual(observer.acks.get(timeout=3), ('unsub', mid))
        self.subscribe(observer, 'home/+/state')
        self.publish(publisher, 'home/one/state', 'yes')
        self.assertEqual(observer.messages.get(timeout=3)[1], b'yes')
        self.publish(publisher, 'home/one/extra/state', 'no')
        with self.assertRaises(queue.Empty):
            observer.messages.get(timeout=.1)

    def raw(self, keepalive=10):
        s = socket.create_connection(('127.0.0.1', self.tcp), timeout=3)
        self.addCleanup(s.close)
        s.sendall(packet(0x10, text('MQTT') + bytes([4, 0xc2]) + struct.pack('!H', keepalive) + text('raw') + text('test') + text('test-password')))
        self.assertEqual(s.recv(4), b'\x20\x02\x00\x00')
        return s

    def test_websocket_fragmentation_control_and_rejections(self):
        def upgrade(headers=''):
            s = socket.create_connection(('127.0.0.1', self.ws), timeout=3)
            self.addCleanup(s.close)
            key = base64.b64encode(b'0123456789abcdef').decode()
            s.sendall((f'GET /mqtt HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n'
                       f'Connection: keep-alive, Upgrade\r\nSec-WebSocket-Version: 13\r\n'
                       f'Sec-WebSocket-Protocol: mqtt\r\nSec-WebSocket-Key: {key}\r\n{headers}\r\n').encode())
            reply = b''
            while b'\r\n\r\n' not in reply:
                reply += s.recv(4096)
            self.assertIn(b'101 Switching Protocols', reply)
            self.assertIn(base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()), reply)
            return s

        def frame(op, body, final=True, masked=True):
            mask = b'1234'
            header = bytes([(0x80 if final else 0) | op])
            header += bytes([(0x80 if masked else 0) | len(body)])
            return header + (mask + bytes(b ^ mask[i % 4] for i, b in enumerate(body)) if masked else body)

        def read(s, n):
            result = b''
            while len(result) < n:
                chunk = s.recv(n - len(result))
                self.assertTrue(chunk)
                result += chunk
            return result

        s = upgrade()
        connect = packet(0x10, text('MQTT') + bytes([4, 0xc2, 0, 10]) + text('ws-raw') + text('test') + text('test-password'))
        s.sendall(frame(2, connect[:3], final=False) + frame(9, b'ping') + frame(0, connect[3:]))
        self.assertEqual(read(s, 6), b'\x8a\x04ping')
        self.assertEqual(read(s, 6), b'\x82\x04\x20\x02\x00\x00')
        s.sendall(frame(2, b'\xc0\x00'))
        self.assertEqual(read(s, 4), b'\x82\x02\xd0\x00')
        for op, body, final, masked in [(1, b'text', True, True), (0, b'orphan', True, True), (9, b'fragment', False, True), (2, connect, True, False)]:
            invalid = upgrade()
            invalid.sendall(frame(op, body, final, masked))
            self.assertEqual(invalid.recv(1), b'', (op, final, masked))

    @unittest.skipUnless(os.getenv('HUB_TEST_WORKERS') == '1', 'build_host.sh SDK workers required')
    def test_actual_sdk_workers_share_broker_and_home_assistant(self):
        observer = self.client('ha-observer', ws=True)
        self.subscribe(observer, '#')
        records, workers = [], []

        def wait(predicate):
            deadline = time.monotonic() + 12
            while not predicate():
                self.assertLess(time.monotonic(), deadline, 'SDK worker observation timeout ' + repr([(t, len(p)) for t, p, r in records[-45:]]))
                try:
                    records.append(observer.messages.get(timeout=.1))
                except queue.Empty:
                    self.assertTrue(all(w.poll() is None for w in workers))

        try:
            for model in ['M1', 'A1', 'TC1']:
                binary = ROOT.parent / 'build/account-host' / model
                self.assertTrue(binary.is_file(), str(binary))
                workers.append(subprocess.Popen([str(binary), '127.0.0.1', str(self.tcp), 'test', '45'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True))
                base = f'phicomm/v2/u_74657374/{model.lower()}/aabbccddeeff'
                wait(lambda: any(t == base + '/state' for t, p, r in records))
                minimum = {'M1': 5, 'A1': 4, 'TC1': 17}[model]
                wait(lambda: sum(bool(t.startswith('homeassistant/') and p and t.endswith('/config') and json.loads(p).get('json_attributes_topic') == base + '/state') for t, p, r in records) >= minimum)
            for model, action, params in [('M1', 'display.set', {'brightness': 50}), ('A1', 'fan.set', {'on': True, 'speed': 40}), ('TC1', 'outlet.set', {'index': 0, 'on': True})]:
                base = f'phicomm/v2/u_74657374/{model.lower()}/aabbccddeeff'
                request = 'hub-' + model
                self.publish(observer, base + '/command', json.dumps({'request_id': request, 'action': action, 'params': params}))
                wait(lambda: any(t == base + '/result' and json.loads(p).get('request_id') == request and json.loads(p).get('code') in ('ok', 'accepted') for t, p, r in records))
            late = self.client('ha-late', ws=True)
            self.subscribe(late, '#')
            retained = []
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                try:
                    retained.append(late.messages.get(timeout=.2))
                except queue.Empty:
                    if sum(t.startswith('homeassistant/') and t.endswith('/config') for t, p, r in retained) >= 26:
                        break
            self.assertGreaterEqual(sum(t.startswith('homeassistant/') and t.endswith('/config') and r for t, p, r in retained), 26)
            self.assertFalse(any(t.endswith('/result') and r for t, p, r in retained))
        finally:
            failed = __import__('sys').exc_info()[0] is not None
            for worker in workers:
                worker.terminate()
                out = worker.communicate(timeout=5)[0]
                if not failed:
                    self.assertIn('CONTROL', out, out[-2000:])
                else:
                    print(out[-4000:], flush=True)

    def test_keepalive_malformed_and_retained_command(self):
        s = self.raw(keepalive=1)
        self.assertEqual(s.recv(1), b'')
        for malformed in [b'\xc1\x00', b'\x30\xff\xff\xff\xff', packet(0x31, text('home/command') + b'bad')]:
            s = self.raw()
            s.sendall(malformed)
            self.assertEqual(s.recv(1), b'')
        good = self.client('survivor')
        self.subscribe(good, 'home/state')
        self.publish(good, 'home/state', 'still alive')
        self.assertEqual(good.messages.get(timeout=3)[1], b'still alive')

    def test_large_fragmented_packets_and_partial_header_timeout(self):
        observer = self.client('observer', ws=True)
        self.subscribe(observer, 'large')
        s = self.raw()
        body = b'x' * 8000
        encoded = packet(0x30, text('large') + body) + b'\xc0\x00'
        # Arbitrary TCP boundaries, including a whole next packet in the last read.
        for at in range(0, len(encoded), 137):
            s.sendall(encoded[at:at+137])
        self.assertEqual(observer.messages.get(timeout=3)[1], body)
        self.assertEqual(s.recv(2), b'\xd0\x00')
        s.sendall(b'\x30\x80')
        s.settimeout(7)
        self.assertEqual(s.recv(1), b'')

    def test_512_byte_topic_and_exact_packet_capacity_over_tcp_and_websocket(self):
        topic = 'long/' + 'x' * 507
        observer = self.client('long-topic-observer', ws=True)
        self.subscribe(observer, topic)
        publisher = self.raw()
        payload = b'p' * (8192 - len(topic.encode()) - 4)
        encoded = packet(0x32, text(topic) + b'\0\7' + payload)
        self.assertEqual((len(payload), len(encoded)), (7676, 8195))
        for at in range(0, len(encoded), 137):
            publisher.sendall(encoded[at:at+137])
        self.assertEqual(publisher.recv(4), b'\x40\x02\0\7')
        self.assertEqual(observer.messages.get(timeout=3), (topic, payload, False))
        # QoS 0 has two fewer wire bytes; the shared publish budget still
        # reserves the QoS 1 packet id and rejects one extra payload byte.
        publisher.sendall(packet(0x30, text(topic) + payload + b'x'))
        self.assertEqual(publisher.recv(1), b'')
        with self.assertRaises(queue.Empty):
            observer.messages.get(timeout=.15)


if __name__ == '__main__':
    unittest.main()
