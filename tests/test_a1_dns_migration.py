"""Exercise the inferred A1 HTTP/TLS contracts on loopback, without a device."""
import binascii
import hashlib
import http.client
import json
from pathlib import Path
import shutil
import ssl
import struct
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'docs/public/assets/tools/a1-dns-migration/a1_dns_migration.py'


def native_image(product=b'PHICOMM_A1_EMW3080\0'):
    application = bytearray(264)
    struct.pack_into('<III', application, 8, 0xc89346, 0x20010000, 3)
    struct.pack_into('<I', application, 28, 0x08088000 + 96)
    application[96:96 + len(product)] = product
    crc = binascii.crc_hqx(application[8:], 0)
    struct.pack_into('<IHH', application, 0, len(application) - 8, crc, crc)
    data = bytes(0x75000) + application
    return data + hashlib.md5(data).digest()


class MigrationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.directory = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def command(self, data, check=False, digest=None):
        image = self.directory / 'native.raw'
        image.write_bytes(data)
        return [sys.executable, str(SCRIPT), '--image', str(image), '--sha256', digest or hashlib.sha256(data).hexdigest(),
                '--mac', '001122334455', '--device-ip', '127.0.0.1', '--router-ip', '127.0.0.1',
                '--server-ip', '127.0.0.7', '--state-dir', str(self.directory / 'state')] + (['--check-only'] if check else [])

    def test_validation_only_and_invalid_images_start_no_services_or_certificates(self):
        good = native_image()
        result = subprocess.run(self.command(good, check=True), capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('no network services', result.stdout)
        cases = [native_image(b'PHICOMM_M1_EMW3080\0'), good[:-1] + bytes([good[-1] ^ 1]), b'signed-wrapper' + good,
                 good[:0x75000 + 100] + b'X' + good[0x75000 + 101:]]
        for data in cases:
            result = subprocess.run(self.command(data, check=True), capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
        result = subprocess.run(self.command(good, check=True, digest='0' * 64), capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.directory / 'state').exists())

    @unittest.skipUnless(shutil.which('openssl'), 'OpenSSL required for local TLS')
    def test_stock_routes_identity_manifest_and_http_range(self):
        data = native_image()
        with (self.directory / 'server.log').open('w+') as log:
            process = subprocess.Popen(self.command(data), stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        self.fail('Server exited: ' + (self.directory / 'server.log').read_text())
                    if '"kind": "ready"' in (self.directory / 'server.log').read_text():
                        break
                    time.sleep(.05)
                else:
                    self.fail('Loopback service did not become ready')

                def request(path, secure=False, headers=None):
                    cls = http.client.HTTPSConnection if secure else http.client.HTTPConnection
                    options = {'context': ssl._create_unverified_context()} if secure else {}
                    connection = cls('127.0.0.7', 29443 if secure else 29080, timeout=3, **options)
                    try:
                        connection.request('GET', path, headers=headers or {})
                        response = connection.getresponse()
                        return response.status, dict(response.getheaders()), response.read()
                    finally:
                        connection.close()

                status, _, payload = request('/aircleanerfirmware/checkUpdate', secure=True)
                self.assertEqual(status, 200)
                manifest = json.loads(payload)
                self.assertEqual(manifest, dict(url='http://127.0.0.7:29080/a1-native.bin', ver='v1.48',
                                                md5=hashlib.md5(data).hexdigest(), enforce=True))
                self.assertNotIn('wifiSoftware', manifest)
                status, _, payload = request('/device/active?mac=00:11:22:33:44:55&productId=30', secure=True)
                self.assertEqual((status, json.loads(payload)), (200, {'device_id': '001122334455'}))
                for path in ('/device/active?mac=001122334456&productId=30', '/device/active?mac=001122334455&productId=1'):
                    self.assertEqual(request(path)[0], 403)
                self.assertEqual(request('/firmware/checkUpdate')[0], 404)
                status, headers, payload = request('/a1-native.bin?resume=1', headers={'Range': 'bytes=479230-479250'})
                self.assertEqual(status, 206)
                self.assertEqual(payload, data[479230:479251])
                self.assertEqual(headers['Content-Range'], f'bytes 479230-479250/{len(data)}')
                self.assertEqual(request('/a1-native.bin', headers={'Range': f'bytes={len(data)}-'})[0], 416)
            finally:
                process.terminate()
                process.wait(timeout=3)


if __name__ == '__main__':
    unittest.main()
