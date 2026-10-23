# 斐讯 M1 免拆刷机：内网上传与公网 DNS 迁移

M1 可以先通过原厂热点连接路由器，再从电脑上传社区固件，无需原厂 App。本文只更新 Wi-Fi 模块，保留原主控程序。

**原厂首次迁移必须使用 0.9.21 或更新版本。** 选择 M1 **完整版原生 OTA 镜像 `phicomm_m1.ota.bin.raw`**；旧版本可能在首次启动时反复重启，不要先刷旧版再升级。本文固定使用 0.9.21，便于核对下载文件。

包含启动修复的候选已在一台 M1 完成原厂内网 OTA、社区热点网页、重新配网及传感器通信验证。正式包仍应按本文检查实际启动结果；公网 DNS 迁移尚未实测，也没有随文可用的公共迁移 DNS。A1 不适用本次 M1 实测结论。

选择电脑系统后查看相应命令。命令中的设备 IP 要替换为路由器中查到的实际地址。

<!-- os-picker -->

## 1. 选择内网还是公网

| 路径 | 用户如何操作 | 当前状态 |
| --- | --- | --- |
| 内网免拆 | 电脑向原厂配置服务上传原生 OTA 镜像 | M1 修复候选已实机通过；首次迁移从 0.9.21 起 |
| 公网免拆 | 路由器 DNS 指向兼容服务，设备自行下载 | 服务、TLS 与完整迁移流程仍待实测 |

推荐使用内网方式。电脑可通过网线或 Wi-Fi 连接同一路由器，无需把设备端口开放到公网。

## 2. 先给原厂 M1 配网

已经能在路由器客户端列表找到 M1 时，保留现有配置，记下 IP 和机身 MAC，无需再次复位。

尚未联网时，让设备进入原厂配网模式，以出现开放热点 `EasyLink_XXXXXX` 为准。连接热点后，通过 `10.0.0.1:8000/config-write-uap` 提交家中 **2.4 GHz** Wi-Fi 信息，不需要原厂 App。完整操作和分系统命令见 [原厂配网指南](stock-provision)。

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

原厂设置不一定能直接沿用。上传后原 IP 不通时，先扫描社区热点；这次实测需要重新配置 Wi-Fi。

| 阶段 | 热点 | 热点内访问地址 |
| --- | --- | --- |
| 原厂配网 | `EasyLink_XXXXXX`，开放热点 | `http://10.0.0.1:8000/config-read` |
| 社区首次启动 | `M1-AP-XXXXXX`，开放热点 | `http://192.168.0.1/`，端口 80 |
| 社区已联网 | 从路由器查新 IP | `http://设备IP/`，端口 80 |

1. 找到 MAC 后缀对应的 `M1-AP-XXXXXX`，连接后保持使用该网络，即使电脑提示没有 Internet。
2. 浏览器打开 `http://192.168.0.1/`，在首次设置页面填写 2.4 GHz Wi-Fi，保存并等待连接。
3. 电脑连回路由器，按机身 MAC 查新 IP。新 IP 可能与原厂时相同，也可能变化；打开 `http://设备IP/`，不再使用原厂的 8000 端口。
4. 确认型号为 M1、版本为 **0.9.21 或更新**，温湿度、PM2.5 等读数持续刷新，屏幕和设备通信正常。
5. 确认安装与联网完成后，可正常断电再通电，检查能否重新启动、自动联网并保留配置。本次候选实测未覆盖这一步，请保留自己的检查结果。

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

## 6. 公网：用兼容服务承接原厂更新

M1 开机后的 OTA 线程会等待主控版本信息，再检查更新。这说明触发并不一定依赖 App，但原主控必须能够正常响应。

这份备份引用的接口为：

| 用途 | 原厂地址 |
| --- | --- |
| 激活 | `https://phiclouds.phicomm.com/device/active?mac=…&productId=1` |
| 检查更新 | `https://aircat.phicomm.com/firmware/checkUpdate?mac=…&wifiSoftwareVer=…&mainSoftwareVer=…` |
| 长连接业务 | `aircat.phicomm.com:9000` |

这些域名来自固件，不代表已核验当前在线状态。自建迁移服务时，应通过受控 DNS 让目标设备访问兼容服务，并保留原 DNS 设置供迁移结束后恢复。

### 用户侧 DNS 设置与检查

