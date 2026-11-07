"""Validate reusable boot backups without device access or changing provenance."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import flash_screen

BOOT = bytes((i * 137 ^ (i >> 8)) & 255 for i in range(0x14000))


class BackupReuseTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        digest = hashlib.sha256(BOOT).hexdigest()
        # Use a synthetic reviewed loader for this offline test only.
        known = patch.object(flash_screen, 'SUPPORTED_BOOT_SHA256', digest)
        known.start()
        self.addCleanup(known.stop)
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

    def test_default_still_requires_same_session(self):
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

    def test_matching_unreviewed_loader_rejected(self):
        data = bytearray(BOOT)
        data[45000] ^= 1
        digest = hashlib.sha256(data).hexdigest()
        for number in (1, 2):
            (self.directory / f'boot-read{number}.bin').write_bytes(data)
        for read in self.report['reads']:
            read['sha256'] = digest
        self.write_report()
        with self.assertRaisesRegex(ValueError, 'Unrecognized bootloader'):
            flash_screen.validate_backup(self.directory, 5678, reuse=True)

    def test_unverified_report_rejected(self):
        self.report['matching'] = False
        self.write_report()
        with self.assertRaises(ValueError):
            flash_screen.validate_backup(self.directory, 5678, reuse=True)


if __name__ == '__main__':
    unittest.main()
