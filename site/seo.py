"""Search metadata and discovery files for public, canonical documentation URLs."""
from html import escape
from xml.etree import ElementTree as ET

from pages import PAGES, SITE_URL

SITE_TITLE = '斐讯社区固件 · PHICOMM COMMUNITY'
EN_INDEX_TITLE = 'TC1 / M1 / A1 / DAIKIN / S30-V1B community firmware documentation'
EN_INDEX_DESCRIPTION = 'Open-source firmware for discontinued IoT devices or devices whose cloud service providers have ended support. Guides for TC1, M1, A1, DAIKIN, S30-V1B and IoTCloud.'
PRODUCT_TITLES = {
    'm1-mcu-update': '斐讯 M1 主控 MCU 网络升级教程（免拆机）',
    'tc1-guide': '斐讯 TC1 智能排插功能与操作',
    'tc1-flash': '斐讯 TC1 有线刷机与恢复（A1 硬件版本）',
    'tc1-no-disassembly': '斐讯 TC1 免拆刷机：原理与自备镜像',
    'tc1-no-disassembly-ethernet-wifi-ap': '斐讯 TC1 免拆刷机图文教程（有线网卡、无线网卡与 AP）',
    'm1-mcu-firmware': '斐讯 M1 ATSAMD20G18A 主控开源固件、下载与原厂备份',
    'm1-no-disassembly': '斐讯 M1 原厂配网与免拆刷机：局域网 OTA / DNS 迁移',
    'a1-no-disassembly': '斐讯 A1 原厂配网与免拆刷机：局域网 OTA / DNS 迁移',
    'm1-guide': '斐讯 M1 空气检测仪功能与操作',
    'daikin-guide': 'DAIKIN BRY88AX151K 空气检测仪功能与安装',
    'a1-guide': '斐讯 A1 空气净化器功能与操作',
    'a1-flash': '斐讯 A1 有线刷机：免焊 J-Link 与串口恢复',
    'm1-flash': '斐讯 M1 有线刷机：串口 Bootloader 与 J-Link 恢复',
}


def metadata(title, description, path, language='zh-CN', alternates=()):
    """All URLs are absolute; translations keep their own canonical URL."""
    esc = lambda value: escape(str(value), quote=True)
    url = SITE_URL + path
    tags = [f'<title>{esc(title)}</title>',
            f'<meta name="description" content="{esc(description)}">',
            f'<link rel="canonical" href="{esc(url)}">',
            '<meta property="og:type" content="website">',
            '<meta property="og:site_name" content="PHICOMM COMMUNITY">',
            f'<meta property="og:title" content="{esc(title)}">',
            f'<meta property="og:description" content="{esc(description)}">',
            f'<meta property="og:url" content="{esc(url)}">',
            f'<meta property="og:locale" content="{"en_US" if language == "en" else "zh_CN"}">',
            '<meta name="twitter:card" content="summary">',
            f'<meta name="twitter:title" content="{esc(title)}">',
            f'<meta name="twitter:description" content="{esc(description)}">']
    tags.extend(f'<link rel="alternate" hreflang="{esc(lang)}" href="{esc(SITE_URL + target)}">'
                for lang, target in alternates)
    return ''.join(tags)


def guide_metadata(slug, title, summary, language='zh'):
    path = f'guides/{"en/" if language == "en" else ""}{slug}.html'
    title = f'{title} · Phicomm community firmware' if language == 'en' else f'{PRODUCT_TITLES.get(slug, title)} · {SITE_TITLE}'
    alternates = [('zh-CN', f'guides/{slug}.html'),
                  ('en', f'guides/en/{slug}.html'),
                  ('x-default', f'guides/{slug}.html')]
    return metadata(title, summary, path, language, alternates)


def write_discovery(output):
    """List real content only; aliases, binaries and control apps are omitted.

    No fabricated lastmod dates: rebuilding alone does not update the content.
    """
    paths = ['', 'en/', 'guides/en/'] + [f'guides/{prefix}{slug}.html'
                                for prefix in ('', 'en/') for slug, *_ in PAGES]
    namespace = 'http://www.sitemaps.org/schemas/sitemap/0.9'
    ET.register_namespace('', namespace)
    root = ET.Element(f'{{{namespace}}}urlset')
    for path in paths:
        file = output / (path + 'index.html' if not path or path.endswith('/') else path)
        if not file.is_file():
            raise ValueError(f'Sitemap target is missing: {path or "/"}')
        entry = ET.SubElement(root, f'{{{namespace}}}url')
        ET.SubElement(entry, f'{{{namespace}}}loc').text = SITE_URL + path
    ET.indent(root, space='  ')
    ET.ElementTree(root).write(output / 'sitemap.xml', encoding='utf-8', xml_declaration=True)
    (output / 'robots.txt').write_text(
        f'User-agent: *\nAllow: /\n\nSitemap: {SITE_URL}sitemap.xml\n', encoding='utf-8')
