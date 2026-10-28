"""Compile the Linux sender and inspect encoded records/frames without a radio."""
import importlib.util
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'docs/public/assets/tools/easylink-linux/easylink_v2_linux.c'
spec = importlib.util.spec_from_file_location('easylink_reference', ROOT / 'tools/a1_easylink.py')
reference = importlib.util.module_from_spec(spec)
# dataclasses resolves annotations through sys.modules.
import sys
sys.modules[spec.name] = reference
spec.loader.exec_module(reference)


@unittest.skipUnless(shutil.which('cc'), 'C compiler required')
class InjectionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.tmp.name) / 'sender'
        subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', str(SOURCE), '-o', str(cls.binary)], check=True)
        # Override only transport and pacing; no socket or interface is opened.
        harness = Path(cls.tmp.name) / 'frames.c'
        harness.write_text('#define main sender_main\n#include "' + str(SOURCE) + '"\n#undef main\n' + r'''
ssize_t sendto(int fd, const void *data, size_t n, int flags, const struct sockaddr *dst, socklen_t size) {
    (void)fd; (void)flags; (void)dst; (void)size;
    if (fwrite(data, 1, n, stdout) != n) return -1;
    return (ssize_t)n;
}
int nanosleep(const struct timespec *requested, struct timespec *remaining) {
    (void)requested; (void)remaining; return 0;
}
int main(void) {
    struct sockaddr_ll addr = {0};
    unsigned char mac[6] = {2, 0, 0, 0, 0, 1}, ip[4] = {192, 168, 1, 23};
    monitor_mode = 1;
    if (transmit(-1, &addr, mac, ip, 118, 0, 0, 20, 0)) return 1;
    return transmit(-1, &addr, mac, ip, 126, 8, 12, 23, 1);
}
''')
        cls.frames = Path(cls.tmp.name) / 'frames'
        subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', str(harness), '-o', str(cls.frames)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_record_matches_decoder_fixture_for_utf8_padding_and_keys(self):
        for ssid, key in [('HomeWiFi', '12345678'), ('家用网络', 'long-password'), ('s', ''), ('s', 'aF01' * 16)]:
            with self.subTest(ssid=ssid):
                result = subprocess.run([str(self.binary), '--fixture', '192.168.1.23', '1'],
                                        input=ssid + '\n' + key + '\n', text=True, capture_output=True, check=True)
                self.assertEqual(bytes.fromhex(result.stdout), reference.encode(ssid, key, '192.168.1.23'))

    def test_bad_credentials_are_rejected_before_opening_a_socket(self):
        for ssid, key in [('', '12345678'), ('中' * 11, '12345678'), ('s', '1234567'), ('s', 'g' * 64)]:
            result = subprocess.run([str(self.binary), '--fixture', '192.168.1.23', '1'],
                                    input=ssid + '\n' + key + '\n', text=True, capture_output=True)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(result.stdout, '')

    def test_radiotap_frames_preserve_multicast_mac_lengths_and_ip_checksum(self):
        output = subprocess.check_output([str(self.frames)])
        sizes = [12 + 24 + 8 + 20 + 8 + payload for payload in (20, 23)]
        self.assertEqual(len(output), sum(sizes))
        offset = 0
        for size, mac, payload in zip(sizes, (b'\x01\x00\x5e\x76\x00\x00', b'\x01\x00\x5e\x7e\x08\x0c'), (20, 23)):
            frame = output[offset:offset + size]
            offset += size
            self.assertEqual(struct.unpack_from('<H', frame, 2)[0], 12)
            self.assertEqual(frame[12:14], b'\x08\x02')  # FromDS
            self.assertEqual(frame[16:22], mac)
            self.assertEqual(frame[36:44], b'\xaa\xaa\x03\x00\x00\x00\x08\x00')
            ip = frame[44:64]
            self.assertEqual(struct.unpack_from('!H', ip, 2)[0], 28 + payload)
            self.assertEqual(ip[12:16], socket.inet_aton('192.168.1.23'))
            total = sum(struct.unpack('!10H', ip))
            while total >> 16:
                total = (total & 65535) + (total >> 16)
            self.assertEqual(total, 65535)
            self.assertEqual(struct.unpack_from('!H', frame, 68)[0], 8 + payload)


if __name__ == '__main__':
    unittest.main()
