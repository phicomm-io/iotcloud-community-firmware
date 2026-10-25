"""Publication checks current and legacy routing without using remote services."""
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
from urllib.parse import urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/release'))
import publish
from products import targets


def release(tag):
    value = dict(tag=tag, version=tag[1:], commit='c' * 40, models={})
    for index, (model, variant) in enumerate(targets(tag)):
        entry = dict(sha256=f'{index:064x}')
        if variant == 'full':
            value['models'][model] = entry
        else:
            value['models'][model].setdefault('variants', {})[variant] = entry
    return value


class PublishTests(unittest.TestCase):
    def test_smoke_requires_ten_v1_and_nine_frozen_legacy_routes_on_both_mirrors(self):
        current, bridge = release('v1.1.0'), release('v1.0.0')
        visited = []
        def response(request, **kwargs):
            visited.append(request.full_url)
            parts = urlsplit(request.full_url).path.split('/')
            if parts[2] == 'v1':
                selected, model, variant = current, parts[3], parts[4]
            else:
                selected, model = bridge, parts[2]
                variant = parts[3] if len(parts) == 5 else 'full'
            item = selected['models'][model]
            entry = item if variant == 'full' else item['variants'][variant]
            return io.BytesIO(json.dumps(dict(tag=selected['tag'], model=model,
                                             firmware_variant=variant, sha256=entry['sha256'])).encode())
        with patch.object(publish.urllib.request, 'build_opener') as factory:
            factory.return_value.open.side_effect = response
            publish.smoke(current, bridge)
        self.assertEqual(len(visited), 38)
        self.assertTrue(any('/ota/v1/daikin/hub/' in url for url in visited))
        self.assertFalse(any('/ota/daikin/hub/' in url for url in visited))
        self.assertTrue(all('release=v1.0.0' in url for url in visited if '/ota/v1/' not in url))

    def test_later_release_cannot_promote_without_bridge(self):
        candidate = release('v1.1.0')
        previous = json.dumps(release('v0.9.24')).encode()
        stored = {'stable.json': previous}
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / 'release.json').write_text(json.dumps(candidate))
            def put(key, body, immutable=False):
                stored[key] = body
            with patch.object(publish, 'usage', return_value=0), patch.object(publish, 'get', side_effect=stored.get), \
                 patch.object(publish, 'put', side_effect=put), patch.object(publish, 'verify'), \
                 patch.object(sys, 'argv', ['publish.py', tmp, '--promote']):
                with self.assertRaisesRegex(ValueError, 'publish v1.0.0 bridge'):
                    publish.main()
            self.assertEqual(stored['stable.json'], previous)

    def test_failed_smoke_restores_previous_stable(self):
        candidate, bridge = release('v1.1.0'), release('v1.0.0')
        previous = json.dumps(bridge).encode()
        stored = {'stable.json': previous, 'bridge-v1.0.0.json': previous}
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / 'release.json').write_text(json.dumps(candidate))
            def put(key, body, immutable=False):
                stored[key] = body
            with patch.object(publish, 'usage', return_value=0), patch.object(publish, 'get', side_effect=stored.get), \
                 patch.object(publish, 'put', side_effect=put), patch.object(publish, 'verify'), \
                 patch.object(publish, 'smoke', side_effect=ValueError('smoke failure')), \
                 patch.object(sys, 'argv', ['publish.py', tmp, '--promote']):
                with self.assertRaisesRegex(ValueError, 'smoke failure'):
                    publish.main()
            self.assertEqual(stored['stable.json'], previous)
            self.assertNotIn('releases/index.json', stored)


if __name__ == '__main__':
    unittest.main()
