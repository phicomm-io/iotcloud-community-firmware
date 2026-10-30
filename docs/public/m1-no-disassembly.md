# M1 原厂配网与免拆安装

本文只更新 M1 联网模块，保留传感器主控，包含两种原厂配网方式和两种 OTA 方式。有条件建议使用官方 App 配网。

首次迁移使用 **M1 完整版原生 OTA 镜像，版本至少 0.9.21**。以下固定使用稳定版 **v0.9.27**，操作期间保持供电。

<!-- os-picker -->

## 1. 给原厂 M1 配网

路由器已列出 M1 时，保留现有配置，记下 IP 和机身 MAC，直接进入下一步。否则选择以下一种方式。目标网络使用 2.4 GHz WPA2/AES，开启 DHCP，关闭客户端隔离。

### 方式一：热点 HTTP 配网

**按住机身按键 3–4 秒后松开**，在约两分钟内连接开放热点 `EasyLink_XXXXXX`，后缀为设备 MAC 最后六个十六进制字符。

短按切换屏幕；**按住约 9 秒后松开会恢复原厂网络和应用设置并重启**。重新配网使用 3–4 秒操作。

### 连接热点并确认接口

电脑连接目标 `EasyLink_XXXXXX`，出现“无 Internet”提示不影响本地通信。固件设置的设备地址为 `10.0.0.1`，HTTP 端口为 8000。

<!-- os:windows -->

```powershell
$DeviceIp = "10.0.0.1"
curl.exe --http1.0 --noproxy "*" --connect-timeout 3 --max-time 5 "http://${DeviceIp}:8000/config-read"
```

<!-- /os -->

<!-- os:mac linux -->

```sh
DeviceIp="10.0.0.1"
curl --http1.0 --noproxy '*' --connect-timeout 3 --max-time 5 "http://${DeviceIp}:8000/config-read"
```

<!-- /os -->

若不通，检查电脑是否仍连接设备热点，以及 WLAN 是否取得 `10.0.0.x` 地址。热点窗口可能已过期，需要重新进入配网状态。这里不需要 TC1 教程里的有线附加 IP 或 TFTP 服务。

### 提交路由器信息

替换网络名称和密码，在同一个终端窗口运行：

<!-- os:windows -->

```powershell
$Config = [ordered]@{
    SSID = "RescueWiFi"
    PASSWORD = "replace-with-your-wifi-password"
    DHCP = $true
    IDENTIFIER = 1
}
$ConfigFile = Join-Path $PWD.Path "m1-wifi.json"
$Json = $Config | ConvertTo-Json -Compress
[System.IO.File]::WriteAllText($ConfigFile, $Json, [System.Text.UTF8Encoding]::new($false))

curl.exe --http1.0 --noproxy "*" --connect-timeout 3 --max-time 10 --header "Content-Type: application/json" --header "Expect:" --data-binary "@$ConfigFile" "http://${DeviceIp}:8000/config-write-uap"
```

<!-- /os -->

<!-- os:mac linux -->

