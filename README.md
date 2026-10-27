# IoTCloud 设备社区固件

[English](README.en.md)

为 TC1 智能排插、M1 空气检测仪、A1 空气净化器和 DAIKIN 大金空气检测仪提供社区固件。配网后直接访问设备网页，也可接入 MQTT、Home Assistant 或 IoTCloud；局域网使用无需平台账号。

**[设备与功能](https://phicomm-io.github.io/#devices) · [安装升级与现有下载](https://phicomm-io.github.io/#install) · [常见问题](https://phicomm-io.github.io/#faq) · [参与开发](https://phicomm-io.github.io/#develop)**

## 设备与功能

| 设备 | 日常功能 | 安装与使用 |
| --- | --- | --- |
| TC1 智能排插（A1 硬件版本） | 六路插口、功率与电量、定时、童锁、按键操作 | [安装](https://phicomm-io.github.io/guides/tc1-no-disassembly.html) · [操作](https://phicomm-io.github.io/guides/tc1-guide.html) |
| M1 空气检测仪 | PM2.5、甲醛、温湿度、屏幕亮度与 A1 联动；配套开源主控可扩展颗粒物详情、CO₂ 显示和音乐 | [安装](https://phicomm-io.github.io/guides/m1-no-disassembly.html) · [操作](https://phicomm-io.github.io/guides/m1-guide.html) |
| A1 空气净化器 | 风机、自动模式、滤芯、灯环、夜间模式与多检测仪联动；另有新风版 | [安装](https://phicomm-io.github.io/guides/a1-m1-flash.html) · [操作](https://phicomm-io.github.io/guides/a1-guide.html) |
| DAIKIN 大金空气检测仪 | PM2.5、PM10、CO₂、温湿度、TVOC 与可选甲醛，支持 A1 联动 | [安装与操作](https://phicomm-io.github.io/guides/daikin-guide.html) |

TC1 的 A1 硬件版本与 A1 空气净化器是不同产品，固件不能混用。TC1 A2 暂不支持。M1 / A1 适用于原装 EMW3080B / MK3080B 模组；大金请按教程核对 ESP32 / 4 MB Flash 型号。

**M1 的 Wi-Fi 模块与传感器主控分别升级。** [GPL-3.0 开源主控](https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20)推荐用于原厂 **11.x**；原厂 **17.1.2.3 / 17.x** 差异较大，尚待分析，暂不建议迁移。按实际读到的主控版本判断，未知版本不作推测。普通 Wi-Fi OTA 不更新主控，基本空气读数无需另刷主控。详见[主控兼容说明](https://phicomm-io.github.io/guides/m1-mcu-firmware.html)。

## 安装与更新

1. 阅读[刷机前准备](https://phicomm-io.github.io/guides/flash-prep.html)，确认硬件、备份、供电和恢复方式。
2. 从[现有下载](https://phicomm-io.github.io/#releases)选择机型与变体，核对 `SHA256SUMS`，按对应教程安装。
3. 按[快速开始](https://phicomm-io.github.io/guides/quick-start.html)配网，访问设备 IP；按需接入 [MQTT / Home Assistant](https://phicomm-io.github.io/guides/mqtt-link.html) 或 [IoTCloud](https://phicomm-io.github.io/guides/iotcloud-connect.html)。

已有社区固件的日常网页更新使用匹配产品的 `.ota.bin.signed`；原生应用 `.ota.bin.raw` 仅用于教程明确支持的迁移或救援入口。`.all.bin` 是有线安装的完整镜像，不能用于网页 OTA。更新和恢复见[固件更新](https://phicomm-io.github.io/guides/firmware-update.html)。

大金原厂 OTA 布局的应用槽为 **`0x180000`（1.5 MiB）**，普通版首次迁移按[原厂 OTA 教程](https://phicomm-io.github.io/guides/daikin-stock-ota.html)执行。v1.0.0-rc.3 的 `full → hub` 签名切换见[中枢指南](https://phicomm-io.github.io/guides/daikin-guide.html#普通版切换为中枢)，需支持该入口的新固件和符合设备容量的镜像。

## v1.0.0-rc.3 Beta 候选版

**RC 实际属于 Beta 测试版本，后续仍会有较大改动，不建议日常使用；日常使用推荐稳定版 [v0.9.27](https://phicomm-io.github.io/#releases)。** 当前候选为 [v1.0.0-rc.3](https://github.com/phicomm-io/iotcloud-community-firmware/releases/tag/v1.0.0-rc.3)，完整发布说明按来源保留 rc、rc.1、rc.2 的累计变化；旧候选不再作为下载入口。联网固件、M1 传感器主控、前端和平台分别发布，配套版本以说明为准。

| 方向 | 候选版功能 |
| --- | --- |
| 使用与升级 | 中英文界面、设备语言、配置备份恢复、迁移失败保护与升级差异 |
| M1 | 甲醛 / PM2.5 两位置分别选本机传感器、TC1 功率、A1 主风机生效输出（0–100%）、大金 CO₂ 等数据或 API；双击整体切换原始 / 用户读数，各自过期回退，颜色按阈值或固定值；PM2.5 稳定处理，甲醛 / PM2.5 超阈值蜂鸣独立开关、默认关闭，色灯阈值、闹钟与手动鸣叫独立 |
| A1 恢复 | rc.3 新增 Wi-Fi 键短触组合：短按两次、第三次按住 1–2 秒后松手重置网络；短按三次、第四次按住 1–2 秒后松手恢复出厂。保留原 5 秒 / 10 秒长按，新增手势尚待实机验收 |
| DAIKIN `hub` | 独立中枢变体：本地账户、TCP / WebSocket MQTT Broker、局域网 Home Assistant、上游 MQTT 客户端与巴法云转发；自动列出本机，网页密码可选；普通版继续用于传感器 |
| 配套接口与恢复 | 沿用 rc.1 的通用数值字段及 M1 显示配置接口、rc.2 的 M1 配置恢复与网页恢复入口修复；自定义调用方须使用当前接口和配套前端 |

Apple 家庭采用 `hub` 的实验性 HomeKit HAP 桥接，配对、控制和重启恢复仍待真机验证；当前不包含 Matter。

## 构建与参与

接口、协议、配置兼容性和测试向量集中维护在[中英文开发文档](https://phicomm-io.github.io/iotcloud-community-protocols/)。先阅读[贡献指南](CONTRIBUTING.md)，使用 `feature/* → dev → main` 工作流。

| 项目 | 内容 |
| --- | --- |
| [iotcloud-community-firmware](https://github.com/phicomm-io/iotcloud-community-firmware) | 固件、构建与用户指南 |
| [iotcloud-community-frontend](https://github.com/phicomm-io/iotcloud-community-frontend) | 设备 WebUI、IoTCloud 与在线演示 |
| [iotcloud-community-mqtt](https://github.com/phicomm-io/iotcloud-community-mqtt) | 平台服务、MQTT 与部署 |
| [iotcloud-community-protocols](https://github.com/phicomm-io/iotcloud-community-protocols) | 跨仓库开发约定 |
| [phicomm_m1_mcu_atsamd20](https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20) | M1 传感器主控 |

按[构建依赖](docs/public/build-deps.md)准备 Linux 工具链与 SDK，然后执行：

```sh
python3 -m pip install -r requirements-build.txt
./build.sh                  # TC1、M1、A1
./DAIKIN/build.sh           # DAIKIN 普通版，需要 PlatformIO
```

单机型构建与产物说明见 [TC1](TC1/README.md)、[M1](M1/README.md)、[A1](A1/README.md)、[DAIKIN](DAIKIN/README.md)。前端由 `frontend.lock.json` 的完整提交和 SHA-256 固定，协议由 `protocols.lock.json` 固定；不要手改生成的网页或演示。

用户文档源在 `docs/public/`，站点布局源在 `site/`。文档检查运行 `python3 -m unittest discover -s tests -p test_site_docs.py`；固件改动按贡献指南运行对应测试与构建。

## 许可

本项目维护的代码采用 [GPL-3.0](LICENSE)。SDK 与第三方组件按各自原声明适用，保留原版权与许可，详见[第三方索引](THIRD_PARTY_NOTICES.md)。
