"""Exercise version provenance using isolated Git repositories and source trees."""
import importlib.util
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('version_tool', Path(__file__).resolve().parents[1] / 'tools/check_version.py')
version_tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(version_tool)


class VersionTests(unittest.TestCase):
    def test_ota_metadata_channel_follows_build_provenance(self):
        with patch.dict('os.environ', {}, clear=True):
            self.assertIn('"/ota/v1"', version_tool.ota_config({'base_version':'1.0.0'}))
            for version in ('1.0.0-rc', '1.0.0-rc.4'):
                self.assertIn('"/ota/candidate"', version_tool.ota_config({'base_version':version}))
        with patch.dict('os.environ', {'PHICOMM_OTA_CHANNEL':'candidate'}, clear=True):
            self.assertIn('"/ota/candidate"', version_tool.ota_config({'base_version':'1.0.0'}))
        with patch.dict('os.environ', {'PHICOMM_OTA_CHANNEL':'stable'}, clear=True):
            with self.assertRaisesRegex(ValueError, 'cannot use stable'):
                version_tool.ota_config({'base_version':'1.0.0-rc.4'})
        with patch.dict('os.environ', {'PHICOMM_OTA_CHANNEL':'auto'}, clear=True):
            with self.assertRaisesRegex(ValueError, 'candidate or stable'):
                version_tool.ota_config({'base_version':'1.0.0'})

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.git('init', '-q')
        self.git('config', 'user.email', 'test@example.invalid')
        self.git('config', 'user.name', 'Version Test')
        (self.root / 'source').write_text('first')
        self.git('add', 'source')
        self.git('commit', '-qm', 'first')
        self.commit = self.git('rev-parse', 'HEAD')

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.root), *args], text=True, stderr=subprocess.DEVNULL).strip()

    def test_tagged_clean_and_annotated_tag(self):
        self.git('tag', '-a', 'v1.2.3', '-m', 'release')
        result = version_tool.resolve_version(self.root, 'v1.2.3')
        self.assertEqual(result['version'], '1.2.3-' + self.commit[:6])
        self.assertEqual(result['git_commit'], self.commit)
        self.assertFalse(result['dirty'])

    def test_history_ignores_unrelated_and_invalid_tags(self):
        self.git('tag', 'v1.2.3')
        self.git('checkout', '-qb', 'other')
        (self.root / 'source').write_text('other')
        self.git('commit', '-qam', 'other')
        self.git('tag', 'v99.0.0')
        self.git('checkout', '-q', '--detach', self.commit)
        (self.root / 'source').write_text('next')
        self.git('commit', '-qam', 'next')
        self.git('tag', 'v2.0.0-invalid')
        result = version_tool.resolve_version(self.root)
        self.assertEqual(result['git_tag'], 'v1.2.3')
        self.assertEqual(result['version'], '1.2.3-' + self.git('rev-parse', 'HEAD')[:6])

    def test_dirty_staged_unstaged_and_untracked(self):
        self.git('tag', 'v1.2.3')
        source = self.root / 'source'
        source.write_text('changed')
        self.assertTrue(version_tool.resolve_version(self.root)['version'].endswith('-dirty'))
        self.git('add', 'source')
        self.assertTrue(version_tool.resolve_version(self.root)['dirty'])
        self.git('reset', '--hard', '-q', 'HEAD')
        (self.root / 'new-source').write_text('new')
        self.assertTrue(version_tool.resolve_version(self.root)['dirty'])
        with self.assertRaises(ValueError):
            version_tool.resolve_version(self.root, 'v1.2.3')

    def test_no_tag(self):
        self.assertEqual(version_tool.resolve_version(self.root)['version'], '0.0.0-' + self.commit[:6])

    def test_v1_target_version_is_available_before_first_v1_tag(self):
        self.git('tag', 'v0.9.24')
        (self.root / 'VERSION').write_text('1.0.0\n')
        self.git('add', 'VERSION')
        self.git('commit', '-qm', 'prepare v1')
        result = version_tool.resolve_version(self.root)
        self.assertEqual(result['base_version'], '1.0.0')
        self.assertIsNone(result['git_tag'])
        self.assertEqual(result['version'], '1.0.0-' + self.git('rev-parse', 'HEAD')[:6])
        self.git('tag', 'v1.0.0')
        self.assertEqual(version_tool.resolve_version(self.root, 'v1.0.0')['base_version'], '1.0.0')

    def test_release_tag_must_match_target_version(self):
        (self.root / 'VERSION').write_text('1.0.0\n')
        self.git('add', 'VERSION')
        self.git('commit', '-qm', 'prepare v1')
        self.git('tag', 'v1.0.1')
        with self.assertRaisesRegex(ValueError, 'differs from VERSION'):
            version_tool.resolve_version(self.root, 'v1.0.1')
        (self.root / 'VERSION').write_text('01.0.0\n')
        with self.assertRaisesRegex(ValueError, 'VERSION must contain'):
            version_tool.resolve_version(self.root)

    def test_candidate_preserves_full_identity_and_matches_core_version(self):
        (self.root / 'VERSION').write_text('1.0.0\n')
        self.git('add', 'VERSION')
        self.git('commit', '-qm', 'prepare v1 candidate')
        for tag in ('v1.0.0-rc', 'v1.0.0-rc.0', 'v1.0.0-rc.2'):
            self.git('tag', tag)
            metadata = version_tool.resolve_version(self.root, tag)
            self.assertEqual(metadata['base_version'], tag[1:])
            self.assertEqual(metadata['version'], tag[1:] + '-' + self.git('rev-parse', 'HEAD')[:6])
            self.git('tag', '-d', tag)
        self.git('tag', 'v1.0.0-rc.2')
        self.assertEqual(version_tool.resolve_version(self.root)['base_version'], '1.0.0-rc.2')
        self.git('tag', 'v1.0.1-rc')
        with self.assertRaisesRegex(ValueError, 'differs from VERSION'):
            version_tool.resolve_version(self.root, 'v1.0.1-rc')
        (self.root / 'VERSION').write_text('1.0.0-rc\n')
        with self.assertRaisesRegex(ValueError, 'VERSION must contain'):
            version_tool.resolve_version(self.root)

    def test_non_git_and_parent_repository(self):
        child = self.root / 'export'
        child.mkdir()
        self.assertEqual(version_tool.resolve_version(child)['version'], '0.0.0-000000-dirty')
        with tempfile.TemporaryDirectory() as exported:
            self.assertEqual(version_tool.resolve_version(exported)['version'], '0.0.0-000000-dirty')
        with patch.object(version_tool, 'git', side_effect=FileNotFoundError):
            self.assertEqual(version_tool.resolve_version(self.root)['version'], '0.0.0-000000-dirty')

    def test_release_rejects_invalid_missing_and_wrong_commit(self):
        for tag in ['v01.2.3', 'v1.2', 'v1.2.3-dirty']:
            with self.assertRaises(ValueError):
                version_tool.resolve_version(self.root, tag)
        with self.assertRaises(subprocess.CalledProcessError):
            version_tool.resolve_version(self.root, 'v1.2.3')
        self.git('tag', 'v1.2.3')
        (self.root / 'source').write_text('next')
        self.git('commit', '-qam', 'next')
        with self.assertRaises(ValueError):
            version_tool.resolve_version(self.root, 'v1.2.3')

    def test_packaging_keeps_generated_snapshot(self):
        self.git('tag', 'v1.2.3')
        metadata = version_tool.resolve_version(self.root)
        with patch.object(version_tool, 'GENERATED', self.root / 'generated'):
            version_tool.generate(metadata)
            (self.root / 'source').write_text('later change')
            self.assertEqual(version_tool.build_metadata(), metadata)
            self.assertEqual(version_tool.project_version(), metadata['version'])
            self.assertIn(metadata['version'], (self.root / 'generated/build_version.h').read_text())


if __name__ == '__main__':
    unittest.main()