在当前目录创建 UTF-8 JSON 文件。把示例 SSID、密码替换成实际值；字符串中的 `"` 和 `\` 按 JSON 规则转义。密码会保留在文件及终端输入记录中，使用自己的电脑操作。

```sh
umask 077
cat > m1-wifi.json <<'JSON'
{"SSID":"RescueWiFi","PASSWORD":"replace-with-your-wifi-password","DHCP":true,"IDENTIFIER":1}
JSON
curl --http1.0 --noproxy '*' --connect-timeout 3 --max-time 10 --header 'Content-Type: application/json' --header 'Expect:' --data-binary '@m1-wifi.json' "http://${DeviceIp}:8000/config-write-uap"
```

<!-- /os -->

字段放在 JSON 顶层，大小写与示例一致。`DHCP` 为布尔值；`IDENTIFIER` 为配网回调使用的整数，不是原厂账号或绑定令牌。

原厂逻辑在响应后等待约 1 秒，关闭热点并尝试连接路由器。因此连接断开是可能的正常过程。电脑随后连回目标路由器，再查看 M1 的 DHCP 租约。

正文文件包含明文密码，操作完成后删除：

<!-- os:windows -->

```powershell
Remove-Item -LiteralPath $ConfigFile
```

<!-- /os -->

<!-- os:mac linux -->

```sh
rm -- m1-wifi.json
```

<!-- /os -->


### 方式二：Linux EasyLink V2

免 App 配网使用 **Linux mac80211 帧注入**，需要支持此功能的无线网卡；也可使用官方 App。

准备支持 monitor 模式和帧注入的备用无线网卡、Linux、`iw` 与 C 编译器。目标网络使用 2.4 GHz WPA2/AES，记下实际信道；实测信道 1 可用，不要求信道 6。电脑可通过另一张网卡连接路由器。

下载 [Linux 注入发送器源码](assets/tools/easylink-linux/easylink_v2_linux.c) 与 [SHA256SUMS](assets/tools/easylink-linux/SHA256SUMS)。以下在 Linux 终端执行，`phy1`、信道 `1` 和 `192.168.1.23` 分别替换为备用网卡、目标信道和电脑在目标局域网的 IPv4：

```sh
sha256sum -c SHA256SUMS
cc -O2 -Wall -Wextra -o easylink_v2_linux easylink_v2_linux.c
iw dev
sudo iw phy phy1 interface add easylink-mon type monitor
sudo ip link set easylink-mon up
sudo iw dev easylink-mon set channel 1
sudo ./easylink_v2_linux easylink-mon 192.168.1.23 60
```

按提示输入目标 Wi-Fi 名称、密码（密码不回显），发送开始后，按住 M1 机身按键 3–4 秒后松开。发送结束后删除临时 monitor 接口：

```sh
sudo iw dev easylink-mon del
```

在路由器中按机身 MAC 确认设备获得 IP，再进行下一步。

## 2. 核对内网设备身份

替换示例 IP，在后续步骤保持同一个终端窗口：

<!-- os:windows -->

```powershell
$DeviceIp = "192.168.1.123"
curl.exe --noproxy "*" --connect-timeout 3 --max-time 5 "http://${DeviceIp}:8000/config-read"
```

<!-- /os -->

<!-- os:mac linux -->

```sh
DeviceIp="192.168.1.123"
curl --noproxy '*' --connect-timeout 3 --max-time 5 "http://${DeviceIp}:8000/config-read"
```

<!-- /os -->

将配置响应中的设备名称、MAC 后缀与机身标签、路由器记录核对。不通时先检查 IP、配网结果和客户端隔离，确认目标后再上传。

## 3. 方式一：局域网上传 OTA

下载以下两份 **0.9.27** 文件，放进同一个空文件夹，然后在该文件夹打开终端：

- [M1 完整版原生 OTA 镜像]({{SITE_URL}}releases/v0.9.27/phicomm_m1.ota.bin.raw)
- [SHA256SUMS 校验清单]({{SITE_URL}}releases/v0.9.27/SHA256SUMS)

**核对型号为 M1、版本至少 0.9.21。** 本文首次迁移使用上面指定的完整版文件；不使用 `no-frontend-resources` 精简版。`.all.bin` 和整片备份用于其他恢复流程，`.ota.bin.signed` 用于已经运行社区固件的设备，都不能作为原厂接口的上传正文。不要将这些文件改名为 `.raw` 使用。

先计算镜像的 SHA-256，并与清单中 **`phicomm_m1.ota.bin.raw` 对应的那一行**逐字符比较；不一致时停止。

<!-- os:windows -->

```powershell
Get-FileHash -Algorithm SHA256 .\phicomm_m1.ota.bin.raw
Select-String -Path .\SHA256SUMS -Pattern '\s+\*?phicomm_m1\.ota\.bin\.raw$'
```

<!-- /os -->

<!-- os:mac -->

```sh
shasum -a 256 ./phicomm_m1.ota.bin.raw
awk '$2 == "phicomm_m1.ota.bin.raw" { print }' ./SHA256SUMS
```

<!-- /os -->

<!-- os:linux -->

```sh
sha256sum ./phicomm_m1.ota.bin.raw
awk '$2 == "phicomm_m1.ota.bin.raw" { print }' ./SHA256SUMS
```

<!-- /os -->

供电和网络保持稳定，执行一次上传。命令固定 HTTP/1.0、关闭代理和 Expect，不自动重试：

<!-- os:windows -->

```powershell
curl.exe --http1.0 --noproxy "*" --connect-timeout 5 --max-time 300 --header "Content-Type: application/ota-stream" --header "Expect:" --data-binary "@phicomm_m1.ota.bin.raw" --write-out "uploaded=%{size_upload} http=%{http_code}\n" "http://${DeviceIp}:8000/OTA"
```

<!-- /os -->

<!-- os:mac linux -->

```sh
curl --http1.0 --noproxy '*' --connect-timeout 5 --max-time 300 --header 'Content-Type: application/ota-stream' --header 'Expect:' --data-binary '@phicomm_m1.ota.bin.raw' --write-out 'uploaded=%{size_upload} http=%{http_code}\n' "http://${DeviceIp}:8000/OTA"
```

<!-- /os -->

**实测完整上传后，原厂端会直接关闭连接，curl 可能显示 `(52) Empty reply from server` 和 `http=000`。**记下上传字节数，保持供电，等待约 2–3 分钟，按第 5 节查社区热点和版本；不要因为这个报错立刻重发、按复位键或断电。

## 4. 方式二：DNS 迁移：让原厂 M1 主动下载

局域网上传一直卡住时，可以使用这条替代路径。原厂联网模块 `14.2.4.1`、主控 `11.1.2.1` 已实机验证此路径可用。


### 需要准备什么

- 已按第 1、2 节联网并核对身份的原厂 M1；在路由器中固定它的 DHCP 地址，迁移期间不要换 IP。
- 一台能 SSH 登录的 OpenWrt / ImmortalWrt 路由器，具有 `dnsmasq`、`nft`，M1 和电脑在同一局域网。
- 一台保持开机的 Linux 电脑，安装 Python 3 和 `openssl`。以下服务器和路由命令在 Linux / 路由器 shell 执行；Windows、macOS 读者可使用一台同网段 Linux 主机运行服务。
- 已核对 SHA-256 的 **M1 完整版原生 OTA**。按第 3 节选择文件，不用 `.all.bin`、`.signed` 或主控 MCU 镜像。源码构建目录的原生文件可叫 `phicomm_m1.ota.bin`，发布下载文件可叫 `.ota.bin.raw`；以格式和校验为准，不靠改名转换。
- 下载配套 [迁移服务脚本](assets/tools/m1-dns-migration/m1_dns_migration.py) 与 [SHA256SUMS](assets/tools/m1-dns-migration/SHA256SUMS)。在脚本目录执行 `sha256sum -c SHA256SUMS`。

下面统一使用示例：路由器 `192.168.1.1`、电脑 `192.168.1.10`、M1 `192.168.1.123`、M1 MAC `00:11:22:33:44:55`。**全部替换为自己的值**。电脑的 `29080`、`29443`、`29000` 和路由器的 `5533` 端口须空闲；防火墙需允许 M1 / 路由器访问电脑这些端口。

### 第一步：在电脑启动临时服务

将脚本、镜像放在同一目录，把 `--sha256` 替换为可信校验清单中的实际值：

```sh
python3 m1_dns_migration.py \
  --image ./phicomm_m1.ota.bin.raw \
  --sha256 '替换为校验清单中该镜像的64位SHA256' \
  --mac 00:11:22:33:44:55 \
  --device-ip 192.168.1.123 \
  --router-ip 192.168.1.1 \
  --server-ip 192.168.1.10
