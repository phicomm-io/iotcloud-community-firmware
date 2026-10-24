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
from markdown.extensions.toc import slugify_unicode

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tools/release'))
from pages import ALIASES, GROUPS, PAGES, PARENTS, SITE_URL, EN_GROUPS, EN_PAGES, validate_translations  # noqa: E402
from seo import EN_INDEX_TITLE, EN_INDEX_DESCRIPTION, guide_metadata, metadata, write_discovery  # noqa: E402

EXTENSIONS = ['tables', 'fenced_code', 'attr_list', 'toc']
SITE_NAME = 'PHICOMM COMMUNITY'
# Nav anchors live on the home page, so guides reach them through '../'.
HOME_ANCHORS = [('devices', '设备'), ('features', '功能'), ('iotcloud', 'IotCloud'), ('demos', '在线演示'),
                ('docs', '文档'), ('releases', '下载')]


def esc(text):
    return html.escape(str(text), quote=True)


def header(base, language="zh"):
    if language == "en":
        nav = "".join(f'<a href="{base}#{anchor}">{label}</a>' for anchor, label in [("devices","Devices"),("features","Features"),("iotcloud","IotCloud"),("demos","Demos"),("docs","Documentation"),("releases","Downloads")])
        return (f'<header class="site-header"><div class="wrap"><a class="brand" href="{base}"><span class="mark">P</span><span class="word"><b>{SITE_NAME}</b><span>Community firmware</span></span></a><nav class="site-nav" aria-label="Main navigation">{nav}</nav><div class="prefs"><label class="sr" for="themePick">Theme</label><select id="themePick"><option value="system">System</option><option value="light">Light</option><option value="dark">Dark</option></select></div></div></header>')
    nav = ''.join(f'<a href="{base}#{anchor}">{esc(label)}</a>' for anchor, label in HOME_ANCHORS)
    return (f'<header class="site-header"><div class="wrap">'
            f'<a class="brand" href="{base}./"><span class="mark" aria-hidden="true">斐</span>'
            f'<span class="word"><b>{esc(SITE_NAME)}</b><span>社区固件</span></span></a>'
            f'<nav class="site-nav" aria-label="主导航">{nav}</nav>'
            f'<div class="prefs"><label class="sr" for="themePick">主题</label>'
            f'<select id="themePick"><option value="system">跟随系统</option>'
            f'<option value="light">浅色</option><option value="dark">深色</option></select></div>'
            f'</div></header>')


def footer(language="zh"):
    if language == "en":
        return ('<footer class="site-footer"><div class="wrap">'
                '<div><b>PHICOMM COMMUNITY</b><p>Community-maintained, not affiliated with the manufacturer. '
                'Check the documentation for your release. The firmware repository has no single license covering all code.</p></div>'
                '<div><b>Documentation</b><ul><li><a href="quick-start.html">Quick start</a></li>'
                '<li><a href="faq.html">Troubleshooting</a></li><li><a href="release-log.html">Release Log</a></li></ul></div>'
                '<div><b>Source</b><ul><li><a href="https://github.com/phicomm-io/phicomm-community-firmware">Firmware</a></li>'
                '<li><a href="https://github.com/phicomm-io/phicomm-community-mqtt">MQTT platform</a></li></ul></div>'
                '<div><b>Community</b><p>QQ group: 1021750178</p><a href="https://qm.qq.com/q/9L150V85kk?from=tim" target="_blank" rel="noopener noreferrer">Join the QQ group (Chinese)</a></div></div></footer>')
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
            '</ul></div><div><b>交流与反馈</b><p>QQ 群：1021750178</p>'
            '<a href="https://qm.qq.com/q/9L150V85kk?from=tim" target="_blank" rel="noopener noreferrer">加入 QQ 群</a></div></div></footer>')


def guide_nav(active_slug, language="zh"):
    out = []
    for _, title, items in (EN_GROUPS if language == "en" else GROUPS):
        out.append(f'<p class="guide-group">{esc(title)}</p>')
        for slug, page_title, _, _ in items:
            current = ' aria-current="page"' if slug == active_slug else ''
            child = ' class="guide-child"' if slug in PARENTS else ''
            out.append(f'<a href="{slug}.html"{child}{current}>{esc(page_title)}</a>')
    label = 'Documentation navigation' if language == 'en' else '文档导航'
    return f'<nav class="guide-nav" aria-label="{label}">{"".join(out)}</nav>'


