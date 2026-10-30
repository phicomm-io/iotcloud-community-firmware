# A1 原厂配网与免拆安装

适用于原装 EMW3080B / MK3080B 模块的 A1 空气净化器。**通过原厂局域网 OTA 安装社区固件已证实可行。** 本文提供 EasyLink 配网、局域网上传，以及根据原厂固件整理的 DNS 主动下载路径。

使用 **A1 完整版原生 OTA 镜像**，以下固定为稳定版 **v0.9.27**。保持正常供电、装好滤芯；有线恢复见 [A1 刷机](a1-flash)。

<!-- os-picker -->

## 1. 给原厂 A1 配网

路由器已列出 A1 时，保留现有配置，记下 IP 和机身 MAC，直接进入下一步。需要重新配网时使用 EasyLink，有条件建议使用官方 App。

单独按住面板上的 **Wi-Fi 图标键 1–2 秒后松开**，接收窗口约 60 秒。原厂这条路径监听无线配网包，不创建配网热点。

### Linux EasyLink V2 配网

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

按提示输入目标 Wi-Fi 名称、密码（密码不回显），发送开始后，单独按住 A1 面板的 Wi-Fi 图标键 1–2 秒后松开。发送结束后删除临时 monitor 接口：

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

- [A1 完整版原生 OTA 镜像]({{SITE_URL}}releases/v0.9.27/phicomm_a1.ota.bin.raw)
- [SHA256SUMS 校验清单]({{SITE_URL}}releases/v0.9.27/SHA256SUMS)

**核对型号为 A1。** 本文首次迁移使用上面指定的完整版文件；不使用 `no-frontend-resources` 精简版。`.all.bin` 和整片备份用于其他恢复流程，`.ota.bin.signed` 用于已经运行社区固件的设备，都不能作为原厂接口的上传正文。不要将这些文件改名为 `.raw` 使用。

先计算镜像的 SHA-256，并与清单中 **`phicomm_a1.ota.bin.raw` 对应的那一行**逐字符比较；不一致时停止。

<!-- os:windows -->

```powershell
Get-FileHash -Algorithm SHA256 .\phicomm_a1.ota.bin.raw
Select-String -Path .\SHA256SUMS -Pattern '\s+\*?phicomm_a1\.ota\.bin\.raw$'
```

<!-- /os -->

<!-- os:mac -->

```sh
shasum -a 256 ./phicomm_a1.ota.bin.raw
awk '$2 == "phicomm_a1.ota.bin.raw" { print }' ./SHA256SUMS
```

<!-- /os -->

<!-- os:linux -->

```sh
sha256sum ./phicomm_a1.ota.bin.raw
awk '$2 == "phicomm_a1.ota.bin.raw" { print }' ./SHA256SUMS
```

<!-- /os -->

供电和网络保持稳定，执行一次上传。命令固定 HTTP/1.0、关闭代理和 Expect，不自动重试：

<!-- os:windows -->

```powershell
curl.exe --http1.0 --noproxy "*" --connect-timeout 5 --max-time 300 --header "Content-Type: application/ota-stream" --header "Expect:" --data-binary "@phicomm_a1.ota.bin.raw" --write-out "uploaded=%{size_upload} http=%{http_code}\n" "http://${DeviceIp}:8000/OTA"
```

<!-- /os -->

<!-- os:mac linux -->

```sh
curl --http1.0 --noproxy '*' --connect-timeout 5 --max-time 300 --header 'Content-Type: application/ota-stream' --header 'Expect:' --data-binary '@phicomm_a1.ota.bin.raw' --write-out 'uploaded=%{size_upload} http=%{http_code}\n' "http://${DeviceIp}:8000/OTA"
```

<!-- /os -->

原厂端可能在接收后直接关闭连接，curl 显示 `(52) Empty reply from server` 和 `http=000`。记下上传字节数，保持供电，等待约 2–3 分钟，按第 5 节查社区热点和版本；不要因为这个报错立刻重发、按复位键或断电。

## 4. 方式二：DNS 重定向，让原厂主动下载

此路径依据原厂 **v1.47** 固件整理，尚未完成 A1 实机验证，建议优先使用局域网上传。原厂服务如下：

| 用途 | 域名 / 请求 |
| --- | --- |
| 激活 | `phicloudsym.phicomm.com:443/device/active?mac=…&productId=30` |
| 更新检查 | `aircleaner.phicomm.com:443/aircleanerfirmware/checkUpdate` |
| 业务连接 | `aircleanerconnecter.phicomm.com:9000` |

更新清单直接使用顶层 `url`、`md5`、`ver`、`enforce`。原厂 v1.47 要求版本数值更高（例如 `v1.48`）且 `enforce: true`；此值仅用于满足原厂版本比较，安装后的社区版本为 0.9.27。原厂这条 TLS 路径不校验证书，但服务端需要兼容旧客户端。

### 在 Linux 电脑启动临时服务

