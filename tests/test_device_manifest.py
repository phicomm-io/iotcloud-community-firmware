"""OTA JSON/MessagePack parity and candidate isolation, without network access."""
import copy
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/release'))
import archive_pages
import device_manifest as manifest
import publish
from products import targets


def unpack(data):
    """Independent reader for the manifest's standard MP wire types."""
    offset = 0
    def read(size):
        nonlocal offset
        value = data[offset:offset + size]
        offset += size
        if len(value) != size:
            raise ValueError('truncated value')
        return value
    def value():
        kind = read(1)[0]
        if kind <= 0x7f:
            return kind
        if 0x80 <= kind <= 0x8f:
            return dict((value(), value()) for _ in range(kind & 15))
        if 0xa0 <= kind <= 0xbf:
            return read(kind & 31).decode('utf-8')
        if kind in (0xcc, 0xcd, 0xce):
            return int.from_bytes(read({0xcc: 1, 0xcd: 2, 0xce: 4}[kind]), 'big')
        if kind in (0xd9, 0xda):
            size = int.from_bytes(read(1 if kind == 0xd9 else 2), 'big')
            return read(size).decode('utf-8')
        raise ValueError('unexpected wire type')
    result = value()
    if offset != len(data):
        raise ValueError('trailing data')
    return result


def release(tag='v1.0.0-rc.4'):
    result = dict(schema=1, tag=tag, version=tag[1:], commit='a' * 40,
                  notes='/releases/' + tag + '/notes.md', models={})
    for model, variant in targets(tag):
        entry = dict(downloads=dict(signed=dict(path='/releases/' + tag + '/' + model + '-' + variant + '.signed',
                                                size=123456, sha256='b' * 64)))
        if variant == 'full':
            result['models'][model] = entry
        else:
            result['models'][model].setdefault('variants', {})[variant] = entry
    return result


