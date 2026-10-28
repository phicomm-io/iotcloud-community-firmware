# 不用原厂 App，给斐讯 A1 / M1 配网

原厂云端停服后，设备连接家里 Wi-Fi 的能力仍在固件里。A1 可以接收电脑发出的 EasyLink 配网数据；M1 可以开启热点，由电脑直接提交 Wi-Fi 参数。这两条路径都可以绕开原厂 App。

M1 已实机验证配网可用；A1 经原厂固件逆向分析，理论上也可用。

适用版本：A1 原厂 `v1.47`；M1 原厂联网模块 `14.2.4.1`、主控 `11.1.2.1`。

<!-- os-picker -->

## 1. 已经联网的设备，先保留现状

如果路由器客户端列表能找到设备，就已具备局域网操作条件，无需再次配网，也无需先清除配置。

如果换过路由器，但记得设备原来使用的 Wi-Fi 名称和密码，可以建立同名、同密码及兼容加密方式的 2.4 GHz 网络，开启 DHCP，观察设备是否自动重连。这要求设备仍保存旧配置。

初次验证建议使用独立的 2.4 GHz WPA2-Personal/AES 网络、短英文名称和 8–63 字符密码；可以使用 b/g/n 混合模式，不必强制为 b。避免 WPA3-only。双频同名时，先确认发送电脑确实连在 2.4 GHz；必要时临时关闭 5 GHz。电脑需要能访问同一路由器下的设备，关闭访客网络或客户端隔离。

## 2. A1：先触发设备接收，再用电脑发送

### 怎样进入配网模式

**单独按住 A1 面板上的 Wi-Fi 图标键 1–2 秒，再松开。** 接收窗口约 60 秒，已经保存过 Wi-Fi 也可以重新配网。

先启动下面的发送工具，再按 Wi-Fi 图标键，无需连接设备热点。

### 推荐：在本机网页里填写参数

准备 Python 3.9 或更新版本，下载 [A1 EasyLink 工具包](assets/tools/a1-easylink.zip)，解压后打开 `a1-easylink` 文件夹。工具只使用 Python 标准库，无需安装 pip 依赖；发送器、启动器和网页目录要放在一起。可用 [SHA-256 清单](assets/SHA256SUMS)核对压缩包。

电脑通过 **Wi-Fi** 连接希望 A1 加入的那个 2.4 GHz 网络。这一步不能用电脑接网线代替，因为 A1 要监听电脑无线发出的帧。先查询 Wi-Fi 网卡的 IPv4；它不是路由器网关，也不是 VPN 或有线网卡的地址。

<!-- os:windows -->

在 PowerShell 运行，找到 WLAN 网卡的 IPv4，例如 `192.168.1.23`：

```powershell
ipconfig
py -3 --version
py -3 .\a1_easylink_web.py
```

<!-- /os -->

<!-- os:mac -->

在终端查询硬件端口，找到 Wi-Fi 对应接口，再查询其 IPv4。示例为 `en0`，以实际结果替换：

```sh
networksetup -listallhardwareports
ipconfig getifaddr en0
python3 --version
python3 ./a1_easylink_web.py
```

<!-- /os -->

<!-- os:linux -->

在终端运行，找到无线网卡（通常为 `wlan0` 或 `wlp…`）的 IPv4：

```sh
ip -4 address
python3 --version
python3 ./a1_easylink_web.py
```

<!-- /os -->

浏览器会打开 `http://127.0.0.1:8765`；未自动打开时，手动访问终端打印的地址。填写相同的 Wi-Fi 名称、密码和 WLAN IPv4。

点击“开始发送”，随即单独按住 A1 的 **Wi-Fi 图标键 1–2 秒后松开**。默认发送 50 秒，期间保持电脑连接目标 Wi-Fi。发送结束后，到路由器客户端列表确认 A1 是否取得 IP。端口被占用时，在启动命令末尾加 `--port 0` 自动选择。

