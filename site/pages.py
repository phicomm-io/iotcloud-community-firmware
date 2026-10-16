#!/usr/bin/env python3
"""Single source of truth for the public documentation structure.

Both the generated site (site/build.py) and the Wiki sync
(tools/release/wiki_sync.py) read this table, so the two public surfaces stay
in step. Order here is the order shown in navigation, the docs index and the
page footers.
"""

# (group id, group title, [(slug, title, summary, wiki page name), ...])
GROUPS = [
    ("common", "开始使用", [
        ("quick-start", "快速开始", "认识三款设备、配网与访问方式", "快速开始"),
        ("flash-prep", "刷机前准备", "供电与接线的通用要求、两个模块的下载接口", "刷机前准备"),
        ("firmware-update", "固件更新", "更新来源、包的选择与失败处理", "固件更新"),
        ("mqtt-link", "MQTT 与联动", "MQTT 模式、管理平台与 A1 × M1 联动", "MQTT与联动"),
        ("faq", "故障排查", "连接状态、灯语总则与常见问题", "故障排查"),
    ]),
    ("tc1", "TC1 智能排插", [
        ("tc1-guide", "功能与操作", "六路开关、计量、童锁、定时与按键", "TC1-功能与操作"),
        ("tc1-flash", "刷机", "EMW3031：J-Link 首次安装与恢复", "TC1-刷机"),
    ]),
    ("m1", "M1 空气检测仪", [
        ("m1-guide", "功能与操作", "读数、屏幕亮度、指示灯与恢复", "M1-功能与操作"),
        ("m1-flash", "刷机", "EMW3080B：保留 Bootloader 的首刷", "M1-刷机"),
    ]),
    ("a1", "A1 空气净化器", [
        ("a1-guide", "功能与操作", "风机、灯环、夜间模式与联动", "A1-功能与操作"),
        ("a1-flash", "刷机", "EMW3080B：完整地址烧录与恢复", "A1-刷机"),
    ]),
    ("dev", "开发与维护", [
        ("build-deps", "构建依赖", "工具链、SDK 归档与本地构建", "构建依赖"),
        ("sources", "资源来源", "刷机图片与工具的来源与校验", "资源来源"),
        ("release-log", "Release Log", "各版本发布说明", "Release-Log"),
    ]),
]

PAGES = [page for _, _, items in GROUPS for page in items]
BY_SLUG = {slug: (title, summary, wiki) for slug, title, summary, wiki in PAGES}
GROUP_OF = {slug: (gid, gtitle) for gid, gtitle, items in GROUPS for slug, _, _, _ in items}

# Public site base URL, used wherever documentation links to the live site
# (downloads, the demo index). One place to change if the site moves hosts.
SITE_URL = 'https://fujr.github.io/phicomm-community-pages/'
