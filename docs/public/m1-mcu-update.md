# M1 主控 MCU 网络升级

M1 主控固件可以通过网络更新，**不需要 J-Link，也不需要拆机**。当前使用电脑上的 Python 脚本升级，电脑需能通过局域网访问 M1。

**本文刷的是“传感器主控”，不是“Wi-Fi MCU”。** EMW3080 联网模块负责 Wi-Fi 和网页；ATSAMD20G18A 传感器主控负责与机内传感器通信，以及屏幕、按键和蜂鸣器。为降低新手误刷导致设备无法使用的风险，传感器主控没有 WebUI 上传入口。请使用下文电脑脚本，通过 MCU 升级 API 烧录并校验；普通网页“固件更新”只更新 Wi-Fi MCU。

**先核对主控版本：推荐原厂 11.x 迁移，17.1.2.3 / 17.x 差异较大，暂不建议迁移。** 本教程不会把联网版本当作主控版本；版本未知时先查明硬件与原始版本。

<!-- os-picker -->

## 1. 确认 Wi-Fi 固件版本

原厂 11.x 主控需要支持**整包缓冲转发**的社区 Wi-Fi 固件。不要仅凭版本号判断：通过 `capabilities.get` 或 `/api/v1/capabilities` 检查 `data.mcu_ota_max_bytes`，当前实现为 **32768**。本教程测试组合使用 **M1 Wi-Fi v1.0.0-rc.3 与传感器主控 v1.0.0-rc**，仍须核对缓冲能力后再更新。**RC 实际属于 Beta 测试版本，后续仍会有较大改动，不建议日常使用；日常联网固件推荐稳定版 v0.9.27。** 旧版 v0.9.26 起提供的逐块桥接，以及 `1.0.0-39a5b0`，均不包含这个新能力。

原因是原厂 11.1.2.1 在开始接收后有约 10 秒的复位窗口。逐块 HTTP 往返可能超过这个窗口；新接口先在 Wi-Fi 模块接收完整文件，再在本机快速逐块发送并核对应答。新的电脑工具会自动选用它；原厂主控配旧 Wi-Fi 时，会在发送升级帧前拒绝。已经运行社区主控的设备仍可使用旧逐块桥接。

如需在网页中使用主控的 RTTTL 音乐、四种提示音和闹钟曲目，配套 Wi-Fi 固件需要 **v0.9.27 或更新版本**，还需另外刷入主控 v0.9.3 或更新版本并通过能力检测；网页颗粒物详情需要主控 v0.9.4 的完整帧能力。只升级 Wi-Fi 不会自动获得这些功能，详见[主控功能与版本条件](m1-mcu-firmware)。

## 2. 下载主控文件与脚本

从 [MCU v1.0.0-rc Release](https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20/releases/tag/v1.0.0-rc) 下载以下两个文件，放在同一目录，保留文件名：

- `m1-mcu-ota.bin`
- `m1-mcu-ota.json`

这两个文件必须来自同一个 Release，不要混用不同版本。脚本会核对镜像长度、SHA-256、芯片目标、应用地址及清单。下载区还提供 SHA256SUMS 和独立的调试恢复包，网络更新不需要使用恢复包或原厂整片备份。

电脑需要 Python 3。使用下列固定工具版本 **`01af1ebccdaf48b73d02eaaf7023f68d5a0d280f`**，它支持原厂主控的整包缓冲转发；**MCU v1.0.0-rc 标签内的工具不包含这项修复**。工具版本与镜像版本分开：仍使用上面 Release 的 `m1-mcu-ota.bin` 和 JSON，不要重新构建或替换镜像。

```sh
git clone https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20.git
cd phicomm_m1_mcu_atsamd20
git checkout --detach 01af1ebccdaf48b73d02eaaf7023f68d5a0d280f
```

也可下载[此工具版本的源码 ZIP](https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20/archive/01af1ebccdaf48b73d02eaaf7023f68d5a0d280f.zip)，解压后进入源码目录。脚本仅使用 Python 标准库，无需安装 pip 依赖。不要只单独下载 `m1_network.py`：它还需要同目录的 `tools/pack_ota.py`。

## 3. 填写自己的设备信息

从路由器或 M1 网页获取当前 IP，并在设备信息中查看设备 ID。设备 ID 用于核对目标身份，不是随意填写的名称。IP 改变后需使用新地址。

