import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

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
