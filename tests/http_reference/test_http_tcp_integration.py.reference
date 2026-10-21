"""Real loopback TCP experiments executing extracted A1/M1 production HTTP C.

Run: python3 -m unittest tests.test_http_tcp_integration
SANITIZE=1 instruments the generated C. PHICOMM_HTTP_EVIDENCE saves generated
sources, compiler commands, server metrics/stderr and experiment observations.

The production parser, send functions, listener and request workers are executed.
Linux TCP and pthread queues replace MiCO networking/RTOS. The small test route
adapter does not execute service JSON, Wi-Fi, MQTT or the proprietary MOC kernel.
MiCO backlog 0 is translated to Linux backlog 64 (MiCO ignores this argument).
TCP_MAX_CONN_NUM/readback are configuration stubs, not a simulated SDK capacity.
"""
import concurrent.futures
import contextlib
import hashlib
import http.client
import json
import os
from pathlib import Path
import select
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = Path(os.environ['PHICOMM_HTTP_EVIDENCE']) if 'PHICOMM_HTTP_EVIDENCE' in os.environ else None


def fragment(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def generated_source(product, timeout=10000):
    source = (ROOT / product / 'http.c').read_text()
    prefix = product.lower()
    transport = fragment(source, '#define HTTP_PENDING_CLIENTS', 'static void message(')
    transport += fragment(source, 'static bool read_request(', 'static bool authorized(')
    transport = transport.replace(f'#define HTTP_RESPONSE_TIMEOUT_MS 10000u',
                                  f'#define HTTP_RESPONSE_TIMEOUT_MS {timeout}u')
    workers = source[source.index('/* Keep accept responsive'):]
    harness = (ROOT / 'tests/http_tcp_harness.c').read_text()
    return harness.replace('/*PRODUCTION_TRANSPORT*/', transport).replace(
        '/*PRODUCTION_WORKERS*/', workers).replace(prefix + '_', 'app_').replace(product + '_', 'APP_')


class Server:
    def __init__(self, binary, policy=0, name='experiment', send_buffer=4096):
        self.stderr = tempfile.TemporaryFile(mode='w+')
        self.process = subprocess.Popen([str(binary), str(policy), str(send_buffer)], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=self.stderr, text=True, bufsize=1)
        self.name, self.observations = name, []
        self.port = self.receive()['port']

    def receive(self):
        if not select.select([self.process.stdout], [], [], 5)[0]:
            raise AssertionError('server command timed out')
        line = self.process.stdout.readline()
        if not line:
            self.stderr.seek(0)
            raise AssertionError('server exited: ' + self.stderr.read())
        value = json.loads(line)
        self.observations.append(value)
        return value

    def command(self, command):
        self.process.stdin.write(command + '\n'); self.process.stdin.flush()
        return self.receive()

    def observe(self, kind, **data):
        self.observations.append({'experiment': kind, **data})

    def wait(self, predicate, seconds=5):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            state = self.command('REPORT')
            if predicate(state):
                return state
            time.sleep(.01)
        raise AssertionError(('server did not reach state', state))

    def connect(self, receive_buffer=None):
        client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        client.settimeout(5)
        if receive_buffer:
            client.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
        client.connect(('127.0.0.1', self.port))
        return client

    def read(self, client, delay=0, chunk=65536):
        time.sleep(delay)
        result, reset = bytearray(), False
        try:
            while True:
                block = client.recv(chunk)
                if not block:
                    break
                result.extend(block)
        except ConnectionResetError:
            reset = True
        return bytes(result), reset

    def request(self, path='/api/v1/schedules', body=b'', method='GET'):
        with self.connect() as client:
            length = f'Content-Length: {len(body)}\r\n' if body else ''
            raw = (f'{method} {path} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n'
                   f'Content-Type: application/json\r\n{length}\r\n').encode() + body
            client.sendall(raw)
            return self.read(client)

    def finish(self):
        try:
            self.command('RELEASE')
            state = self.wait(lambda s: s['open_clients'] == 0)
            assert state['accepted'] == state['closed']
            assert not state['close_errors'] and not state['body_allocations']
            self.process.stdin.write('STOP\n'); self.process.stdin.flush()
            self.receive()
            code = self.process.wait(timeout=5)
            self.stderr.seek(0); errors = self.stderr.read()
            assert code == 0, (code, errors)
            if EVIDENCE:
                (EVIDENCE / (self.name + '.json')).write_text(json.dumps(self.observations, indent=2))
                (EVIDENCE / (self.name + '.stderr')).write_text(errors)
        finally:
            if self.process.poll() is None:
                self.process.kill(); self.process.wait()
            self.process.stdin.close(); self.process.stdout.close(); self.stderr.close()


class HttpTcpIntegration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.binaries = {}
        if EVIDENCE:
            EVIDENCE.mkdir(parents=True, exist_ok=True, mode=0o700)
        for product in ('A1', 'M1'):
            for timeout in (10000, 350):
                name = product + '-' + str(timeout)
                directory = EVIDENCE or Path(cls.temporary.name)
                source, binary = directory / (name + '.c'), directory / name
                source.write_text(generated_source(product, timeout))
                command = [os.environ.get('CC', 'cc'), '-std=c11', '-pthread', '-Wall', '-Wextra',
                           '-Werror', '-Wno-unused-function', '-Wno-misleading-indentation',
                           '-g', str(source), '-o', str(binary)]
                if os.environ.get('SANITIZE') == '1':
                    command += ['-fno-omit-frame-pointer', '-fsanitize=address,undefined']
                subprocess.run(command, check=True)
                cls.binaries[product, timeout] = binary
                if EVIDENCE:
                    (directory / (name + '-compile.json')).write_text(json.dumps({
                        'command': command, 'production_source_sha256': hashlib.sha256(
                            (ROOT / product / 'http.c').read_bytes()).hexdigest(),
                        'generated_c_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
                        'response_timeout_ms': timeout,
                        'adaptations': ['pthread RTOS adapter', 'loopback ephemeral bind',
                            'Linux backlog64 because MiCO ignores backlog0',
                            'configuration-only MiCO TCP option stubs', 'small test route adapter']}, indent=2))

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    @contextlib.contextmanager
    def server(self, product, policy=0, timeout=10000, suffix='', send_buffer=4096):
        server = Server(self.binaries[product, timeout], policy,
                        self._testMethodName + '-' + product + '-' + str(policy) + suffix, send_buffer)
        try:
            yield server
        finally:
            server.finish()

    def assert_response(self, result, size=32, code=200):
        raw, reset = result
        self.assertFalse(reset)
        header, body = raw.split(b'\r\n\r\n', 1)
        self.assertTrue(header.startswith(f'HTTP/1.1 {code} '.encode()), header)
        self.assertEqual(body, b'x' * size)

    def test_repeated_two_bursts_with_health_connection(self):
        for product in ('A1', 'M1'):
            with self.subTest(product=product), self.server(product) as server:
                controlled = 0
                with concurrent.futures.ThreadPoolExecutor(max_workers=9) as pool:
                    for _ in range(12):
                        first = [pool.submit(server.request) for _ in range(9)]
                        for future in first:
                            result = future.result()
                            if result[0].startswith(b'HTTP/1.1 503 '):
                                self.assertFalse(result[1])
                                self.assertTrue(result[0].endswith(b'{"code":"busy"}'))
                                controlled += 1
                            else:
                                self.assert_response(result)
                        second = [pool.submit(server.request) for _ in range(8)]
                        for future in second:
                            result = future.result()
                            if result[0].startswith(b'HTTP/1.1 503 '):
                                self.assertFalse(result[1])
                                self.assertTrue(result[0].endswith(b'{"code":"busy"}'))
                                controlled += 1
                            else:
                                self.assert_response(result)
                state = server.wait(lambda s: s['closed'] == 204)
                self.assertEqual(state['rejected'], controlled)
                self.assertEqual(state['send_failures'], 0)
                self.assertEqual(state['unread_closes'], 0)
                server.observe('two_bursts', repetitions=12, requests=204,
                               clients_per_burst=[9, 8], controlled_503=controlled,
                               reset_failures=0, final=state)

    def test_ota_does_not_raise_listener_limit(self):
        # The host only verifies listener-owner configuration; SDK admission and
        # TCP memory savings are exercised on physical devices separately.
        for product in ('A1', 'M1'):
            with self.subTest(product=product), self.server(product) as server:
                normal = server.command('REPORT')['tcp_limit']
                self.assertGreater(normal, 2)
                server.command('OTA ON')
                self.assert_response(server.request())  # Wake blocking accept.
                server.wait(lambda s: s['tcp_limit'] == 2)
                server.command('OTA OFF')
                self.assert_response(server.request())
                server.wait(lambda s: s['tcp_limit'] == normal)

    def test_queued_output_survives_nonblocking_close(self):
        for product in ('A1', 'M1'):
            for policy in (0, 1):
                with self.subTest(product=product, policy=policy), self.server(
                        product, policy, send_buffer=262144) as server:
                    with server.connect(receive_buffer=4096) as client:
                        client.sendall(b'GET /bytes/65536 HTTP/1.1\r\nHost: localhost\r\n\r\n')
                        result = server.read(client, delay=.08, chunk=4096)
                    self.assert_response(result, 65536)
                    state = server.wait(lambda s: s['closed'] == 1)
                    self.assertEqual(state['send_failures'], 0)
                    self.assertEqual(state['nonblocking_closes'], 1 if policy == 0 else 0)
                    self.assertGreater(state['queued_output_closes'], 0)
                    server.observe('output_then_close', body_bytes=65536, reset=result[1],
                                   delayed_reader_seconds=.08, final=state)

    def test_unread_get_body_resets_after_response_even_if_close_is_blocking(self):
        for product in ('A1', 'M1'):
            for policy in (0, 1, 2):
                with self.subTest(product=product, policy=policy), self.server(product, policy) as server:
                    raw, reset = server.request(body=b'unread-body')
                    self.assertTrue(raw.startswith(b'HTTP/1.1 200 '))
                    self.assertEqual(reset, policy != 2)
                    state = server.wait(lambda s: s['closed'] == 1)
                    self.assertEqual(state['unread_closes'], 0 if policy == 2 else 1)
                    self.assertEqual(state['send_failures'], 0)
                    server.observe('unread_get_body', unread_body_bytes=11, reset=reset,
                                   response_bytes=len(raw), full_response_received=raw.endswith(b'x' * 32),
                                   final=state)

    def test_content_length_client_can_succeed_despite_unread_body_reset(self):
        # This uses the same HTTP parser class as urllib; it stops at the
        # declared body length rather than reading again to observe TCP EOF/RST.
        for product in ('A1', 'M1'):
            with self.subTest(product=product), self.server(product) as server:
                client = http.client.HTTPConnection('127.0.0.1', server.port, timeout=5)
                try:
                    client.request('GET', '/api/v1/schedules', body=b'unread-body')
                    reply = client.getresponse()
                    self.assertEqual(reply.status, 200)
                    self.assertEqual(reply.read(), b'x' * 32)
                finally:
                    client.close()
                state = server.wait(lambda s: s['closed'] == 1)
                self.assertEqual(state['unread_closes'], 1)
                server.observe('content_length_client_with_unread_get_body', http_status=200,
                               body_bytes=32, final=state)

    def test_invalid_request_with_unread_bytes_resets_after_400(self):
        for product in ('A1', 'M1'):
            with self.subTest(product=product), self.server(product) as server:
                with server.connect() as client:
                    client.sendall(b'GET /' + b'x' * 1024 + b' HTTP/1.1\r\nHost: localhost\r\n\r\n')
                    raw, reset = server.read(client)
                self.assertTrue(raw.startswith(b'HTTP/1.1 400 '))
                self.assertTrue(reset)
                state = server.wait(lambda s: s['closed'] == 1)
                self.assertEqual(state['unread_closes'], 1)
                server.observe('overlong_request', http_status=400, reset=reset, final=state)

    def test_consuming_post_body_avoids_unread_close_reset(self):
        for product in ('A1', 'M1'):
            with self.subTest(product=product), self.server(product) as server:
                self.assert_response(server.request('/read-body', b'consumed-body', method='POST'))
                state = server.wait(lambda s: s['closed'] == 1)
                self.assertEqual(state['unread_closes'], 0)
                server.observe('consumed_post_body', body_bytes=13, reset=False, final=state)

    def test_actual_queue_overflow_has_complete_busy_and_no_fd_leak(self):
        for product in ('A1', 'M1'):
            with self.subTest(product=product), self.server(product) as server:
                clients = []
                try:
                    hold = server.connect(); clients.append(hold)
                    hold.sendall(b'GET /hold HTTP/1.1\r\nHost: localhost\r\n\r\n')
                    server.wait(lambda s: s['active'] == 1)
                    for i in range(1):
                        client = server.connect(); clients.append(client)
                        client.sendall(b'GET /api/v1/schedules HTTP/1.1\r\nHost: localhost\r\n\r\n')
                    server.wait(lambda s: s['enqueued'] == 2)
                    raw, reset = server.request()
                    self.assertFalse(reset)
                    self.assertIn(b'HTTP/1.1 503 ', raw)
                    self.assertTrue(raw.endswith(b'{"code":"busy"}'))
                    server.command('RELEASE')
                    for client in clients:
                        self.assert_response(server.read(client))
                    state = server.wait(lambda s: s['closed'] == 3)
                    self.assertEqual(state['rejected'], 1)
                    self.assertEqual(state['queue_peak'], 1)
                    server.observe('queue_overflow', http_status=503, reset=reset,
                                   busy_body_complete=True, final=state)
                finally:
                    for client in clients:
                        client.close()

    def test_real_backpressure_stops_at_shared_deadline_and_recovers(self):
        for product in ('A1', 'M1'):
            with self.subTest(product=product), self.server(product, timeout=350) as server:
                with server.connect() as client:
                    client.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
                    started = time.monotonic()
                    client.sendall(b'GET /bytes/8388608 HTTP/1.1\r\nHost: localhost\r\n\r\n')
                    state = server.wait(lambda s: s['closed'] == 1)
                    elapsed = time.monotonic() - started
                    self.assertGreaterEqual(elapsed, .30)
                    self.assertLess(elapsed, 1.5)
                    self.assertEqual(state['send_failures'], 1)
                    server.observe('send_backpressure', response_timeout_ms=350,
                                   elapsed_seconds=elapsed, attempted_body_bytes=8388608, final=state)
                self.assert_response(server.request())


if __name__ == '__main__':
    unittest.main()