class DeviceManifestTests(unittest.TestCase):
    def test_ten_variants_dual_format_parity_hashes_and_immutability(self):
        candidate = release()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest.write_release(candidate, root)
            self.assertEqual(len(candidate['device_manifests']), 10)
            self.assertEqual(len(list(root.iterdir())), 20)
            for pair in candidate['device_manifests'].values():
                values = {}
                for extension, asset in pair.items():
                    body = (root / Path(asset['path']).name).read_bytes()
                    self.assertEqual(len(body), asset['size'])
                    self.assertEqual(hashlib.sha256(body).hexdigest(), asset['sha256'])
                    values[extension] = json.loads(body) if extension == 'json' else unpack(body)
                self.assertEqual(values['json'], values['msgpack'])
                self.assertEqual(values['msgpack']['version'], '1.0.0-rc.4')
            fetch = lambda path: (root / Path(path).name).read_bytes()
            manifest.verify_manifests(candidate, fetch)
            manifest.write_release(candidate, root)  # byte-identical retry
            changed = copy.deepcopy(candidate)
            changed['commit'] = 'c' * 40
            with self.assertRaisesRegex(ValueError, 'immutable'):
                manifest.write_release(changed, root)
            pair = candidate['device_manifests']['a1/full']
            (root / Path(pair['msgpack']['path']).name).write_bytes(b'corrupt')
            with self.assertRaisesRegex(ValueError, 'content differs'):
                manifest.verify_manifests(candidate, fetch)
            pair['json']['sha256'] = '0' * 64
            with self.assertRaisesRegex(ValueError, 'metadata differs'):
                manifest.verify_manifests(candidate, fetch)

    def test_candidate_pointer_does_not_modify_stable_or_immutable_and_cleans_stale(self):
        candidate = release()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stable = root / 'ota/v1/a1/full/latest.json'
            stable.parent.mkdir(parents=True)
            stable.write_bytes(b'existing stable pointer')
            archive_pages.write_candidate_latest([release('v1.0.0-rc.3'), candidate], root)
            current = root / 'ota/candidate/a1/full/latest.msgpack'
            self.assertEqual(unpack(current.read_bytes())['tag'], candidate['tag'])
            self.assertEqual(stable.read_bytes(), b'existing stable pointer')
            self.assertFalse((root / 'releases').exists())
            archive_pages.write_candidate_latest([], root)
            self.assertFalse(current.exists())
            self.assertFalse(current.with_suffix('.json').exists())
            self.assertEqual(stable.read_bytes(), b'existing stable pointer')

    def test_archive_mirrors_and_validates_both_immutable_formats(self):
        candidate = release()
        candidate.update(prerelease=True, channel='candidate')
        with tempfile.TemporaryDirectory() as source, tempfile.TemporaryDirectory() as output:
            source = Path(source)
            responses = {'/releases/index.json': json.dumps(dict(schema=1, releases=[])).encode()}
            for original in candidate['models'].values():
                for entry in (original, *original.get('variants', {}).values()):
                    asset = entry['downloads']['signed']
                    body = b'signed fixture'
                    asset.update(size=len(body), sha256=hashlib.sha256(body).hexdigest())
                    responses[asset['path']] = body
            manifest.write_release(candidate, source)
            for pair in candidate['device_manifests'].values():
                for asset in pair.values():
                    responses[asset['path']] = (source / Path(asset['path']).name).read_bytes()
            responses[candidate['notes']] = b'notes'
            responses['/releases/' + candidate['tag'] + '/SHA256SUMS'] = b'checksums'
            responses['/releases/candidates.json'] = json.dumps(dict(schema=1, channel='candidate', releases=[candidate])).encode()
            def fetch(request, **kwargs):
                return io.BytesIO(responses[request.full_url.removeprefix('https://example')])
            with patch.object(archive_pages, 'urlopen', side_effect=fetch):
                archive_pages.mirror('https://example', output, True)
            pair = candidate['device_manifests']['m1/full']
            for asset in pair.values():
                self.assertEqual((Path(output) / asset['path'].lstrip('/')).read_bytes(), responses[asset['path']])
            # An internally self-consistent hash cannot hide different JSON/MP values.
            asset = pair['msgpack']
            responses[asset['path']] = b'bad canonical content'
            asset.update(size=len(responses[asset['path']]), sha256=hashlib.sha256(responses[asset['path']]).hexdigest())
            responses['/releases/candidates.json'] = json.dumps(dict(schema=1, channel='candidate', releases=[candidate])).encode()
            with patch.object(archive_pages, 'urlopen', side_effect=fetch):
                with self.assertRaisesRegex(ValueError, 'metadata differs'):
                    archive_pages.mirror('https://example', output, True)

    def test_wire_boundaries_utf8_and_reject_unsupported_values(self):
        for value in [0, 127, 128, 255, 256, 65535, 65536, 2**32 - 1,
                      'x' * 31, 'x' * 32, 'x' * 255, 'x' * 256, '版本' * 100]:
            self.assertEqual(unpack(manifest.pack(value)), value)
        self.assertEqual(manifest.pack(65536), b'\xce' + struct.pack('>I', 65536))
        for value in [True, -1, 2**32, 1.5, [], None, 'x' * 65536]:
            with self.assertRaises(ValueError):
                manifest.pack(value)
        with self.assertRaisesRegex(ValueError, '4096'):
            manifest.encoded({'path': 'x' * 4096})

    def test_stable_smoke_requires_new_v1_binary_pointers_match_json(self):
        stable = release('v1.0.0')
        for original in stable['models'].values():
            for entry in (original, *original.get('variants', {}).values()):
                entry.update(entry['downloads']['signed'])
        with tempfile.TemporaryDirectory() as tmp:
            manifest.write_release(stable, Path(tmp))
        visited = []
        def response(request, **kwargs):
            from urllib.parse import urlsplit
            visited.append(request.full_url)
            parts = urlsplit(request.full_url).path.split('/')
            current = parts[2] == 'v1'
            model = parts[3] if current else parts[2]
            variant = parts[4] if current else parts[3] if len(parts) == 5 else 'full'
            entry = stable['models'][model]
            if variant != 'full':
                entry = entry['variants'][variant]
            extension = parts[-1].split('.')[-1]
            return io.BytesIO(manifest.encoded(manifest.document(stable, model, variant, entry))[extension])
        with patch.object(publish.urllib.request, 'build_opener') as factory:
            factory.return_value.open.side_effect = response
            publish.smoke(stable)
            self.assertEqual(sum('/latest.msgpack?' in url for url in visited), 20)
            factory.return_value.open.side_effect = lambda request, **kwargs: io.BytesIO(b'bad') if '/latest.msgpack?' in request.full_url else response(request)
            with self.assertRaisesRegex(ValueError, 'Binary latest differs'):
                publish.smoke(stable)

    def test_old_immutable_catalog_needs_no_new_asset(self):
        manifest.verify_manifests(release(), lambda path: self.fail('must not fetch'))


if __name__ == '__main__':
    unittest.main()
