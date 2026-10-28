# 斐讯 M1 免拆安装：局域网上传与 DNS 迁移

M1 可以先通过原厂热点连接路由器，再从电脑上传社区固件，无需原厂 App。本文只更新 Wi-Fi 模块，保留原主控程序。

**原厂首次迁移必须使用 0.9.21 或更新版本。** 选择 M1 **完整版原生 OTA 镜像 `phicomm_m1.ota.bin.raw`**；旧版本可能在首次启动时反复重启，不要先刷旧版再升级。本文固定使用 0.9.21，便于核对下载文件。

本页提供原厂局域网上传和自建 DNS 迁移两条路径，只更新 M1 Wi-Fi 模块。安装后按第 5 节确认版本、网页和传感器工作；A1 空气净化器请使用自己的安装教程。

选择电脑系统后查看相应命令。命令中的设备 IP 要替换为路由器中查到的实际地址。

<!-- os-picker -->

## 1. 准备同一局域网

电脑通过网线或 Wi-Fi 连接 M1 所在路由器，关闭客户端隔离。无需向公网开放设备端口。局域网上传见第 4 节；上传卡住时可参考第 6 节的自建 DNS 迁移。没有可直接填写的公共迁移 DNS。

## 2. 先给原厂 M1 配网

已经能在路由器客户端列表找到 M1 时，保留现有配置，记下 IP 和机身 MAC，无需再次复位。

需要重新配网时，**按住机身按键约 3–4 秒后松开**，等待出现开放热点 `EasyLink_XXXXXX`。连接后，通过 `10.0.0.1:8000/config-write-uap` 提交家中 **2.4 GHz** Wi-Fi 信息，不需要原厂 App。完整操作见 [原厂配网指南](stock-provision)。

短按是切换屏幕显示；**按住约 9 秒后松开会恢复原厂网络和应用设置并重启**。重新配网只需按 3–4 秒。

配网窗口约两分钟。热点消失后，电脑连回目标路由器，按机身 MAC 查 M1 的 DHCP 地址。Wi-Fi 图标闪烁、X 图标或热点消失都不能单独证明成功，也不要反复按配网键打断连接。

## 3. 核对内网设备身份

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

## 4. 下载 0.9.21，校验并上传一次

下载以下两份 **0.9.21** 文件，放进同一个空文件夹，然后在该文件夹打开终端：

- [M1 完整版原生 OTA 镜像]({{SITE_URL}}releases/v0.9.21/phicomm_m1.ota.bin.raw)
- [SHA256SUMS 校验清单]({{SITE_URL}}releases/v0.9.21/SHA256SUMS)

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

**实测完整上传后，原厂端会直接关闭连接，curl 可能显示 `(52) Empty reply from server` 和 `http=000`。** 这既不能单独判失败，也不能当成功。记下上传字节数，保持供电，等待约 2–3 分钟，按第 5 节查社区热点和版本；不要因为这个报错立刻重发、按复位键或断电。

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

出现社区热点只说明新系统已经提供配网入口；以实际版本、网页访问和传感器更新为成功依据。后续从社区固件升级时，按 [固件更新](firmware-update) 使用签名 OTA 包。

## 6. DNS 迁移：让原厂 M1 主动下载

局域网上传一直卡住时，可以使用这条替代路径。2026-10-26 已在一台原厂 M1 跑通：原厂 Wi-Fi `14.2.4.1`、主控 `11.1.2.1`，迁移到完整版 `1.0.0-39a5b0`。完整下载、社区热点、网页、重新配网、软件重启后自动联网和持续传感器读数均已确认；未覆盖物理断电冷启动。该 1.0.0 是当次构建版本，不表示已经发布同名正式 Release，也不代表所有原厂版本都验证通过。

这次从两台电脑向 `:8000/OTA` 上传都在前几 KB 卡住；改为设备主动下载后完成。接收卡住的根因尚未确定，不要把 HTTP 000 或 Wi-Fi 慢闪当作成功，也不要连续重发。

