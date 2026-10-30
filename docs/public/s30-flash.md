# S30-V1B 免拆与串口刷机

适用于 **ORVIBO S30-V1B**：BL2028N 模块、BK7231N 芯片、HLW8032 计量。已在原厂 **`2M.1220`** 上实测通过免拆网络 OTA，也保留拆机串口备份、安装与恢复方式。默认固件采用原厂免拆布局，保留本机启动器和工厂 MAC/RF；**首次安装社区固件需重新配网，不保留原厂应用配置**。安装后的操作见 [S30-V1B 功能与操作](s30-guide)。其他 S30 系列或不同模块的设备不能仅凭外壳相同套用本教程。

## RC4 文件下载

当前候选版本为 **v1.0.0-rc.4（Beta）**。只下载与所选方法对应的文件：

| 用途 | 正式文件 |
| --- | --- |
| 原厂免拆安装、社区网页 URL OTA | [BL2028N_OTA_1.0.0-rc.4.rbl](https://github.com/phicomm-io/iotcloud-community-firmware/releases/download/v1.0.0-rc.4/BL2028N_OTA_1.0.0-rc.4.rbl) |
| 保留原厂启动器的串口安装，从 `0x11000` 写入 | [BL2028N_STOCK_UA_1.0.0-rc.4.bin](https://github.com/phicomm-io/iotcloud-community-firmware/releases/download/v1.0.0-rc.4/BL2028N_STOCK_UA_1.0.0-rc.4.bin) |
| 校验值 | [SHA256SUMS-S30](https://github.com/phicomm-io/iotcloud-community-firmware/releases/download/v1.0.0-rc.4/SHA256SUMS-S30) |

例如仅下载 RBL 后，在同一目录运行 `grep "  BL2028N_OTA_1.0.0-rc.4.rbl$" SHA256SUMS-S30 | sha256sum -c -`；macOS 可用 `shasum -a 256`，与校验文件对应行核对。不要使用其它版本或其它设备的包。

下载文件名按发布版本固定；设备显示的版本还包含六位 Git 提交后缀。

## 先选择刷机方式

| 当前状态 | 方法 | 文件 |
| --- | --- | --- |
| 原厂 `2M.1220`，已经联网，电脑可访问同一局域网 | 下文的免拆网络安装 | `BL2028N_OTA_<版本>.rbl` |
| 无法联网、应用损坏或需要完整备份 | 拆机串口方式 | `BL2028N_STOCK_UA_<版本>.bin`，从 `0x11000` 写入 |

目前网页**只支持 URL 下载升级，不支持选择本地文件上传**。不能把固件下载到电脑后直接拖入页面。默认不生成含其它启动器的 QIO；未加密的 `BL2028N_app_*.bin` 和 UG 包也不能替代上述文件。

## 原厂免拆网络安装

### 适用范围与准备

此方法适用于原厂 **`2M.1220`**。设备须已连接 Wi-Fi，电脑能访问设备，并已取得本机工厂 MAC。其他原厂版本请使用下文串口方式。

电脑需要 Python、`paho-mqtt`、本地 HTTPS 证书，以及一个**专用的临时 TLS MQTT broker**。普通明文 MQTT 在本次原厂版本中不能完成连接，不能用 `mqtt://` 代替 `ssl://`。不需要改路由器 DNS。不要复用日常 broker 的设备账号，也不要把测试 broker 暴露到公网。

本流程会把原厂设备的 CGW/云连接指向测试服务。首次社区启动使用默认配置；原厂账号、Wi-Fi、MQTT、配对及计量配置不迁移。请先记录需要重新设置的内容。

### 本地 TLS MQTT 服务示例

可在 Linux/macOS 上使用 Python、OpenSSL 与 Mosquitto。先安装这些软件，再在独立的临时目录中准备服务。下面的 `192.168.1.10` 应替换为电脑真实局域网地址；不要填设备地址。

```sh
python3 -m venv .venv
. .venv/bin/activate
python3 -m pip install paho-mqtt==2.1.0
OTA_HOST=192.168.1.10
openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
  -keyout gateway-key.pem -out gateway-cert.pem \
  -subj "/CN=$OTA_HOST" -addext "subjectAltName=IP:$OTA_HOST"
mosquitto_passwd -c broker.passwd s30test
chmod 600 gateway-key.pem broker.passwd
```

`mosquitto_passwd` 会提示输入专用测试密码。创建 `broker.conf`，将下面三个文件路径替换为本目录的绝对路径：

```text
listener 18892
allow_anonymous false
password_file /absolute/path/broker.passwd
certfile /absolute/path/gateway-cert.pem
keyfile /absolute/path/gateway-key.pem
persistence false
```

在单独终端以当前用户运行 `mosquitto -c broker.conf -v`。对应的 `mqtt.private.json` 使用该电脑 IP、端口 `18892`、账号 `s30test` 和刚设置的密码。安装工具的 `--mqtt-ca` 和 `--cert` 都可指向 `gateway-cert.pem`，`--key` 指向 `gateway-key.pem`。设备需要在原 Wi-Fi 上保持联网；电脑防火墙须允许来自该设备的上述端口。

### 运行本地安装工具

使用与候选固件一起审查的 [s30_stock_ota.py 工具](https://github.com/phicomm-io/iotcloud-community-firmware/blob/v1.0.0-rc.4/tools/s30_stock_ota.py)。本机、目标设备与临时 broker 的 IPv4 地址必须相互可达；放行本机 TCP `15683`（HTTPS CGW）、`8080`（固件 HTTP）及所选 broker 端口。证书须包含服务 IP，临时 broker 使用其对应证书，电脑用 `--mqtt-ca` 校验证书。

准备两个权限为 `600` 的私有 JSON 文件，不要提交到 Git 或公开：

```json
{"ssid":"你的 2.4 GHz Wi-Fi","password":"Wi-Fi 密码"}
```

```json
{"host":"临时 broker 的 IPv4","port":18892,"username":"短测试账号","password":"短测试密码"}
```

MQTT 用户名、密码各限 1–31 字节，使用短地址和专用测试账号。

取得对应 `.rbl` 与 `SHA256SUMS-S30`，先校验，随后运行；替换全部占位符：

```sh
mkdir -p tools
curl --fail --location https://raw.githubusercontent.com/phicomm-io/iotcloud-community-firmware/v1.0.0-rc.4/tools/s30_stock_ota.py -o tools/s30_stock_ota.py
chmod 600 wifi.private.json mqtt.private.json
grep "  BL2028N_OTA_1.0.0-rc.4.rbl$" SHA256SUMS-S30 | sha256sum -c -
python3 tools/s30_stock_ota.py \
  --device <原厂设备IP> --device-id <本机12位工厂MAC> \
  --host <电脑局域网IP> \
  --wifi-file wifi.private.json --mqtt-file mqtt.private.json \
  --mqtt-ca gateway-cert.pem --cert gateway-cert.pem --key gateway-key.pem \
  --rbl 'BL2028N_OTA_1.0.0-rc.4.rbl' --sha256 <该文件公布的SHA256>
```

工具通过 CoAP `/qlink/netinfo` 配置本地 HTTPS CGW，原厂设备经 HTTPS 获取临时 TLS MQTT 连接，再接收 AndLink 文件命令并下载 RBL。

### 确认安装成功

1. 工具提示 RBL 完整 HTTP 发送后，等待设备自动安装并重启，期间保持供电。
2. 查找 `S30M_V1B_XXXXXX` 社区热点，连接后打开 **http://192.168.0.1**，重新设置 Wi-Fi。
3. 设备联网后打开其新 IP，确认首页为 `iotcloud · S30-V1B`、版本与所选包一致，检查开关和计量。未设管理密码时直接进入，不应弹出登录框。
4. 确认后停止专用测试 broker，删除临时证书和账号文件。无需保留电脑服务；后续升级使用社区网页的 URL OTA。

若一直没有下载或社区热点，保留工具错误记录，按下文串口备份／恢复路径排查。

## 拆机串口方式

### 供电与准备

将插座从市电拔下，断开负载和原电源，确认断电后再拆机。**拆机、连接电脑串口和刷写期间均不能接入市电。** 使用 3.3V 逻辑电平的 USB-TTL 串口适配器，以及稳定、独立、隔离的 3.3V 直流电源；不要向模块接入 5V。

USB 串口适配器的 3.3V 输出可能无法承担 Wi-Fi 启动电流，表现为刷入成功但启动或联网时反复复位。使用独立电源时，断开适配器的 3.3V 供电线，保留 TX、RX、GND，电源负极与串口 GND、设备 GND 共地。

刷写前准备本设备对应版本的固件与 `SHA256SUMS`，先校验文件。串口软件如 minicom 必须退出，避免与刷机工具同时占用端口。下面使用 [bk7231tools](https://github.com/tuya-cloudcutter/bk7231tools#usage)，示例端口 `/dev/ttyUSB0` 应替换为实际端口。

## 接点位置与接线

**以插头面 PCB 朝向自己为准**：PCB **右上方外侧**有两个电源板与 MCU 模块的连接触点，**左侧为 GND，右侧为 3V3**。串口使用板上的 **VTX / VRX** 测试点；部分丝印带数字后缀，如 VTX1 / VRX1。

| 设备接点 | 连接到 |
| --- | --- |
| VTX 测试点 | USB-TTL 的 RXD |
| VRX 测试点 | USB-TTL 的 TXD |
| 插头面 PCB 右上方外侧：左触点 GND | 电源负极和 USB-TTL GND |
| 同一对触点：右触点 3V3 | 独立隔离电源的 3.3V 正极 |

左右关系仅指上述观察方向。接线前核对 PCB 朝向与接点，不要将翻面后的左右位置直接套用。VTX/VRX 是设备收发方向，因此 TX 与 RX 交叉连接。

### 先做完整备份

在工具开始等待连接后，给模块的 **3.3V 电源断电再上电**，或短暂将已确认的 CEN/RESET 接地后释放。保持 USB 串口适配器连接。按住插座日常开关按钮不能代替芯片复位。

本次验证设备的 Flash 为 2 MiB，完整读取范围是 `0x000000` 至 `0x1fffff`。首次备份读取两次，分别在工具等待时复位，且保留工具默认的校验：

```sh
bk7231tools read_flash -d /dev/ttyUSB0 -b 115200 --timeout 60 -s 0 -l 0x200000 s30-stock-1.bin
bk7231tools read_flash -d /dev/ttyUSB0 -b 115200 --timeout 60 -s 0 -l 0x200000 s30-stock-2.bin
sha256sum s30-stock-1.bin s30-stock-2.bin
cmp s30-stock-1.bin s30-stock-2.bin
```

两个文件都应为 2097152 字节，SHA-256 相同，`cmp` 正常退出且没有输出。保存备份及校验值，再进行任何写入。备份含本机 MAC、射频参数和可能的原厂凭据，应私下保管；其他设备的备份不能替代本机备份。

### 选择镜像并写入

本次验证的 BL2028N 使用**零密钥 bootloader**，串口保留 bootloader 刷写应选：

```text
BL2028N_STOCK_UA_<版本>.bin
写入起点：0x11000
```

`STOCK_UA` 是原厂布局的零密钥、带 CRC 的应用镜像，写入范围止于 `0x132000`，包含原厂安装约定的应用尾块，**不含 bootloader 或本机工厂数据**。设备芯片仍为 BK7231N。旧版 `BK7231M_UA` 的尾块/分区位置不同，不能因同为零密钥而混用。若此前已换成其它启动器，先用本机备份恢复并核对启动区；仅写应用不能替换启动器。

校验固件后执行，替换下方版本占位符；等待连接时再复位模块：

```sh
bk7231tools write_flash -d /dev/ttyUSB0 -b 115200 --timeout 60 -s 0x11000 -S 0 'BL2028N_STOCK_UA_<版本>.bin'
```

使用 UA 应用镜像并从 `0x11000` 写入，保留本机 bootloader、原厂 RF/TLV 与尾部配置。**不要全片擦除，也不要将 `.rbl`、`.UG.bin` 或其他产品的 `.ota.bin` 当作该串口镜像。** QIO 包含 bootloader，不是本节默认刷法。

工具校验通过后，建议再次读取完整 Flash，核对写入范围与 UA 文件一致、范围外尤其 bootloader 和原厂参数保持不变。当前样机 RF/TLV 扇区位于 `0x1e0000`，其中的有效工厂 MAC 必须保留。

### 首次启动与恢复

1. 刷写完成后，重新给模块的隔离 3.3V 电源上电，先确认出现 `S30M_V1B_` 加 MAC 后六位大写十六进制的热点。
2. 连接热点，打开 **http://192.168.0.1**，按[使用说明](s30-guide)配网并确认设备型号和版本。初始热点密码、网页管理密码均为空。
3. 完成断电，拆除全部串口和外接供电连线，检查接线处并恢复完整绝缘装配后，才可恢复原供电和连接负载。

负载和计量测试须在拆除下载器、恢复完整绝缘装配及原供电后进行。

无法握手时先排查端口占用、交叉 TX/RX、共地、接触可靠性、供电及复位时机。写入后无法启动时，保留备份与完整刷机日志，检查镜像、起点和 bootloader 密钥格式。若此前发生 OTA 中断，应按对应版本的恢复流程处理遗留升级数据；不要用反复全片擦除或其他设备的 RF/MAC 数据尝试恢复。

## 社区固件后续 URL OTA

社区固件的后续更新归入 [常见功能 → 升级](s30-guide#升级)。首次安装请选上文的原厂免拆或串口方法。
