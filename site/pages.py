#!/usr/bin/env python3
"""Single source of truth for the public documentation structure.

Both the generated site (site/build.py) and the Wiki sync
(tools/release/wiki_sync.py) read this table, so the two public surfaces stay
in step. Order here is the order shown in navigation, the docs index and the
page footers.
"""

# (group id, group title, [(slug, title, summary, wiki page name), ...])
GROUPS = [
    ('m1', 'M1 空气检测仪', [
        ('m1-no-disassembly', 'M1 原厂配网与免拆安装', '热点 / Linux EasyLink 配网，局域网 / DNS 两种 OTA', 'M1-免拆刷机'),
        ('m1-flash', 'M1 有线安装与恢复', '推荐串口 Bootloader，也可使用 J-Link', 'M1-刷机'),
        ('m1-guide', 'M1 功能与操作', '读数、屏幕亮度、指示灯与恢复', 'M1-功能与操作'),
        ('m1-mcu-firmware', 'M1 开源传感器主控', '新增功能、主控 Release 与原厂备份', 'M1-主控开源固件'),
        ('m1-mcu-update', 'M1 传感器主控升级', 'Wi-Fi 最低版本、脚本升级与版本确认', 'M1-主控网络升级'),
    ]),
    ('a1', 'A1 空气净化器', [
        ('a1-no-disassembly', 'A1 原厂配网与免拆安装', 'EasyLink 配网、已验证的局域网 OTA 与 DNS 升级路径', 'A1-免拆刷机'),
        ('a1-flash', 'A1 有线安装与恢复', '推荐免焊 J-Link 工厂测试点，也可使用串口', 'A1-刷机'),
        ('a1-guide', 'A1 功能与操作', '风机、灯环、夜间模式与联动', 'A1-功能与操作'),
    ]),
    ('tc1', 'TC1 智能排插', [
        ('tc1-no-disassembly-ethernet-wifi-ap', '免拆刷机图文教程', '有线网卡、无线网卡与 AP：下载文件并按系统操作', 'TC1-免拆刷机-有线无线AP'),
        ('tc1-no-disassembly', '免拆原理与自备镜像', '原厂 OTA 原理、镜像打包与其他网络拓扑', 'TC1-免拆刷机'),
        ('tc1-flash', 'TC1 有线安装与恢复', 'EMW3031：J-Link 首次安装与恢复', 'TC1-刷机'),
        ('tc1-guide', 'TC1 功能与操作', '六路开关、计量、童锁、定时与按键', 'TC1-功能与操作'),
    ]),
    ('daikin', 'DAIKIN 大金检测仪', [
        ('daikin-guide', 'DAIKIN 功能、安装与恢复', 'BRY88AX151K：通用 ESP32 固件、读数与 A1 联动', 'DAIKIN-功能与安装'),
        ('daikin-stock-ota', 'DAIKIN 原厂 OTA 安装', 'BRY88AB151K：按键触发、配套发送器与实机迁移', 'DAIKIN-原厂OTA'),
    ]),
    ('common', '通用设置与排查', [
        ('quick-start', '快速开始', '设备、配网与访问方式', '快速开始'),
        ('flash-prep', '刷机前准备', '供电与接线的通用要求、两个模块的下载接口', '刷机前准备'),
        ('firmware-update', '固件更新', '更新来源、包的选择与失败处理', '固件更新'),
        ('iotcloud-connect', '接入 IoTCloud', '注册账号、配置 MQTT 与确认设备上线', '接入-IotCloud'),
        ('mqtt-link', 'MQTT 与联动', 'MQTT 模式、管理平台与 A1 数据联动', 'MQTT与联动'),
        ('faq', '故障排查', '连接状态、灯语总则与常见问题', '故障排查'),
    ]),
    ('s30', 'S30-V1B 计量插座', [
        ('s30-flash', '免拆与串口刷机', '原厂网络 OTA、VTX / VRX、完整备份与应用镜像', 'S30-V1B-刷机'),
        ('s30-guide', '功能与操作', '配网、单路开关、红蓝灯、长按恢复与计量', 'S30-V1B-功能与操作'),
    ]),
    ('dev', '开发与维护', [
        ('iotcloud-deploy', '自行部署 IoTCloud', '静态前端托管、独立后端与 MQTT 连接', 'IotCloud-部署'),
        ('build-deps', '构建依赖', '工具链、SDK 归档与本地构建', '构建依赖'),
        ('sources', '资源来源', '刷机图片与工具的来源与校验', '资源来源'),
        ('release-log', '更新日志 / Release Log', '按日期查阅版本变化与 v1 Beta 功能', 'Release-Log'),
    ]),
]

