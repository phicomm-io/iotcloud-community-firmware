# 不用原厂 App，给斐讯 A1 / M1 配网

原厂云端停服后，设备连接家里 Wi-Fi 的能力仍在固件里。A1 可以接收电脑发出的 EasyLink 配网数据；M1 可以开启热点，由电脑直接提交 Wi-Fi 参数。这两条路径都可以绕开原厂 App。

本文针对已分析的两份原厂备份。A1 工具已通过电脑端测试及原厂 ARM 解码指令仿真，真实 A1 无线配网仍待验证。M1 热点提交曾返回 HTTP 200；另已用路由器的 2.4 GHz 无线接口发送 EasyLink V2，并核对 M1 的关联、DHCP 和原厂 HTTP 身份，确认实机联网。普通电脑驱动的发包兼容性仍须单独检查。

本文结论仅对应本次备份中的 A1 `v1.47` 与 M1 `14.2.4.1`，不保证其他原厂版本具有相同入口。

<!-- os-picker -->

## 1. 已经联网的设备，先保留现状

如果路由器客户端列表能找到设备，就已具备局域网操作条件，无需再次配网，也无需先清除配置。

如果换过路由器，但记得设备原来使用的 Wi-Fi 名称和密码，可以建立同名、同密码及兼容加密方式的 2.4 GHz 网络，开启 DHCP，观察设备是否自动重连。这要求设备仍保存旧配置。

初次验证建议使用独立的 2.4 GHz WPA2-Personal/AES 网络、短英文名称和 8–63 字符密码；可以使用 b/g/n 混合模式，不必强制为 b。避免 WPA3-only。双频同名时，先确认发送电脑确实连在 2.4 GHz；必要时临时关闭 5 GHz。电脑需要能访问同一路由器下的设备，关闭访客网络或客户端隔离。

## 2. A1：先触发设备接收，再用电脑发送

### 怎样进入配网模式

A1 原厂触摸程序检测到特定触摸输入持续约 1 秒，且本次按压尚未触发过、当前没有配网任务时，才启动 EasyLink。接收窗口约为 60 秒；已经保存过 Wi-Fi 不妨碍该触发分支。

**目前尚未确认这组输入对应面板上的哪个触摸图标。** 因此，本文不能将其写成“长按某键几秒”的已验证操作；约 1 秒是固件采样阈值，不能直接替代硬件操作验证。也不能仅凭提示日志认定首次开机、断网或改 DNS 会自动进入配网。

原厂 A1 的这条路径监听无线配网包，不能照搬社区固件的热点名称。电脑开始发包也不会自动唤起设备端接收，必须先确认配网入口有效。

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

确认设备端触发入口后，点击“开始发送”，并立即让 A1 进入接收窗口。默认发送 50 秒，期间保持电脑连接目标 Wi-Fi。发送结束后，到路由器客户端列表确认 A1 是否取得 IP。端口被占用时，在启动命令末尾加 `--port 0` 自动选择。

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

EasyLink V2 把两个数据字节编码到组播地址 `239.126.X.Y`，对应目的 MAC `01:00:5e:7e:X:Y`，把序号编码到 UDP 包长度。Wi-Fi 加密不会隐藏帧头的目的 MAC，因此设备可以在尚未加入网络时接收这些地址信息。Wi-Fi 密码也在编码数据中，配网时应使用可信的无线环境。

普通 UDP 广播不能替代这套编码。浏览器需要本机 Python 服务实际发送组播，无法仅靠普通在线网页直接发包。

## 3. M1：连接设备热点，提交 Wi-Fi 参数

### 怎样进入配网模式

原厂 M1 的配网线程创建开放热点 `EasyLink_XXXXXX`，后缀是设备 MAC 的最后六个十六进制字符，启动窗口参数为 120 秒。

Wi-Fi 模块代码注册了一个 GPIO 按键：按下超过 50 毫秒、不到 3 秒后松开会启动配网；按住满 3 秒则清除参数并重启。主控也有独立的串口恢复默认入口。

**这不等于已经确认外壳按键直接连接该 GPIO。** 若机身有明确标注的配网或复位键，可先短按并观察是否出现 `EasyLink_` 热点；没有出现时，不能据此继续猜测长按时长。未配置的系统启动分支会启动自动配网，但实际是否进入应观察热点确认。

EasyLink V2 也能为 M1 配网，本次成功在 2.4 GHz 信道 1 完成，因此不能断言必须信道 6。V2 依赖空口帧中的组播目的 MAC 和长度；操作系统显示已发出 UDP 不等于这些帧已被设备接收。优先使用以下热点 HTTP 方式，可以绕开发送驱动和组播转发差异。

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
| M1 看不到热点 | 设备端配网入口是否有效；是否已经联网；窗口是否过期 |
| M1 图标闪烁、X 状态变化 | 以目标 MAC 的 DHCP 租约及 HTTP 身份为准；图标还可能反映原厂云端连接 |
| M1 提交后热点消失 | 先连接回路由器查租约，不能仅凭断开判断失败 |
| 有租约但接口不通 | IP 是否对应目标设备；双方是否被访客网络或客户端隔离 |

联网后可继续阅读 [A1 内网 / 公网免拆刷机](a1-no-disassembly) 或 [M1 内网 / 公网免拆刷机](m1-no-disassembly)。**M1 原厂首次迁移必须从 0.9.21 或更新版本开始**；A1 仍须先取得已实机验证的适配镜像，不能套用 M1 的结论。

## 附录：进入配网模式的证据范围

| 设备 | 原厂代码已确认 | 仍需硬件验证 |
| --- | --- | --- |
| A1 | 触摸返回 `FF 00 非零`；第三字节连续有效至少 10 次采样；本次按压未触发且当前无配网任务；随后启动 60 秒接收 | 寄存器与面板图标的对应、实际操作反馈 |
| M1 | GPIO 枚举 12 的短按启动配网、3 秒长按清参数；主控串口另有恢复默认入口；热点 HTTP 完成后保存联网配置 | 外壳按键连接方式及实际操作时长 |

按键入口依据原机二进制，未用第三方文章推定机身按键时长。M1 的无线联网已在上述条件验证；A1 的真实接收、两款设备的外壳按键映射及其他网卡仍需分别验证。不能把静态调用链或电脑发送成功视为实机完成。
