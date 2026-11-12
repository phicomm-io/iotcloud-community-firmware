"""Published candidates must expose both native S30 installation formats."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/release'))
from native_pages import collect, collect_release


class NativePages(unittest.TestCase):
    def fixture(self, tag, commit='a' * 40, rbl=b'native-rbl'):
        version = tag[1:]
        assets = {f'BL2028N_OTA_{version}.rbl': rbl,
                  f'BL2028N_STOCK_UA_{version}.bin': b'native-ua'}
        metadata = {'project_head': commit, 'dirty': False, 'version': version + '-' + commit[:6],
                    'version_metadata': {'base_version': version, 'git_tag': tag,
                        'git_commit': commit, 'dirty': False, 'version': version + '-' + commit[:6]},
                    'flash_layout': {'name': 's30-v1b-stock-v1', 'bootloader_included': False},
                    'release_files': {name: {'sha256': hashlib.sha256(body).hexdigest(), 'size': len(body)}
                                      for name, body in assets.items()}}
        assets['build-s30.json'] = json.dumps(metadata).encode()
        assets['validation-s30.json'] = b'{"status":"PASS"}'
        assets['SHA256SUMS-S30'] = ''.join(hashlib.sha256(body).hexdigest() + '  ' + name + '\n'
                                          for name, body in assets.items()).encode()
        return assets

    def test_future_candidate_is_discovered_without_editing_source(self):
        with tempfile.TemporaryDirectory() as tmp:
            archive = Path(tmp)
            (archive / 'releases').mkdir()
            (archive / 'releases/index.json').write_text('{"releases":[{"tag":"v0.9.27"}]}')
            (archive / 'releases/candidates.json').write_text('{"releases":[{"tag":"v1.0.0-rc.6"}]}')
            assets = self.fixture('v1.0.0-rc.6')
            collect(archive, lambda url: assets[url.rsplit('/', 1)[1]])
            releases = json.loads((archive / 'releases/native-downloads.json').read_text())['releases']
            self.assertEqual([r['tag'] for r in releases], ['v1.0.0-rc.6'])
            self.assertEqual(set(releases[0]['models']['s30']['downloads']), {'rbl', 'stock_ua'})
            # Retained metadata and images support an offline rebuild.
            collect(archive, lambda url: self.fail('Unexpected download: ' + url))

    def test_corrupt_or_wrong_build_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            assets = self.fixture('v1.0.0-rc.5')
            assets['BL2028N_OTA_1.0.0-rc.5.rbl'] = b'corrupt'
            with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                collect_release('v1.0.0-rc.5', Path(tmp), lambda url: assets[url.rsplit('/', 1)[1]])
        with tempfile.TemporaryDirectory() as tmp:
            assets = self.fixture('v1.0.0-rc.4')
            with self.assertRaisesRegex(ValueError, 'identity'):
                collect_release('v1.0.0-rc.5', Path(tmp), lambda url: assets[
                    url.rsplit('/', 1)[1].replace('rc.5', 'rc.4')])

    def test_reissued_candidate_replaces_stale_retained_native_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            archive = Path(tmp)
            tag = 'v1.0.0-rc.5'
            folder = archive / 'releases' / tag
            folder.mkdir(parents=True)
            for name, body in self.fixture(tag).items():
                (folder / name).write_bytes(body)
            (archive / 'releases/candidates.json').write_text(json.dumps({'releases': [
                {'tag': tag, 'commit': 'b' * 40}]}))
            updated = self.fixture(tag, 'b' * 40, b'reissued-native-rbl')
            collect(archive, lambda url: updated[url.rsplit('/', 1)[1]])
            release = json.loads((archive / 'releases/native-downloads.json').read_bytes())['releases'][0]
            self.assertEqual(release['source_commit'], 'b' * 40)
            self.assertEqual((folder / f'BL2028N_OTA_{tag[1:]}.rbl').read_bytes(), b'reissued-native-rbl')
            self.assertEqual(release['models']['s30']['downloads']['rbl']['sha256'],
                             hashlib.sha256(b'reissued-native-rbl').hexdigest())
            collect(archive, lambda url: self.fail('Unexpected re-download: ' + url))
            with self.assertRaisesRegex(ValueError, 'identity'):
                collect_release(tag, archive, lambda url: updated[url.rsplit('/', 1)[1]], 'c' * 40)


if __name__ == '__main__':
    unittest.main()
