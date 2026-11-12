# Daikin G3 / 大金空气质量检测（3代）

BRY88BBA15G 的 ESP32 应用，复用 iotcloud 公共 HTTP、MessagePack、MQTT、鉴权、LAN 和 WebUI。屏幕 MCU 独立发布于 [iotcloud-bry88bba15g-screenmcu](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu)，精确来源见 `screen-protocol.lock.json`。操作教程见 [G3 guide](../docs/public/daikin-g3-guide.md)，接口见 [SERVICES.md](SERVICES.md)。

## RC6 行为

- 保留原厂屏幕协议；社区屏幕只支持锁定的 RC6 协议：SCN3 9520 字节、诊断 V3、资源报告 V2。未声明社区能力时使用原厂消息。
- 原厂 MCU 可接收四家天气提供商的当前温湿度与八类图标；新数据到达后主动更新，关闭或过期时恢复缺测。室外 PM2.5 未接入；地点标签与三天预报需社区 MCU。
- UART1 TX=GPIO16、RX=GPIO17，默认 115200 8N1。社区 MCU 在恢复阶段可协商 230400、460800、921600 或自动选择；探测超时回退 115200。传感器由屏幕 MCU 管理。TVOC 数值为 ppb，也可按原厂等级显示；未确认的 HCHO 字段不发布有效值。
- 页面支持时钟、数值、文字、图表、天气、提醒，支持主题、壁纸、标题隐藏、中英文、32–256 点本地历史和真实屏幕比例预览。
- 支持外部文字／图片推送、黑屏临时唤醒、定时或触摸确认关闭。队列最多 16 条、图片最多 4 张；前四条以圆角卡片堆叠，双击前触摸键按 0（原页面）→1→2→3→4→0 切换。浏览器和 `tools/screen_media.py` 可先缩放、压缩图片。
- 摄像头图片组件使用 HTTP(S) MJPEG：MCU 主动请求、解析与硬件解码，ESP32 仅透传 DNS/TCP/TLS。隐藏页面、黑屏、通知遮挡和 OTA 时暂停拉流。完整 HTTP/MQTT/UDP 接口见[媒体协议](https://phicomm-io.github.io/iotcloud-community-protocols/zh/daikin-g3-media/)。
- 亮度 0–100% 实时设置，支持供电／电池休眠和每周计划。启动最多 10%；OTA 保持低亮度。PD12 在启动和升级时关闭 AM1008L，业务就绪后恢复。
- MCU 默认在五秒后提前结束正常维护窗口；可关闭自动继续，也可手动保持或继续。异常维护不会自动放行业务。原厂引导区及来电开机门控保持原样。
- NOR 字库、壁纸均校验后缓存到 SDRAM；失败限时降级，不进入重启循环。通信和恢复服务独立保留。
- ESP32 可从 GitHub、国内和国际镜像检查并安装签名更新，RC 走候选通道。屏幕应用、字库由资源页下载校验后安装，MCU 大文件上传需要浏览器在局域网直连设备。

## 存储和恢复

保留原厂 NVS、otadata、分区表和两份应用槽。ESP32 OTA 只写备用应用及 otadata。屏幕应用安装不改引导区；检查精确目标、向量、摘要和 NOR 回读，再激活已有引导程序。未知非空资源区不擦除。物理掉电中断屏幕内部 Flash 搬运尚未验证。

社区 MCU 可只读导出前 80 KiB。`tools/backup_screen_boot.py` 读取两遍，摘要一致后保存记录；原厂 MCU 没有这个接口。备份可选，已有有效备份可以用 `tools/flash_screen.py --reuse-boot-backup` 复用。新脚本与配套 ESP32 允许直接发送完整 MCU 应用，不要求原厂版本白名单或先装恢复镜像；新 MCU 应用也不再核对引导 MD5 白名单。原厂串口安装仍依赖已有引导的复制协议，其他引导需核对兼容性，详见教程。免拆 ESP32 迁移工具位于 [tools/daikin_g3_migrate](../tools/daikin_g3_migrate/README.md)。

## Build / 构建

```sh
~/.cache/phicomm/python3/bin/pio run -d DAIKIN -e bry88bba15g
cmake -S BRY88BBA15G -B ~/.cache/phicomm/bry88-bridge-host \
  -DSCREEN_MCU_SOURCE=/path/to/iotcloud-bry88bba15g-screenmcu \
  -DARDUINOJSON_DIR=/path/to/ArduinoJson/src
cmake --build ~/.cache/phicomm/bry88-bridge-host -j4
ctest --test-dir ~/.cache/phicomm/bry88-bridge-host --output-on-failure
```

The G3 application reuses the common iotcloud/openiot networking and frontend. RC6 supports the factory MCU and only the pinned current community protocol (SCN3, diagnostic V3, resource report V2). Sensor data comes from the display MCU; TVOC supports ppb or factory grades.

Stock MCUs receive current temperature, humidity and eight icon categories from all four weather providers. Fresh results update immediately; disabled/expired data clears to missing values. Outdoor PM2.5 is unavailable. Location labels and three-day forecasts require the community MCU.

Compose clocks, values, text, charts, weather and reminders with themes, wallpaper, language, hidden headings and 32–256-point local history. Brightness is adjustable from 0–100%, with idle timers and weekly schedules. Startup is limited to 10%; OTA uses dimmed output. PD12 disables AM1008L during startup/OTA and restores it when business services are ready.

External text/image notifications support temporary wake, timed dismissal and touch confirmation. The queue holds 16 messages and four images. Double tap the front touch key to cycle the normal page (0) and the first four stacked notification cards (1–4). The browser and `tools/screen_media.py` resize and compress images before upload. The camera image widget uses HTTP(S) MJPEG: the MCU requests, parses and decodes images in hardware while ESP32 relays DNS/TCP/TLS bytes. Hidden pages, sleep, notifications and OTA pause pulling. UART defaults to 115200 and can negotiate 230400, 460800, 921600 or automatic selection during recovery, falling back to 115200 on timeout. See the [media API](https://phicomm-io.github.io/iotcloud-community-protocols/en/daikin-g3-media/) for HTTP/MQTT/UDP commands.

Normal MCU maintenance resumes automatically after five seconds unless disabled or held. Fault recovery stays available. Optional NOR resources load into validated SDRAM caches and degrade on failure without a reset loop. ESP32 checks signed online updates through stable/candidate channels; the LAN resource page downloads and verifies screen/font updates. Large MCU uploads require direct LAN access.

Application updates preserve both bootloaders and stock partitions. The factory power-on key gate remains unchanged. Community MCU boot reads are optional and read-only; matching backups are reusable. The new tool/ESP32 can send a complete application directly to stock firmware, without stock-version or recovery-only gates. The new MCU removes the boot MD5 allowlist. UART installation still depends on the existing factory copy contract; see the [English G3 guide](../docs/public/en/daikin-g3-guide.md). Power loss during the MCU's internal Flash copy remains untested.
