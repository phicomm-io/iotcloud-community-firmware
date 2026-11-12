# TC1 智能排插（A1 硬件版本）

## 功能

- 六路插口独立开关、总开关与原电源按键。
- 实时功率、总电量、今日与昨日电量统计；可设置计量系数。
- 定时任务、按键连击与长按动作、童锁、电源指示灯设置。
- 网页配置 Wi-Fi、MQTT、设备名、插口名称、上报周期与管理密码。
- Home Assistant 自动发现、本地文件及 HTTP 地址 OTA。

仅支持带 EMW3031 模块的 TC1 **A1 硬件版本**，不支持 A2，也不是 A1 空气净化器。

## 构建方法

Linux x86_64，推荐 Ubuntu 22.04。安装 `build-essential bzip2 curl git perl python3 zlib1g-dev libncurses5 libtinfo5`，在仓库根目录执行：

```sh
./TC1/build.sh
```

三款固件统一使用 `common/mico-sdk.json` 固定的 MiCO 3.6.4（提交 `6c465211d3ff8797cd835e400ec54a06530dd476`）。共享下载源缓存为 `build/mico-upstream.git`；TC1 板级补丁应用于 `build/tc1-sdk-3.6.4/mico-os`，构建目录为 `build/tc1-workspace-3.6.4/`。旧版 `build/tc1-sdk/` 缓存不受影响。

脚本下载并校验 ARM GCC 5.4.1 和 Python 2.7.18，生成网页资源后构建 `TC1@MK3031@moc`。可用绝对路径 `TC1_TOOLS_DIR` 指定工具缓存，用 `TC1_PYTHON2` 指定 Python 2，用 `TC1_SDK_DIR` 指定该提交的独立 Git checkout。离线 SDK 和共享缓存用法见[构建依赖](../docs/public/build-deps.md)。`TC1_SDK_ARCHIVE` 已由公共 Git 来源替代。

MiCO 升级涉及参数 CRC 格式和闭源内核更新，差异及验证见[迁移说明](../docs/validation/mico-sdk-unification-20261016.md)。旧固件的签名 OTA 校验固定了旧内核摘要，不能直接通过该路径安装新内核；已在一台用户授权的 TC1 上通过未签名恢复通道验证迁移与配置读回保留，其他设备及长期运行尚未验证。

产物在 `build/tc1-release/`：`phicomm_tc1_a1.ota.bin`、`phicomm_tc1_a1.all.bin`、应用及 ELF/MAP、`manifest.json`、`SHA256SUMS`。网页升级只选 `.ota.bin`。构建不会递增版本，也不会自动刷机。

## 使用方法

首次连接 `TC1-AP-<MAC后六位>` 热点，访问 `http://192.168.0.1/`，设置家庭 Wi-Fi；连接后通过路由器查询 DHCP 地址访问网页。

填写 MQTT Broker 后，在 Home Assistant 启用 MQTT 自动发现。插口名称、设备名称、童锁、功率和电量可在网页与 HA 中配置或查看。按键动作可自定义；默认长按约 5 秒配网，约 10 秒恢复出厂，恢复出厂会清除配置。童锁开启时，可在上电时按住电源键进入配网热点。

固件升级选择 Releases 中的 `phicomm_tc1_a1.ota.bin`，详细步骤见 [TC1 有线安装与恢复](https://phicomm-io.github.io/guides/tc1-flash.html)（首次安装的通用要求见 [刷机前准备](https://phicomm-io.github.io/guides/flash-prep.html)）。TC1 OTA 不提供 M1/A1 的完整产品校验，务必核对型号和下载校验和。

### 原生 MessagePack SDK profile

TC1 的配网保留产品自身的按键/恢复 SoftAP、WebUI 的 `config.set`（`section: wifi`）、WiFi 扫描、连接验证与失败回滚。SDK 的 WLAN 自动配网在本产品中原本未启用；CLI 的扫描/状态功能和 WiFi/AP/EasyLink 驱动源码仍保留。

该 profile 不再启动旧 SDK 8000 端口的 `/config-read`、`/config-write`、`/config-write-uap` JSON 管理服务，也不再广播指向该端口的 `_easylink._tcp`。原生产品的 MQTT 发现协议继续保留。这是去除旧 JSON 兼容端点，不是替换按键进入的产品 SoftAP。私有 SDK 构建排除对应 config server/menu 源文件，并使用无浮点 printf 的 nano libc；最终 ELF 必须通过 `check_no_json_objects.py`。离线配网回归不代表实机配网验收。