这一流程以服务维护者已公布**完成目标设备验证的 DNS 地址和迁移镜像**为前提。目前尚无随文可用的地址，不能把示例主机名当成现成服务。

1. 记录路由器原来的 DNS 设置和设备网络信息。
2. 在设备所在网络的路由器 DHCP / LAN DNS 设置中，填入维护者提供的地址；部分路由器由自身代理 DNS，需同时按维护者说明设置转发。只改电脑 DNS 不会让设备自动使用它。
3. 按路由器说明让设备重新取得租约、刷新解析；不要在传输或写入固件时重启设备。若设备使用静态 DNS，需另行确认其实际解析去向。
4. 用同网电脑查询更新域名，确认解析结果符合维护者公布值；再由兼容服务日志确认**目标设备自身**的激活、更新检查和下载请求。

<!-- os:windows -->

```powershell
Resolve-DnsName aircat.phicomm.com -Type A
```

<!-- /os -->

<!-- os:mac linux -->

```sh
dig +short A aircat.phicomm.com
```

<!-- /os -->

电脑查到新地址只证明电脑的解析结果，不能证明设备也已使用该 DNS，更不能证明已刷写成功。迁移完成后恢复用户原有 DNS，避免继续依赖临时服务。

### 给服务维护者的更新清单

原厂把 Wi-Fi 和主控更新分开处理。迁移 Wi-Fi 模块时，只提供 `wifiSoftware`；不要顺带下发 `mainSoftware`。

下面是候选响应结构，URL、MD5 与版本仅作格式示例，不能直接作为真实升级清单：

```json
{
  "wifiSoftware": {
    "url": "http://migration.example/m1-native.bin",
    "ver": "14.2.4.2",
    "md5": "<实际下载正文的32位MD5十六进制>",
    "enforce": true
  }
}
```

已追踪逻辑要求远端 Wi-Fi 版本字符串与本地不同，并满足强制更新条件。示例 `ver` 是原厂比较使用的元数据，不表示镜像内社区固件的版本，也不能照搬 A1 的数值比较规则。

原厂下载器会给 URL 追加 MAC 查询参数，并有 Range / 续传相关路径。文件服务需要兼容实际形成的请求，不能因为本机浏览器能下载文件就认定原厂客户端下载兼容。

### DNS 之后，服务端还必须做什么

```text
M1 已联网，原主控正常
  → DNS 指向兼容服务
  → 完成必要的激活 / 状态交互
  → 主控返回版本，Wi-Fi 模块检查更新
  → 解析 wifiSoftware，下载并校验镜像
  → 提交 Wi-Fi OTA，重启后运行新固件
```

服务端至少需要兼容激活、HTTPS 更新接口、Wi-Fi 清单和文件下载，并能够观察各步骤的实际请求。仅搭一个文件下载站，或只修改 DNS，都不足以完成这条流程。

静态调用显示检查更新时未启用 CA 证书验证，但旧客户端支持的 TLS 版本、密码套件、证书算法和 SNI 行为仍要实测。固件下载用 MD5 校验内容；MD5 不等于原厂私钥签名，也不能证明任意格式的镜像都能启动。

目前没有随文提供完成设备兼容验证的公共服务，所以还不能写成“填入某个 DNS 后重启即可刷机”的用户步骤。

## 7. 常见问题

| 现象 | 处理 |
| --- | --- |
| `Empty reply` / HTTP 000 | 核对是否完整上传，保持通电等待，查社区热点及版本，先不要重发 |
| 原厂地址不通，未见预期热点 | 社区名称为 `M1-AP-XXXXXX`；查路由器租约与无线扫描，不能只看旧 IP |
| 社区热点有，但旧 IP 不通 | 连接社区热点，在 `192.168.0.1` 重新配置 Wi-Fi |
| 图标闪烁但有 DHCP 租约 | 继续用 HTTP 核对设备；图标可能反映原厂云连接状态 |
| 等待后仍无网页、热点或网络记录 | 保存文件哈希、上传输出和设备表现，停止重复写入，按 [串口 / J-Link 恢复指南](a1-m1-flash) 读取启动日志 |
| 改 DNS 后没有更新 | 公网迁移仍待验证；检查设备实际 DNS、兼容服务日志和更新触发条件 |

本页的已验证路径限于 M1 Wi-Fi 模块的内网迁移；不包含主控 MCU 刷写，也不将 A1 或公网迁移列为已验证。
