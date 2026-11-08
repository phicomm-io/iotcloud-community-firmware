"""Exercise optional backups and complete-app installation without hardware."""
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

import msgpack

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import flash_screen

BOOT = bytes((i * 137 ^ (i >> 8)) & 255 for i in range(0x14000))


class BackupReuseTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        digest = hashlib.sha256(BOOT).hexdigest()
        self.report = {'matching': True, 'mcu_session': 1234, 'reads': [
            {'sha256': digest}, {'sha256': digest}]}
        for number in (1, 2):
            (self.directory / f'boot-read{number}.bin').write_bytes(BOOT)
        self.write_report()

    def write_report(self):
        (self.directory / 'boot-backup.json').write_text(json.dumps(self.report))

    def test_explicit_reuse_preserves_original_session(self):
        before = (self.directory / 'boot-backup.json').read_bytes()
        flash_screen.validate_backup(self.directory, 5678, reuse=True)
        self.assertEqual((self.directory / 'boot-backup.json').read_bytes(), before)

    def test_explicit_backup_checks_same_session_unless_reused(self):
        with self.assertRaises(ValueError):
            flash_screen.validate_backup(self.directory, 5678)

    def test_different_reads_rejected(self):
        data = bytearray(BOOT)
        data[45000] ^= 1
        (self.directory / 'boot-read2.bin').write_bytes(data)
        with self.assertRaises(ValueError):
            flash_screen.validate_backup(self.directory, 5678, reuse=True)

    def test_incomplete_read_rejected(self):
        (self.directory / 'boot-read1.bin').write_bytes(BOOT[:-1])
        with self.assertRaises(ValueError):
            flash_screen.validate_backup(self.directory, 5678, reuse=True)

    def test_matching_different_loader_accepted(self):
        data = bytearray(BOOT)
        data[45000] ^= 1
        digest = hashlib.sha256(data).hexdigest()
        for number in (1, 2):
            (self.directory / f'boot-read{number}.bin').write_bytes(data)
        for read in self.report['reads']:
            read['sha256'] = digest
        self.write_report()
        self.assertEqual(flash_screen.validate_backup(self.directory, 5678, reuse=True), digest)

    def test_unverified_report_rejected(self):
        self.report['matching'] = False
        self.write_report()
        with self.assertRaises(ValueError):
            flash_screen.validate_backup(self.directory, 5678, reuse=True)


class DirectInstallTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.image = self.directory / 'full-app.bin'
        data = bytearray(b'\xff' * 0x67000)
        struct.pack_into('<II', data, 0, 0x20010000, 0x14401)
        marker = b'iotcloud-screen/BRY88BBA15G;application=0x14000;install=factory-uart;'
        data[64:64 + len(marker)] = marker
        self.image.write_bytes(data)
        self.sha = hashlib.sha256(data).hexdigest()
        self.md5 = hashlib.md5(data).hexdigest()
        self.calls = []
        self.community = False
        self.started = False
        self.installed = False
        self.uploaded = False
        self.bad_readback = False
        self.exports = 0
        self.boot_modes = ['normal']
        owner = self

        class Device:
            url = 'http://test-device'
            headers = {}

            def __init__(self, url):
                self.url = url

            def command(self, action, params=None):
                owner.calls.append((action, params))
                recovery = {'available': owner.community, 'session': 1,
                            'ota_activation': owner.community, 'boot_exported': 0}
                if owner.installed or (owner.started and not owner.community):
                    mode = owner.boot_modes[0]
                    if len(owner.boot_modes) > 1:
                        owner.boot_modes.pop(0)
                    recovery.update(available=True, session=2, image_md5=owner.md5, mode=mode)
                if action == 'status.get':
                    return {'hardware_model': 'BRY88BBA15G', 'screen': {
                        'mcu_version': 'SV.42.99.01.00', 'recovery': recovery,
                        'ota': {'state': 'staged_verified' if owner.started else 'idle',
                                'sent': 0x67000 if owner.started else 0}}}
                if action == 'screen.recovery.get':
                    return recovery
                if action == 'screen.update.start':
                    owner.started = True
                    return {'accepted': True}
                if action == 'screen.update.install':
                    owner.installed = True
                    return {'accepted': True}
                raise AssertionError(f'Unexpected command: {action}')

        def upload(request, timeout):
            self.assertEqual(timeout, 30)
            self.assertEqual(request.full_url, 'http://test-device/api/v1/screen/ota/upload')
            self.assertEqual(request.data, self.image.read_bytes())
            owner.uploaded = True
            return io.BytesIO(msgpack.packb({'code': 'ok', 'data': {
                'sha256': 'bad' if owner.bad_readback else owner.sha}}, use_bin_type=True))

        for mock in (patch.object(flash_screen, 'Device', Device),
                     patch.object(flash_screen.urllib.request, 'urlopen', upload),
                     patch('sys.stdout', io.StringIO())):
            mock.start()
            self.addCleanup(mock.stop)

    def run_tool(self, mode='install'):
        argv = ['flash_screen.py', '--url', 'http://test-device', '--image', str(self.image),
                '--mode', mode, '--log', str(self.directory / 'install.jsonl')]
        with patch.object(sys, 'argv', argv):
            flash_screen.main()

    def test_stock_full_application_without_backup_or_version_whitelist(self):
        self.run_tool()
        self.assertTrue(self.uploaded)
        self.assertIn(('screen.update.start', {'mode': 'factory-bootstrap', 'sha256': self.sha}), self.calls)
        self.assertFalse(self.installed)  # Original UART receiver installs itself.
        records = [json.loads(line) for line in (self.directory / 'install.jsonl').read_text().splitlines()]
        self.assertEqual(records[-1]['event'], 'verified_running')
        self.assertEqual(records[-1]['data']['image_md5'], self.md5)

    def test_explicit_factory_mode_also_accepts_complete_application(self):
        self.run_tool('factory-bootstrap')
        self.assertTrue(self.started)

    def test_community_install_needs_no_boot_export_or_backup(self):
        self.community = True
        self.run_tool()
        self.assertIn(('screen.update.start', {'mode': 'stage'}), self.calls)
        self.assertIn(('screen.update.install', {'sha256': self.sha}), self.calls)
        self.assertTrue(self.installed)

    def test_stock_matching_digest_waits_for_normal_startup(self):
        self.boot_modes = ['window', 'normal']
        with patch.object(flash_screen.time, 'sleep'):
            self.run_tool()
        records = [json.loads(line) for line in (self.directory / 'install.jsonl').read_text().splitlines()]
        self.assertEqual(records[-1]['data']['mode'], 'normal')

    def test_stock_matching_digest_does_not_accept_startup_fault(self):
        self.boot_modes = ['fault_recovery']
        with self.assertRaisesRegex(RuntimeError, 'failed to start normally'):
            self.run_tool()

    def test_community_matching_digest_does_not_accept_startup_fault(self):
        self.community = True
        self.boot_modes = ['fault_recovery']
        with self.assertRaisesRegex(RuntimeError, 'failed to start normally'):
            self.run_tool()

    def test_stage_only_on_stock_rejected_before_upload(self):
        with self.assertRaisesRegex(ValueError, 'auto-installs'):
            self.run_tool('stage')
        self.assertFalse(self.uploaded)

    def test_bad_esp_readback_cannot_start_uart_transfer(self):
        self.bad_readback = True
        with self.assertRaisesRegex(ValueError, 'readback'):
            self.run_tool()
        self.assertFalse(self.started)

    def test_wrong_vectors_rejected_before_device_access(self):
        data = bytearray(self.image.read_bytes())
        struct.pack_into('<I', data, 4, 0x1001)
        self.image.write_bytes(data)
        with self.assertRaisesRegex(ValueError, 'vectors'):
            self.run_tool()
        self.assertFalse(self.calls)


if __name__ == '__main__':
    unittest.main()