def pager(slug, language="zh"):
    pages = EN_PAGES if language == "en" else PAGES
    order = [page[0] for page in pages]
    index = order.index(slug)
    parts = []
    if index > 0:
        prev = pages[index - 1]
        parts.append(f'<a href="{prev[0]}.html">{"Previous" if language == "en" else "上一页"}<b>{esc(prev[1])}</b></a>')
    if index < len(pages) - 1:
        nxt = pages[index + 1]
        parts.append(f'<a class="next" href="{nxt[0]}.html">{"Next" if language == "en" else "下一页"}<b>{esc(nxt[1])}</b></a>')
    return f'<div class="guide-pager">{"".join(parts)}</div>' if parts else ''


def docs_index():
    """Home-page index of every guide, grouped the same way as the sidebar."""
    out = []
    for _, title, items in GROUPS:
        links = ''.join(
            f'<li class="{"doc-child" if slug in PARENTS else "doc-page"}"><a href="guides/{slug}.html">{esc(page_title)}'
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


def render_guide_body(text, language):
    """Keep OS choices in Markdown; Wiki and no-JS readers retain all steps."""
    def render(fragment):
        return markdown.markdown(fragment, extensions=EXTENSIONS,
                                 extension_configs={'toc': {'slugify': slugify_unicode}})

    def os_block(match):
        systems = match[1].split()
        if not systems or set(systems) - {'windows', 'mac', 'linux'}:
            raise ValueError(f'Unknown guide operating system: {match[1]}')
        return f'<div data-guide-os="{esc(" ".join(systems))}">\n{render(match[2])}\n</div>'

    text = re.sub(r'<!-- os:([\w ]+) -->\s*\n(.*?)<!-- /os -->', os_block, text, flags=re.S)
    legend = 'Your computer' if language == 'en' else '选择电脑系统'
    hint = ('Only instructions for your selected system are shown. Your choice is saved.' if language == 'en'
            else '只展示所选系统的操作，选择会自动保存。')
    picker = (f'<fieldset class="guide-os-picker" hidden><legend>{legend}</legend>'
              '<div class="guide-os-options">' + ''.join(
                  f'<label><input type="radio" name="guide-os" value="{value}"><span>{label}</span></label>'
                  for value, label in [('mac', 'macOS'), ('windows', 'Windows'), ('linux', 'Linux')])
              + f'</div><p>{hint}</p></fieldset>')
    return render(text.replace('<!-- os-picker -->', picker))


def render_guide(slug, title, summary, source, language="zh", notes_manifest=None):
    english = language == "en"
    base = "../../" if english else "../"
    text = source.read_text(encoding="utf-8")
    if english:
        text = text.replace("](assets/", "](../assets/")
    switch = (f'<a href="../{slug}.html" lang="zh-CN">简体中文</a> · English' if english else
              f'简体中文 · <a href="en/{slug}.html" lang="en">English</a>')
    body = render_guide_body(rewrite_links(expand_site_url(text)), language)
    scripts = ''
    if slug == 'release-log' and notes_manifest is not None:
        from notes_ui import body as notes_body
        body = notes_body(notes_manifest, 'en' if english else 'zh-CN')
        scripts = ''.join(f'<script src="{base}releases/notes/{name}.js"></script>'
                          for name in ('core', 'client', 'page'))
    return (f'<!doctype html><html lang="{"en" if english else "zh-CN"}"><head><meta charset="utf-8">'
            f'<meta name="viewport" content="width=device-width,initial-scale=1">'
            f'{guide_metadata(slug, title, summary, language)}'
            f'<link rel="stylesheet" href="{base}style.css"><script src="{base}theme.js"></script></head><body>'
            f'<a class="skip" href="#main">{"Skip to content" if english else "跳到主要内容"}</a>'
            f'{header(base, language)}'
            f'<div class="guide-layout">'
            f'{guide_nav(slug, language)}'
            f'<main class="guide-main" id="main">'
            f'<p class="guide-language" aria-label="Language">{switch}</p><article class="prose">{body}</article>'
            f'{pager(slug, language)}'
            f'</main></div>'
            f'{footer(language)}'
            f'<script src="{base}site.js"></script>{scripts}</body></html>')


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
    parser.add_argument('--archive', type=Path, help='Retained published archive; originals are copied unchanged')
    parser.add_argument('--formal', action='store_true', help='Verify actual firmware, signatures and source locks')
    args = parser.parse_args()
    validate_translations(ROOT)
    out = args.output
    out.mkdir(parents=True, exist_ok=True)
    # Prepare the archive before compiling notes or generating any page. Local
    # builds may use the fixed catalog snapshot; publication requires real files.
    from user_notes import DATA, write_public
    if args.archive:
        if out.resolve() == args.archive.resolve():
            parser.error('output must be separate from the retained archive')
        for folder in ('releases', 'ota'):
            if (args.archive / folder).is_dir():
                shutil.copytree(args.archive / folder, out / folder, dirs_exist_ok=True)
    else:
        (out / 'releases').mkdir(exist_ok=True)
        shutil.copyfile(DATA / 'catalog.snapshot.json', out / 'releases/index.json')
    notes_manifest = write_public(out, archive=out if args.archive else None, formal=args.formal)

    for name in ['style.css', 'theme.js', 'site.js', 'releases.js']:
        shutil.copyfile(HERE / name, out / name)
    (out / '.nojekyll').touch()

    home = (HERE / 'index.html').read_text(encoding='utf-8')
    placeholder = '<div class="doc-groups" id="docs-index"></div>'
    if placeholder not in home:
        raise SystemExit('site/index.html: docs index container not found')
    home = home.replace(placeholder, f'<div class="doc-groups" id="docs-index">{docs_index()}</div>')
    (out / 'index.html').write_text(home, encoding='utf-8')

    # Both languages are built together, so missing translations fail publication.
    english_dir = out / 'guides' / 'en'
    english_dir.mkdir(parents=True, exist_ok=True)
    for slug, title, summary, _ in EN_PAGES:
        source = ROOT / 'docs' / 'public' / 'en' / f'{slug}.md'
        if not source.is_file():
            raise SystemExit(f'Missing English guide: {source}')
        (english_dir / f'{slug}.html').write_text(render_guide(slug, title, summary, source, 'en', notes_manifest), encoding='utf-8')
    english_links = ''.join(f'<section><h2>{esc(title)}</h2><ul>' + ''.join(
        f'<li class="{"doc-child" if slug in PARENTS else "doc-page"}"><a href="{slug}.html">{esc(name)}</a> — {esc(summary)}</li>' for slug, name, summary, _ in items) + '</ul></section>'
        for _, title, items in EN_GROUPS)
    (english_dir / 'index.html').write_text(
        '<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">'
        + metadata(EN_INDEX_TITLE, EN_INDEX_DESCRIPTION, 'guides/en/', 'en') + '<link rel="stylesheet" href="../../style.css">'
        '<script src="../../theme.js"></script></head><body>' + header('../../', 'en') +
        '<main class="wrap prose" id="main"><p><a href="../../#docs" lang="zh-CN">简体中文</a> · English</p>'
        '<h1>English documentation</h1><p>Guides for TC1, M1, A1, DAIKIN and IotCloud.</p>' + english_links + '</main>' + footer('en') +
        '<script src="../../site.js"></script></body></html>', encoding='utf-8')
    sources = markdown_sources()
    missing, guides = [], out / 'guides'
    guides.mkdir(exist_ok=True)
    for slug, title, summary, _ in PAGES:
        source = sources[slug]
        if not source.exists():
            missing.append(f'{slug} ({source.relative_to(ROOT)})')
            continue
        (guides / f'{slug}.html').write_text(render_guide(slug, title, summary, source, notes_manifest=notes_manifest), encoding='utf-8')
    if missing:
        print('Missing guide sources: ' + ', '.join(missing), file=sys.stderr)

    for alias, target in ALIASES.items():
        (guides / f'{alias}.html').write_text(
            '<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">'
            f'<meta http-equiv="refresh" content="0;url={target}.html">'
            f'<link rel="canonical" href="{SITE_URL}guides/{target}.html"><title>刷机教程</title>'
            f'</head><body><a href="{target}.html">查看 A1 / M1 刷机教程</a></body></html>',
            encoding='utf-8')

    # Every demo comes from the same pinned frontend release as firmware UI.
    from frontend_release import fetch
    bundle = fetch(ROOT / 'frontend.lock.json')
    if (out / 'demos').exists():
        shutil.rmtree(out / 'demos')
    shutil.copytree(bundle / 'demos', out / 'demos')
    if not (bundle / 'local/index.html').is_file():
        raise SystemExit('Pinned frontend release has no standalone local console')
    if (out / 'local').exists():
        shutil.rmtree(out / 'local')
    shutil.copytree(bundle / 'local', out / 'local')

    shutil.copytree(ROOT / 'docs' / 'public' / 'assets', guides / 'assets', dirs_exist_ok=True)
    write_discovery(out)
    from user_notes import verify_output
    verify_output(out, formal=args.formal)
    print(f'Wrote {len(PAGES) - len(missing)} guide pages to {out}')


if __name__ == '__main__':
    main()
