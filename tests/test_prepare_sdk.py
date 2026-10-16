"""Offline checks for the shared, pinned MiCO source cache."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from tools import prepare_sdk


class PrepareSDK(unittest.TestCase):
    def test_shared_source_independent_overlays_and_wrong_pin_rejection(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'upstream'
            subprocess.run(['git', 'init', '-q', str(source)], check=True)
            (source / 'makefiles').mkdir()
            (source / 'makefiles/Makefile').write_text('original\n')
            subprocess.run(['git', '-C', str(source), 'add', '.'], check=True)
            subprocess.run(['git', '-C', str(source), '-c', 'user.name=test',
                            '-c', 'user.email=test@example.invalid', 'commit', '-qm', 'SDK'], check=True)
            commit = prepare_sdk.git('-C', source, 'rev-parse', 'HEAD')
            subprocess.run(['git', '-C', str(source), 'tag', 'sdk-test'], check=True)
            metadata = dict(prepare_sdk.SDK, commit=commit, ref='sdk-test')
            environment = {'MICO_SDK_REPOSITORY': str(source), 'MICO_SDK_CACHE': str(root / 'cache.git')}
            first, second = root / 'TC1/mico-os', root / 'M1/mico-os'
            with patch.dict(os.environ, environment), patch.object(prepare_sdk, 'SDK', metadata):
                prepare_sdk.prepare(first)
                (first / 'makefiles/Makefile').write_text('TC1 overlay\n')
                # No second upstream access is needed after the source cache exists.
                with patch.dict(os.environ, {'MICO_SDK_REPOSITORY': str(root / 'offline')}):
                    prepare_sdk.prepare(second)
                prepare_sdk.prepare(first)
                self.assertEqual((first / 'makefiles/Makefile').read_text(), 'TC1 overlay\n')
                self.assertEqual((second / 'makefiles/Makefile').read_text(), 'original\n')
                with patch.object(prepare_sdk, 'SDK', dict(metadata, commit='0' * 40)):
                    with self.assertRaisesRegex(ValueError, 'commit mismatch'):
                        prepare_sdk.prepare(first)
                self.assertEqual((first / 'makefiles/Makefile').read_text(), 'TC1 overlay\n')
                # An archive cache or incomplete checkout must never be overwritten.
                archive = root / 'legacy'
                archive.mkdir()
                (archive / 'keep').write_text('legacy SDK')
                with self.assertRaisesRegex(ValueError, 'Not a MiCO Git checkout'):
                    prepare_sdk.prepare(archive)
                self.assertEqual((archive / 'keep').read_text(), 'legacy SDK')
                (second / 'makefiles/Makefile').unlink()
                with self.assertRaisesRegex(ValueError, 'Incomplete'):
                    prepare_sdk.prepare(second)

    def test_moved_tag_does_not_install_unpinned_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'source'
            subprocess.run(['git', 'init', '-q', str(source)], check=True)
            (source / 'file').write_text('wrong source')
            subprocess.run(['git', '-C', str(source), 'add', '.'], check=True)
            subprocess.run(['git', '-C', str(source), '-c', 'user.name=test',
                            '-c', 'user.email=test@example.invalid', 'commit', '-qm', 'wrong'], check=True)
            subprocess.run(['git', '-C', str(source), 'tag', 'sdk-test'], check=True)
            cache, destination = root / 'cache.git', root / 'TC1/mico-os'
            with patch.dict(os.environ, {'MICO_SDK_REPOSITORY': str(source), 'MICO_SDK_CACHE': str(cache)}), \
                    patch.object(prepare_sdk, 'SDK', dict(prepare_sdk.SDK, commit='0' * 40, ref='sdk-test')):
                with self.assertRaisesRegex(ValueError, 'commit mismatch'):
                    prepare_sdk.prepare(destination)
            self.assertFalse(cache.exists())
            self.assertFalse(destination.exists())


if __name__ == '__main__':
    unittest.main()
