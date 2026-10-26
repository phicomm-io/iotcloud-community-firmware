import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from urllib.error import HTTPError

spec = importlib.util.spec_from_file_location('archive_pages', Path(__file__).resolve().parents[1] / 'tools/release/archive_pages.py')
archive = importlib.util.module_from_spec(spec)
spec.loader.exec_module(archive)


class ArchiveTests(unittest.TestCase):
    def run_mirror(self, corrupt=False, path='/releases/v1/firmware.bin'):
        data = b'verified firmware'
        item = dict(path=path, size=len(data), sha256=hashlib.sha256(data).hexdigest())
        catalog = dict(releases=[dict(tag='v1', notes='/releases/v1/notes.md', models=dict(tc1=dict(downloads=dict(raw=item))))])
        responses = {'/releases/index.json': json.dumps(catalog).encode(), path: b'corrupt' if corrupt else data,
                     '/releases/v1/notes.md': b'notes', '/releases/v1/SHA256SUMS': b'checksums'}
        def request(req, **kwargs):
            return io.BytesIO(responses[req.full_url.removeprefix('https://archive.example')])
        with tempfile.TemporaryDirectory() as tmp, patch.object(archive, 'urlopen', side_effect=request):
            archive.mirror('https://archive.example', tmp)
            self.assertEqual((Path(tmp)/path.lstrip('/')).read_bytes(), data)
            self.assertTrue((Path(tmp)/'releases/index.json').exists())

    def test_verified_archive(self):
        self.run_mirror()

    def test_corrupt_asset_never_publishes_catalog(self):
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            self.run_mirror(corrupt=True)

    def test_reject_traversal(self):
        with self.assertRaisesRegex(ValueError, 'Invalid release path'):
            self.run_mirror(path='/releases/../../escape.bin')

    def test_device_manifest_follows_stable_and_never_falls_back_to_raw(self):
        signed = dict(path='/releases/v1/firmware.bin.signed', size=192, sha256='a' * 64)
        raw = dict(path='/releases/v1/firmware.bin.raw', size=1, sha256='b' * 64)
        stable = dict(tag='v1.0.0', version='1.0.0', commit='c' * 40, notes='/releases/v1.0.0/notes.md',
                      models=dict(m1=dict(downloads=dict(signed=signed, raw=raw)),
                                  tc1=dict(downloads=dict(raw=raw))))
        catalog = dict(stable='v1.0.0', releases=[dict(tag='v2.0.0', models={}), stable])
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)
            stale = output/'ota/tc1/latest.json'
            stale.parent.mkdir(parents=True)
            stale.write_text('stale')
            archive.write_latest(catalog, output)
            data = (output/'ota/m1/latest.json').read_bytes()
            manifest = json.loads(data)
            self.assertEqual(manifest['version'], '1.0.0')
            self.assertEqual(manifest['model'], 'm1')
            self.assertEqual(manifest['path'], signed['path'])
            self.assertEqual(manifest['release_log'], stable['notes'])
            self.assertLess(len(data), 4096)
            self.assertFalse(stale.exists())
            self.assertFalse((output/'ota/a1/latest.json').exists())

    def test_invalid_stable_reference_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(ValueError, 'Stable release is absent'):
                archive.write_latest(dict(stable='v2', releases=[]), Path(tmp))

    def release(self, tag):
        def entry(variant):
            signed = dict(path=f'/releases/{tag}/daikin-{variant}.ota.bin.signed', size=1234,
                          sha256=('a' if variant == 'full' else 'b') * 64)
            return dict(firmware_variant=variant, downloads=dict(signed=signed))
        model = entry('full')
        model['variants'] = dict(hub=entry('hub'))
        return dict(tag=tag, version=tag[1:], commit='c' * 40, notes=f'/releases/{tag}/notes.md', models=dict(daikin=model))

    def test_v1_variants_advance_while_legacy_stops_at_bridge(self):
        bridge, current = self.release('v1.0.0'), self.release('v1.2.3')
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)
            archive.write_latest(dict(stable=current['tag'], releases=[current, bridge]), output)
            for variant in ('full', 'hub'):
                value = json.loads((output / f'ota/v1/daikin/{variant}/latest.json').read_text())
                self.assertEqual(value['tag'], current['tag'])
                self.assertEqual(value['firmware_variant'], variant)
            legacy = json.loads((output / 'ota/daikin/latest.json').read_text())
            self.assertEqual(legacy['tag'], 'v1.0.0')
            self.assertFalse((output / 'ota/daikin/hub/latest.json').exists())

    def test_bridge_required_and_old_catalog_removes_stale_v1_variants(self):
        bridge, current = self.release('v1.0.0'), self.release('v1.0.1')
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)
            with self.assertRaisesRegex(ValueError, 'bridge release is absent'):
                archive.write_latest(dict(stable=current['tag'], releases=[current]), output)
            archive.write_latest(dict(stable=current['tag'], releases=[current, bridge]), output)
            historical = self.release('v0.9.24')
            del historical['models']['daikin']['variants']
            archive.write_latest(dict(stable=historical['tag'], releases=[historical]), output)
            self.assertFalse((output / 'ota/v1/daikin/full/latest.json').exists())
            self.assertFalse((output / 'ota/v1/daikin/hub/latest.json').exists())
            legacy = json.loads((output / 'ota/daikin/latest.json').read_text())
            self.assertEqual(legacy['tag'], 'v0.9.24')

    def test_explicit_candidates_are_retained_beside_the_unchanged_stable_catalog(self):
        stable = self.release('v0.9.27')
        candidate = self.release('v1.0.0-rc')
        candidate.update(prerelease=True, channel='candidate')
        stable_catalog = json.dumps(dict(schema=1, stable=stable['tag'], releases=[stable])).encode()
        candidate_catalog = json.dumps(dict(schema=1, channel='candidate', releases=[candidate])).encode()
        responses = {'/releases/index.json': stable_catalog}
        candidate_responses = {'/releases/candidates.json': candidate_catalog}
        for release, mapping in ((stable, responses), (candidate, candidate_responses)):
            for model in release['models'].values():
                for entry in (model, *model.get('variants', {}).values()):
                    for item in entry['downloads'].values():
                        body = b'stable image' if release is stable else b'candidate image'
                        item.update(size=len(body), sha256=hashlib.sha256(body).hexdigest())
                        mapping[item['path']] = body
            mapping[release['notes']] = b'reviewed notes'
            mapping[f"/releases/{release['tag']}/SHA256SUMS"] = b'checksums'
        stable_catalog = json.dumps(dict(schema=1, stable=stable['tag'], releases=[stable])).encode()
        candidate_catalog = json.dumps(dict(schema=1, channel='candidate', releases=[candidate])).encode()
        responses['/releases/index.json'] = stable_catalog
        candidate_responses['/releases/candidates.json'] = candidate_catalog
        def request(req, **kwargs):
            return io.BytesIO(responses[req.full_url.removeprefix('https://archive.example')])
        with tempfile.TemporaryDirectory() as tmp, patch.object(archive, 'urlopen', side_effect=request):
            output = Path(tmp)
            archive.mirror('https://archive.example', output, True, candidate_responses.__getitem__)
            self.assertEqual((output / 'releases/index.json').read_bytes(), stable_catalog)
            self.assertEqual((output / 'releases/candidates.json').read_bytes(), candidate_catalog)
            self.assertTrue((output / 'releases/v1.0.0-rc/notes.md').exists())
            manifest = json.loads((output / 'ota/daikin/latest.json').read_bytes())
            self.assertEqual(manifest['tag'], 'v0.9.27')
            self.assertFalse((output / 'ota/v1/daikin/hub/latest.json').exists())
            candidate_asset = candidate['models']['daikin']['downloads']['signed']['path']
            (output / candidate_asset.lstrip('/')).unlink()
            candidate_responses[candidate_asset] = b'corrupt candidate'
            with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                archive.mirror('https://archive.example', output, True, candidate_responses.__getitem__)
            self.assertEqual((output / 'releases/index.json').read_bytes(), stable_catalog)
            self.assertEqual((output / 'releases/candidates.json').read_bytes(), candidate_catalog)
            def missing_asset(path):
                if path == candidate_asset:
                    raise HTTPError(path, 404, 'Candidate asset missing', None, None)
                return candidate_responses[path]
            with self.assertRaises(HTTPError):
                archive.mirror('https://archive.example', output, True, missing_asset)

    def test_only_missing_first_candidate_catalog_is_accepted(self):
        catalog = json.dumps(dict(schema=1, stable=None, releases=[])).encode()
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)
            def missing(path):
                raise HTTPError(path, 404, 'No candidate catalog', None, None)
            with patch.object(archive, 'urlopen', return_value=io.BytesIO(catalog)):
                archive.mirror('https://archive.example', output, True, missing)
            self.assertEqual((output / 'releases/index.json').read_bytes(), catalog)
            self.assertFalse((output / 'releases/candidates.json').exists())
            def failed(path):
                raise HTTPError(path, 503, 'Service unavailable', None, None)
            with patch.object(archive, 'urlopen', return_value=io.BytesIO(catalog)):
                with self.assertRaises(HTTPError):
                    archive.mirror('https://archive.example', output, True, failed)
            self.assertEqual((output / 'releases/index.json').read_bytes(), catalog)
