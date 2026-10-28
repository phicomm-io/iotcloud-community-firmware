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

    def test_candidate_archive_refresh_replaces_retained_notes_pointer(self):
        import hashlib
        import json
        import shutil
        import subprocess
        archive = Path(self._tmp.name) / 'candidate-archive'
        retained = archive / 'releases/notes/candidate'
        retained.mkdir(parents=True)
        catalog = (ROOT / 'docs/release-notes/catalog.snapshot.json').read_bytes()
        (archive / 'releases/index.json').write_bytes(catalog)
        (archive / 'releases/candidates.json').write_text(json.dumps({
            'schema': 1, 'channel': 'candidate', 'releases': []}))
        # A previously generated snapshot must not survive a catalog refresh.
        shutil.copyfile(self.out / 'releases/notes/index.json', retained / 'index.json')
        output = Path(self._tmp.name) / 'candidate-pages'
        subprocess.run([sys.executable, str(ROOT / 'site/build.py'), str(output),
                        '--archive', str(archive)], check=True)
        self.assertEqual((output / 'releases/index.json').read_bytes(), catalog)
        pointer = json.loads((output / 'releases/notes/candidate/index.json').read_bytes())
        self.assertEqual(pointer['channel'], 'candidate')
        self.assertEqual(pointer['source']['scope'], 'candidate')
        for name in ['catalog', 'manifest']:
            entry = pointer[name]
            resource = (output / entry['path'].lstrip('/')).read_bytes()
            self.assertEqual(hashlib.sha256(resource).hexdigest(), entry['sha256'])
            self.assertEqual(len(resource), entry['size'])
        combined = json.loads((output / pointer['catalog']['path'].lstrip('/')).read_bytes())
        self.assertEqual(combined, json.loads(catalog))

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
        targets = re.findall(r'href="([^"]+)"', hero)
        self.assertEqual(targets.index('local/'), targets.index('iotcloud/') + 1)

    def test_bilingual_home_navigation_and_published_urls(self):
        for relative, guide_base in (('index.html', 'guides/'), ('en/index.html', '../guides/en/')):
            home = (self.out / relative).read_text()
            nav = re.search(r'<nav class="site-nav"[^>]*>(.*?)</nav>', home).group(1)
            targets = re.findall(r'href="([^"]+)"', nav)
            self.assertEqual(targets, ['#devices', '#install', '#releases', '#faq', '#develop'])
            for anchor in ('devices', 'install', 'faq', 'develop', 'features', 'iotcloud',
                           'demos', 'docs', 'releases', 'community', 'm1-mcu', 'm1-mcu-releases'):
                self.assertIn(f'id="{anchor}"', home)
            for slug, *_ in PAGES:
                self.assertIn(f'href="{guide_base}{slug}.html"', home)
            self.assertIn('data-home-language', home)
            self.assertNotIn('{{', home)
            self.assertNotRegex(home, r'href="[^\"]*/releases/(?:download/)?v1[./]')

    def test_home_local_html_links_exist(self):
        from urllib.parse import urlsplit, unquote
        for target in re.findall(r'href="([^"]+)"', (self.out / 'index.html').read_text()):
            url = urlsplit(target)
            if not url.scheme and not url.netloc and url.path.endswith('.html'):
                self.assertTrue((self.out / unquote(url.path)).is_file(), target)

    def test_device_flashing_guides_are_separate_and_discoverable(self):
        for device in ('m1', 'a1'):
            items = next(items for group, _, items in GROUPS if group == device)
            self.assertIn(device + '-flash', [item[0] for item in items])
            self.assertNotIn('a1-m1-flash', [item[0] for item in items])
            html = (self.out / 'guides' / (device + '-flash.html')).read_text()
            for anchor in ('j-link-刷机', 'bootloader-刷机'):
                self.assertIn(f'id="{anchor}"', html)
            self.assertIn(f'phicomm_{device}.all.bin', html)
            other = 'a1' if device == 'm1' else 'm1'
            self.assertNotIn(f'phicomm_{other}.', html)
        m1 = (self.out / 'guides/m1-flash.html').read_text()
        self.assertLess(m1.index('<h2 id="bootloader-刷机"'), m1.index('<h2 id="j-link-刷机"'))
        a1 = (self.out / 'guides/a1-flash.html').read_text()
        self.assertLess(a1.index('<h2 id="j-link-刷机"'), a1.index('<h2 id="bootloader-刷机"'))

    def test_device_entries_reach_install_update_use_and_recovery(self):
        from html import unescape
        from urllib.parse import unquote, urlsplit

        devices = ('m1', 'a1', 'tc1', 'daikin')
        for language in ('', 'en/'):
            home = self.out / (language + 'index.html')
            paths = [home, self.out / 'guides/en/index.html'] if language else [home]
            paths += [self.out / 'guides' / language / (slug + '.html') for slug, *_ in PAGES]
            for path in paths:
                text = path.read_text()
                entries = re.findall(r'<div class="device-doc-actions"[^>]*data-device-actions="([^"]+)"[^>]*>(.*?)</div>', text)
                self.assertEqual([device for device, _ in entries], list(devices), path)
                for device, entry in entries:
                    links = re.findall(r'<a href="([^"]+)" data-guide-action="([^"]+)"', entry)
                    self.assertEqual([action for _, action in links], ['install', 'update', 'use', 'recover'])
                    for href, action in links:
                        url = urlsplit(unescape(href))
                        target = path.parent / unquote(url.path)
                        self.assertTrue(target.is_file(), f'{path}: missing {device} {action}: {href}')
                        if url.fragment:
                            self.assertIn(f'id="{unquote(url.fragment)}"', target.read_text(), href)
                        if action in ('use', 'recover'):
                            self.assertEqual(target.name, device + '-guide.html', href)
                if path == home:
                    for device in devices:
                        self.assertIn(f'href="#docs-{device}"', text)
                        self.assertIn(f'id="docs-{device}" open', text)

    def test_retired_combined_urls_reach_a_readable_device_chooser(self):
        for language in ('', 'en/'):
            for alias in ('a1-m1-flash', 'stock-provision'):
                html = (self.out / 'guides' / language / f'{alias}.html').read_text()
                self.assertIn('url=quick-start.html', html)
                self.assertIn('href="quick-start.html"', html)
            html = (self.out / 'guides' / language / 'v1-features.html').read_text()
            self.assertIn('release-log.html?channel=candidate#v1-overview', html)

    def test_all_demos_link_back_to_the_website(self):
        import importlib.util
        import shutil
        spec = importlib.util.spec_from_file_location('demo_navigation', ROOT / 'site/demo_navigation.py')
        navigation = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(navigation)
        for model in ('tc1', 'm1', 'a1', 'daikin', 'platform'):
            page = self.out / 'demos' / model / 'index.html'
            text = page.read_text()
            self.assertIn('data-demo-home href="../../"', text)
            self.assertIn('src="../../demo-nav.js"', text)
            self.assertIn('href="../../demo-nav.css"', text)
        # A Pages publish restores an independently built platform demo.
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)
            shutil.copytree(self.out / 'demos', output / 'demos')
            platform = output / 'demos/platform/index.html'
            platform.write_text('<html><head></head><body><div id="app"></div></body></html>')
            navigation.add_navigation(output)
            once = platform.read_text()
            navigation.add_navigation(output)
            self.assertEqual(platform.read_text(), once)
            self.assertEqual(once.count('data-demo-home href='), 1)
            self.assertTrue((output / 'demo-nav.js').is_file())
            self.assertTrue((output / 'demo-nav.css').is_file())

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
        self.assertEqual(len(urls), 3 + 2 * len(PAGES))
        for url in urls:
            self.assertTrue(url.startswith(SITE_URL), url)
            relative = url.removeprefix(SITE_URL)
            page = self.out / (relative + 'index.html' if not relative or relative.endswith('/') else relative)
            text = page.read_text()
            self.assertEqual(re.findall(r'<link rel="canonical" href="([^"]+)">', text), [url])
            self.assertIn('<meta name="description"', text)
        self.assertIn(f'Sitemap: {SITE_URL}sitemap.xml', (self.out / 'robots.txt').read_text())

    def test_stock_guides_keep_all_os_commands_without_javascript(self):
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
                        raise AssertionError('Instructions must remain readable without JavaScript')
                if tag == 'input' and attrs.get('name') == 'guide-os':
                    self.choices.append(attrs['value'])

        for language in ('', 'en/'):
            for slug in ('a1-no-disassembly', 'm1-no-disassembly'):
                text = (self.out / 'guides' / language / f'{slug}.html').read_text()
                parser = OSParser()
                parser.feed(text)
                self.assertEqual(parser.systems, {'mac', 'windows', 'linux'})
                self.assertEqual(parser.choices, ['mac', 'windows', 'linux'])
                self.assertNotIn('<!-- os:', text)
                self.assertNotIn('```', text)
                self.assertIn('curl.exe', text)
                self.assertRegex(text, r'curl (?:--http1\.0 )?--noproxy')

    def test_easylink_download_is_complete_and_matches_sources(self):
        import hashlib
        import zipfile
        assets = self.out / 'guides/assets'
        archive = assets / 'tools/a1-easylink.zip'
        files = ('a1_easylink.py', 'a1_easylink_web.py',
                 'a1_easylink_web/index.html', 'a1_easylink_web/app.js',
                 'a1_easylink_web/style.css')
        with zipfile.ZipFile(archive) as bundle:
            self.assertEqual(set(bundle.namelist()), {'a1-easylink/' + f for f in files})
            for name in files:
                self.assertEqual(bundle.read('a1-easylink/' + name),
                                 (ROOT / 'tools' / name).read_bytes())
        checksum = hashlib.sha256(archive.read_bytes()).hexdigest()
        self.assertIn(checksum + '  tools/a1-easylink.zip', (assets / 'SHA256SUMS').read_text())


if __name__ == '__main__':
    unittest.main()