```

出现 `"kind": "ready"` 后保持窗口打开。脚本核对 SHA-256、原生 M1 标识、长度、CRC 和尾部 MD5，再启动服务；证书及私钥保存在 `m1-migration-state/`。它只为指定 MAC 提供激活与更新清单，**不返回 `mainSoftware`**。原厂激活接口使用 MAC 作为临时设备 ID；它不是原厂账号绑定或云服务恢复。

脚本为原厂更新服务提供兼容旧客户端的临时 HTTPS，并生成两天有效的自签证书。只在自己的局域网使用，迁移完成即停止，不开放到公网。

### 第二步：在路由器设置仅针对这台 M1 的解析

SSH 登录路由器后，在同一个窗口执行。此方式不修改 `/etc/hosts`、全局 DNS 或 DHCP 配置，而是启动独立解析实例，只将指定 M1 的 DNS 请求送给它。

```sh
RouterIp=192.168.1.1
ServerIp=192.168.1.10
DeviceIp=192.168.1.123
mkdir -p /tmp/m1-dns-migration
cp /etc/hosts /tmp/m1-dns-migration/hosts.before
uci export dhcp > /tmp/m1-dns-migration/dhcp.before
nft list ruleset > /tmp/m1-dns-migration/nft.before

dnsmasq --conf-file=/dev/null --no-hosts \
  --port=5533 --listen-address="$RouterIp" --bind-interfaces \
  --no-resolv --server=127.0.0.1#53 \
  --host-record="aircat.phicomm.com,$RouterIp" \
  --host-record="phiclouds.phicomm.com,$RouterIp" \
  --pid-file=/tmp/m1-dns-migration/dns.pid \
  --log-queries --log-facility=/tmp/m1-dns-migration/dns.log

