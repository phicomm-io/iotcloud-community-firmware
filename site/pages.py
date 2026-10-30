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
        ('m1-automation', 'M1 自动化、联动与远程显示', '来源设备 ID、字段 ID、双区域显示与读数失效', 'M1-自动化与远程显示'),
        ('m1-mcu-firmware', 'M1 开源传感器主控', '新增功能、主控 Release 与原厂备份', 'M1-主控开源固件'),
        ('m1-mcu-update', 'M1 传感器主控升级', 'Wi-Fi 最低版本、脚本升级与版本确认', 'M1-主控网络升级'),
    ]),
    ('a1', 'A1 空气净化器', [
        ('a1-no-disassembly', 'A1 原厂配网与免拆安装', 'EasyLink 配网、局域网 OTA 与 DNS 升级路径', 'A1-免拆刷机'),
        ('a1-flash', 'A1 有线安装与恢复', '推荐免焊 J-Link 工厂测试点，也可使用串口', 'A1-刷机'),
        ('a1-guide', 'A1 功能与操作', '风机、灯环、夜间模式与联动', 'A1-功能与操作'),
        ('a1-automation', 'A1 自动化、联动与远程显示', '传感器绑定、自动调速与风机输出字段 ID', 'A1-自动化与远程显示'),
    ]),
    ('tc1', 'TC1 智能排插', [
        ('tc1-no-disassembly-ethernet-wifi-ap', '免拆刷机图文教程', '有线网卡、无线网卡与 AP：下载文件并按系统操作', 'TC1-免拆刷机-有线无线AP'),
        ('tc1-no-disassembly', '免拆原理与自备镜像', '原厂 OTA 原理、镜像打包与其他网络拓扑', 'TC1-免拆刷机'),
        ('tc1-flash', 'TC1 有线安装与恢复', 'EMW3031：J-Link 首次安装与恢复', 'TC1-刷机'),
        ('tc1-guide', 'TC1 功能与操作', '六路开关、计量、童锁、定时与按键', 'TC1-功能与操作'),
        ('tc1-automation', 'TC1 自动化、联动与远程显示', 'M1 显示 power 字段与开发版功率规则', 'TC1-自动化与远程显示'),
        ('tc1-features', 'TC1 特色功能', '六路命名、总功率计量、自定义按键与童锁', 'TC1-特色功能'),
    ]),
    ('daikin', 'DAIKIN 大金检测仪', [
        ('daikin-guide', 'DAIKIN 功能、安装与恢复', 'BRY88AX151K：通用 ESP32 固件、读数与 A1 联动', 'DAIKIN-功能与安装'),
        ('daikin-stock-ota', 'DAIKIN 原厂 OTA 安装', 'BRY88AB151K：按键触发、配套发送器与实机迁移', 'DAIKIN-原厂OTA'),
        ('daikin-automation', 'DAIKIN 自动化、联动与远程显示', 'A1 传感器来源、M1 字段选择与中枢职责', 'DAIKIN-自动化与远程显示'),
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
        ('s30-guide', 'S30-V1B 常见功能', '配网、日常使用、升级与恢复', 'S30-V1B-功能与操作'),
        ('s30-automation', 'S30-V1B 自动化、联动与远程显示', '开发版功率规则、MQTT 来源与计量字段 ID', 'S30-V1B-自动化与远程显示'),
        ('s30-features', 'S30-V1B 特色功能', '来电策略、计量校准、灯语与本地按键', 'S30-V1B-特色功能'),
    ]),
    ('dev', '开发与维护', [
        ('iotcloud-deploy', '自行部署 IoTCloud', '静态前端托管、独立后端与 MQTT 连接', 'IotCloud-部署'),
        ('build-deps', '构建依赖', '工具链、SDK 归档与本地构建', '构建依赖'),
        ('sources', '资源来源', '刷机图片与工具的来源与校验', '资源来源'),
        ('release-log', '更新日志', '按日期查阅版本变化，也可比较升级差异', 'Release-Log'),
    ]),
]

GROUPS.sort(key=lambda group: ['m1', 'a1', 'tc1', 's30', 'daikin', 'common', 'dev'].index(group[0]))