页面支持停止发送。关闭标签页后，任务仍会继续到设定时间；点击“停止”或在工具终端按 Ctrl+C 才会提前停止。页面显示“发送结束”只代表电脑发送完毕，不代表 A1 已联网。

### 也可以直接使用命令行

<!-- os:windows -->

```powershell
py -3 .\a1_easylink.py --ssid "RescueWiFi" --interface-ip 192.168.1.23
```

<!-- /os -->

<!-- os:mac linux -->

```sh
python3 ./a1_easylink.py --ssid "RescueWiFi" --interface-ip 192.168.1.23
```

<!-- /os -->

密码在终端交互输入，不会显示。无密码网络需明确加 `--open`；加 `--dry-run` 只检查参数和编码，不发包。

工具通过本机 Python 服务发送 EasyLink 无线配网包，普通在线网页无法直接代替。配网数据包含 Wi-Fi 密码，请在可信的无线环境中使用。

## 3. M1：连接设备热点，提交 Wi-Fi 参数

### 怎样进入配网模式

**按住 M1 机身按键约 3–4 秒后松开**，等待出现开放热点 `EasyLink_XXXXXX`。后缀是设备 MAC 的最后六个十六进制字符。

短按是切换屏幕显示；长按约 3 秒进入配网；**按住约 9 秒后松开会恢复原厂网络和应用设置并重启**。重新配网只需按 3–4 秒。

未配置的设备开机会自动进入配网。看到热点后，尽量在两分钟内连接并提交参数；热点未出现或窗口已过期时，重新按住 3–4 秒触发。

M1 也支持 EasyLink V2，已在 2.4 GHz 信道 1 实测成功，不限于信道 6。推荐优先使用下面的热点 HTTP 方式。

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

## 4. 怎样确认配网完成

从路由器客户端列表取得设备 IP，电脑连接同一路由器，再读取配置接口。以下 IP 必须换成设备的新地址：

<!-- os:windows -->

```powershell
$DeviceIp = "192.168.1.123"
curl.exe --http1.0 --noproxy "*" --connect-timeout 3 --max-time 5 "http://${DeviceIp}:8000/config-read"
```

<!-- /os -->

<!-- os:mac linux -->

```sh
DeviceIp="192.168.1.123"
curl --http1.0 --noproxy '*' --connect-timeout 3 --max-time 5 "http://${DeviceIp}:8000/config-read"
```

<!-- /os -->

确认设备身份、网络可达后，再正常断电上电，检查能否自动重连。这样才能分别确认本次联网和配置持久化。HTTP 200、热点消失或工具发送完成，都不能单独替代这个检查。

| 现象 | 优先检查 |
| --- | --- |
| A1 一直收不到 | 是否实际进入接收窗口；电脑是否通过目标 2.4 GHz Wi-Fi 发包；WLAN IPv4 是否正确 |
| A1 发送结束仍未联网 | SSID、密码、加密方式；组播过滤、VPN 或驱动行为；重新进入接收窗口后重试 |
| M1 看不到热点 | 是否按住机身按键 3–4 秒；刷新无线列表，窗口结束后重新触发；不要持续按到 9 秒恢复默认 |
| M1 图标闪烁、X 状态变化 | 以目标 MAC 的 DHCP 租约及 HTTP 身份为准；图标还可能反映原厂云端连接 |
| M1 提交后热点消失 | 先连接回路由器查租约，不能仅凭断开判断失败 |
| 有租约但接口不通 | IP 是否对应目标设备；双方是否被访客网络或客户端隔离 |

联网后可继续阅读 [A1 内网 / 公网免拆刷机](a1-no-disassembly) 或 [M1 内网 / 公网免拆刷机](m1-no-disassembly)。**M1 原厂首次迁移必须从 0.9.21 或更新版本开始**；A1 仍须先取得已实机验证的适配镜像，不能套用 M1 的结论。
