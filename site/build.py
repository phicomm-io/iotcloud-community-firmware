#!/usr/bin/env python3
"""Build the public allowlist; never copy the private source tree or Wiki.

Guide pages come from docs/public/*.md, laid out by site/pages.py so file
names, navigation order and the Wiki pages cannot drift apart. The page shell
(header, sidebar, footer) is generated here so every guide shares the site's
design language instead of carrying its own markup.
"""
import argparse, html, re, shutil, sys
from pathlib import Path

import markdown

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / 'common' / 'web'))
from pages import GROUPS, PAGES, SITE_URL  # noqa: E402

EXTENSIONS = ['tables', 'fenced_code', 'attr_list']
SITE_NAME = 'PHICOMM COMMUNITY'
# Nav anchors live on the home page, so guides reach them through '../'.
HOME_ANCHORS = [('devices', '设备'), ('features', '功能'), ('demos', '在线演示'),
                ('docs', '文档'), ('releases', '下载')]


def esc(text):
    return html.escape(str(text), quote=True)


def header(base):
    nav = ''.join(f'<a href="{base}#{anchor}">{esc(label)}</a>' for anchor, label in HOME_ANCHORS)
    return (f'<header class="site-header"><div class="wrap">'
            f'<a class="brand" href="{base}./"><span class="mark" aria-hidden="true">斐</span>'
            f'<span class="word"><b>{esc(SITE_NAME)}</b><span>社区固件</span></span></a>'
            f'<nav class="site-nav" aria-label="主导航">{nav}</nav>'
            f'<div class="prefs"><label class="sr" for="themePick">主题</label>'
            f'<select id="themePick"><option value="system">跟随系统</option>'
            f'<option value="light">浅色</option><option value="dark">深色</option></select></div>'
            f'</div></header>')


def footer():
    return ('<footer class="site-footer"><div class="wrap">'
            f'<div><b>{esc(SITE_NAME)}</b><p style="margin-top:8px">社区维护项目，与原厂无隶属关系。'
            '固件、指南及发布说明以对应版本为准。本仓库没有覆盖全部代码的统一许可证。</p></div>'
            '<div><b>文档</b><ul>'
            '<li><a href="quick-start.html">快速开始</a></li>'
            '<li><a href="faq.html">故障排查</a></li>'
            '<li><a href="build-deps.html">构建依赖</a></li>'
            '<li><a href="release-log.html">Release Log</a></li></ul></div>'
            '<div><b>源码</b><ul>'
            '<li><a href="https://github.com/phicomm-io/phicomm-community-firmware" rel="noopener noreferrer">固件仓库</a></li>'
            '<li><a href="https://github.com/phicomm-io/phicomm-community-mqtt" rel="noopener noreferrer">管理平台</a></li>'
            '</ul></div></div></footer>')


def guide_nav(active_slug):
    out = []
    for _, title, items in GROUPS:
        out.append(f'<p class="guide-group">{esc(title)}</p>')
        for slug, page_title, _, _ in items:
            current = ' aria-current="page"' if slug == active_slug else ''
            out.append(f'<a href="{slug}.html"{current}>{esc(page_title)}</a>')
    return f'<nav class="guide-nav" aria-label="文档导航">{"".join(out)}</nav>'


def pager(slug):
    order = [page[0] for page in PAGES]
    index = order.index(slug)
    parts = []
    if index > 0:
        prev = PAGES[index - 1]
        parts.append(f'<a href="{prev[0]}.html">上一页<b>{esc(prev[1])}</b></a>')
    if index < len(PAGES) - 1:
        nxt = PAGES[index + 1]
        parts.append(f'<a class="next" href="{nxt[0]}.html">下一页<b>{esc(nxt[1])}</b></a>')
    return f'<div class="guide-pager">{"".join(parts)}</div>' if parts else ''


def docs_index():
    """Home-page index of every guide, grouped the same way as the sidebar."""
    out = []
    for _, title, items in GROUPS:
        links = ''.join(
            f'<li><a href="guides/{slug}.html">{esc(page_title)}'
            f'<span>{esc(summary)}</span></a></li>'
            for slug, page_title, summary, _ in items)
        out.append(f'<div class="doc-group"><h3>{esc(title)}</h3><ul>{links}</ul></div>')
    return ''.join(out)


