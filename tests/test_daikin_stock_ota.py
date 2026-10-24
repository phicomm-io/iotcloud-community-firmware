"""Exercise the public sender against fragmented stock-protocol replies."""
import importlib.util
from pathlib import Path
import struct
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('daikin_stock_ota', ROOT / 'docs/public/assets/tools/daikin-stock-ota/daikin_stock_ota.py')
ota = importlib.util.module_from_spec(SPEC)
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


if __name__ == '__main__':
    unittest.main()
