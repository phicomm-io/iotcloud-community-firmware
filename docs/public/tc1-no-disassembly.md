# TC1 免拆刷机

**适用于 TC1 A1 硬件（EMW3031 / MK3031）、原厂版本 1.0.0.26。** 其他原厂版本、TC1 A2 与 A1 空气净化器不适用。

不拆壳、不接串口或 SWD：通过 Wi-Fi 让设备从本地 TFTP 服务器下载社区镜像并完成切换，全过程不需要原厂云服务。先阅读[刷机前准备](flash-prep)与[固件更新](firmware-update)。

刷写会擦除非活动应用槽并重启设备；原厂保留槽**不构成自动回退保证**。刷写期间保持供电，排插上不要连接负载。

## 需要准备什么

| 角色 | 需要做到什么 |
| --- | --- |
| 配网客户端 | 连接 TC1 的开放配网热点，获得 `10.10.10.x/24`，能访问 `10.10.10.1:8000` |
| OTA 热点与文件服务器 | 提供 2.4 GHz 开放热点 `PHICOMM_OTA_AP`；设备能通过该热点访问 `10.0.0.2/8` 上的 TFTP，下载 `mico_ota.bin` |

操作系统和设备型号不限。可以用一台电脑或手机接 TC1 热点，另一台电脑、便携路由器或无线 AP 承载 OTA 网络；TFTP 既可以跑在 AP 本机，也可以跑在 AP 桥接的有线网络中的电脑上。

### 推荐方案：一台有线＋无线网卡的电脑和一台路由器

准备一台同时有有线网卡、无线网卡的电脑，以及一台无线路由器。电脑的有线网卡提供固件，无线网卡连接 TC1 配网热点并发送触发请求；路由器负责提供 OTA 热点。**能实现同等连接关系的拓扑也可以使用。**

```text
电脑有线网卡（10.0.0.2/8，TFTP 服务）──网线──路由器 LAN 口
电脑无线网卡 ──Wi-Fi── TC1 配网热点 Phicomm_TC1_XXXX
TC1 进入工厂 OTA 模式后 ──Wi-Fi── 路由器 PHICOMM_OTA_AP
```

按以下顺序操作，先准备好接收设备的 OTA 网络，再触发刷机：

1. 按下文「生成升级文件」准备好 `mico_ota.bin`。将路由器的 **2.4 GHz Wi-Fi** 名称设为 **`PHICOMM_OTA_AP`**，**无密码**，关闭客户端隔离；该 Wi-Fi 必须与 LAN 口桥接互通。
2. 电脑有线网卡用网线连接路由器的 **LAN 口**，手动设为 **`10.0.0.2` / `255.0.0.0`（/8）**，无需填写网关和 DNS。确认路由器及其他设备没有占用 `10.0.0.2`。
3. 在电脑上启动 **tftpd 等 TFTP 服务**，服务目录放入 `mico_ota.bin`，监听有线网卡的 `10.0.0.2`；按下文「建立 OTA 热点与 TFTP」放行防火墙并检查下载。保持服务和日志窗口开启。
4. 按下文「进入配网模式」复位 **TC1 A1 硬件版排插**，看到 `Phicomm_TC1_XXXX` 后，用电脑的**无线网卡**连接它，确认获得 `10.10.10.x/24`，并只读核对原厂版本。有线连接与 TFTP 服务保持运行。
5. 按下文「触发刷机」运行 Python 脚本生成栈溢出 payload，再用紧随其后的 `curl` 命令发送给 TC1。电脑通过无线网卡访问 `10.10.10.1`，通过有线网卡提供 `10.0.0.2` 上的文件，两块网卡无需共享网络或互相桥接。
6. 设备进入工厂 OTA 模式后，会离开配网热点，自动连接路由器的 `PHICOMM_OTA_AP`。此时电脑无线连接断开是预期现象；观察电脑上的 **TFTP 日志**，应看到设备请求并下载 `mico_ota.bin`。等待传输、校验和重启完成，再按下文确认新固件热点与本地网页。

这里的传输方向是**电脑向 TC1 发送文件、TC1 从电脑下载固件**；TFTP 工具通常显示读取请求（RRQ）或发送文件记录。无需让电脑无线网卡切换到路由器热点。

### 其他设备组合

也可以用两台独立设备分别承担配网客户端和 OTA 网络。若用单块无线网卡同时连接 TC1 热点并提供 OTA 热点，该网卡及驱动必须支持 Wi-Fi 客户端与热点并行运行；先建立 OTA 热点，再连接 TC1 配网热点。

## 生成升级文件

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

## 进入配网模式

