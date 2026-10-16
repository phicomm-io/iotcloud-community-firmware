#!/usr/bin/env python3
"""Signed OTA verifier and pre-network candidate validation regressions."""
import hashlib
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from cryptography.hazmat.primitives.asymmetric import ec

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('package', ROOT / 'tools/ota/package.py')
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)
sys.path.insert(0, str(ROOT / 'tests/device'))
import deploy


class PackageVerifier(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.key = ec.generate_private_key(ec.SECP256R1())
        public = self.key.public_key().public_numbers()
        raw_key = public.x.to_bytes(32, 'big') + public.y.to_bytes(32, 'big')
        values = ','.join(f'0x{byte:02x}' for byte in raw_key)
        self.keyring = self.root / 'keyring.h'
        self.keyring.write_text(
            '#define OTA_KEY_COUNT 1\nstatic const struct {unsigned id; unsigned char key[64];} '
            f'ota_keys[] = {{{{1, {{{values}}}}}}};\n')
        self.payload = b'candidate payload for OTA verifier tests'
        self.commit = 'a' * 40
        self.signed = package.sign(self.payload, 'a1', '0.9.4', self.commit, self.key)

    def tearDown(self):
        self.temp.cleanup()

    def test_trusted_package_and_tampering(self):
        verified = package.verify_package(self.signed, 'a1', self.keyring)
        self.assertEqual(verified['version'], '0.9.4')
        self.assertEqual(verified['commit'], self.commit)
        self.assertEqual(verified['payload_sha256'], hashlib.sha256(self.payload).hexdigest())

        signature_tamper = bytearray(self.signed)
        signature_tamper[128] ^= 1
        with self.assertRaisesRegex(ValueError, 'signature'):
            package.verify_package(bytes(signature_tamper), 'a1', self.keyring)

        model_tamper = bytearray(self.signed)
        model_tamper[16] = 2
        with self.assertRaisesRegex(ValueError, 'model'):
            package.verify_package(bytes(model_tamper), 'a1', self.keyring)

        version_tamper = bytearray(self.signed)
        version_tamper[64:69] = b'0.9.5'
        with self.assertRaisesRegex(ValueError, 'version'):
            package.verify_package(bytes(version_tamper), 'a1', self.keyring)

    def test_manifest_mismatch_is_rejected_before_device_creation(self):
        candidate = self.root / 'candidate'
        candidate.mkdir()
        name = 'phicomm_a1.ota.bin'
        (candidate / name).write_bytes(self.payload)
        (candidate / (name + '.signed')).write_bytes(self.signed)
        digest = hashlib.sha256(self.payload).hexdigest()
        manifest = {
            'target': 'A1@MK3080B@moc',
            'files': {name: digest},
            'image': {'size': len(self.payload), 'sha256': digest},
            'base_version': '0.9.5',
            'git_commit': self.commit,
            'dirty': False,
            'version': '0.9.5-aaaaaa',
        }
        (candidate / 'manifest.json').write_text(json.dumps(manifest))

        with patch.object(deploy, 'verify_package',
                          side_effect=lambda image, model, keyring_path=None:
                          package.verify_package(image, model, self.keyring)):
            with self.assertRaisesRegex(ValueError, 'base version'):
                deploy.validate_candidate('a1', candidate, self.keyring)

        with patch.object(deploy, 'verify_package',
                          side_effect=lambda image, model, keyring_path=None:
                          package.verify_package(image, model, self.keyring)):
            with patch.object(deploy, 'Device', side_effect=AssertionError('device accessed')):
                with patch.object(sys, 'argv', ['deploy.py', '--model', 'a1', '--ip', '127.0.0.1',
                                                '--device-id', 'test', '--candidate', str(candidate),
                                                '--evidence', str(self.root / 'evidence')]):
                    with self.assertRaisesRegex(ValueError, 'base version'):
                        deploy.main()


if __name__ == '__main__':
    unittest.main()
