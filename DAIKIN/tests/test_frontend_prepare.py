"""Exercise the SCons frontend packer without PlatformIO or generated DUT files."""
import gzip
import json
import os
from pathlib import Path
import re
import runpy
import sys
import tempfile
from types import ModuleType
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / 'prepare.py'


class BuildEnv(dict):
    def Append(self, **values):
        pass


class FrontendPrepareTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bundle = self.root / 'bundle'
        (self.bundle / 'devices/daikin').mkdir(parents=True)
        self.original = gzip.compress(b'<html><head></head><body>Original standalone device</body></html>', mtime=0)
        (self.bundle / 'devices/daikin/index.html.gz').write_bytes(self.original)
        (self.bundle / 'hub/device-assets').mkdir(parents=True)
        self.files = {
            'index.html': b'<html>Hub account</html>',
            'standalone-daikin.html': b'<html><head><script src="/iotcloud/device-assets/device.js"></script></head></html>',
            'device-assets/device.js': b'window.fixture="shared device source";',
            'device-assets/device.css': b'body{margin:0}',
            'icons/icon.png': b'\x89PNG\r\n\x1a\nfixture',
            'sw.js': b'not served on the hub',
            'config.json': b'not served on the hub',
        }
        for name, data in self.files.items():
            path = self.bundle / 'hub' / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        (self.root / 'frontend.lock.json').write_text(json.dumps({'commit': 'fixture'}))
        self.release = ModuleType('frontend_release')
        self.release.fetch = lambda path: self.bundle
        self.version = ModuleType('check_version')
        self.version.resolve_version = lambda path: {'version': 'v-test'}

    def run_build(self, variant, local=None):
        project = self.root / 'DAIKIN'
        project.mkdir(exist_ok=True)
        env = BuildEnv(PROJECT_DIR=str(project), PIOENV='bry88ab-hub' if variant == 'hub' else 'bry88ab')
        with patch.dict(sys.modules, {'frontend_release': self.release, 'check_version': self.version}), patch.dict(os.environ):
            if local is None:
                os.environ.pop('PHICOMM_FRONTEND_SOURCE', None)
            else:
                os.environ['PHICOMM_FRONTEND_SOURCE'] = str(local)
            runpy.run_path(str(SCRIPT), init_globals={'env': env, 'Import': lambda name: None})
        return project / 'generated' / env['PIOENV']

    def root_bytes(self, out):
        text = (out / 'web.h').read_text()
        return bytes(map(int, re.search(r'web_data\[\] = \{([\d,]*)\}', text)[1].split(',')))

    def test_full_pinned_page_keeps_exact_compressed_bytes(self):
        out = self.run_build('full')
        self.assertEqual(self.root_bytes(out), self.original)
        self.assertFalse((out / 'hub_web.h').exists())

    def test_full_local_page_keeps_inline_renderer_and_gzip(self):
        source = self.root / 'source'
        (source / 'common/web').mkdir(parents=True)
        raw = b'<html><head></head><body>Local full inline page</body></html>'
        (source / 'common/web/page.py').write_text('def render_page(path, *, minify=False):\n assert minify\n return ' + repr(raw) + '\n')
        with patch('subprocess.check_output', side_effect=lambda cmd, **kwargs: 'fixture\n' if 'rev-parse' in cmd else ''):
            out = self.run_build('full', source)
        self.assertEqual(self.root_bytes(out), gzip.compress(raw, mtime=0))

    def test_hub_root_is_embedded_once_and_every_asset_roundtrips(self):
        import zopfli.gzip
        out = self.run_build('hub')
        self.assertEqual(gzip.decompress(self.root_bytes(out)), self.files['standalone-daikin.html'])
        self.assertEqual(self.root_bytes(out), zopfli.gzip.compress(self.files['standalone-daikin.html'], numiterations=500))
        header = (out / 'hub_web.h').read_text()
        arrays = re.findall(r'static const unsigned char hub_asset_\d+\[\] = \{([\d,]*)\}', header)
        retained = [(name, data) for name, data in sorted(self.files.items()) if name not in ('standalone-daikin.html', 'sw.js', 'config.json')]
        self.assertEqual(len(arrays), len(retained))
        for encoded, (name, original) in zip(arrays, retained):
            compressed = bytes(map(int, encoded.split(',')))
            self.assertEqual(gzip.decompress(compressed), original, name)
            expected = gzip.compress(original, compresslevel=9, mtime=0) if name.endswith('.png') else zopfli.gzip.compress(original, numiterations=500)
            self.assertEqual(compressed, expected, name)
            self.assertIn('/iotcloud/' + name, header)
        self.assertNotIn('standalone-daikin.html', header)
        first = {name: (out / name).read_bytes() for name in ('web.h', 'hub_web.h', 'frontend.json')}
        self.run_build('hub')
        self.assertEqual(first, {name: (out / name).read_bytes() for name in first})

    def test_hub_rejects_a_bundle_without_shared_root(self):
        (self.bundle / 'hub/standalone-daikin.html').unlink()
        with self.assertRaisesRegex(RuntimeError, 'Hub shared frontend missing'):
            self.run_build('hub')


if __name__ == '__main__':
    unittest.main()
