"""Exercise branch routing and release provenance in isolated Git histories."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('branch_policy', Path(__file__).resolve().parents[1] / 'tools/check_branch_policy.py')
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)


class BranchPolicyTests(unittest.TestCase):
    def pull(self, head, base):
        policy.check('pull_request', {'pull_request': {'head': {'ref': head}, 'base': {'ref': base}}}, 'refs/pull/1/merge')

    def test_feature_development_release_and_hotfix_routes(self):
        for head, base in [('feature/hub', 'dev'), ('dev', 'main'), ('hotfix/ota', 'main'),
                           ('hotfix/ota', 'dev'), ('main', 'dev')]:
            with self.subTest(head=head, base=base):
                self.pull(head, base)

    def test_bypass_and_archive_routes_are_rejected(self):
        for head, base in [('feature/hub', 'main'), ('main', 'main'), ('dev', 'dev'),
                           ('feature/', 'dev'), ('archive/0.9.x', 'dev'), ('dev', 'archive/0.9.x')]:
            with self.subTest(head=head, base=base), self.assertRaises(ValueError):
                self.pull(head, base)

    def test_only_active_branches_build(self):
        for event in ('push', 'workflow_dispatch'):
            for branch in ('main', 'dev', 'feature/hub', 'hotfix/ota'):
                policy.check(event, {}, 'refs/heads/' + branch)
            for branch in ('archive/0.9.x', 'feature/', 'preview', '0.9'):
                with self.subTest(event=event, branch=branch), self.assertRaises(ValueError):
                    policy.check(event, {}, 'refs/heads/' + branch)

    def test_invalid_and_frozen_tags_fail_before_git(self):
        for tag in ('v0.9.25', 'v0.9.28-rc', 'v1.0.0-rc.01', 'v1.0.0-beta', 'v1.0', 'v01.0.0', 'v1.00.0', 'other'):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                policy.check('push', {}, 'refs/tags/' + tag)

    def test_stable_tag_must_point_to_checkout_and_belong_to_main(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            def git(*args):
                return subprocess.check_output(['git', '-C', tmp, *args], stderr=subprocess.DEVNULL, text=True).strip()
            git('init', '-q')
            git('config', 'user.email', 'test@example.invalid')
            git('config', 'user.name', 'Branch Test')
            (root / 'source').write_text('main')
            git('add', 'source')
            git('commit', '-qm', 'main')
            main = git('rev-parse', 'HEAD')
            git('update-ref', 'refs/remotes/origin/main', main)
            git('tag', '-a', 'v1.0.0', '-m', 'stable')
            policy.check('push', {}, 'refs/tags/v1.0.0', root)
            for tag in ('v1.0.0-rc', 'v1.0.0-rc.0', 'v1.0.0-rc.1'):
                git('tag', tag)
                policy.check('push', {}, 'refs/tags/' + tag, root)
            (root / 'source').write_text('uncommitted')
            with self.assertRaisesRegex(ValueError, 'clean source tree'):
                policy.check('push', {}, 'refs/tags/v1.0.0-rc', root)
            (root / 'source').write_text('main')
            git('checkout', '-qb', 'dev')
            (root / 'source').write_text('development')
            git('commit', '-qam', 'development')
            git('tag', 'v1.1.0')
            with self.assertRaises(subprocess.CalledProcessError):
                policy.check('push', {}, 'refs/tags/v1.1.0', root)
            with self.assertRaisesRegex(ValueError, 'checked-out commit'):
                policy.check('push', {}, 'refs/tags/v1.0.0', root)


if __name__ == '__main__':
    unittest.main()
