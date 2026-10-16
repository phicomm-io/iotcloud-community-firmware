#!/usr/bin/env python3
"""Structure checks for the public site manifest and build output.

Guards the two failure modes that are invisible in a browser until a reader
hits them: a manifest entry with no source page, and a source page that no
manifest entry publishes (silently dropped from the site and the Wiki).
"""
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'site'))
from pages import GROUPS, PAGES  # noqa: E402


class ManifestTests(unittest.TestCase):
    def test_slugs_unique(self):
        slugs = [slug for slug, _, _, _ in PAGES]
        self.assertEqual(len(slugs), len(set(slugs)), 'duplicate slug in site/pages.py')

    def test_wiki_names_unique(self):
        names = [wiki for _, _, _, wiki in PAGES]
        self.assertEqual(len(names), len(set(names)), 'two pages map to the same Wiki page')

    def test_every_manifest_page_has_a_source(self):
        missing = [slug for slug, _, _, _ in PAGES
                   if not (ROOT / 'docs' / 'public' / f'{slug}.md').exists()
                   and not (slug == 'release-log' and (ROOT / 'docs' / 'release' / 'Release-Log.md').exists())]
        self.assertEqual(missing, [], f'manifest entries without a source markdown: {missing}')

    def test_every_source_is_published(self):
        published = {slug for slug, _, _, _ in PAGES}
        on_disk = {p.stem for p in (ROOT / 'docs' / 'public').glob('*.md')}
        orphans = sorted(on_disk - published)
        self.assertEqual(orphans, [], f'docs/public pages missing from site/pages.py: {orphans}')

    def test_group_shape(self):
        for group in GROUPS:
            self.assertEqual(len(group), 3, f'group must be (id, title, items): {group}')
            self.assertTrue(group[2], f'group {group[0]} has no pages')


class BuildTests(unittest.TestCase):
    """Build-output checks. Skipped where the Markdown dependency is absent.

    The firmware host-test job runs this file without the site's Python
    dependencies; a missing optional package must not fail that job.
    """

    @classmethod
    def setUpClass(cls):
        try:
            import markdown  # noqa: F401
        except ImportError:
            raise unittest.SkipTest('Markdown is not installed; skipping site build checks')
        cls._tmp = tempfile.TemporaryDirectory()
        cls.out = Path(cls._tmp.name) / 'pages'
        import subprocess
        subprocess.run([sys.executable, str(ROOT / 'site' / 'build.py'), str(cls.out)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def test_every_page_is_generated(self):
        for slug, title, _, _ in PAGES:
            page = self.out / 'guides' / f'{slug}.html'
            self.assertTrue(page.exists(), f'{slug} not built')
            self.assertIn(title, page.read_text(encoding='utf-8'), f'{slug} is missing its title')

    def test_pages_carry_the_site_shell(self):
        for slug, _, _, _ in PAGES:
            html = (self.out / 'guides' / f'{slug}.html').read_text(encoding='utf-8')
            self.assertIn('site-header', html, f'{slug} has no header')
            self.assertIn('guide-nav', html, f'{slug} has no sidebar')
            self.assertIn('site-footer', html, f'{slug} has no footer')

    def test_no_unrewritten_manifest_links(self):
        """Inter-page links must be .html on the site; a bare slug means a broken link."""
        known = {slug for slug, _, _, _ in PAGES}
        for slug, _, _, _ in PAGES:
            html = (self.out / 'guides' / f'{slug}.html').read_text(encoding='utf-8')
            for target in re.findall(r'href="([^"]+)"', html):
                if target.startswith(('http', '#', 'mailto:')) or '/' in target:
                    continue
                bare = target[:-5] if target.endswith('.html') else target
                if bare in known:
                    self.assertTrue(target.endswith('.html'),
                                    f'{slug}: link to {target} was not rewritten to .html')

    def test_home_links_every_page(self):
        home = (self.out / 'index.html').read_text(encoding='utf-8')
        for slug, title, _, _ in PAGES:
            self.assertIn(f'guides/{slug}.html', home, f'home page does not link {slug}')

    def test_assets_ship(self):
        self.assertTrue((self.out / 'guides' / 'assets' / 'SHA256SUMS').exists())
        self.assertTrue((self.out / 'style.css').exists())
        self.assertTrue((self.out / '.nojekyll').exists())


if __name__ == '__main__':
    unittest.main()
