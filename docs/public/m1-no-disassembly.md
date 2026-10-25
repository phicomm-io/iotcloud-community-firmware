# 斐讯 M1 免拆安装：局域网上传

M1 可以先通过原厂热点连接路由器，再从电脑上传社区固件，无需原厂 App。本文只更新 Wi-Fi 模块，保留原主控程序。

**原厂首次迁移必须使用 0.9.21 或更新版本。** 选择 M1 **完整版原生 OTA 镜像 `phicomm_m1.ota.bin.raw`**；旧版本可能在首次启动时反复重启，不要先刷旧版再升级。本文固定使用 0.9.21，便于核对下载文件。

本页适用于 M1 Wi-Fi 模块的原厂局域网 OTA。安装后按第 5 节确认版本、网页和传感器工作；A1 空气净化器请使用自己的安装教程。

选择电脑系统后查看相应命令。命令中的设备 IP 要替换为路由器中查到的实际地址。

<!-- os-picker -->

## 1. 准备同一局域网

电脑通过网线或 Wi-Fi 连接 M1 所在路由器，关闭客户端隔离。无需向公网开放设备端口。暂无已验证的公共 DNS 迁移服务，请使用下文局域网方式。

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

## 6. 常见问题

| 现象 | 处理 |
| --- | --- |
| `Empty reply` / HTTP 000 | 核对是否完整上传，保持通电等待，查社区热点及版本，先不要重发 |
| 原厂地址不通，未见预期热点 | 社区名称为 `M1-AP-XXXXXX`；查路由器租约与无线扫描，不能只看旧 IP |
| 社区热点有，但旧 IP 不通 | 连接社区热点，在 `192.168.0.1` 重新配置 Wi-Fi |
| 图标闪烁但有 DHCP 租约 | 继续用 HTTP 核对设备；图标可能反映原厂云连接状态 |
| 等待后仍无网页、热点或网络记录 | 保存文件哈希、上传输出和设备表现，停止重复写入，按 [串口 / J-Link 恢复指南](a1-m1-flash) 读取启动日志 |

本流程只更新 Wi-Fi 模块；传感器主控需要[单独升级](m1-mcu-update)。
