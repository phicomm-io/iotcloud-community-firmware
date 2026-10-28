# A1 免拆安装兼容性

本页的 A1 指空气净化器，不是 TC1 排插的 A1 硬件版本。

**原厂 A1 `v1.47` 尚无已验证的免拆迁移包，也没有可直接使用的公共迁移 DNS。首次安装请使用 [A1 / M1 有线安装与恢复](a1-m1-flash)。不要把普通 `.raw`、`.signed` 或整片镜像直接上传到原厂 OTA 接口。** 其他原厂版本同样需要单独确认兼容性。

已经运行社区固件时，按[固件更新](firmware-update)操作，无需走原厂迁移流程。

## 原厂设备联网

若路由器已列出设备，保留现有 Wi-Fi 配置，记下 IP 与 MAC。尚未联网时，按[原厂配网指南](stock-provision)使用 EasyLink 工具。电脑需连接目标 2.4 GHz Wi-Fi；无需原厂 App。 在工具中开始发送后，单独按住 **Wi-Fi 图标键 1–2 秒后松开**，接收窗口约 60 秒。

## 只读检查连接

以下命令只读取原厂配置服务。替换示例 IP，并与机身标签、路由器中的 MAC 核对；不要公开完整配置响应。

<!-- os-picker -->

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

没有响应时，先检查 IP、Wi-Fi 和路由器客户端隔离。接口可访问仅说明设备联网，不代表某个固件适合首次迁移。

## 安装后检查

完成有线安装后，按[快速开始](quick-start)配网，确认型号、版本、风机与触摸控制，再检查断电重启后能否自动联网及保留配置。遇到问题见[故障排查](faq)。
