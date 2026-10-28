"""Exercise the public sender against fragmented stock-protocol replies."""
import importlib.util
from contextlib import redirect_stdout
import io
from pathlib import Path
import struct
import socket
import sys
import threading
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('daikin_stock_ota', ROOT / 'docs/public/assets/tools/daikin-stock-ota/daikin_stock_ota.py')
ota = importlib.util.module_from_spec(SPEC)
# Importing a downloadable asset must not add bytecode to the public bundle.
with patch.object(sys, 'dont_write_bytecode', True):
    SPEC.loader.exec_module(ota)


class Peer:
    def __init__(self, replies):
        self.replies = bytearray(replies)
        self.sent = []

    def recv(self, count):
        # Deliberately fragment every reply, including uint32 ACKs.
        part = bytes(self.replies[:1])
        del self.replies[:1]
        return part

    def sendall(self, data):
        self.sent.append(data)

    def __enter__(self):
        return self

    def __exit__(self, *args):
        pass

    def settimeout(self, value):
        pass

    def setsockopt(self, *args):
        pass


class SenderTests(unittest.TestCase):
    def test_transfer_partial_acks_and_end_separation(self):
        image = bytes(range(256)) * 4 + b'last'
        peer = Peer(b'OkeyToDo' + struct.pack('<III', 500, 524, 4) + b'OK')
        with patch.object(ota.time, 'sleep') as delay:
            ota.upload(peer, image, 'SV.02.00.01.16')
        self.assertEqual(peer.sent[:2], [b'Kit2 Version Up', b'SV.02.00.01.16'])
        self.assertEqual(b''.join(peer.sent[2:-2]), image)
        self.assertEqual(peer.sent[-2], b'End')
        self.assertEqual(len(peer.sent[-1]), 1)
        total = sum(image)
        while total > 255:
            total = (total & 255) + (total >> 8)
        self.assertEqual(peer.sent[-1], bytes([255-total]))
        delay.assert_called_once_with(3)

    def test_bad_ack_prevents_commit(self):
        peer = Peer(b'OkeyToDo' + struct.pack('<I', 9999))
        with self.assertRaisesRegex(RuntimeError, 'Invalid ACK'):
            ota.upload(peer, b'image', 'SV.02.00.01.16')
        self.assertNotIn(b'End', peer.sent)

    def test_final_rejection_is_failure(self):
        peer = Peer(b'OkeyToDo' + struct.pack('<I', 5) + b'NG')
        with patch.object(ota.time, 'sleep'), self.assertRaisesRegex(RuntimeError, 'rejected'):
            ota.upload(peer, b'image', 'SV.02.00.01.16')

    def test_probe_stops_at_first_acceptance_without_firmware_or_commit(self):
        peers = [Peer(b'OkeyNo') for _ in range(5)] + [Peer(b'OkeyToDo')]
        versions = ota.version_range('SV.02.00.01.16', 'SV.02.00.01.32')
        output = io.StringIO()
        with patch.object(ota.socket, 'create_connection', side_effect=peers) as connect, \
                patch.object(ota.time, 'sleep') as delay, redirect_stdout(output):
            ota.probe_versions('192.0.2.1', versions)
        self.assertEqual(connect.call_count, 6)
        self.assertEqual(delay.call_args_list, [unittest.mock.call(2)] * 5)
        for peer, version in zip(peers, versions):
            self.assertEqual(peer.sent, [b'Kit2 Version Up', version.encode()])
        self.assertIn('有条件推测当前版本：SV.02.00.01.20', output.getvalue())
        self.assertIn('固件数据发送量：0 字节', output.getvalue())

    def test_probe_only_accepts_explicit_no_followed_by_eof(self):
        self.assertEqual(ota.probe(Peer(b'OkeyNo'), 'SV.02.00.01.16'), (False, b'No', 'EOF'))
        for reply in (b'', b'N', b'NoX', b'To', b'NG'):
            with self.subTest(reply=reply), self.assertRaises(RuntimeError):
                ota.probe(Peer(b'Okey' + reply), 'SV.02.00.01.16')

    def test_probe_stops_on_timeout_connection_failure_or_malformed_reply(self):
        versions = ota.version_range('SV.02.00.01.16', 'SV.02.00.01.32')
        for problem in (socket.timeout('timed out'), ConnectionResetError('reset'), Peer(b'OkeyNG')):
            output = io.StringIO()
            with self.subTest(problem=problem), \
                    patch.object(ota.socket, 'create_connection', side_effect=[problem]) as connect, \
                    patch.object(ota.time, 'sleep') as delay, redirect_stdout(output), \
                    self.assertRaisesRegex(RuntimeError, 'communication error'):
                ota.probe_versions('192.0.2.1', versions)
            self.assertEqual(connect.call_count, 1)
            delay.assert_not_called()
            self.assertNotIn('有条件推测当前版本：', output.getvalue())
            self.assertIn('0 字节', output.getvalue())

    def test_probe_has_no_version_inference_without_a_boundary(self):
        for reply in (b'No', b'ToDo'):
            output = io.StringIO()
            with patch.object(ota.socket, 'create_connection', return_value=Peer(b'Okey' + reply)), redirect_stdout(output):
                ota.probe_versions('192.0.2.1', ['SV.02.00.01.16'])
            self.assertNotIn('有条件推测当前版本：', output.getvalue())

    def test_probe_wire_bytes_stop_after_the_version_header(self):
        for reply in (b'No', b'ToDo'):
            client, server = socket.socketpair()
            captured = []

            def device():
                with server:
                    server.settimeout(2)
                    captured.append(ota.receive(server, len(b'Kit2 Version Up')))
                    server.sendall(b'Okey')
                    captured.append(ota.receive(server, len(b'SV.02.00.01.21')))
                    server.sendall(reply)
                    if reply == b'No':
                        server.shutdown(socket.SHUT_WR)
                    captured.append(server.recv(1024))

            thread = threading.Thread(target=device)
            thread.start()
            try:
                with client:
                    client.settimeout(2)
                    accepted, _, _ = ota.probe(client, 'SV.02.00.01.21')
                    self.assertEqual(accepted, reply == b'ToDo')
            finally:
                client.close()
                thread.join(timeout=3)
            self.assertFalse(thread.is_alive())
            self.assertEqual(captured, [b'Kit2 Version Up', b'SV.02.00.01.21', b''])

    def test_cli_rejects_mixed_modes_and_implicit_upload_before_network(self):
        cases = [
            ['--probe-versions', 'SV.02.00.01.16', 'SV.02.00.01.32', 'image.bin'],
            ['--probe-versions', 'SV.02.00.01.32', 'SV.02.00.01.16'],
            ['--probe-versions', 'SV.02.00.01.16', 'SV.02.00.02.32'],
            ['image.bin', '--sha256', '0' * 64],
        ]
        for arguments in cases:
            with self.subTest(arguments=arguments), patch.object(sys, 'argv', ['ota'] + arguments), \
                    patch.object(ota.socket, 'create_connection') as connect, \
                    patch.object(ota.Path, 'read_bytes') as read, self.assertRaises(SystemExit):
                ota.main()
            connect.assert_not_called()
            read.assert_not_called()


if __name__ == '__main__':
    unittest.main()
