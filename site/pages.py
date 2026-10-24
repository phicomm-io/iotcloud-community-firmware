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
        ("quick-start", "快速开始", "设备、配网与访问方式", "快速开始"),
        ("flash-prep", "刷机前准备", "供电与接线的通用要求、两个模块的下载接口", "刷机前准备"),
        ("firmware-update", "固件更新", "更新来源、包的选择与失败处理", "固件更新"),
        ("iotcloud-connect", "接入 IotCloud", "注册账号、配置 MQTT 与确认设备上线", "接入-IotCloud"),
        ("mqtt-link", "MQTT 与联动", "MQTT 模式、管理平台与 A1 数据联动", "MQTT与联动"),
        ("faq", "故障排查", "连接状态、灯语总则与常见问题", "故障排查"),
    ]),
    ("tc1", "TC1 智能排插", [
        ("tc1-guide", "功能与操作", "六路开关、计量、童锁、定时与按键", "TC1-功能与操作"),
        ("tc1-flash", "刷机", "EMW3031：J-Link 首次安装与恢复", "TC1-刷机"),
        ("tc1-no-disassembly", "免拆刷机", "原厂配网服务触发 TFTP OTA，无需拆机", "TC1-免拆刷机"),
        ("tc1-no-disassembly-ethernet-wifi-ap", "一个有线＋一个无线＋一个 AP", "图文步骤、配套文件与 Windows / Linux / macOS 命令", "TC1-免拆刷机-有线无线AP"),
    ]),
    ("m1", "M1 空气检测仪", [
        ("m1-guide", "功能与操作", "读数、屏幕亮度、指示灯与恢复", "M1-功能与操作"),
        ("stock-provision", "原厂固件配网（免 App）", "A1 EasyLink 与 M1 热点配网，按系统显示命令", "A1-M1-原厂配网"),
        ("m1-no-disassembly", "免拆刷机：内网 / 公网", "原厂 OTA 上传、镜像要求与 DNS 迁移边界", "M1-免拆刷机"),
        ("a1-m1-flash", "刷机：J-Link / Bootloader", "共用教程：模组飞线、工厂测试点与串口刷机", "A1-M1-刷机"),
    ]),
    ("daikin", "DAIKIN 空气检测仪", [
        ("daikin-guide", "功能、安装与恢复", "BRY88AX151K：通用 ESP32 固件、读数与 A1 联动", "DAIKIN-功能与安装"),
    ]),
    ("a1", "A1 空气净化器", [
        ("a1-guide", "功能与操作", "风机、灯环、夜间模式与联动", "A1-功能与操作"),
        ("stock-provision", "原厂固件配网（免 App）", "A1 EasyLink 与 M1 热点配网，按系统显示命令", "A1-M1-原厂配网"),
        ("a1-no-disassembly", "免拆刷机：内网 / 公网", "原厂 OTA 上传、镜像要求与 DNS 迁移边界", "A1-免拆刷机"),
        ("a1-m1-flash", "刷机：J-Link / Bootloader", "共用教程：模组飞线、工厂测试点与串口刷机", "A1-M1-刷机"),
    ]),
    ("dev", "开发与维护", [
        ("iotcloud-deploy", "IotCloud 部署", "静态前端托管、独立后端与 MQTT 连接", "IotCloud-部署"),
        ("build-deps", "构建依赖", "工具链、SDK 归档与本地构建", "构建依赖"),
        ("sources", "资源来源", "刷机图片与工具的来源与校验", "资源来源"),
        ("release-log", "Release Log", "各版本发布说明", "Release-Log"),
    ]),
]

# A shared guide can appear under both products; build and sync it only once.
PAGES = list({page[0]: page for _, _, items in GROUPS for page in items}.values())

# Child guides share their parent's product group and follow it in navigation.
PARENTS = {"tc1-no-disassembly-ethernet-wifi-ap": "tc1-no-disassembly"}

