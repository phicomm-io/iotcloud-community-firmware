"""Publication order and snapshot isolation for static OTA note mirrors."""
import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/release'))
import publish_user_notes as publisher


class PublishNotesTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.output = Path(self.tmp.name)
        self.catalog = b'{"schema":1,"releases":[]}'
        self.manifest = b'{"schema":1,"changes":[]}'
        digest = hashlib.sha256(self.manifest).hexdigest()
        self.path = 'releases/notes/manifest.' + digest + '.json'
        (self.output / 'releases/notes').mkdir(parents=True)
        (self.output / 'releases/index.json').write_bytes(self.catalog)
        (self.output / self.path).write_bytes(self.manifest)
        self.pointer = json.dumps({'schema': 1,
            'manifest': {'path': '/' + self.path, 'size': len(self.manifest), 'sha256': digest},
            'catalog': {'path': '/releases/index.json', 'size': len(self.catalog),
                        'sha256': hashlib.sha256(self.catalog).hexdigest()}}).encode()
        (self.output / 'releases/notes/index.json').write_bytes(self.pointer)
        self.expected = {'/' + self.path: self.manifest,
                         '/releases/notes/index.json': self.pointer,
                         '/releases/index.json': self.catalog}

    def test_complete_immutable_resource_precedes_pointer_and_both_mirrors_are_read(self):
        requests = []
        expected = self.expected
        class Response:
            status = 200
            def __init__(self, body): self.body = body
            def __enter__(self): return self
            def __exit__(self, *args): pass
            def read(self): return self.body
        class Opener:
            def open(self, request, timeout):
                from urllib.parse import urlsplit
                url = urlsplit(request.full_url)
                requests.append((url.netloc, url.path))
                return Response(expected[url.path])
        with patch.object(publisher, 'verify_output') as verify, \
                patch.object(publisher, 'get', return_value=self.catalog), \
                patch.object(publisher, 'put') as put, \
                patch.object(publisher, 'build_opener', return_value=Opener()):
            publisher.publish(self.output)
        verify.assert_called_once_with(self.output, formal=True)
        self.assertEqual(put.call_args_list[0].args, (self.path, self.manifest, True))
        self.assertEqual(put.call_args_list[1].args, ('releases/notes/index.json', self.pointer))
        self.assertEqual(put.call_count, 2)
        self.assertEqual(len(requests), 6)
        self.assertEqual({host for host, _ in requests}, {'phicomm.fjrcn.com', 'phicomm.fjrcn.cn'})

    def test_changed_catalog_prevents_all_uploads(self):
        with patch.object(publisher, 'verify_output'), \
                patch.object(publisher, 'get', return_value=b'new catalog'), \
                patch.object(publisher, 'put') as put:
            with self.assertRaisesRegex(ValueError, 'catalog differs'):
                publisher.publish(self.output)
        put.assert_not_called()

    def test_failed_immutable_upload_cannot_advance_pointer(self):
        with patch.object(publisher, 'verify_output'), \
                patch.object(publisher, 'get', return_value=self.catalog), \
                patch.object(publisher, 'put', side_effect=RuntimeError('upload failed')) as put:
            with self.assertRaisesRegex(RuntimeError, 'upload failed'):
                publisher.publish(self.output)
        self.assertEqual(put.call_count, 1)
        self.assertEqual(put.call_args.args[0], self.path)


if __name__ == '__main__':
    unittest.main()