def rewrite_links(text):
    """Point inter-page links at the generated .html names.

    Guide markdown links a sibling by slug (``quick-start`` or
    ``quick-start.md``), which is also what the Wiki sync understands.
    """
    known = {page[0] for page in PAGES}

    def one(match):
        target = match.group(1).strip()
        anchor = ''
        if '#' in target:
            target, anchor = target.split('#', 1)
            anchor = '#' + anchor
        if target in known:
            return f']({target}.html{anchor})'
        if target.endswith('.md') and target[:-3] in known:
            return f']({target[:-3]}.html{anchor})'
        return match.group(0)

    return re.sub(r'\]\(([^\s)]+)\)', one, text)


def expand_site_url(text):
    """Fill the {{SITE_URL}} placeholder that docs use to link the live site."""
    return text.replace('{{SITE_URL}}', SITE_URL)


def render_guide(slug, title, summary, source):
    body = markdown.markdown(
        rewrite_links(expand_site_url(source.read_text(encoding='utf-8'))), extensions=EXTENSIONS)
    return (f'<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">'
            f'<meta name="viewport" content="width=device-width,initial-scale=1">'
            f'<meta name="description" content="{esc(summary)}">'
            f'<title>{esc(title)} · {esc(SITE_NAME)}</title>'
            f'<link rel="stylesheet" href="../style.css"><script src="../theme.js"></script></head><body>'
            f'<a class="skip" href="#main">跳到主要内容</a>'
            f'{header("../")}'
            f'<div class="guide-layout">'
            f'{guide_nav(slug)}'
            f'<main class="guide-main" id="main">'
            f'<article class="prose">{body}</article>'
            f'{pager(slug)}'
            f'</main></div>'
            f'{footer()}'
            f'<script src="../site.js"></script></body></html>')


def markdown_sources():
    """slug -> source path. Release Log is frozen from the Wiki into docs/release."""
    sources = {}
    for slug, _, _, _ in PAGES:
        public = ROOT / 'docs' / 'public' / f'{slug}.md'
        if slug == 'release-log':
            frozen = ROOT / 'docs' / 'release' / 'Release-Log.md'
            sources[slug] = frozen if frozen.exists() else public
        else:
            sources[slug] = public
    return sources


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    out = args.output
    out.mkdir(parents=True, exist_ok=True)

    for name in ['style.css', 'theme.js', 'site.js', 'releases.js']:
        shutil.copyfile(HERE / name, out / name)
    (out / '.nojekyll').touch()

    home = (HERE / 'index.html').read_text(encoding='utf-8')
    placeholder = '<div class="doc-groups" id="docs-index"></div>'
    if placeholder not in home:
        raise SystemExit('site/index.html: docs index container not found')
    home = home.replace(placeholder, f'<div class="doc-groups" id="docs-index">{docs_index()}</div>')
    (out / 'index.html').write_text(home, encoding='utf-8')

    sources = markdown_sources()
    missing, guides = [], out / 'guides'
    guides.mkdir(exist_ok=True)
    for slug, title, summary, _ in PAGES:
        source = sources[slug]
        if not source.exists():
            missing.append(f'{slug} ({source.relative_to(ROOT)})')
            continue
        (guides / f'{slug}.html').write_text(render_guide(slug, title, summary, source), encoding='utf-8')
    if missing:
        print('Missing guide sources: ' + ', '.join(missing), file=sys.stderr)

    # Demos render the same firmware sources through an isolated adapter.
    from page import render_page
    for model, source in [('tc1', 'TC1/http_server/web/index.html'),
                          ('m1', 'M1/web/index.html'),
                          ('a1', 'A1/web/index.html')]:
        dest = out / 'demos' / model
        dest.mkdir(parents=True, exist_ok=True)
        adapter = (ROOT / 'common' / 'web' / 'demo.js').read_text(encoding='utf-8').replace('__MODEL__', model)
        text = render_page(ROOT / source).decode()
        text = text.replace('<head>', '<head><script>' + adapter + '</script>', 1)
        (dest / 'index.html').write_text(text, encoding='utf-8')

    shutil.copytree(ROOT / 'docs' / 'public' / 'assets', guides / 'assets', dirs_exist_ok=True)
    print(f'Wrote {len(PAGES) - len(missing)} guide pages to {out}')


if __name__ == '__main__':
    main()
