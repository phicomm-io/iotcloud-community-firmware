"""Static migration/review guarantees; no firmware build or device mutations."""
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

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/release'))
import user_notes as notes


class UserNotesTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.data = Path(self.tmp.name) / 'data'
        shutil.copytree(notes.DATA, self.data)

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


if __name__ == '__main__':
    unittest.main()
