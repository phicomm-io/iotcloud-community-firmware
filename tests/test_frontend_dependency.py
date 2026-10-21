"""Locked candidate acquisition must retain the same release validation boundary."""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
from urllib.error import HTTPError

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/ci'))
import frontend_release
import prepare_frontend


class FrontendDependencyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.lock = dict(schema=1, repository='phicomm-io/phicomm-community-frontend',
                         commit='4' * 40, release='frontend-' + '4' * 40,
                         asset='frontend.tar.gz', sha256='0' * 64)
        self.data = self.archive()
        self.lock['sha256'] = hashlib.sha256(self.data).hexdigest()
        self.lock_path = self.root / 'lock.json'
        self.lock_path.write_text(json.dumps(self.lock))
        self.output = self.root / 'ci/frontend.tar.gz'

    def archive(self, **manifest_changes):
        files = {'platform/index.html': b'platform', 'demos/platform/index.html': b'demo'}
        for model in ('tc1', 'm1', 'a1'):
            for name in (f'devices/{model}/index.html', f'devices/{model}/index.html.gz',
                         f'demos/{model}/index.html'):
                files[name] = name.encode()
        manifest = dict(schema=1, repository=self.lock['repository'], commit=self.lock['commit'],
                        files={name: hashlib.sha256(data).hexdigest() for name, data in files.items()})
        manifest.update(manifest_changes)
        files['manifest.json'] = json.dumps(manifest).encode()
        data = io.BytesIO()
        with tarfile.open(fileobj=data, mode='w:gz') as archive:
            for name, content in files.items():
                member = tarfile.TarInfo(name)
                member.size = len(content)
                archive.addfile(member, io.BytesIO(content))
        return data.getvalue()

    def missing(self, code=404):
        return HTTPError('https://github.com/release', code, 'missing', {}, None)

    def test_release_is_verified_without_building_source(self):
        with patch.object(prepare_frontend, 'urlopen', return_value=io.BytesIO(self.data)), \
                patch.object(prepare_frontend, 'build_source') as build:
            result = prepare_frontend.prepare(self.lock_path, self.output, True)
        build.assert_not_called()
        self.assertEqual(result['source'], 'release')
        self.assertEqual(self.output.read_bytes(), self.data)
        self.assertEqual(json.loads(self.output.with_name('frontend.lock.json').read_text()), self.lock)

    def test_unpublished_release_requires_explicit_candidate_policy(self):
        with patch.object(prepare_frontend, 'urlopen', side_effect=self.missing()), \
                patch.object(prepare_frontend, 'build_source') as build:
            with self.assertRaises(HTTPError):
                prepare_frontend.prepare(self.lock_path, self.output)
        build.assert_not_called()
        self.assertFalse(self.output.exists())

    def test_only_404_can_select_candidate_source(self):
        for code in (401, 403, 429, 500, 503):
            with self.subTest(code=code), \
                    patch.object(prepare_frontend, 'urlopen', side_effect=self.missing(code)), \
                    patch.object(prepare_frontend, 'build_source') as build:
                with self.assertRaises(HTTPError):
                    prepare_frontend.prepare(self.lock_path, self.output, True)
                build.assert_not_called()

    def test_candidate_must_match_the_same_archive_hash(self):
        self.output.parent.mkdir()
        self.output.write_bytes(b'previous verified archive')
        with patch.object(prepare_frontend, 'urlopen', side_effect=self.missing()), \
                patch.object(prepare_frontend, 'build_source', return_value=self.data + b'changed'):
            with self.assertRaisesRegex(ValueError, 'SHA-256 mismatch'):
                prepare_frontend.prepare(self.lock_path, self.output, True)
        self.assertEqual(self.output.read_bytes(), b'previous verified archive')

    def test_exact_candidate_records_provenance_and_is_consumable(self):
        with patch.object(prepare_frontend, 'urlopen', side_effect=self.missing()), \
                patch.object(prepare_frontend, 'build_source', return_value=self.data) as build:
            result = prepare_frontend.prepare(self.lock_path, self.output, True)
        self.assertEqual(build.call_args.args[0], self.lock)
        self.assertEqual(result['source'], 'candidate-source')
        self.assertEqual(result['commit'], self.lock['commit'])
        self.assertEqual(result['sha256'], self.lock['sha256'])
        # The normal firmware consumer verifies the handed-off archive again.
        with patch.dict('os.environ', PHICOMM_FRONTEND_ARCHIVE=str(self.output)):
            bundle = frontend_release.fetch(self.lock_path, self.root / 'bundle')
        self.assertEqual((bundle / 'devices/m1/index.html').read_bytes(), b'devices/m1/index.html')

    def test_archive_hash_does_not_replace_manifest_identity_validation(self):
        wrong_identity = self.archive(commit='9' * 40)
        self.lock['sha256'] = hashlib.sha256(wrong_identity).hexdigest()
        self.lock_path.write_text(json.dumps(self.lock))
        with patch.object(prepare_frontend, 'urlopen', return_value=io.BytesIO(wrong_identity)):
            with self.assertRaisesRegex(ValueError, 'manifest commit/repository mismatch'):
                prepare_frontend.prepare(self.lock_path, self.output)
        self.assertFalse(self.output.exists())

    def test_source_build_rejects_a_different_generated_lock(self):
        source = self.root / 'source'

        def run(args, **kwargs):
            if args[-1] == 'tools/build_release.py':
                (source / 'build').mkdir()
                different = dict(self.lock, commit='9' * 40, release='frontend-' + '9' * 40)
                (source / 'build/frontend.lock.json').write_text(json.dumps(different))

        with patch.object(prepare_frontend.subprocess, 'run', side_effect=run), \
                patch.object(prepare_frontend.subprocess, 'check_output', return_value=self.lock['commit']):
            with self.assertRaisesRegex(ValueError, 'Rebuilt frontend lock differs'):
                prepare_frontend.build_source(self.lock, source)


if __name__ == '__main__':
    with contextlib.redirect_stdout(io.StringIO()):
        unittest.main()
