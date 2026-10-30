"""Native S30 publication must preserve payloads and source identity."""
import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/release'))
from s30_assets import collect


class S30ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.run = Path(self.temp.name) / 'run'
        self.out = Path(self.temp.name) / 'out'
        (self.run / 'firmware').mkdir(parents=True)
        self.commit = 'abc123' + '0' * 34
        self.base = '1.0.0-rc.4'
        self.version = self.base + '-abc123'
        self.manifest = dict(version=self.version, project_head=self.commit, dirty=False,
            version_metadata=dict(version=self.version, base_version=self.base,
                git_commit=self.commit, dirty=False),
            flash_layout=dict(name='s30-v1b-stock-v1', bootloader_included=False))
        self.write_manifest()
        (self.run / 'firmware-validation.json').write_text(json.dumps(dict(status='PASS')))
        checks = []
        self.bodies = {}
        for role, suffix in (('OTA', 'rbl'), ('STOCK_UA', 'bin'), ('app', 'bin'), ('UG', 'bin')):
            filename = f'BL2028N_{role}_{self.version}.{suffix}'
            body = ('native-payload-' + role).encode()
            (self.run / 'firmware' / filename).write_bytes(body)
            checks.append(hashlib.sha256(body).hexdigest() + '  ' + filename + '\n')
            self.bodies[f'BL2028N_{role}_{self.base}.{suffix}'] = body
        (self.run / 'firmware/SHA256SUMS').write_text(''.join(checks))

    def write_manifest(self):
        (self.run / 'build-manifest.json').write_text(json.dumps(self.manifest))

    def test_native_release_aliases_preserve_bytes_and_git_version(self):
        collect(self.run, self.out)
        for name, body in self.bodies.items():
            self.assertEqual((self.out / name).read_bytes(), body)
        manifest = json.loads((self.out / 'build-s30.json').read_text())
        self.assertEqual(manifest['version'], self.version)
        self.assertEqual(manifest['project_head'], self.commit)
        self.assertEqual(set(manifest['release_files']), set(self.bodies))
        for line in (self.out / 'SHA256SUMS-S30').read_text().splitlines():
            checksum, name = line.split('  ', 1)
            self.assertEqual(hashlib.sha256((self.out / name).read_bytes()).hexdigest(), checksum)
        collect(self.run, self.out)  # exact retries are safe

    def test_rejects_dirty_mismatched_identity_and_wrong_layout(self):
        original = copy.deepcopy(self.manifest)
        changes = [('dirty', True), ('project_head', 'f' * 40), ('version', self.version + '-dirty'),
                   ('flash_layout', dict(name='openbeken', bootloader_included=True))]
        for key, value in changes:
            self.manifest = dict(original, **{key: value})
            self.write_manifest()
            with self.subTest(key=key), self.assertRaises(ValueError):
                collect(self.run, self.out)
        self.manifest = copy.deepcopy(original)
        self.manifest['version_metadata']['base_version'] = '../../outside'
        self.write_manifest()
        with self.assertRaises(ValueError):
            collect(self.run, self.out)
        self.assertFalse(self.out.exists())

    def test_rejects_corrupt_or_unverified_native_image(self):
        validation = self.run / 'firmware-validation.json'
        validation.write_text(json.dumps(dict(status='FAIL')))
        with self.assertRaises(ValueError):
            collect(self.run, self.out)
        validation.write_text(json.dumps(dict(status='PASS')))
        image = next((self.run / 'firmware').glob('*.rbl'))
        image.write_bytes(b'corrupted')
        with self.assertRaisesRegex(ValueError, 'checksums differ'):
            collect(self.run, self.out)
        self.assertFalse(self.out.exists())

    def test_refuses_replacement_before_writing_any_asset(self):
        self.out.mkdir()
        existing = self.out / 'validation-s30.json'
        existing.write_bytes(b'other release')
        with self.assertRaisesRegex(ValueError, 'Refuse to replace'):
            collect(self.run, self.out)
        self.assertEqual(list(self.out.iterdir()), [existing])
        self.assertEqual(existing.read_bytes(), b'other release')


if __name__ == '__main__':
    unittest.main()
