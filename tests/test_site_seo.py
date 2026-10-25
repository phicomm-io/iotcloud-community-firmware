"""Check canonical discovery and bilingual metadata without fetching firmware UI."""
import json
import sys
import tempfile
import unittest
from html.parser import HTMLParser
from pathlib import Path
from xml.etree import ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'site'))
from pages import PAGES, EN_PAGES, SITE_URL  # noqa: E402
from seo import guide_metadata, metadata, write_discovery  # noqa: E402


class Head(HTMLParser):
    def __init__(self, text):
        super().__init__()
        self.links = []
        self.metas = []
        self.title = ''
        self.in_title = False
        self.feed(text)

    def handle_starttag(self, tag, attrs):
        if tag == 'link':
            self.links.append(dict(attrs))
        if tag == 'meta':
            self.metas.append(dict(attrs))
        if tag == 'title':
            self.in_title = True

    def handle_endtag(self, tag):
        if tag == 'title':
            self.in_title = False

    def handle_data(self, data):
        if self.in_title:
            self.title += data


class SearchTests(unittest.TestCase):
    def test_guide_titles_unique_and_language_pairs_reciprocal(self):
        titles = []
        for language, pages in [('zh', PAGES), ('en', EN_PAGES)]:
            for slug, title, summary, _ in pages:
                head = Head(guide_metadata(slug, title, summary, language))
                titles.append(head.title)
                prefix = 'en/' if language == 'en' else ''
                self.assertEqual([l['href'] for l in head.links if l['rel'] == 'canonical'],
                                 [SITE_URL + f'guides/{prefix}{slug}.html'])
                self.assertEqual({l['hreflang']: l['href'] for l in head.links if l['rel'] == 'alternate'},
                                 {'zh-CN': SITE_URL + f'guides/{slug}.html',
                                  'en': SITE_URL + f'guides/en/{slug}.html',
                                  'x-default': SITE_URL + f'guides/{slug}.html'})
        self.assertEqual(len(titles), len(set(titles)))

    def test_metadata_escapes_attributes_and_text(self):
        title, description = 'TC1 < A1 & "M1"', 'An "air" & <power> guide'
        head = Head(metadata(title, description, 'guides/tc1-guide.html'))
        self.assertEqual(head.title, title)
        self.assertEqual(next(m['content'] for m in head.metas if m.get('name') == 'description'), description)
        self.assertEqual(next(m['content'] for m in head.metas if m.get('property') == 'og:title'), title)

    def test_sitemap_covers_published_content_without_aliases_or_apps(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            expected = ['', 'en/', 'guides/en/'] + [f'guides/{prefix}{slug}.html'
                        for prefix in ('', 'en/') for slug, *_ in PAGES]
            for path in expected:
                file = out / (path + 'index.html' if not path or path.endswith('/') else path)
                file.parent.mkdir(parents=True, exist_ok=True)
                file.touch()
            write_discovery(out)
            root = ET.parse(out / 'sitemap.xml').getroot()
            urls = [e.text for e in root.iter('{http://www.sitemaps.org/schemas/sitemap/0.9}loc')]
            self.assertEqual(urls, [SITE_URL + p for p in expected])
            self.assertEqual(len(urls), len(set(urls)))
            self.assertIn('Allow: /', (out / 'robots.txt').read_text())
            self.assertIn(f'Sitemap: {SITE_URL}sitemap.xml', (out / 'robots.txt').read_text())
            (out / 'guides/quick-start.html').unlink()
            with self.assertRaisesRegex(ValueError, 'quick-start.html'):
                write_discovery(out)

    def test_home_has_one_canonical_and_truthful_site_identity(self):
        import re
        home = (ROOT / 'site/index.html').read_text()
        head = Head(home.split('</head>')[0])
        self.assertEqual([l['href'] for l in head.links if l['rel'] == 'canonical'], [SITE_URL])
        for word in ('斐讯', 'TC1', 'M1', 'A1'):
            self.assertIn(word, head.title)
        schema = json.loads(re.search(r'<script type="application/ld\+json">(.*?)</script>', home).group(1))
        self.assertEqual(schema['url'], SITE_URL)
        self.assertEqual(schema['@type'], 'WebSite')
        self.assertNotIn('noindex', home)


if __name__ == '__main__':
    unittest.main()