cat > /tmp/m1-dns-migration/rules.nft <<EOF_RULES
table ip m1_dns_migration {
 chain prerouting {
  type nat hook prerouting priority -110; policy accept;
  ip saddr $DeviceIp udp dport 53 counter redirect to :5533
  ip saddr $DeviceIp tcp dport 53 counter redirect to :5533
  ip saddr $DeviceIp ip daddr $RouterIp tcp dport 443 counter dnat to $ServerIp:29443
  ip saddr $DeviceIp ip daddr $RouterIp tcp dport 9000 counter dnat to $ServerIp:29000
 }
 chain postrouting {
  type nat hook postrouting priority 101; policy accept;
  ip saddr $DeviceIp ip daddr $ServerIp tcp dport { 29443, 29000 } counter snat to $RouterIp
 }
}
EOF_RULES
nft -c -f /tmp/m1-dns-migration/rules.nft && nft -f /tmp/m1-dns-migration/rules.nft
```

规则使用独立表名；若提示表已存在，先按第五步清理上次设置，不要重复叠加。其他域名继续由路由器原 DNS 解析。9000 端口仅接收原厂状态上报，脚本不发送控制指令。

只在 `/etc/hosts` 加两行，可能使全网其他设备一起受影响，而且仍缺少兼容的 HTTPS、更新清单与下载服务。这里用单设备规则完成整个链路。

### 第三步：重启 M1，观察设备自己的请求

确认服务已就绪且当前没有上传或写入任务，再给 M1 断电约 10 秒并通电。保持网络和供电稳定，观察电脑日志和路由器日志：

```sh
tail -f /tmp/m1-dns-migration/dns.log
```

电脑日志通常依次出现：

1. `tls`、`/device/active?mac=…&productId=1`：原厂设备访问兼容服务。
2. `/firmware/checkUpdate?...wifiSoftwareVer=...&mainSoftwareVer=...`：设备主动检查更新，依赖原主控正常返回版本。
3. `/m1-native.bin?设备MAC`、`download_complete`：完整镜像已发送。服务同时支持查询参数和 Range。
4. `cloud_tcp` 内 `wifiSoftware.ifSuccess: true`：原厂上报更新成功。随后按下一节确认社区版本。

响应中的 `14.2.4.2` 用于满足原厂 `14.2.4.1` 的版本比较；安装后的社区版本为 0.9.27。没有这些请求时检查设备联网、DNS 规则计数、服务器端口和主控状态，不要反复重启或换文件强刷。

### 第四步：确认社区固件并重新配网

等待数分钟，扫描 `M1-AP-XXXXXX`，按第 5 节访问 `192.168.0.1` 并设置 2.4 GHz Wi-Fi。查新 DHCP 地址，用 `/api/v1/status` 核对完整 `device_id`、实际 `version`、`wifi_connected`、`sensor_valid` 和持续增加的 `rx_frames`；网页应正常打开。软件重启后还应自动联网、保留配置。


### 第五步：恢复路由器并停止临时服务

迁移验证完成，或确认本次测试已经停止且不再传输 / 写入后，在路由器运行：

```sh
nft delete table ip m1_dns_migration
kill "$(cat /tmp/m1-dns-migration/dns.pid)"
cmp /etc/hosts /tmp/m1-dns-migration/hosts.before
uci export dhcp > /tmp/m1-dns-migration/dhcp.after
cmp /tmp/m1-dns-migration/dhcp.before /tmp/m1-dns-migration/dhcp.after
```

两个 `cmp` 无输出表示这两份原配置未变。保留日志；不需要把旧快照整份覆盖回路由器。在电脑服务窗口按 `Ctrl+C`，完成后可删除临时证书目录。后续升级使用社区签名 OTA，不再依赖本服务。

## 5. 连接社区热点并完成首次设置

首次迁移可能需要重新配网。上传后原 IP 不通时，先扫描社区热点。

| 阶段 | 热点 | 热点内访问地址 |
| --- | --- | --- |
| 原厂配网 | `EasyLink_XXXXXX`，开放热点 | `http://10.0.0.1:8000/config-read` |
| 社区首次启动 | `M1-AP-XXXXXX`，开放热点 | `http://192.168.0.1/`，端口 80 |
| 社区已联网 | 从路由器查新 IP | `http://设备IP/`，端口 80 |

