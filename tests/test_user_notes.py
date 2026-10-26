"""Static migration/review guarantees; no firmware build or device mutations."""
import base64
import copy
import hashlib
import importlib.util
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/release'))
import user_notes as notes


class UserNotesTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.data = Path(self.tmp.name) / 'data'
        shutil.copytree(notes.DATA, self.data)
        # Tests exercise the fixed, published historical snapshot. A prepared
        # future version must not silently enter its records or public results.
        known = {r['version'] for r in notes.read(self.data / 'catalog.snapshot.json')['releases']}
        order = notes.read(self.data / 'release-order.json')
        order['releases'] = [r for r in order['releases'] if r['version'] in known]
        (self.data / 'release-order.json').write_bytes(notes.encoded(order))
        changes = notes.read(self.data / 'changes.json')
        changes['changes'] = [c for c in changes['changes'] if c['version'] in known]
        (self.data / 'changes.json').write_bytes(notes.encoded(changes))

    def change(self, name, edit):
        path = self.data / name
        document = json.loads(path.read_bytes())
        edit(document)
        path.write_bytes(notes.encoded(document))

    def test_migration_is_deterministic_and_does_not_change_raw_sources(self):
        before = {p.name: p.read_bytes() for p in (self.data / 'sources').glob('*.md')}
        first = notes.write_records(self.data)
        self.assertEqual(first, notes.write_records(self.data))
        self.assertEqual(len(first['records']), 142)
        self.assertEqual(len({r['block_id'] for r in first['records']}), 142)
        self.assertEqual(before, {p.name: p.read_bytes() for p in (self.data / 'sources').glob('*.md')})
        self.assertEqual((self.data / 'records.json').read_bytes(), notes.encoded(first))

    def test_raw_change_invalidates_the_whole_release_review(self):
        path = self.data / 'sources/v0.9.23.md'
        path.write_bytes(path.read_bytes() + b'\nChanged original\n')
        rows = [r for r in notes.migrate(self.data)['records'] if r['tag'] == 'v0.9.23']
        self.assertTrue(all(r['review']['status'] == 'pending' for r in rows))
        self.assertTrue(any(r['review']['invalidated'] for r in rows))
        with self.assertRaisesRegex(ValueError, 'provenance'):
            notes.compile_manifest(self.data)

    def test_public_manifest_has_only_reviewed_prose_without_internal_records(self):
        manifest, _ = notes.compile_manifest(self.data)
        self.assertEqual(len(manifest['releases']), 23)
        self.assertEqual(len(manifest['changes']), 81)
        serialized = json.dumps(manifest)
        for private in ('block_sha256', 'release_sha256', 'source_refs', 'change_refs', 'invalidated'):
            self.assertNotIn(private, serialized)
        self.assertTrue(all(c['review_status'] == 'approved' for c in manifest['changes']))
        pending = notes.pending_report(self.data)['pending']
        self.assertEqual([r['block_id'] for r in pending], ['v0.9.0:b001', 'v0.9.5:b002'])

    def test_changed_review_blocks_all_affected_changes_and_coverage(self):
        self.change('reviews.json', lambda d: d['blocks']['v0.9.23:b001'].update(status='pending'))
        manifest, _ = notes.compile_manifest(self.data)
        last = manifest['releases'][-1]
        self.assertTrue(all(c['status'] == 'pending' for c in last['coverage']))
        approved = {c['change_id'] + '@' + c['version'] for c in manifest['changes']}
        refs = notes.read(notes.DATA / 'reviews.json')['blocks']['v0.9.23:b001']['change_refs']
        self.assertFalse(approved.intersection(refs))

    def test_missing_translation_has_no_other_language_fallback(self):
        self.change('changes.json', lambda d: d['changes'][-1]['title'].pop('en'))
        manifest, _ = notes.compile_manifest(self.data)
        row = manifest['changes'][-1]
        self.assertNotIn('en', row['title'])
        release = next(r for r in manifest['releases'] if r['version'] == row['version'])
        self.assertTrue(any('en' not in c['locales'] for c in release['coverage']))
        self.assertIn('Translation pending review', notes.history_html(manifest, 'en'))

    def test_service_translation_does_not_block_firmware_coverage(self):
        document = notes.read(self.data / 'changes.json')
        row = next(c for c in document['changes'] if c['component'] == 'service' and c['version'] not in ('0.9.0', '0.9.5'))
        row['title'].pop('en')
        row['internal_review_reason'] = 'PRIVATE REVIEW TEXT'
        (self.data / 'changes.json').write_bytes(notes.encoded(document))
        manifest, _ = notes.compile_manifest(self.data)
        release = next(r for r in manifest['releases'] if r['version'] == row['version'])
        self.assertTrue(all('en' in c['locales'] for c in release['coverage']))
        self.assertNotIn('PRIVATE REVIEW TEXT', json.dumps(manifest))

    def test_published_archive_metadata_cannot_be_rewritten(self):
        self.change('catalog.snapshot.json', lambda d: d['releases'][0].update(commit='0' * 40))
        with self.assertRaisesRegex(ValueError, 'snapshot hash'):
            notes.compile_manifest(self.data)

    def test_explicit_release_chain_is_required(self):
        self.change('release-order.json', lambda d: d['releases'].pop())
        with self.assertRaisesRegex(ValueError, 'chain declaration'):
            notes.compile_manifest(self.data)

    def test_candidate_notes_preserve_stable_selection_and_require_their_own_scope(self):
        tag, ver = 'v1.0.0-rc', '1.0.0-rc'
        catalog = notes.read(self.data / 'catalog.snapshot.json')
        stable = catalog['stable']
        stable_catalog = copy.deepcopy(catalog)
        source = catalog['releases'][0]
        release = copy.deepcopy(source)
        release.update(tag=tag, version=ver, channel='candidate', prerelease=True, notes=f'/releases/{tag}/notes.md')
        for _, _, entry in notes.targets(release):
            for asset in entry['downloads'].values():
                asset['path'] = asset['path'].replace('/' + source['tag'] + '/', '/' + tag + '/')
        catalog['releases'].append(release)
        archive = Path(self.tmp.name) / 'archive'
        (archive / 'releases').mkdir(parents=True)
        (archive / 'releases/index.json').write_bytes(notes.encoded(catalog))
        original = b'Candidate external display configuration and device features.\n'
        (self.data / 'sources' / (tag + '.md')).write_bytes(original)
        identities = notes.read(self.data / 'archive-identities.json')
        identities['releases'][tag] = copy.deepcopy(identities['releases'][source['tag']])
        identities['releases'][tag]['notes_sha256'] = notes.digest(original)
        (self.data / 'archive-identities.json').write_bytes(notes.encoded(identities))
        self.change('release-order.json', lambda d: d['releases'].append(dict(version=ver, previous='0.9.23', branch='main', channel='candidate')))
        changes = notes.read(self.data / 'changes.json')
        row = copy.deepcopy(changes['changes'][-1])
        row.update(change_id='candidate-display', version=ver, source_refs=[tag + ':b001'])
        row['scope']['channels'] = ['candidate']
        changes['changes'].append(row)
        (self.data / 'changes.json').write_bytes(notes.encoded(changes))
        self.change('reviews.json', lambda d: d['blocks'].update({tag + ':b001': dict(status='approved',
            block_sha256=notes.digest(original.strip()), release_sha256=notes.digest(original),
            change_refs=['candidate-display@' + ver])}))
        manifest, _ = notes.compile_manifest(self.data, archive=archive)
        self.assertEqual(manifest['stable'], stable[1:])
        self.assertIn(dict(branch='main', channel='candidate'), manifest['supported_scopes'])
        candidate = next(r for r in manifest['releases'] if r['version'] == ver)
        self.assertEqual(candidate['channel'], 'candidate')
        self.assertTrue(all(c['scope']['channels'] == ['candidate'] for c in candidate['coverage']))
        (archive / 'releases/index.json').write_bytes(notes.encoded(stable_catalog))
        (archive / 'releases/candidates.json').write_bytes(notes.encoded(dict(schema=1, channel='candidate', releases=[release])))
        stable_manifest = notes.write_public(archive, self.data, archive=archive)
        self.assertFalse(any(r['channel'] == 'candidate' for r in stable_manifest['releases']))
        before = {p.relative_to(archive): p.read_bytes() for p in archive.rglob('*') if p.is_file()}
        candidate_manifest = notes.write_public(archive, self.data, archive=archive, include_candidates=True)
        self.assertEqual(candidate_manifest['stable'], stable[1:])
        self.assertIn(dict(branch='main', channel='candidate'), candidate_manifest['supported_scopes'])
        for path, body in before.items():
            self.assertEqual((archive / path).read_bytes(), body)
        pointer = notes.read(archive / 'releases/notes/candidate/index.json')
        self.assertEqual(pointer['channel'], 'candidate')
        self.assertEqual(pointer['source'], dict(scope='candidate', kind='relative-archive'))
        for name in ('manifest', 'catalog'):
            self.assertRegex(pointer[name]['path'], r'^/releases/notes/candidate/' + name + r'\.[a-f0-9]{64}\.json$')
        combined = notes.read(notes.valid_path(archive, pointer['catalog']))
        self.assertEqual(combined['stable'], stable)
        self.assertIn(tag, {r['tag'] for r in combined['releases']})
        self.assertEqual(notes.verify_output(archive, self.data, include_candidates=True), candidate_manifest)
        self.assertEqual(notes.write_public(archive, self.data, archive=archive, include_candidates=True), candidate_manifest)
        resource = notes.valid_path(archive, pointer['catalog'])
        original_catalog = resource.read_bytes()
        resource.write_bytes(original_catalog + b' ')
        with self.assertRaisesRegex(ValueError, 'resource hash mismatch'):
            notes.verify_output(archive, self.data, include_candidates=True)
        resource.write_bytes(original_catalog)
        self.change('changes.json', lambda d: d['changes'][-1]['scope'].update(channels=['stable']))
        with self.assertRaisesRegex(ValueError, 'explicit release channel'):
            notes.compile_manifest(self.data, archive=archive, include_candidates=True)

    def test_v4_is_not_a_duplicate_feature_and_v0_is_insufficient(self):
        manifest, _ = notes.compile_manifest(self.data)
        statuses = {r['version']: r['history_status'] for r in manifest['releases']}
        self.assertEqual(statuses['0.9.4'], 'no_user_changes')
        self.assertEqual(statuses['0.9.0'], 'insufficient')
        self.assertFalse(any(c['version'] == '0.9.4' for c in manifest['changes']))

    def test_historical_frontend_identities_and_catalog_are_pinned(self):
        manifest, catalog = notes.compile_manifest(self.data)
        self.assertEqual(manifest['archive_sha256'], hashlib.sha256(catalog).hexdigest())
        for release in manifest['releases']:
            for target in release['targets']:
                self.assertEqual(target['frontend'], manifest['sources'][target['source_commit']]['frontend'])
        last = manifest['releases'][-1]['targets'][0]
        self.assertEqual(last['frontend']['lock']['commit'], '5dbbea7a1525fc7486e7bd1c6afd9b4f734a055e')

    def test_hashed_manifest_is_immutable_and_index_is_consistent(self):
        output = Path(self.tmp.name) / 'public'
        (output / 'releases').mkdir(parents=True)
        shutil.copyfile(self.data / 'catalog.snapshot.json', output / 'releases/index.json')
        manifest = notes.write_public(output, self.data)
        pointer = notes.read(output / 'releases/notes/index.json')
        target = notes.valid_path(output, pointer['manifest'])
        self.assertEqual(target.read_bytes(), notes.encoded(manifest))
        self.assertEqual(notes.write_public(output, self.data), manifest)
        with self.assertRaisesRegex(ValueError, 'Immutable'):
            notes.immutable(target, b'changed')
        with self.assertRaisesRegex(ValueError, 'escapes|Invalid'):
            notes.valid_path(output, dict(path='/releases/../secret'))

    def test_original_wiki_snapshot_is_retained_byte_for_byte(self):
        original = (self.data / 'archived-wiki/Release-Log.md').read_bytes()
        self.assertEqual(notes.digest(original), '829fdfb7d1d4777438305bcd9d8631acfb59f1d977798b8b5c58e7f71b5f9195')
        self.assertEqual(notes.digest(original), notes.read(self.data / 'archived-wiki/wiki-snapshot.json')['sha256'])

    def test_cli_reuses_browser_algorithm_and_reports_failure_without_changes(self):
        manifest, catalog = notes.compile_manifest(self.data)
        mp, cp = Path(self.tmp.name) / 'manifest.json', Path(self.tmp.name) / 'catalog.json'
        mp.write_bytes(notes.encoded(manifest)); cp.write_bytes(catalog)
        cmd = ['node', str(ROOT / 'tools/release/release_notes.cjs'), '--manifest', str(mp), '--catalog', str(cp),
               '--model', 'a1', '--variant', 'full', '--from', '0.9.14', '--to', '0.9.23']
        reply = subprocess.run(cmd, capture_output=True, text=True, check=True)
        self.assertTrue(json.loads(reply.stdout)['ok'])
        reply = subprocess.run(cmd[:-1] + ['0.9.99'], capture_output=True, text=True)
        self.assertEqual(reply.returncode, 1)
        self.assertNotIn('changes', json.loads(reply.stdout))
        reply = subprocess.run(cmd + ['--format', 'text'], capture_output=True, text=True, check=True)
        self.assertIn('0.9.14', reply.stdout)


class LegacyProvenanceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.data = self.root / 'data'
        shutil.copytree(notes.DATA, self.data)

    def catalog(self):
        return notes.read(self.data / 'catalog.legacy-v0.9.json')

    def archive(self, catalog):
        archive = self.root / 'archive'
        (archive / 'releases').mkdir(parents=True, exist_ok=True)
        (archive / 'releases/index.json').write_bytes(notes.encoded(catalog))
        return archive

    def changed_proof(self, change):
        path = self.data / 'legacy-provenance.json'
        document = notes.read(path)
        change(document)
        path.write_bytes(notes.encoded(document))
        return notes.digest(path.read_bytes())

    def test_exact_legacy_scope_retains_original_23_identities(self):
        releases, identities = notes.legacy_provenance(self.data)
        original = notes.read(self.data / 'archive-identities.json')
        self.assertEqual(len(releases), 26)
        self.assertEqual(len(notes.read(self.data / 'legacy-provenance.json')['frontend_locks']), 20)
        self.assertTrue(all(identities[tag] == row for tag, row in original['releases'].items()))
        self.assertTrue(all(tag.startswith('v0.9.') for tag in releases))

    def test_changed_catalog_bytes_fail_the_trusted_pin(self):
        path = self.data / 'catalog.legacy-v0.9.json'
        path.write_bytes(path.read_bytes() + b' ')
        with self.assertRaisesRegex(ValueError, 'provenance hash'):
            notes.legacy_provenance(self.data)

    def test_changed_lock_bytes_fail_even_if_the_proof_file_is_repinned(self):
        def corrupt(document):
            row = next(iter(document['frontend_locks'].values()))
            raw = base64.b64decode(row['bytes_base64'])
            row['bytes_base64'] = base64.b64encode(raw + b' ').decode()
        changed_hash = self.changed_proof(corrupt)
        with self.assertRaisesRegex(ValueError, 'provenance hash'):
            notes.legacy_provenance(self.data)
        with patch.object(notes, 'LEGACY_PROVENANCE_SHA256', changed_hash):
            with self.assertRaisesRegex(ValueError, 'frontend lock provenance'):
                notes.legacy_provenance(self.data)

    def test_changed_legacy_source_identity_is_not_a_lock_fallback(self):
        def corrupt(document):
            row = next(iter(document['releases']['v0.9.27']['targets'].values()))
            row['source_commit'] = '0' * 40
        changed_hash = self.changed_proof(corrupt)
        with patch.object(notes, 'LEGACY_PROVENANCE_SHA256', changed_hash):
            with self.assertRaisesRegex(ValueError, 'source provenance'):
                notes.legacy_provenance(self.data)

    def test_changed_actual_legacy_source_is_rejected(self):
        catalog = self.catalog()
        next(r for r in catalog['releases'] if r['tag'] == 'v0.9.24')['commit'] = '0' * 40
        with self.assertRaisesRegex(ValueError, 'legacy metadata changed'):
            notes.compile_manifest(self.data, archive=self.archive(catalog))

    def test_formal_archive_cannot_drop_a_retained_release(self):
        catalog = self.catalog()
        catalog['releases'] = [r for r in catalog['releases'] if r['tag'] != 'v0.9.24']
        with self.assertRaisesRegex(ValueError, 'legacy metadata changed or is missing'):
            notes.compile_manifest(self.data, archive=self.archive(catalog), formal=True)

    def test_new_candidate_cannot_reuse_a_legacy_lock_without_its_git_object(self):
        self.check_current_lock_required(predeclare=False)

    def test_predeclared_candidate_still_requires_its_actual_git_lock(self):
        self.check_current_lock_required(predeclare=True)

    def check_current_lock_required(self, predeclare):
        catalog = self.catalog()
        candidate = copy.deepcopy(catalog['releases'][0])
        candidate.update(tag='v1.0.0-rc', version='1.0.0-rc', channel='candidate',
                         prerelease=True, notes='/releases/v1.0.0-rc/notes.md')
        catalog['releases'].append(candidate)
        if predeclare:
            path = self.data / 'archive-identities.json'
            identities = notes.read(path)
            identities['releases'][candidate['tag']] = copy.deepcopy(identities['releases']['v0.9.23'])
            path.write_bytes(notes.encoded(identities))
        empty_git = self.root / 'source'
        subprocess.run(['git', 'init', '--quiet', str(empty_git)], check=True)
        with patch.object(notes, 'ROOT', empty_git):
            with self.assertRaisesRegex(ValueError, 'verifiable frontend lock: v1.0.0-rc source 99770efc322e'):
                notes.compile_manifest(self.data, archive=self.archive(catalog), formal=True)


if __name__ == '__main__':
    unittest.main()
