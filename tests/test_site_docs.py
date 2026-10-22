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
from pages import GROUPS, PAGES, EN_PAGES, validate_translations  # noqa: E402


class ManifestTests(unittest.TestCase):
    def test_english_sources_are_current(self):
        validate_translations(ROOT)
        self.assertEqual({p[0] for p in PAGES}, {p[0] for p in EN_PAGES})

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

    def test_published_tc1_no_disassembly_urls_survive_rebuild(self):
        for language in ('', 'en/'):
            guide = self.out / 'guides' / language / 'tc1-no-disassembly.html'
            self.assertTrue(guide.is_file(), f'published URL removed: {guide.name} ({language})')
            text = guide.read_text()
            self.assertIn('PHICOMM_OTA_AP', text)
            self.assertIn('mico_ota.bin', text)
            sibling = (self.out / 'guides' / language / 'tc1-flash.html').read_text()
            self.assertIn('href="tc1-no-disassembly.html"', sibling)
        self.assertIn('guides/tc1-no-disassembly.html', (self.out / 'index.html').read_text())

    def test_interactive_tc1_guide_retains_readable_fallback(self):
        from html.parser import HTMLParser

        class OSParser(HTMLParser):
            def __init__(self):
                super().__init__()
                self.systems = set()
                self.choices = []

            def handle_starttag(self, tag, attrs):
                attrs = dict(attrs)
                if 'data-guide-os' in attrs:
                    self.systems.update(attrs['data-guide-os'].split())
                    if 'hidden' in attrs:
                        raise AssertionError('OS instructions must be readable without JavaScript')
                if tag == 'input' and attrs.get('name') == 'guide-os':
                    self.choices.append(attrs['value'])

        for language in ('', 'en/'):
            text = (self.out / 'guides' / language / 'tc1-no-disassembly-ethernet-wifi-ap.html').read_text()
            parser = OSParser()
            parser.feed(text)
            self.assertEqual(parser.systems, {'mac', 'windows', 'linux'})
            self.assertEqual(parser.choices, ['mac', 'windows', 'linux'])
            self.assertNotIn('<!-- os:', text)
            self.assertNotIn('```', text)
            for command in ('netsh interface ipv4', 'sudo ip address', 'sudo ifconfig'):
                self.assertIn(command, text)

    def test_home_first_use_remains_primary_and_local_follows_iotcloud(self):
        home = (self.out / 'index.html').read_text()
        hero = home.split('<section class="hero">', 1)[1].split('</section>', 1)[0]
        actions = re.search(r'<div class="actions">(.*?)</div>', hero).group(1)
        links = re.findall(r'<a class="([^"]+)" href="([^"]+)">([^<]+)</a>', actions)
        self.assertEqual(links[0], ('btn primary', 'guides/quick-start.html', '第一次使用'))
        self.assertEqual([text for css, _, text in links if 'primary' in css.split()], ['第一次使用'])
        targets = [href for _, href, _ in links]
        self.assertEqual(targets.index('local/'), targets.index('iotcloud/') + 1)
        nav = re.search(r'<nav class="site-nav"[^>]*>(.*?)</nav>', home).group(1)
        nav_targets = re.findall(r'href="([^"]+)"', nav)
        self.assertEqual(nav_targets.index('local/'), nav_targets.index('#iotcloud') + 1)

    def test_home_local_html_links_exist(self):
        from urllib.parse import urlsplit, unquote
        for target in re.findall(r'href="([^"]+)"', (self.out / 'index.html').read_text()):
            url = urlsplit(target)
            if not url.scheme and not url.netloc and url.path.endswith('.html'):
                self.assertTrue((self.out / unquote(url.path)).is_file(), target)

    def test_shared_flashing_guide_under_both_products(self):
        for gid in ('a1', 'm1'):
            items = next(items for group, _, items in GROUPS if group == gid)
            self.assertIn('a1-m1-flash', [item[0] for item in items])
        html = (self.out / 'guides' / 'a1-m1-flash.html').read_text()
        for anchor in ('j-link-刷机', 'bootloader-刷机'):
            self.assertIn(f'id="{anchor}"', html)

    def test_old_flashing_urls_reach_shared_guide(self):
        for alias in ('a1-flash', 'm1-flash'):
            html = (self.out / 'guides' / f'{alias}.html').read_text()
            self.assertIn('url=a1-m1-flash.html', html)

    def test_english_navigation_assets_and_language_switches(self):
        from urllib.parse import urlsplit, unquote
        for slug, _, _, _ in EN_PAGES:
            path = self.out / 'guides/en' / f'{slug}.html'
            text = path.read_text()
            self.assertIn('<html lang="en">', text)
            self.assertIn(f'../{slug}.html', text)
            chinese = (self.out / 'guides' / f'{slug}.html').read_text()
            self.assertIn(f'en/{slug}.html', chinese)
            for target in re.findall(r'(?:href|src)="([^"]+)"', text):
                url = urlsplit(target)
                if url.scheme or url.netloc or not url.path:
                    continue
                dest = path.parent / unquote(url.path)
                self.assertTrue(dest.exists(), f'{slug}: missing {target}')
        self.assertTrue((self.out / 'guides/en/index.html').is_file())

    def test_assets_ship(self):
        self.assertTrue((self.out / 'guides' / 'assets' / 'SHA256SUMS').exists())
        self.assertTrue((self.out / 'style.css').exists())
        self.assertTrue((self.out / '.nojekyll').exists())

    def test_search_discovery_survives_a_full_site_build(self):
        from xml.etree import ElementTree as ET
        from pages import SITE_URL
        urls = [element.text for element in ET.parse(self.out / 'sitemap.xml').iter(
            '{http://www.sitemaps.org/schemas/sitemap/0.9}loc')]
        self.assertEqual(len(urls), 2 + 2 * len(PAGES))
        for url in urls:
            self.assertTrue(url.startswith(SITE_URL), url)
            relative = url.removeprefix(SITE_URL)
            page = self.out / (relative + 'index.html' if not relative or relative.endswith('/') else relative)
            text = page.read_text()
            self.assertEqual(re.findall(r'<link rel="canonical" href="([^"]+)">', text), [url])
            self.assertIn('<meta name="description"', text)
        self.assertIn(f'Sitemap: {SITE_URL}sitemap.xml', (self.out / 'robots.txt').read_text())


if __name__ == '__main__':
    unittest.main()
