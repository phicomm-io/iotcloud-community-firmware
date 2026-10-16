#!/usr/bin/env python3
"""Generate the Wiki pages from the same sources as the public site.

docs/public/*.md is the single source for both surfaces. This tool rewrites the
link forms the site uses into the forms the GitHub Wiki understands:

  * inter-page links  (quick-start)      ->  (快速开始)
  * asset links       assets/x.png       ->  raw.githubusercontent wiki URL

Usage:
    wiki_sync.py --check  <wiki-dir>   # report drift, change nothing
    wiki_sync.py          <wiki-dir>   # write the pages

The Wiki directory is a clone of phicomm-community-firmware.wiki.git.
"""
import argparse, re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'site'))
from pages import GROUPS, PAGES, SITE_URL  # noqa: E402

# Public guide assets also work when the source repository or Wiki is private.
WIKI_ASSETS = SITE_URL + 'guides/assets/'
PAGE_BY_SLUG = {slug: wiki for slug, _, _, wiki in PAGES}

# Release-Log.md flows the other way: it is authored in the Wiki and frozen into
# the repository by tools/release/notes.py. Regenerating it here would delete a
# release section an author just wrote, so it is never written back.
WIKI_AUTHORED = {'release-log'}


def wiki_link_target(slug):
    """URL-encode a wiki page name the way GitHub does for non-ASCII titles."""
    return slug.replace(' ', '-')


def rewrite(text):
    """Turn site link targets into Wiki link targets."""
    text = text.replace('{{SITE_URL}}', SITE_URL)

    def one(match):
        target, rest = match.group(1).strip(), match.group(2)
        anchor = ''
        if '#' in target:
            target, _, frag = target.partition('#')
            anchor = '#' + frag
        if target.startswith('assets/'):
            return f']({WIKI_ASSETS}{target[len("assets/"):]}{anchor})'
        if target in PAGE_BY_SLUG:
            return f']({wiki_link_target(PAGE_BY_SLUG[target])}{anchor})'
        if target.endswith('.md') and target[:-3] in PAGE_BY_SLUG:
            return f']({wiki_link_target(PAGE_BY_SLUG[target[:-3]])}{anchor})'
        return match.group(0)

    return re.sub(r'\]\(([^)\s]+)(\s+"[^"]*")?\)', one, text)


def home_page():
    lines = ['# 斐讯社区固件', '',
             '本项目为斐讯 TC1 智能排插、M1 空气检测仪和 A1 空气净化器维护社区固件。',
             '本地网页控制、MQTT 与 Home Assistant 接入，固件免费下载。', '']
    for _, title, items in GROUPS:
        lines.append(f'## {title}')
        lines.append('')
        for slug, page_title, summary, wiki in items:
            lines.append(f'- [{page_title}]({wiki_link_target(wiki)})：{summary}')
        lines.append('')
    lines += ['## 相关链接', '',
              f'- [项目主页与历史版本归档]({SITE_URL})',
              '- [固件源码仓库](https://github.com/phicomm-io/phicomm-community-firmware)',
              '- [MQTT 远程集中管理平台](https://github.com/phicomm-io/phicomm-community-mqtt)',
              '']
    return '\n'.join(lines)


def source_for(slug):
    public = ROOT / 'docs' / 'public' / f'{slug}.md'
    if slug == 'release-log':
        frozen = ROOT / 'docs' / 'release' / 'Release-Log.md'
        return frozen if frozen.exists() else public
    return public


def expected_pages():
    pages = {'Home.md': home_page()}
    for slug, _, _, wiki in PAGES:
        if slug in WIKI_AUTHORED:
            continue
        source = source_for(slug)
        if not source.exists():
            continue
        text = rewrite(source.read_text(encoding='utf-8'))
        # The Wiki derives its title from the file name; drop the duplicate H1.
        text = re.sub(r'^#\s+.*\n+', '', text, count=1)
        pages[f'{wiki}.md'] = text
    return pages


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('wiki', type=Path, help='clone of the .wiki.git repository')
    parser.add_argument('--check', action='store_true', help='report drift without writing')
    args = parser.parse_args()
    if not args.wiki.is_dir():
        parser.error(f'not a directory: {args.wiki}')

    expected = expected_pages()
    drift, written = [], []
    for name, text in expected.items():
        target = args.wiki / name
        current = target.read_text(encoding='utf-8') if target.exists() else None
        if current == text:
            continue
        (drift if args.check else written).append(name)
        if not args.check:
            target.write_text(text, encoding='utf-8')

    # Pages that the manifest no longer defines are stale and must be removed.
    # Wiki-authored pages are excluded: they are not generated here, but they
    # are still expected to exist.
    known = set(expected) | {f'{PAGE_BY_SLUG[slug]}.md' for slug in WIKI_AUTHORED}
    for path in sorted(args.wiki.glob('*.md')):
        if path.name not in known:
            drift.append(path.name)
            if not args.check:
                path.unlink()

    if args.check:
        if drift:
            print('Wiki out of date: ' + ', '.join(sorted(drift)))
            print('Run without --check to regenerate.')
            return 1
        print('Wiki matches docs/public/.')
        return 0
    if written:
        print('Updated: ' + ', '.join(sorted(written)))
    else:
        print('Wiki already up to date.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
