# M1 主控 MCU 网络升级

M1 主控固件可以通过网络更新，**不需要 J-Link，也不需要拆机**。当前使用电脑上的 Python 脚本升级，电脑需能通过局域网访问 M1。

M1 的 Wi-Fi 模块与 ATSAMD20G18A 主控分别升级。**主控镜像不要填进普通 Wi-Fi 固件更新框。** 本文只更新主控应用，保留原厂 Bootloader。

## 1. 确认 Wi-Fi 固件版本

M1 必须先安装支持主控网络升级的配套社区 Wi-Fi 固件：**公开发布版本最低为 v0.9.26**，该版本加入 `POST /api/v1/mcu/exchange` 主控升级桥接接口。原厂 Wi-Fi 固件及 v0.9.24 或更早的社区固件不能直接使用本文方法；v0.9.25 只有源码 tag，没有公开 Release，因此不作为下载起点。

在 M1 网页查看当前 Wi-Fi 固件版本。低于 v0.9.26 时，先通过正常的 Wi-Fi 固件更新入口安装 [M1 Wi-Fi v0.9.26 或更新版本]({{SITE_URL}}#releases)，确认设备恢复上线，再升级主控。两者版本号相互独立：这里的 v0.9.26 是 **Wi-Fi 版本**，下文 v0.9.4 是 **主控版本**。

如需在网页中使用主控的 RTTTL 音乐、四种提示音和闹钟曲目，配套 Wi-Fi 固件需要 **v0.9.27 或更新版本**，并通过主控能力检测；这与能否使用脚本刷写主控是两个条件。

## 2. 下载主控文件与脚本

从 [MCU v0.9.4 Release](https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20/releases/tag/v0.9.4) 下载以下两个文件，放在同一目录，保留文件名：

- `m1-mcu-ota.bin`
- `m1-mcu-ota.json`

这两个文件必须来自同一个 Release，不要混用不同版本。脚本会核对镜像长度、SHA-256、芯片目标、应用地址及清单。下载区还提供 SHA256SUMS 和独立的调试恢复包，网络更新不需要使用恢复包或原厂整片备份。

电脑需要 Python 3。首次使用可通过 Git 获取 v0.9.4 配套工具；脚本仅使用 Python 标准库，无需安装 pip 依赖：

```sh
git clone --depth 1 --branch v0.9.4 https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20.git
cd phicomm_m1_mcu_atsamd20
```

也可在该 Release 页面下载 Source code ZIP，解压后打开终端并进入源码目录。不要只单独下载 `m1_network.py`：它还需要同目录的 `tools/pack_ota.py`。

## 3. 填写自己的设备信息

从路由器或 M1 网页获取当前 IP，并在设备信息中查看设备 ID。设备 ID 用于核对目标身份，不是随意填写的名称。IP 改变后需使用新地址。

下面的 `DEVICE_IP`、`DEVICE_ID`、`/下载目录/` 都是占位符，请替换成自己的值；命令应在主控源码目录执行。

如果设置了 M1 网页密码，先设置密码环境变量；没有设置则跳过。以下为 Linux / macOS 终端写法：

```sh
export M1_WEB_PASSWORD='你的网页密码'
```

Windows PowerShell 对应写法为 `$env:M1_WEB_PASSWORD='你的网页密码'`，并可将下面的 `python3` 换成 `python`，将文件路径换成 Windows 的实际路径。

## 4. 先检查，不刷写

```sh
python3 tools/m1_network.py \
  --host DEVICE_IP \
  --device-id DEVICE_ID \
  --image /下载目录/m1-mcu-ota.bin \
  --manifest /下载目录/m1-mcu-ota.json
```

以上多行命令的 `\` 续行适用于 Linux / macOS；Windows PowerShell 可将各行合为一行执行。

默认不刷写。检查输出中的 `device_id`、`before_mcu`、`candidate`、`sha256` 和 `bytes`，确认目标设备和候选主控版本正确。此步骤核对本地镜像并读取设备身份与主控版本，**不会实际发送升级帧，也不能代替前面的 Wi-Fi 最低版本检查**。

## 5. 加上 --flash 正式更新

确认检查结果正确后，使用相同参数加上 `--flash`：

```sh
python3 tools/m1_network.py \
  --host DEVICE_IP \
  --device-id DEVICE_ID \
  --image /下载目录/m1-mcu-ota.bin \
  --manifest /下载目录/m1-mcu-ota.json \
  --flash
```

升级期间保持供电，不要同时升级 Wi-Fi、重启设备或修改配置。脚本逐块等待应答并显示进度；主控重启会让 Wi-Fi 暂时断开，工具随后等待网络恢复，并核对同一设备新的主控版本回报，最多等待约 120 秒。

本次 **MCU v0.9.4** 发布包应回报 **`0.9.4-1ce539`**。只有看到 `Verified MCU runtime` 且版本匹配，才表示工具已确认升级后运行版本。`Commit transmitted` 仅表示已发送提交，不能单独作为成功依据。

如果提示版本未确认或超时，先等设备重新上线，核对供电、IP、设备身份和实际主控版本；不要盲目重复刷写。`HTTP 401` 通常需要检查网页密码；升级接口 `404` 时检查 Wi-Fi 固件是否已达到支持版本；设备忙时等待其他升级或配置操作结束。

## 更新范围

该过程**只更新链接于 0x4000 的主控应用，保留原厂 Bootloader**，不会刷入独立新 Boot。原厂 Boot 不具备新 Boot 增加的 CRC 日志与中断搬运恢复保证。原厂整片备份与 Boot 镜像不能作为本流程的应用 OTA 文件。

主控仍为开发预览，已有单台 M1 候选网络升级验证，其他硬件修订和真实掉电恢复需分别验证。功能与原厂备份见 [M1 主控开源固件](m1-mcu-firmware)，实现细节见[主控网络升级协议](https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20/blob/v0.9.4/docs/network-ota.md)。
