#!/usr/bin/env python3
"""Real TLS + production signed-OTA verifier, with deterministic HTTP faults."""
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import socketserver
import ssl
import subprocess
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / 'build/tls-probe'


class Transport(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.directory = Path(cls.tmp.name)
        cls.image = (WORK / 'a1.signed').read_bytes()
        assert cls.image[:8] == b'PHOTA001'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                        '-keyout', str(cls.directory / 'key.pem'), '-out', str(cls.directory / 'cert.pem'),
                        '-days', '1', '-subj', '/CN=deliberately-wrong-host.invalid'],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cls.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        cls.context.minimum_version = cls.context.maximum_version = ssl.TLSVersion.TLSv1_2
        cls.context.set_ciphers('AES128-GCM-SHA256')
        cls.context.load_cert_chain(cls.directory / 'cert.pem', cls.directory / 'key.pem')

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_case(self, fault='', model=3, capacity=2048, window=1024, document=None):
        payload = bytearray(self.image if document is None else document)
        if fault == 'signature':
            payload[128] ^= 1
        if fault == 'body':
            payload[-1] ^= 1
        if fault == 'raw':
            payload = payload[192:]
        context = self.context

        class Handler(socketserver.BaseRequestHandler):
            def handle(self):
                self.request.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                try:
                    with context.wrap_socket(self.request, server_side=True) as stream:
                        reader = stream.makefile('rb')
                        while True:
                            first_line = reader.readline(2048)
                            if not first_line:
                                return
                            headers = {}
                            while True:
                                line = reader.readline(2048)
                                if line == b'\r\n':
                                    break
                                if not line:
                                    return
                                key, value = line.decode().strip().split(':', 1)
                                headers[key.lower()] = value.strip()
                            start, stop = map(int, re.fullmatch(r'bytes=(\d+)-(\d+)', headers['range']).groups())
                            stop = min(stop, len(payload) - 1)
                            body = bytes(payload[start:stop + 1])
                            total = len(payload) + (1 if fault == 'changed_total' and start else 0)
                            offset = start + (1 if fault == 'wrong_offset' else 0)
                            status = '200 OK' if fault == 'ignored_range' else '206 Partial Content'
                            length = len(body) + (1 if fault == 'wrong_length' else 0)
                            lines = [f'HTTP/1.1 {status}', f'Content-Length: {length}',
                                     f'Content-Range: bytes {offset}-{stop}/{total}']
                            if fault == 'chunked':
                                lines.append('Transfer-Encoding: chunked')
                            if fault == 'duplicate_length':
                                lines.append(f'Content-Length: {length}')
                            if fault == 'close' and start == 0:
                                lines.append('Connection: close')
                            wire = ('\r\n'.join(lines) + '\r\n\r\n').encode() + body
                            if fault == 'truncated':
                                stream.sendall(wire[:-10])
                                return
                            if fault == 'fragmented':
                                for position in range(0, len(wire), 97):
                                    stream.sendall(wire[position:position + 97])
                            else:
                                stream.sendall(wire)
                            if fault == 'close' and start == 0:
                                return
                except (OSError, ValueError, ssl.SSLError):
                    # The client closes immediately on rejected input.
                    pass

        class Server(socketserver.ThreadingTCPServer):
            allow_reuse_address = True
            daemon_threads = True

        with Server(('127.0.0.1', 0), Handler) as server:
            thread = threading.Thread(target=server.serve_forever, kwargs={'poll_interval': 0.01}, daemon=True)
            thread.start()
            output = self.directory / f'{fault or "valid"}-{capacity}.raw'
            process = subprocess.run([os.environ.get('PROBE_CLIENT', str(WORK / 'client')), 'localhost', '/firmware',
                                      str(capacity), str(window), str(model), str(output),
                                      str(server.server_address[1])], text=True, capture_output=True, timeout=60)
            server.shutdown()
            thread.join()
        values = dict(part.split('=', 1) for part in process.stdout.split())
        self.assertTrue(values, process.stderr)
        print(json.dumps({'case': fault or 'valid', 'model': model, 'buffer': capacity,
                          'exit': process.returncode, 'metrics': values}), flush=True)
        if process.returncode == 0:
            expected = document if model == 0 else self.image[192:]
            self.assertEqual(hashlib.sha256(output.read_bytes()).digest(), hashlib.sha256(expected).digest())
        return process.returncode, values

    def test_metadata_stream_does_not_enter_ota_staging(self):
        for document in [b'{"version":"0.9.5"}', ('# Release log\n' + '更新说明\n' * 600).encode(), b'x' * 16384]:
            with self.subTest(length=len(document)):
                code, values = self.run_case(model=0, document=document)
                self.assertEqual(code, 0)
                self.assertEqual(values['erases'], '0')
                self.assertEqual(values['reads'], '0')
                self.assertEqual(values['authenticated'], '0')

    def test_metadata_over_limit_rejected_before_sink_write(self):
        code, values = self.run_case(model=0, document=b'x' * 16385)
        self.assertNotEqual(code, 0)
        self.assertEqual(values['error'], 'stream_limit')
        self.assertEqual(values['writes'], '0')
        self.assertEqual(values['erases'], '0')

    def test_valid_signed_with_untrusted_wrong_hostname_certificate(self):
        code, values = self.run_case()
        self.assertEqual(code, 0)
        self.assertEqual(values['authenticated'], '1')
        self.assertEqual(values['erases'], '1')
        self.assertGreater(int(values['reads']), 0)

    def test_fragmented_tls_and_http(self):
        self.assertEqual(self.run_case('fragmented')[0], 0)

    def test_connection_close_reconnects_without_restarting_staging(self):
        code, values = self.run_case('close')
        self.assertEqual(code, 0)
        self.assertEqual(values['connections'], '2')
        self.assertEqual(values['erases'], '1')

    def test_signature_raw_and_model_rejected_before_erase(self):
        for fault, model in [('signature', 3), ('raw', 3), ('', 2)]:
            with self.subTest(fault=fault, model=model):
                code, values = self.run_case(fault, model)
                self.assertNotEqual(code, 0)
                self.assertEqual(values['erases'], '0')
                self.assertEqual(values['authenticated'], '0')
                self.assertEqual(values['error'], 'signature_or_write')
                self.assertEqual(values['tls_error'], '0')

    def test_corrupted_payload_rejected_on_flash_readback(self):
        code, values = self.run_case('body')
        self.assertNotEqual(code, 0)
        self.assertEqual(values['authenticated'], '1')
        self.assertEqual(values['error'], 'readback')

    def test_invalid_or_incomplete_range(self):
        for fault in ['ignored_range', 'wrong_offset', 'wrong_length', 'changed_total',
                      'chunked', 'duplicate_length', 'truncated']:
            with self.subTest(fault=fault):
                code, values = self.run_case(fault)
                self.assertNotEqual(code, 0)
                self.assertGreater(int(values['requests']), 0)
                self.assertEqual(values['error'], 'body' if fault == 'truncated' else
                                 'headers' if fault in ['chunked', 'duplicate_length'] else 'range')


if __name__ == '__main__':
    unittest.main()