# Product documentation uses the same sections on the home page and sidebar.
# Each entry is (slug, Chinese anchor, English anchor, Chinese label, English label).
# Empty labels use the page title; published page URLs remain stable.
DEVICE_SECTIONS = {}
for device, install in {
    'm1': [('m1-no-disassembly', '', '', '原厂免拆：局域网上传／DNS 下载', 'Stock OTA: LAN upload / DNS download'),
           ('m1-flash', 'bootloader-刷机', 'bootloader-flashing', '串口 Bootloader', 'Serial bootloader'),
           ('m1-flash', 'j-link-刷机', 'j-link-flashing', 'J-Link', 'J-Link')],
    'a1': [('a1-no-disassembly', '', '', '原厂免拆：局域网上传／DNS 下载', 'Stock OTA: LAN upload / DNS download'),
           ('a1-flash', 'j-link-刷机', 'j-link-flashing', 'J-Link 工厂测试点', 'J-Link factory test points'),
           ('a1-flash', 'bootloader-刷机', 'bootloader-flashing', '串口 Bootloader', 'Serial bootloader')],
    'tc1': [('tc1-no-disassembly-ethernet-wifi-ap', '', '', '原厂免拆网络安装', 'Stock network installation'),
            ('tc1-no-disassembly', '', '', '免拆原理与自备镜像', 'OTA internals and custom images'),
            ('tc1-flash', '', '', 'J-Link 有线安装', 'Wired J-Link installation')],
    's30': [('s30-flash', '原厂免拆网络安装', 'network-installation-from-stock-firmware', '原厂免拆网络安装', 'Stock network installation'),
            ('s30-flash', '拆机串口方式', 'disassembly-and-uart', '拆机串口安装', 'Serial installation')],
    'daikin': [('daikin-stock-ota', '', '', 'BRY88AB151K 原厂 OTA', 'BRY88AB151K stock OTA'),
               ('daikin-guide', '首次安装', 'first-installation', 'ESP32 串口安装（核对型号）', 'ESP32 serial installation (check model)')],
}.items():
    recovery = {'m1': ('按键与恢复', 'buttons-and-recovery'),
                'a1': ('恢复手势', 'recovery-gestures'),
                'tc1': ('恢复手势', 'recovery-gestures'),
                's30': ('恢复', 'recovery'),
                'daikin': ('日常更新与恢复', 'updates-and-recovery')}[device]
    update = (device + '-guide', '升级', 'updates') if device == 's30' else (
        ('daikin-guide', '日常更新与恢复', 'updates-and-recovery') if device == 'daikin' else ('firmware-update', '', ''))
    features = {
        'm1': [('m1-guide', '颗粒物详情', 'particle-details', '颗粒物与扩展传感器', 'Particles and optional sensors'),
               ('m1-mcu-firmware', '', '', '', ''), ('m1-mcu-update', '', '', '', '')],
        'a1': [('a1-guide', '灯环', 'light-ring', '灯环与滤芯寿命', 'Ring and filter life'),
               ('a1-guide', '可选新风风机', 'optional-fresh-air-fan', '可选新风风机', 'Optional fresh-air fan')],
        'tc1': [('tc1-features', '', '', '', '')],
        's30': [('s30-features', '', '', '', '')],
        'daikin': [('daikin-guide', '读数与操作', 'readings-and-controls', '传感器、补偿与校准', 'Sensors, compensation and calibration'),
                   ('daikin-guide', '中枢版本', 'hub-edition', '可选中枢版本', 'Optional hub edition')],
    }[device]
    DEVICE_SECTIONS[device] = [
        ('install', '刷机', 'Flashing', install),
        ('common', '常见功能', 'Common functions', [
            (device + '-guide', '', '', '使用与配网', 'Use and Wi-Fi setup'),
            (*update, '升级', 'Updates'),
            (device + '-guide', *recovery, '恢复', 'Recovery')]),
        ('automation', '自动化、联动与远程显示', 'Automation, linkage and remote display', [
            (device + '-automation', '', '', '配置方法、设备 ID 与字段 ID', 'Setup, device IDs and field IDs')]),
        ('features', '特色功能', 'Device features', features),
    ]

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
 's30-guide': ('S30-V1B common functions', 'Wi-Fi setup, daily use, updates and recovery'),
 's30-automation': ('S30-V1B automation and remote display', 'Development power rules and MQTT metering field IDs'),
 's30-features': ('S30-V1B features', 'Power restoration, meter calibration, indicators and button'),
 'tc1-automation': ('TC1 automation and remote display', 'The power field for M1 and development power rules'),
 'tc1-features': ('TC1 features', 'Six outlets, total power, button mappings and child lock'),
 'm1-automation': ('M1 automation and remote display', 'Source device IDs, field IDs, two display regions and expiry'),
 'a1-automation': ('A1 automation and remote display', 'Sensor pairing, automatic fan control and output field ID'),
 'daikin-automation': ('DAIKIN automation and remote display', 'A1 sensor source, M1 field selection and hub responsibilities'),
 'a1-flash': ('A1 wired installation and recovery', 'Recommended: solder-free J-Link factory points; serial alternative'),
 'm1-flash': ('M1 wired installation and recovery', 'Recommended: serial bootloader; J-Link alternative'),
 'iotcloud-deploy': ('Deploy IoTCloud', 'China / US nodes, hosting and local devices'),
 'build-deps': ('Build dependencies', 'Toolchains, SDK archives and local builds'),
 'sources': ('Asset sources', 'Image / tool provenance and checksums'),
 'release-log': ('Release Log', 'Release history and upgrade comparisons'),
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