### 需要准备什么

- 已按第 2、3 节联网并核对身份的原厂 M1；在路由器中固定它的 DHCP 地址，迁移期间不要换 IP。
- 一台能 SSH 登录的 OpenWrt / ImmortalWrt 路由器，具有 `dnsmasq`、`nft`，M1 和电脑在同一局域网。
- 一台保持开机的 Linux 电脑，安装 Python 3 和 `openssl`。以下服务器和路由命令在 Linux / 路由器 shell 执行；Windows、macOS 读者可使用一台同网段 Linux 主机运行服务。
- 已核对 SHA-256 的 **M1 完整版原生 OTA**。按第 4 节选择文件，不用 `.all.bin`、`.signed` 或主控 MCU 镜像。源码构建目录的原生文件可叫 `phicomm_m1.ota.bin`，发布下载文件可叫 `.ota.bin.raw`；以格式和校验为准，不靠改名转换。
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
4. `cloud_tcp` 内 `wifiSoftware.ifSuccess: true`：原厂上报更新成功；这仍不是社区系统已经启动的证明。

响应中的 `14.2.4.2` 只是让实测原厂 `14.2.4.1` 识别出更新的比较值，**不是社区镜像版本**。不要据此判断最终安装版本。没有这些请求时检查设备联网、DNS 规则计数、服务器端口和主控状态，不要反复重启或换文件强刷。

### 第四步：确认社区固件并重新配网

等待数分钟，扫描 `M1-AP-XXXXXX`，按第 5 节访问 `192.168.0.1` 并设置 2.4 GHz Wi-Fi。查新 DHCP 地址，用 `/api/v1/status` 核对完整 `device_id`、实际 `version`、`wifi_connected`、`sensor_valid` 和持续增加的 `rx_frames`；网页应正常打开。软件重启后还应自动联网、保留配置。

只看到完整下载或原厂成功上报时，先保持供电并查热点、租约和版本，不要再次下发更新。

### 第五步：恢复路由器并停止临时服务

迁移验证完成，或确认本次测试已经停止且不再传输 / 写入后，在路由器运行：

```sh
nft delete table ip m1_dns_migration
kill "$(cat /tmp/m1-dns-migration/dns.pid)"
cmp /etc/hosts /tmp/m1-dns-migration/hosts.before
uci export dhcp > /tmp/m1-dns-migration/dhcp.after
cmp /tmp/m1-dns-migration/dhcp.before /tmp/m1-dns-migration/dhcp.after
```

两个 `cmp` 无输出表示这两份原配置未变。保留日志；不需要把旧快照整份覆盖回路由器。在电脑服务窗口按 `Ctrl+C`，完成后可删除临时证书目录。后续升级使用社区签名 OTA，不再依赖本服务。A1 不适用本次 M1 实测结论。

## 7. 常见问题

| 现象 | 处理 |
| --- | --- |
| `Empty reply` / HTTP 000 | 核对是否完整上传，保持通电等待，查社区热点及版本，先不要重发 |
| 原厂地址不通，未见预期热点 | 社区名称为 `M1-AP-XXXXXX`；查路由器租约与无线扫描，不能只看旧 IP |
| 社区热点有，但旧 IP 不通 | 连接社区热点，在 `192.168.0.1` 重新配置 Wi-Fi |
| 上传只进行几 KB 就停住 | 等请求结束；确认原厂服务恢复后，改用第 6 节的主动下载流程，不要连续重发 |
| 图标闪烁但有 DHCP 租约 | 继续用 HTTP 核对设备；图标可能反映原厂云连接状态 |
| 等待后仍无网页、热点或网络记录 | 保存文件哈希、上传输出和设备表现，停止重复写入，按 [串口 / J-Link 恢复指南](a1-m1-flash) 读取启动日志 |

本流程只更新 Wi-Fi 模块；传感器主控需要[单独升级](m1-mcu-update)。