下面的 `DEVICE_IP`、`DEVICE_ID`、`/下载目录/` 都是占位符，请替换成自己的值；命令应在主控源码目录执行。

如果设置了 M1 网页密码，先设置密码环境变量；没有设置则跳过。请选择自己的系统查看命令：

<!-- os:windows -->

```powershell
$env:M1_WEB_PASSWORD='你的网页密码'
```

<!-- /os -->

<!-- os:mac linux -->

```sh
export M1_WEB_PASSWORD='你的网页密码'
```

<!-- /os -->

Windows 命令使用单行写法，示例下载目录为 `C:/Downloads/`，请换成自己保存镜像和 JSON 的目录；路径含空格时保留两侧引号。

## 4. 先检查，不刷写

<!-- os:windows -->

```powershell
py -3 tools/m1_network.py --host DEVICE_IP --device-id DEVICE_ID --image "C:/Downloads/m1-mcu-ota.bin" --manifest "C:/Downloads/m1-mcu-ota.json"
```

<!-- /os -->

<!-- os:mac linux -->

```sh
python3 tools/m1_network.py \
  --host DEVICE_IP \
  --device-id DEVICE_ID \
  --image /下载目录/m1-mcu-ota.bin \
  --manifest /下载目录/m1-mcu-ota.json
```

<!-- /os -->

macOS / Linux 的多行命令请整体复制，行末 `\` 表示命令还没结束；Windows 命令已经合为一行。

默认不刷写。检查输出中的 `device_id`、`before_mcu`、`candidate`、`sha256`、`bytes` 和 `mcu_ota_max_bytes`，确认目标设备和待写入的主控版本正确。此步骤核对本地镜像并读取设备身份与主控版本，**不会实际发送升级帧，也不能代替前面的 Wi-Fi 最低版本检查**。

## 5. 加上 --flash 正式更新

确认检查结果正确后，使用相同参数加上 `--flash`：

<!-- os:windows -->

```powershell
py -3 tools/m1_network.py --host DEVICE_IP --device-id DEVICE_ID --image "C:/Downloads/m1-mcu-ota.bin" --manifest "C:/Downloads/m1-mcu-ota.json" --flash
```

<!-- /os -->

<!-- os:mac linux -->

```sh
python3 tools/m1_network.py \
  --host DEVICE_IP \
  --device-id DEVICE_ID \
  --image /下载目录/m1-mcu-ota.bin \
  --manifest /下载目录/m1-mcu-ota.json \
  --flash
```

<!-- /os -->

升级期间保持供电，不要同时升级 Wi-Fi、重启设备或修改配置。缓冲方式先发送整包，再由 Wi-Fi 本机逐块等待应答；旧桥接方式在电脑上显示块进度；主控重启会让 Wi-Fi 暂时断开，工具随后等待网络恢复，并核对同一设备新的主控版本回报，最多等待约 120 秒。

本次 **MCU v1.0.0-rc** 发布包应回报 **`1.0.0-rc-97a03b`**。只有看到 `Verified MCU runtime` 且版本匹配，才表示工具已确认升级后运行版本。`Commit transmitted` 仅表示已发送提交，不能单独作为成功依据。

如果提示版本未确认或超时，先等设备重新上线，核对供电、IP、设备身份和实际主控版本；不要盲目重复刷写。`HTTP 401` 通常需要检查网页密码；升级接口 `404` 时检查 Wi-Fi 固件是否已达到支持版本；设备忙时等待其他升级或配置操作结束。

## 更新范围

该过程**只更新链接于 0x4000 的主控应用，保留原厂 Bootloader**，不会刷入独立新 Boot。原厂 Boot 不具备新 Boot 增加的 CRC 日志与中断搬运恢复保证。原厂整片备份与 Boot 镜像不能作为本流程的应用 OTA 文件。

使用对应 Release 的镜像与清单，升级期间保持供电。功能与原厂备份见 [M1 主控开源固件](m1-mcu-firmware)，实现细节见[主控网络升级协议](https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20/blob/main/docs/network-ota.md)。

## 本次验证

2026-10-26，一台原厂主控 11.1.2.1 的 M1 使用整包接口升级到 `1.0.0-rc-97a03b`。镜像 26052 字节，提交响应约 6.6 秒；重启后版本、按键和双区域显示能力均匹配，现场确认屏幕、指示灯和单击开关屏正常。保留原厂 Boot，未据此验证其他硬件修订、17.x 或升级中断电恢复。
