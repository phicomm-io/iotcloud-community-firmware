# 第三方说明 / Third-party notices

[中文介绍](README.md#许可) · [English introduction](README.en.md#license) · [许可文本目录](licenses/README.md)

本项目维护的代码采用 [GPL-3.0](LICENSE)。SDK 与第三方组件保留各自的版权、许可和免责声明。

Project-maintained code uses GPL-3.0. SDKs and third-party components retain their own copyright notices, licenses and disclaimers.

## 仓库内代码 / Code in this repository

| 组件与实际路径 | 许可与来源 |
| --- | --- |
| Bosch BMP3 SensorAPI：[M1/bmp3_vendor](M1/bmp3_vendor/README.md) | BSD-3-Clause；v2.0.6，提交 `db4cf8e4140c593b8c3d85f8c6c07335c7ffa9dc`。[原许可](M1/bmp3_vendor/LICENSE)及各源码头保留 |
| micro-ecc：[common/ota/vendor](common/ota/vendor/README.md) | BSD-2-Clause；提交 `541b3a78026420a3e369c4c9281c396b5e531113`。[原许可](common/ota/vendor/LICENSE.txt)保留 |
| newlib 数字转换：[common/json_numeric](common/json_numeric/) | David M. Gay / AT&T / Lucent 和 Berkeley 的许可保留于源文件头；[完整通知汇集](licenses/newlib-numeric.txt) |
| TC1 中保留的 MXCHIP 示例代码 | [TC1/TC1.mk](TC1/TC1.mk)、[mico_config.h](TC1/mico_config.h)、[app_httpd.c](TC1/http_server/app_httpd.c)、[app_httpd.h](TC1/http_server/app_httpd.h) 和 [ota_server.h](TC1/ota_server/ota_server.h)保留各自 MIT 许可及版权头 |
| HomeSpan：[DAIKIN/lib/HomeSpan](DAIKIN/lib/HomeSpan/) | MIT；1.9.1，提交 `d6fcc50a0b44c559780db1cef5e14078e2f41541`。[原许可](DAIKIN/lib/HomeSpan/LICENSE)与[上游及修改索引](DAIKIN/lib/HomeSpan/UPSTREAM.json)保留；用于开发中的 `hub` HAP 桥接 |

本产品包含由加州大学伯克利分校开发的软件。This product includes software developed by the University of California, Berkeley. 此归属说明对应 `common/json_numeric/scan.c` 的随附许可。

## 构建时获取的依赖 / Downloaded build dependencies

这些目录由构建下载，版本以相应配置和锁文件为准，组件许可保留于下载包内。

| 依赖 | 本项目中的定位与许可入口 |
| --- | --- |
| MiCO SDK 3.6.4 | [common/mico-sdk.json](common/mico-sdk.json) 固定 `6c465211d3ff8797cd835e400ec54a06530dd476`。默认路径为 `build/tc1-sdk-3.6.4/mico-os`、`build/m1-sdk/mico-os`、`build/a1-sdk/mico-os`。[SDK 原声明](https://github.com/MXCHIP/mico-os/blob/6c465211d3ff8797cd835e400ec54a06530dd476/README.md#L59)、[系统源码声明](https://github.com/MXCHIP/mico-os/blob/6c465211d3ff8797cd835e400ec54a06530dd476/MiCO/system/mico_system_init.c#L10)、[LwIP COPYING](https://github.com/MXCHIP/mico-os/blob/6c465211d3ff8797cd835e400ec54a06530dd476/MiCO/net/LwIP/ver1.5.0/COPYING)、[FreeRTOS 许可](https://github.com/MXCHIP/mico-os/blob/6c465211d3ff8797cd835e400ec54a06530dd476/MiCO/RTOS/FreeRTOS/ver9.0.0/License/license.txt) |
| Arduino-ESP32 / ESP-IDF 组件 | [DAIKIN/platformio.ini](DAIKIN/platformio.ini) 使用 `espressif32@6.12.0`，Arduino 框架 2.0.17。下载路径为 `$PLATFORMIO_CORE_DIR/packages/framework-arduinoespressif32`，未设置时位于 `~/.platformio`。Arduino 核心为 LGPL-2.1-or-later，其他组件按包内文件声明；[上游核心说明](https://github.com/espressif/arduino-esp32/blob/2.0.17/cores/esp32/Arduino.h) |
| ArduinoJson 6.21.5 | 同一 PlatformIO 配置；位于 `DAIKIN/.pio/libdeps/<环境>/ArduinoJson`。MIT，[原许可](https://github.com/bblanchon/ArduinoJson/blob/v6.21.5/LICENSE.txt) |
| 统一前端 | [frontend.lock.json](frontend.lock.json) 固定完整提交和压缩包 SHA-256，构建缓存位于 `build/frontend/<提交>`。第三方组件见对应前端版本的源码、依赖锁和制品内声明 |
| ARM GCC / newlib、Python、PlatformIO、构建与测试工具 | 工具链和 Python 获取方式见[构建依赖](docs/public/build-deps.md)；Node 测试依赖见 [package-lock.json](package-lock.json)。各工具许可随包保留 |

## 历史 WebUI 许可文本 / Historical WebUI notices

[licenses/](licenses/README.md)保留 jQuery、Chartist、Material Design Lite 和 getmdl-select 的历史许可文本。当前设备界面的组件清单见固定前端版本。
