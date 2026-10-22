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
from pages import GROUPS, PAGES, PARENTS, SITE_URL, EN_GROUPS, EN_PAGES, validate_translations  # noqa: E402

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


def rewrite(text, language="zh"):
    page_by_slug = {slug: wiki for slug, _, _, wiki in (EN_PAGES if language == "en" else PAGES)}
    """Turn site link targets into Wiki link targets."""
    text = text.replace('{{SITE_URL}}', SITE_URL)
    # Wiki has no script support; retain every OS section as ordinary Markdown.
    text = re.sub(r'<!-- (?:os-picker|os:[\w ]+|/os) -->\n*', '', text)

    def one(match):
        target, rest = match.group(1).strip(), match.group(2)
        anchor = ''
        if '#' in target:
            target, _, frag = target.partition('#')
            anchor = '#' + frag
        if target.startswith('assets/'):
            return f']({WIKI_ASSETS}{target[len("assets/"):]}{anchor})'
        if target in page_by_slug:
            return f']({wiki_link_target(page_by_slug[target])}{anchor})'
        if target.endswith('.md') and target[:-3] in page_by_slug:
            return f']({wiki_link_target(page_by_slug[target[:-3]])}{anchor})'
        return match.group(0)

    return re.sub(r'\]\(([^)\s]+)(\s+"[^"]*")?\)', one, text)


def home_page(language="zh"):
    if language == "en":
        lines = ['# Phicomm Community Firmware', '', '[简体中文](Home) · English', '',
                 'Community firmware for the TC1 power strip, M1 air quality monitor and A1 air purifier.',
                 'Local web control, MQTT and Home Assistant integration, with free firmware downloads.', '']
        lines += [f'- [Open IotCloud]({SITE_URL}iotcloud/)',
                  '- [Connect devices to IotCloud](EN-iotcloud-connect)', '']
        for _, title, items in EN_GROUPS:
            lines += ['## ' + title, '']
            lines += [f'{"  " if slug in PARENTS else ""}- [{name}]({wiki}): {summary}' for slug, name, summary, wiki in items]
            lines += ['']
        lines += ['## Community', '', '- QQ group: **1021750178** (Chinese)',
                  '- [Join the QQ group](https://qm.qq.com/q/9L150V85kk?from=tim)',
                  '- If the link does not open, search for the group number in QQ.', '',
                  '## Links', '', f'- [Project site and downloads]({SITE_URL})',
                  f'- [English website guides]({SITE_URL}guides/en/)',
                  '- [Firmware source](https://github.com/phicomm-io/phicomm-community-firmware)',
                  '- [MQTT platform](https://github.com/phicomm-io/phicomm-community-mqtt)', '']
        return '\n'.join(lines)
    lines = ['# 斐讯社区固件', '', '简体中文 · [English](EN-Home)', '',
             '本项目为斐讯 TC1 智能排插、M1 空气检测仪和 A1 空气净化器维护社区固件。',
             '本地网页控制、MQTT 与 Home Assistant 接入，固件免费下载。', '']
    lines += [f'- [打开 IotCloud 设备管理]({SITE_URL}iotcloud/)',
              '- [接入 IotCloud](接入-IotCloud) · [自行部署](IotCloud-部署)', '']
    for _, title, items in GROUPS:
        lines.append(f'## {title}')
        lines.append('')
        for slug, page_title, summary, wiki in items:
            indent = '  ' if slug in PARENTS else ''
            lines.append(f'{indent}- [{page_title}]({wiki_link_target(wiki)})：{summary}')
        lines.append('')
    lines += ['## 交流与反馈', '', '- QQ 交流群：**1021750178**',
              '- [点击加入 QQ 群](https://qm.qq.com/q/9L150V85kk?from=tim)；链接无法打开时，可在 QQ 中搜索群号。',
              '- 反馈问题请附上型号、固件版本和复现步骤，并遮去密码与令牌等信息。', '',
              '## 相关链接', '',
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
    validate_translations(ROOT)
    pages = {'Home.md': home_page(), 'EN-Home.md': home_page('en'), '_Sidebar.md': '[简体中文](Home) · [English](EN-Home)\n'}
    for slug, _, _, wiki in PAGES:
        if slug in WIKI_AUTHORED:
            continue
        source = source_for(slug)
        if not source.exists():
            continue
        text = rewrite(source.read_text(encoding='utf-8'))
        # The Wiki derives its title from the file name; drop the duplicate H1.
        text = re.sub(r'^#\s+.*\n+', '', text, count=1)
        pages[f'{wiki}.md'] = f'[English](EN-{slug}) · [目录](Home)\n\n' + text
    for slug, _, _, wiki in EN_PAGES:
        source = ROOT / 'docs' / 'public' / 'en' / f'{slug}.md'
        if not source.is_file():
            raise FileNotFoundError(f'Missing English guide: {source}')
        text = rewrite(source.read_text(encoding='utf-8'), 'en')
        text = re.sub(r'^#\s+.*\n+', '', text, count=1)
        chinese = PAGE_BY_SLUG[slug]
        pages[f'{wiki}.md'] = f'[简体中文]({wiki_link_target(chinese)}) · [Contents](EN-Home)\n\n' + text
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