# Preserve published URLs when guides are consolidated.
ALIASES = {"a1-flash": "a1-m1-flash", "m1-flash": "a1-m1-flash"}
BY_SLUG = {slug: (title, summary, wiki) for slug, title, summary, wiki in PAGES}
GROUP_OF = {slug: (gid, gtitle) for gid, gtitle, items in GROUPS for slug, _, _, _ in items}

# Public site base URL, used wherever documentation links to the live site
# (downloads, the demo index). One place to change if the site moves hosts.
SITE_URL = 'https://phicomm-io.github.io/'

# English guides share the same slugs, ordering and asset collection.
EN_TITLES = {
 'stock-provision': ('Stock Wi-Fi setup without the app', 'A1 EasyLink and M1 hotspot setup on Windows, macOS and Linux'),
 'a1-no-disassembly': ('A1 no-disassembly migration', 'LAN OTA uploads and public DNS migration prerequisites'),
 'm1-no-disassembly': ('M1 no-disassembly migration', 'Factory LAN OTA, image requirements and DNS migration limits'),
 'quick-start': ('Quick start', 'Devices, Wi-Fi setup and local access'),
 'flash-prep': ('Before flashing', 'Power, backups and image selection'),
 'firmware-update': ('Firmware updates', 'OTA sources, packages and recovery'),
 'iotcloud-connect': ('Connect to IotCloud', 'Register, configure MQTT and bring devices online'),
 'mqtt-link': ('MQTT and automation', 'Home Assistant, IotCloud and A1 / M1 pairing'),
 'faq': ('Troubleshooting', 'Indicators, recovery and common questions'),
 'tc1-guide': ('TC1 operation', 'Outlets, metering, schedules and buttons'),
 'tc1-flash': ('Flash TC1', 'EMW3031 first installation and recovery'),
 'tc1-no-disassembly': ('Flash TC1 without disassembly', 'Stock provisioning OTA over TFTP, no case opening'),
 'tc1-no-disassembly-ethernet-wifi-ap': ('One Ethernet + one Wi-Fi + one AP', 'Illustrated steps, downloads and Windows / Linux / macOS commands'),
 'm1-guide': ('M1 operation', 'Readings, brightness, icons and recovery'),
 'daikin-guide': ('DAIKIN operation and installation', 'BRY88AX151K: universal ESP32 firmware, readings and A1 automation'),
 'a1-guide': ('A1 operation', 'Fan, ring, night mode and automation'),
 'a1-m1-flash': ('Flash A1 / M1', 'SWD, factory points and serial bootloader'),
 'iotcloud-deploy': ('Deploy IotCloud', 'China / US nodes, hosting and local devices'),
 'build-deps': ('Build dependencies', 'Toolchains, SDK archives and local builds'),
 'sources': ('Asset sources', 'Image / tool provenance and checksums'),
 'release-log': ('Release Log', 'Release notes and historical validation scope'),
}
EN_GROUP_NAMES = {'common':'Getting started','tc1':'TC1 power strip',
                  'm1':'M1 air quality monitor','daikin':'DAIKIN air quality monitor','a1':'A1 air purifier',
                  'dev':'Development and maintenance'}
EN_GROUPS = [(gid, EN_GROUP_NAMES[gid], [
    (slug, *EN_TITLES[slug], 'EN-' + slug) for slug, _, _, _ in items
]) for gid, _, items in GROUPS]
EN_PAGES = list({page[0]: page for _, _, items in EN_GROUPS for page in items}.values())


def validate_translations(root):
    """Reject missing or stale English guides before publishing either surface."""
    import hashlib
    import json
    sources = json.loads((root / 'docs/public/en/source-hashes.json').read_text())
    expected = {slug for slug, _, _, _ in PAGES}
    if set(sources) != expected:
        raise ValueError('English translation manifest does not match guide manifest')
    for slug in expected:
        chinese = root / ('docs/release/Release-Log.md' if slug == 'release-log' else f'docs/public/{slug}.md')
        english = root / f'docs/public/en/{slug}.md'
        if not english.is_file() or sources[slug] != hashlib.sha256(chinese.read_bytes()).hexdigest():
            raise ValueError(f'English translation needs review: {slug}')