下载 [a1_dns_migration.py](assets/tools/a1-dns-migration/a1_dns_migration.py) 和 [SHA256SUMS](assets/tools/a1-dns-migration/SHA256SUMS)，安装 Python 3、`openssl`，与已校验的 A1 原生 OTA 放在同一目录。替换以下全部示例 IP、MAC 和 SHA-256：

```sh
sha256sum -c SHA256SUMS
python3 a1_dns_migration.py \
  --image ./phicomm_a1.ota.bin.raw \
  --sha256 REPLACE_WITH_IMAGE_SHA256 \
  --mac 00:11:22:33:44:55 \
  --device-ip 192.168.1.123 \
  --router-ip 192.168.1.1 \
  --server-ip 192.168.1.10
```

出现 `"kind": "ready"` 后保持窗口打开。脚本先校验哈希、A1 型号、原生格式、CRC 和 MD5，再开放服务。旧 TLS 兼容证书保存在 `a1-migration-state/`；临时服务仅用于自己的局域网，完成后停止。

### 在路由器仅重定向这台 A1

准备有 `dnsmasq`、`nft` 的 OpenWrt / ImmortalWrt 路由器，先固定 A1 的 DHCP 地址。SSH 登录后在同一个窗口执行，替换全部示例地址。电脑的 29080、29443、29000 和路由器的 5533 端口需空闲并允许局域网访问。

```sh
RouterIp=192.168.1.1
ServerIp=192.168.1.10
DeviceIp=192.168.1.123
mkdir -p /tmp/a1-dns-migration
cp /etc/hosts /tmp/a1-dns-migration/hosts.before
uci export dhcp > /tmp/a1-dns-migration/dhcp.before
nft list ruleset > /tmp/a1-dns-migration/nft.before

dnsmasq --conf-file=/dev/null --no-hosts \
  --port=5533 --listen-address="$RouterIp" --bind-interfaces \
  --no-resolv --server=127.0.0.1#53 \
  --host-record="aircleaner.phicomm.com,$RouterIp" \
  --host-record="phicloudsym.phicomm.com,$RouterIp" \
  --host-record="aircleanerconnecter.phicomm.com,$RouterIp" \
  --pid-file=/tmp/a1-dns-migration/dns.pid \
  --log-queries --log-facility=/tmp/a1-dns-migration/dns.log

cat > /tmp/a1-dns-migration/rules.nft <<EOF_RULES
table ip a1_dns_migration {
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
nft -c -f /tmp/a1-dns-migration/rules.nft && nft -f /tmp/a1-dns-migration/rules.nft
```

不要重复叠加同名表。以上规则不修改全局 DNS / DHCP 配置，只重定向指定设备；业务 TCP 监听仅记录请求，不发送控制命令。

### 等待原厂检查更新

改 DNS 不会立即触发升级。原厂 v1.47 注册每小时定时器，在时间字符串的小时为 `18`、已激活且 OTA 状态允许时检查。保持设备联网及时间同步；重启本身不能保证立即请求更新。

观察 `/device/active`、`/aircleanerfirmware/checkUpdate`、`/a1-native.bin` 和 `download_complete` 日志。完整下载后按第 5 步检查社区热点和实际版本；没有检查请求时使用局域网上传，不要反复重启或猜测云端控制指令。

### 清理临时服务

确认传输、写入已经结束后，删除独立规则表和解析实例：

```sh
nft delete table ip a1_dns_migration
kill "$(cat /tmp/a1-dns-migration/dns.pid)"
cmp /etc/hosts /tmp/a1-dns-migration/hosts.before
uci export dhcp > /tmp/a1-dns-migration/dhcp.after
cmp /tmp/a1-dns-migration/dhcp.before /tmp/a1-dns-migration/dhcp.after
```

两个 `cmp` 无输出表示原配置未变。在电脑窗口按 Ctrl+C 停止服务，保留日志。

## 5. 连接社区热点并完成首次设置

首次迁移可能需要重新配网。上传后原 IP 不通时，先扫描社区热点。

| 阶段 | 热点 | 热点内访问地址 |
| --- | --- | --- |
| 社区首次启动 | `A1-AP-XXXXXX`，开放热点 | `http://192.168.0.1/`，端口 80 |
| 社区已联网 | 从路由器查新 IP | `http://设备IP/`，端口 80 |

1. 找到 MAC 后缀对应的 `A1-AP-XXXXXX`，连接后保持使用该网络，即使电脑提示没有 Internet。
2. 浏览器打开 `http://192.168.0.1/`，在首次设置页面填写 2.4 GHz Wi-Fi，保存并等待连接。
3. 电脑连回路由器，按机身 MAC 查新 IP。新 IP 可能与原厂时相同，也可能变化；打开 `http://设备IP/`，不再使用原厂的 8000 端口。
4. 确认型号为 A1、版本为 **0.9.27**，风机、灯环和触摸控制正常。
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