两种方法任选其一。成功标志相同：LED 慢闪，广播开放热点 `Phicomm_TC1_XXXX`（`XXXX` 为 MAC 后四位），设备地址 `10.10.10.1/24`，并在 `:8000` 监听配置服务。

- **方法一：** 断电重新上电，在上电 **3 秒内双击配网键**。LED 转为约 500 ms 慢闪，随后出现开放热点。
- **方法二：** 长按配网键，等 LED 转为快闪后保持一段时间再松开，随后快速短按配网键两次；出现开放热点即进入配网模式。不同原厂版本对快闪的定义可能不同，以热点出现为准。

看不到热点时，先断电重试方法一；仍不行时重新核对设备硬件版本与原厂版本，不要反复快速操作。

配网客户端连接该热点后，先只读确认版本：

```sh
curl -s -m 5 http://10.10.10.1:8000/read-domain
```

`VERSION` 应为 `1.0.0.26`，`COMPILE_TIME` 为 `Apr 13 2018 10:09:06`。版本不符时不要继续。

## 建立 OTA 热点与 TFTP

1. 创建 **2.4 GHz** 热点，名称精确为 **`PHICOMM_OTA_AP`**，开放、无密码，不启用客户端隔离。
2. TFTP 服务器的 OTA 侧地址设为 **`10.0.0.2`**、掩码 **`255.0.0.0`（/8）**。服务器在 AP 外部时，把热点桥接到服务器所在二层网络。
3. 在服务器上提供名为 **`mico_ota.bin`** 的文件，允许 TFTP 二进制（octet）读取，512 字节块，不使用选项协商。
4. 放行设备到服务器的 TFTP 请求和传输会话：首包目的端口为 UDP 69，后续数据传输使用服务器另开的端口，只放行 69 可能传不完文件。
5. 先用另一台客户端完整下载一次并核对大小与指纹；主机回环下载只能检查文件服务，不能证明无线侧与防火墙已通。

没有现成 TFTP 服务时，可用下面的只读 Python 服务（只提供该文件，拒绝写入）：

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

## 触发刷机

确认 OTA 热点和 TFTP 服务就绪后，从仍连接 TC1 配网热点的客户端生成社区提供的触发文件并发送一次：

```sh
python3 - <<'PY'
body = b'A' * 392 + (0x1F073D9F).to_bytes(4, 'little') + b'\0'
open('tftp-trigger.bin', 'wb').write(body)
print(len(body))
PY
curl --max-time 30 --header 'Content-Type: application/json' \
  --data-binary @tftp-trigger.bin \
  http://10.10.10.1:8000/OTA
```

返回 `OTA AAAA...` 只表示请求已被接收，**不代表刷写成功**。连接中断或返回未确认时，先观察设备状态，不要立即重复触发。

## 观察下载与首启

设备离开配网状态后会加入 `PHICOMM_OTA_AP`，使用地址 `10.<MAC 后三字节逐字节转十进制>`、掩码 `/8`；每台设备不同，不要照抄示例。

典型顺序：加入 OTA 热点 → 擦除非活动应用槽 → 从 `10.0.0.2` 下载 `mico_ota.bin` → 校验 MD5 → 切换应用槽并重启。TFTP 服务应能看到完整下载记录。

成功首启后会出现开放热点 **`TC1_A1-AP-<MAC 后六位大写>`**，设备地址 **`192.168.0.1`**。连接该热点后打开设备本地网页继续配网。

## 验收与恢复

分别记录四项：原厂识别信息、TFTP 是否完整下载、新固件热点、网页是否可访问。不要只以 HTTP 200、设备掉线或一次 TFTP 传输宣告完成。

确认新固件可用后，关闭临时 OTA 热点与 TFTP 服务，恢复为此次操作调整的网卡地址和防火墙配置。使用 OpenWrt / ImmortalWrt 设备时，按上面的目标自行配置临时接口与 dnsmasq TFTP，结束后删除这些配置并恢复原网络。

## 常见问题

| 现象 | 常见原因 |
| --- | --- |
| 看不到 `Phicomm_TC1_XXXX` | 按键节奏不对，或设备不是 A1 硬件 / 不是 1.0.0.26 |
| `/OTA` 返回 404 或无响应 | 未处于配网模式，或客户端不在 `10.10.10.x/24` |
| 设备没有加入 OTA 热点 | 热点不是 2.4 GHz、名称不是 `PHICOMM_OTA_AP`，或开启了客户端隔离 |
| TFTP 只传了开头 | 防火墙只放行 UDP 69，未放行传输会话端口 |
| TFTP 完成但没有切槽 | 镜像 MD5 不匹配，或镜像超出 692224 B 应用槽 |

本页仅覆盖上文所列设备与版本。其他原厂版本不适用。
