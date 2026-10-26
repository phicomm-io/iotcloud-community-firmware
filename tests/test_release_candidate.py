"""Candidate releases cannot reach stable publication or automatic OTA routes."""
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import textwrap
import unittest
from unittest.mock import patch
from urllib.error import HTTPError

from cryptography.hazmat.primitives.asymmetric import ec

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/release'))
import archive_pages
import catalog
import import_user_notes
import notes
import publish
import verify
import verify_user_notes_release
from products import asset_stem, targets
from release_version import parse_tag, parse_version, version_key


def release(tag):
    identity = parse_tag(tag)
    value = dict(schema=1, tag=tag, version=identity.version, commit='c' * 40, models={})
    if identity.prerelease:
        value.update(prerelease=True, channel='candidate')
    return value


class CandidateVersionTests(unittest.TestCase):
    def test_ordering_and_strict_release_grammar(self):
        versions = ['0.9.27', '1.0.0-rc', '1.0.0-rc.0', '1.0.0-rc.1', '1.0.0-rc.2',
                    '1.0.0-rc.10', '1.0.0', '1.0.1-rc', '1.1.0']
        self.assertEqual(sorted(reversed(versions), key=version_key), versions)
        for value in versions:
            self.assertEqual(parse_version(value), parse_tag('v' + value))
            self.assertEqual(parse_tag('v' + value).core, tuple(map(int, value.split('-')[0].split('.'))))
        for value in ['01.0.0', '1.00.0', '1.0.00', '1.0.0-rc.01', '1.0.0-rc.-1',
                      '1.0.0-rc.1.0', '1.0.0-rc.', '1.0.0-RC', '1.0.0-beta', '1.0.0+meta',
                      '1.0.0-rc+meta', '1.0.0\n', '１.0.0', '1.0.0-rc.١', '1.0.0-abcdef']:
            with self.subTest(value=value), self.assertRaises(ValueError):
                parse_version(value)
        with self.assertRaises(ValueError):
            parse_tag('1.0.0-rc')

    def test_release_note_section_boundaries_and_numbered_candidates(self):
        sections = [('v1.0.0', 'Stable firmware release with complete user-visible changes.'),
                    ('v1.0.0-rc.1', 'Second candidate contains reviewed device features and fixes.'),
                    ('v1.0.0-rc', 'First candidate contains reviewed device features and fixes.'),
                    ('v0.9.27', 'Archived stable release with complete user-visible changes.')]
        text = '\n\n'.join('## ' + tag + '\n\n' + body for tag, body in sections)
        for tag, body in sections:
            self.assertEqual(notes.extract(text, tag), body + '\n')
        with self.assertRaises(ValueError):
            notes.extract(text + '\n## v1.0.0-rc\nDuplicate candidate notes are invalid.', 'v1.0.0-rc')

    def test_candidate_note_import_is_explicit_and_preserves_immutable_originals(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            archive, data = root / 'archive', root / 'data'
            (data / 'sources').mkdir(parents=True)
            stable, candidate = release('v0.9.27'), release('v1.0.0-rc')
            for value in (stable, candidate):
                value['notes'] = f"/releases/{value['tag']}/notes.md"
                target = archive / value['notes'].lstrip('/')
                target.parent.mkdir(parents=True)
                target.write_text('Reviewed user-visible features for ' + value['tag'] + '.\n')
            (data / 'sources/v0.9.27.md').write_bytes((archive / stable['notes'].lstrip('/')).read_bytes())
            (data / 'release-order.json').write_text(json.dumps(dict(schema=1, releases=[dict(version='0.9.27', previous=None, branch='main', channel='stable')])))
            catalog_path = archive / 'releases/index.json'
            catalog_path.write_text(json.dumps(dict(schema=1, stable=stable['tag'], releases=[stable])))
            original = catalog_path.read_bytes()
            (archive / 'releases/candidates.json').write_text(json.dumps(dict(schema=1, channel='candidate', releases=[candidate])))
            import_user_notes.import_archive(archive, data)
            self.assertEqual(len(json.loads((data / 'release-order.json').read_bytes())['releases']), 1)
            import_user_notes.import_archive(archive, data, '0.9.27', True)
            latest = json.loads((data / 'release-order.json').read_bytes())['releases'][-1]
            self.assertEqual(latest, dict(version='1.0.0-rc', previous='0.9.27', branch='main', channel='candidate'))
            self.assertEqual(catalog_path.read_bytes(), original)
            (archive / candidate['notes'].lstrip('/')).write_text('Changed original candidate notes.')
            with self.assertRaisesRegex(ValueError, 'Immutable release-note resource'):
                import_user_notes.import_archive(archive, data, include_candidates=True)


class CandidateSigningTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.directory = Path(self.tmp.name)
        self.key = ec.generate_private_key(ec.SECP256R1())
        public = self.key.public_key().public_numbers()
        raw = public.x.to_bytes(32, 'big') + public.y.to_bytes(32, 'big')
        self.keyring = self.directory / 'keyring.h'
        self.keyring.write_text('{1, {' + ','.join(f'0x{x:02x}' for x in raw) + '}}')

    def test_candidate_channel_and_full_version_are_authenticated(self):
        for tag in ('v1.0.0-rc', 'v1.0.0-rc.0', 'v1.0.0-rc.10'):
            signed = verify.package.sign(b'image', 'm1', tag, 'c' * 40, self.key)
            result = verify.verify(signed, 'm1', tag, 'c' * 40, keyring_path=self.keyring)
            self.assertEqual(result['channel'], 2)
            self.assertTrue(result['prerelease'])
            self.assertEqual(result['version'], tag)
            self.assertEqual(struct.unpack_from('<Q', signed, 24)[0], 1 << 40)
            with self.assertRaisesRegex(ValueError, 'version or commit'):
                verify.verify(signed, 'm1', 'v1.0.0', 'c' * 40, keyring_path=self.keyring)
            changed = bytearray(signed)
            changed[116] = 1
            with self.assertRaisesRegex(ValueError, 'candidate channel'):
                verify.package.verify_package(bytes(changed), 'm1', self.keyring)
            changed = bytearray(signed)
            changed[64 + len(tag) - 1] = ord('1') if tag.endswith('0') else ord('2')
            with self.assertRaisesRegex(ValueError, 'signature|version'):
                verify.package.verify_package(bytes(changed), 'm1', self.keyring)
        stable = verify.package.sign(b'image', 'm1', 'v0.9.27', 'c' * 40, self.key)
        self.assertEqual(verify.package.verify_package(stable, 'm1', self.keyring)['channel'], 1)
        with self.assertRaisesRegex(ValueError, 'candidate channel'):
            verify.package.header(b'image', 'm1', '1.0.0-rc', 'c' * 40, channel=1)

    def test_complete_candidate_catalog_binds_all_ten_signed_variants(self):
        tag = 'v1.0.0-rc'
        for model, variant in targets(tag):
            stem = asset_stem(model, variant)
            payload = bytearray(384)
            payload[0] = 0xe9
            struct.pack_into('<I', payload, 32, 0xabcd5432)
            payload[288:320] = b'BRY88AB151K'.ljust(32, b'\0')
            payload[320:352] = ('PHICOMM-DAIKIN-1:' + variant).encode().ljust(32, b'\0')
            (self.directory / (stem + '.all.bin')).write_bytes(b'full image')
            (self.directory / (stem + '.ota.bin.raw')).write_bytes(payload)
            (self.directory / (stem + '.ota.bin.signed')).write_bytes(
                verify.package.sign(bytes(payload), model, tag, 'c' * 40, self.key))
        value = catalog.make(tag, 'c' * 40, self.directory, 'Reviewed candidate notes.',
                             require_variants=True, keyring_path=self.keyring)
        self.assertEqual(value['channel'], 'candidate')
        self.assertTrue(value['prerelease'])
        self.assertEqual(sum(1 + len(m.get('variants', {})) for m in value['models'].values()), 10)
        self.assertTrue(all(m['path'].startswith('/releases/' + tag + '/') for m in value['models'].values()))
        objects = {'releases/' + tag + '/' + file.name: file.read_bytes() for file in self.directory.iterdir() if file.is_file()}
        def signed(body, model, version, commit, variant):
            return verify.verify(body, model, version, commit, variant, self.keyring)
        with patch.object(publish, 'get', side_effect=objects.get), patch.object(publish, 'verify_signature', side_effect=signed) as checked, \
             patch.object(publish.urllib.request, 'build_opener', side_effect=AssertionError('production mirrors')):
            publish.verify_candidate(value, self.directory)
            self.assertEqual(checked.call_count, 10)
            self.assertEqual({call.kwargs['variant'] for call in checked.call_args_list}, {'full', 'hub', 'no-frontend-resources', 'fresh-air', 'fresh-air-no-frontend-resources'})
        key = 'releases/' + tag + '/phicomm_m1.ota.bin.signed'
        good = objects[key]
        for bad in (None, good[:-1], good[:-1] + bytes([good[-1] ^ 1])):
            objects[key] = bad
            with self.subTest(bad=bad is None), patch.object(publish, 'get', side_effect=objects.get), \
                 patch.object(publish, 'verify_signature', side_effect=signed):
                with self.assertRaisesRegex(ValueError, 'readback mismatch'):
                    publish.verify_candidate(value, self.directory)
        objects[key] = good
        altered = json.loads(json.dumps(value))
        altered['models']['m1']['downloads']['signed']['sha256'] = '0' * 64
        with patch.object(publish, 'get', side_effect=objects.get), patch.object(publish, 'verify_signature', side_effect=signed):
            with self.assertRaisesRegex(ValueError, 'size or hash mismatch'):
                publish.verify_candidate(altered, self.directory)
        with patch.object(publish, 'get', side_effect=objects.get), patch.object(publish, 'verify_signature', side_effect=ValueError('untrusted signature')):
            with self.assertRaisesRegex(ValueError, 'untrusted signature'):
                publish.verify_candidate(value, self.directory)


class CandidatePublicationTests(unittest.TestCase):
    def run_publication(self, candidate, arguments=(), stored=None):
        stored = {} if stored is None else stored
        writes, reads = [], []
        def get(key):
            reads.append(key)
            return stored.get(key)
        def put(key, body, immutable=False):
            if immutable and key in stored and stored[key] != body:
                raise ValueError('immutable object conflict: ' + key)
            writes.append((key, immutable))
            stored[key] = body
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / 'release.json').write_text(json.dumps(candidate))
            (Path(tmp) / 'firmware.bin').write_bytes(b'candidate')
            with patch.object(publish, 'usage', return_value=0), patch.object(publish, 'get', side_effect=get), \
                 patch.object(publish, 'put', side_effect=put), patch.object(publish, 'verify'), \
                 patch.object(publish, 'smoke', side_effect=AssertionError('production smoke must not run')), \
                 patch.object(sys, 'argv', ['publish.py', tmp, *arguments]):
                publish.main()
        return stored, writes, reads

    def test_promote_rejects_candidate_before_any_remote_access(self):
        for flags in [('--promote',), ('--promote', '--archive-only'), ('--promote', '--candidate')]:
            with tempfile.TemporaryDirectory() as tmp:
                (Path(tmp) / 'release.json').write_text(json.dumps(release('v1.0.0-rc')))
                with patch.object(publish, 'usage', side_effect=AssertionError('remote usage')), \
                     patch.object(publish, 'get', side_effect=AssertionError('remote read')), \
                     patch.object(publish, 'put', side_effect=AssertionError('remote write')), \
                     patch.object(sys, 'argv', ['publish.py', tmp, *flags]):
                    with self.assertRaisesRegex(ValueError, 'cannot promote'):
                        publish.main()

    def test_candidate_staging_and_catalog_leave_every_stable_pointer_unchanged(self):
        original = {'stable.json': json.dumps(release('v0.9.27')).encode(),
                    'releases/index.json': b'{"schema":1,"stable":"v0.9.27","releases":[]}',
                    'bridge-v1.0.0.json': b'frozen bridge', 'rollback/stable.json': b'rollback'}
        stored, writes, reads = self.run_publication(release('v1.0.0-rc'), stored=dict(original))
        self.assertTrue(writes)
        self.assertTrue(all(key.startswith('releases/v1.0.0-rc/') and immutable for key, immutable in writes))
        self.assertTrue(all(key.startswith('releases/v1.0.0-rc/') for key in reads))
        for tag in ('v1.0.0-rc.10', 'v1.0.0-rc.2', 'v1.0.0-rc', 'v1.0.0-rc.0'):
            stored, _, _ = self.run_publication(release(tag), stored=stored)
            stored, writes, reads = self.run_publication(release(tag), ('--candidate',), stored)
            self.assertEqual(writes, [('releases/candidates.json', False)])
            self.assertEqual(reads[-1], 'releases/candidates.json')
            self.assertTrue(all(key.startswith('releases/' + tag + '/') for key in reads[:-1]))
        self.assertEqual([r['tag'] for r in json.loads(stored['releases/candidates.json'])['releases']],
                         ['v1.0.0-rc.10', 'v1.0.0-rc.2', 'v1.0.0-rc.0', 'v1.0.0-rc'])
        self.assertNotIn('stable', json.loads(stored['releases/candidates.json']))
        self.assertEqual({key: stored[key] for key in original}, original)
        changed = release('v1.0.0-rc')
        changed['commit'] = 'd' * 40
        stored['releases/v1.0.0-rc/release.json'] = json.dumps(changed).encode()
        with self.assertRaisesRegex(ValueError, 'immutable candidate metadata conflict'):
            self.run_publication(changed, ('--candidate',), stored)

    def test_usage_failure_stops_candidate_before_any_storage_access(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / 'release.json').write_text(json.dumps(release('v1.0.0-rc')))
            with patch.object(publish, 'usage', side_effect=RuntimeError('R2 exceeds 10 GB')), \
                 patch.object(publish, 'get', side_effect=AssertionError('storage accessed')), \
                 patch.object(publish, 'put', side_effect=AssertionError('storage written')), \
                 patch.object(sys, 'argv', ['publish.py', tmp]):
                with self.assertRaisesRegex(RuntimeError, 'exceeds 10 GB'):
                    publish.main()

    def test_candidate_cannot_generate_latest_or_production_smoke_routes(self):
        value = release('v1.0.0-rc')
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)
            existing = output / 'ota/m1/latest.json'
            existing.parent.mkdir(parents=True)
            existing.write_bytes(b'old stable manifest')
            with self.assertRaisesRegex(ValueError, 'cannot be the stable'):
                archive_pages.write_latest(dict(stable=value['tag'], releases=[value]), output)
            self.assertEqual(existing.read_bytes(), b'old stable manifest')
        with patch.object(publish.urllib.request, 'build_opener', side_effect=AssertionError('network')):
            with self.assertRaisesRegex(ValueError, 'no production latest'):
                publish.smoke(value)

    def test_staged_candidate_notes_keep_the_v0927_baseline_and_retain_previous_candidates(self):
        for previous_candidate in (False, True):
            with self.subTest(previous_candidate=previous_candidate), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                retained, staged_release = root / 'retained', root / 'release'
                (retained / 'releases').mkdir(parents=True)
                staged_release.mkdir()
                stable = release('v0.9.27')
                first, target = release('v1.0.0-rc'), release('v1.0.0-rc.1' if previous_candidate else 'v1.0.0-rc')
                stable_catalog = dict(schema=1, stable=stable['tag'], releases=[stable])
                original = json.dumps(stable_catalog).encode()
                (retained / 'releases/index.json').write_bytes(original)
                (staged_release / 'release.json').write_text(json.dumps(target))
                if previous_candidate:
                    (retained / 'releases/candidates.json').write_text(json.dumps(dict(schema=1, channel='candidate', releases=[first])))
                    (retained / 'releases' / first['tag']).mkdir()
                    (retained / 'releases' / first['tag'] / 'notes.md').write_text('Retained candidate notes.')
                def compile_notes(archive, formal):
                    self.assertTrue(formal)
                    combined = json.loads((archive / 'releases/index.json').read_bytes())
                    self.assertEqual(combined['stable'], 'v0.9.27')
                    self.assertEqual({r['tag'] for r in combined['releases']},
                                     {'v0.9.27', target['tag'], *(['v1.0.0-rc'] if previous_candidate else [])})
                    if previous_candidate:
                        self.assertTrue((archive / 'releases/v1.0.0-rc/notes.md').exists())
                    return dict(releases=[dict(tag=target['tag'], history_status='reviewed', baseline_confirmed=True,
                                              coverage=[dict(status='complete', locales=['zh-CN', 'en'])])]), b''
                with patch.object(verify_user_notes_release, 'compile_manifest', side_effect=compile_notes) as checked:
                    verify_user_notes_release.verify_staged(staged_release, retained)
                    checked.assert_called_once()
                self.assertEqual((retained / 'releases/index.json').read_bytes(), original)

    def test_candidate_github_workflow_executes_only_prerelease_commands(self):
        workflow = (ROOT / '.github/workflows/build.yml').read_text()
        def step(name):
            block = workflow.split('      - name: ' + name + '\n', 1)[1]
            return re.split(r'^      - ', block, maxsplit=1, flags=re.M)[0]
        block = step('Publish an isolated GitHub prerelease and candidate catalog')
        self.assertIn("if: steps.release-channel.outputs.channel == 'candidate'", block)
        self.assertIn("if: steps.release-channel.outputs.channel == 'stable'", step('Stage GitHub release and immutable OTA assets'))
        self.assertIn("if: steps.release-channel.outputs.channel == 'stable'", step('Build and publish public Pages allowlist'))
        commands = textwrap.dedent(block.split('        run: |\n', 1)[1])
        for existing in (False, True):
            with self.subTest(existing=existing), tempfile.TemporaryDirectory() as tmp:
                directory = Path(tmp)
                (directory / 'release').mkdir()
                (directory / 'release/image.bin').write_bytes(b'image')
                (directory / 'release/SHA256SUMS').write_bytes(b'checksums')
                executable = directory / 'bin'
                executable.mkdir()
                stub = '#!' + sys.executable + '\n' + textwrap.dedent('''\
                    import json, os, pathlib, shutil, sys
                    name = pathlib.Path(sys.argv[0]).name
                    args = sys.argv[1:]
                    with open(os.environ['CALL_LOG'], 'a') as log: log.write(json.dumps([name, *args]) + '\\n')
                    if name == 'gh' and args[:2] == ['release', 'view']: sys.exit(0 if os.environ['EXISTING'] == '1' else 1)
                    if name == 'gh' and args[:2] == ['release', 'download']:
                        pathlib.Path('existing').mkdir()
                        for file in pathlib.Path('release').iterdir(): shutil.copyfile(file, pathlib.Path('existing') / file.name)
                    ''')
                for name in ('gh', 'python3'):
                    path = executable / name
                    path.write_text(stub)
                    path.chmod(0o755)
                env = dict(os.environ, PATH=str(executable) + ':' + os.environ['PATH'],
                           TAG_NAME='v1.0.0-rc', EXISTING=str(int(existing)), CALL_LOG=str(directory / 'calls'))
                subprocess.run(['bash', '-euo', 'pipefail', '-c', commands], cwd=directory, env=env, check=True)
                calls = [json.loads(line) for line in (directory / 'calls').read_text().splitlines()]
                mutations = [args for command, *args in calls if command == 'gh' and args[:2] in (['release', 'create'], ['release', 'edit'])]
                self.assertTrue(mutations)
                for args in mutations:
                    self.assertIn('--prerelease', args)
                    self.assertIn('--latest=false', args)
                python = [args for command, *args in calls if command == 'python3']
                self.assertEqual(python, [['tools/release/publish.py', 'release'], ['tools/release/publish.py', 'release', '--candidate']])


if __name__ == '__main__':
    unittest.main()