1. 找到 MAC 后缀对应的 `M1-AP-XXXXXX`，连接后保持使用该网络，即使电脑提示没有 Internet。
2. 浏览器打开 `http://192.168.0.1/`，在首次设置页面填写 2.4 GHz Wi-Fi，保存并等待连接。
3. 电脑连回路由器，按机身 MAC 查新 IP。新 IP 可能与原厂时相同，也可能变化；打开 `http://设备IP/`，不再使用原厂的 8000 端口。
4. 确认型号为 M1、版本为 **0.9.21 或更新**，温湿度、PM2.5 等读数持续刷新，屏幕和设备通信正常。
5. 确认安装与联网完成后，可正常断电再通电，检查能否重新启动、自动联网并保留配置。

也可以在同一终端中更新 IP 后，只读核对社区状态：

<!-- os:windows -->

```powershell
$DeviceIp = "192.168.1.123"
curl.exe --noproxy "*" --connect-timeout 3 --max-time 5 "http://${DeviceIp}/api/v1/status"
```

<!-- /os -->

<!-- os:mac linux -->

```sh
DeviceIp="192.168.1.123"
curl --noproxy '*' --connect-timeout 3 --max-time 5 "http://${DeviceIp}/api/v1/status"
```

<!-- /os -->

后续升级按 [固件更新](firmware-update) 使用签名 OTA 包。

## 6. 常见问题

| 现象 | 处理 |
| --- | --- |
| `Empty reply` / HTTP 000 | 核对是否完整上传，保持通电等待，查社区热点及版本，先不要重发 |
| 原厂地址不通，未见预期热点 | 社区名称为 `M1-AP-XXXXXX`；查路由器租约与无线扫描，不能只看旧 IP |
| 社区热点有，但旧 IP 不通 | 连接社区热点，在 `192.168.0.1` 重新配置 Wi-Fi |
| 上传只进行几 KB 就停住 | 等请求结束；确认原厂服务恢复后，改用第 4 节的主动下载流程，不要连续重发 |
| 图标闪烁但有 DHCP 租约 | 继续用 HTTP 核对设备；图标可能反映原厂云连接状态 |
| 等待后仍无网页、热点或网络记录 | 保存文件哈希、上传输出和设备表现，停止重复写入，按 [串口 / J-Link 恢复指南](m1-flash) 读取启动日志 |

本流程只更新 Wi-Fi 模块；传感器主控需要[单独升级](m1-mcu-update)。