# (action, guide slug, Chinese section, English section). Reuse published guides
# rather than making separate copies of shared update and recovery instructions.
DEVICE_ACTIONS = {
    'm1': [('install', 'm1-no-disassembly', '', ''),
           ('update', 'firmware-update', '', ''),
           ('use', 'm1-guide', '', ''),
           ('recover', 'm1-guide', '按键与恢复', 'buttons-and-recovery')],
    'a1': [('install', 'a1-no-disassembly', '', ''),
           ('update', 'firmware-update', '', ''),
           ('use', 'a1-guide', '', ''),
           ('recover', 'a1-guide', '恢复手势', 'recovery-gestures')],
    'tc1': [('install', 'tc1-no-disassembly-ethernet-wifi-ap', '', ''),
            ('update', 'firmware-update', '', ''),
            ('use', 'tc1-guide', '', ''),
            ('recover', 'tc1-guide', '恢复手势', 'recovery-gestures')],
    'daikin': [('install', 'daikin-stock-ota', '', ''),
               ('update', 'daikin-guide', '日常更新与恢复', 'updates-and-recovery'),
               ('use', 'daikin-guide', '', ''),
               ('recover', 'daikin-guide', '日常更新与恢复', 'updates-and-recovery')],
}

# A shared guide can appear under both products; build and sync it only once.
PAGES = list({page[0]: page for _, _, items in GROUPS for page in items}.values())

# Child guides share their parent's product group and follow it in navigation.
PARENTS = {"tc1-no-disassembly": "tc1-no-disassembly-ethernet-wifi-ap"}

# Preserve published URLs when guides are consolidated.
ALIASES = {"a1-m1-flash": "quick-start", "stock-provision": "quick-start",
           "v1-features": "release-log?channel=candidate#v1-overview"}
BY_SLUG = {slug: (title, summary, wiki) for slug, title, summary, wiki in PAGES}
GROUP_OF = {slug: (gid, gtitle) for gid, gtitle, items in GROUPS for slug, _, _, _ in items}

# Public site base URL, used wherever documentation links to the live site
# (downloads, the demo index). One place to change if the site moves hosts.
SITE_URL = 'https://phicomm-io.github.io/'

# English guides share the same slugs, ordering and asset collection.
EN_TITLES = {
 'm1-mcu-update': ('Update the M1 controller over the network', 'Minimum Wi-Fi version, Python flashing and runtime verification'),
 'm1-mcu-firmware': ('M1 open-source controller firmware', 'Added capabilities, controller releases and stock backup'),
 'daikin-stock-ota': ('Daikin stock OTA migration', 'BRY88AB151K button gesture, sender and verified first boot'),
 'a1-no-disassembly': ('A1 stock Wi-Fi setup and OTA installation', 'EasyLink, verified LAN OTA and a firmware-derived DNS update path'),
 'm1-no-disassembly': ('M1 stock Wi-Fi setup and OTA installation', 'Hotspot / Linux EasyLink setup and LAN / DNS OTA'),
 'quick-start': ('Quick start', 'Devices, Wi-Fi setup and local access'),
 'flash-prep': ('Before flashing', 'Power, backups and image selection'),
 'firmware-update': ('Firmware updates', 'OTA sources, packages and recovery'),
 'iotcloud-connect': ('Connect to IoTCloud', 'Register, configure MQTT and bring devices online'),
 'mqtt-link': ('MQTT and automation', 'Home Assistant, IoTCloud and A1 / M1 pairing'),
 'faq': ('Troubleshooting', 'Indicators, recovery and common questions'),
 'tc1-guide': ('TC1 operation', 'Outlets, metering, schedules and buttons'),
 'tc1-flash': ('Flash TC1', 'EMW3031 first installation and recovery'),
 'tc1-no-disassembly': ('OTA internals and custom images', 'Factory OTA, image packing and alternative network topologies'),
 'tc1-no-disassembly-ethernet-wifi-ap': ('Illustrated OTA installation', 'Ethernet, Wi-Fi and an AP: downloads and OS-specific instructions'),
 'm1-guide': ('M1 operation', 'Readings, brightness, icons and recovery'),
 'daikin-guide': ('DAIKIN operation and installation', 'BRY88AX151K: universal ESP32 firmware, readings and A1 automation'),
 'a1-guide': ('A1 operation', 'Fan, ring, night mode and automation'),
 's30-flash': ('S30-V1B network and serial installation', 'Stock network OTA, UART test points, backups and application images'),
 's30-guide': ('S30-V1B operation', 'Wi-Fi, outlet, red/blue LEDs, recovery gestures and metering'),
 'a1-flash': ('A1 wired installation and recovery', 'Recommended: solder-free J-Link factory points; serial alternative'),
 'm1-flash': ('M1 wired installation and recovery', 'Recommended: serial bootloader; J-Link alternative'),
 'iotcloud-deploy': ('Deploy IoTCloud', 'China / US nodes, hosting and local devices'),
 'build-deps': ('Build dependencies', 'Toolchains, SDK archives and local builds'),
 'sources': ('Asset sources', 'Image / tool provenance and checksums'),
 'release-log': ('Release Log', 'Release notes and historical validation scope'),
}
EN_GROUP_NAMES = {'m1': 'M1 air quality monitor', 'a1': 'A1 air purifier',
                  'tc1': 'TC1 power strip', 'daikin': 'DAIKIN air quality monitor',
                  's30': 'S30-V1B metering socket', 'common': 'Shared setup and troubleshooting', 'dev': 'Development and maintenance'}
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
