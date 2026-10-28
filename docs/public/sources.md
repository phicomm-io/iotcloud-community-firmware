# 资源来源

刷机教程中的图片与工具保存在本站 `guides/assets/` 目录。[SHA256SUMS](assets/SHA256SUMS) 包含图片与安装包校验和。

| 文件 | 来源 |
| --- | --- |
| `images/tc1-ota-*.png`、`images/tc1-stock-*.png`、`images/tc1-community-ap.png` | 用户提供的 TC1 免拆刷机操作截图，裁剪保留路由器、网卡、TFTP、原厂识别及首启热点的关键区域 |
| `tools/tc1-no-disassembly/mico_ota.bin` | 与免拆主教程相同的已验证镜像快照（原始镜像 SHA-256：`005b5c4d8a1bb4e70b1b6aa84f1c403e648d3c2d4d683a067a22193543050735`），追加其 16 字节二进制 MD5；固定教程文件，不随最新 Release 自动替换 |
| `tools/tc1-no-disassembly/tftp-trigger.bin` | 按免拆主教程生成的 397 字节触发文件，仅用于 TC1 A1 原厂 1.0.0.26，不是通用升级包 |
| `images/emw3080-pins.png` | [庆科官网 EMW3080 引脚原图](https://docs.mxchip.com/img/zh/1631842737419-34af0113-6807-499a-9ff9-129040293369.png)，保留原图内容，透明背景转为白色 |
| `images/emw3080-swd-pins.png` | [庆科 EMW3080 官方烧录说明](https://docs.mxchip.com/zh/foiwqd8gbgh23xu2/bwyo0fs5t2rquekz/antua5wkt8m3qx7l/lqdz2h5zpp117imt.html)中的 SWD 模块引脚原图 |
| `images/tc1-a1-module.png`、`images/tc1-a1-test-points.png` | 用户提供的 TC1 A1 模组与工厂测试点标注图 |
| `images/a1-jlink-points.png` | 用户提供的 A1 空气净化器新版 J-Link 接线标注图（DIO 上、CLK 下） |
| `images/m1-scd40-wiring.png` | 用户提供的 M1 外接 SCD40 实物接线图，手写 7 / 8 为 GPIO 信号名 |
| `tools/emw3080-openocd.zip`、`tools/emw3080-openocd/` | 本项目的 Python 3 校验、切分、烧录脚本与新版 MX1290 配置；不含第三方二进制与固件。烧录仍需 [zA1 原工具包](https://github.com/a2633063/zA1/blob/f4f67ee88d9287fd1badff55ccd7704bb0f748c4/Jlink烧录文档/3080_JLINK烧录.zip)内配套的编程器及脚本 |
| `images/jlink-swd.png` | [SEGGER 官方 SWD 接口图](https://www.segger.com/fileadmin/images/products/J-Link/Technology/Interface_Description/181129_SWD.svg)，由 SVG 转为白色背景 PNG |
| `images/flashplus-config.png` | [EMW3031 / FlashPlus 工具说明 PDF](https://github.com/IoTDevice/phicomm_tc1_a1/blob/master/工具和文档/EMW3031说明文档_flashplus.pdf)，第 2 页配置区域裁剪；仅用于识别芯片与 Flash 配置，擦除及校验选项以本项目教程为准 |
| `tools/FlashPlus_v1.0.10.msi` | Marvell FlashPlus v1.0.10，[IoTDevice 工具镜像](https://github.com/IoTDevice/phicomm_tc1_a1/blob/master/工具和文档/FlashPlus_v1.0.10.msi)，安装包保持原样 |
| `tools/a1-easylink.zip` | 本项目的 A1 EasyLink 配网工具，包含 `tools/a1_easylink.py`、`tools/a1_easylink_web.py` 和三个网页资源；无固件文件，工具包校验和见清单 |
| `tools/easylink-linux/easylink_v2_linux.c` | 本项目 Linux mac80211 帧注入工具，源自 M1 实机成功使用的 EasyLink V2 发送程序；A1 使用相同协议。普通 UDP 工具仍为实验用途 |
| `tools/a1-dns-migration/a1_dns_migration.py` | 依据 A1 原厂 v1.47 的更新域名、HTTPS 请求、顶层 JSON、版本比较和下载校验逻辑实现；DNS 迁移尚待实机验证 |

图片与第三方工具的版权属于原作者及供应商，保存副本不改变原有授权；FlashPlus 不属于本项目固件源码。**EMW3031 数据手册明确限制配图转载，因此教程仅链接原文。**

## 操作注意事项来源

A1 滤芯安装与上电联锁的说明来自 [zA1 Issue #3](https://github.com/a2633063/zA1/issues/3)：用户反馈未安装滤芯时无法通电，原项目维护者确认已将提醒加入文档。

构建所需的工具链与 SDK 见[构建依赖](build-deps)。

## DAIKIN 与读数参考

DAIKIN 引脚与 PM2005 / PM2105 协议参考 [louliangsheng/daikin-air-sensor](https://github.com/louliangsheng/daikin-air-sensor)。PM2.5 与 PM10 来自同一帧的不同字段。TVOC 使用 SGP30 原生 ppb 输出，≤300 ppb 是沿用本设备指示灯阈值的产品通风参考；参见 [Sensirion SGP30 数据手册](https://sensirion.com/file/datasheet_sgp30/)与[MOX 输出解释](https://sensirion.com/resource/application_note/output_mox_sensor)，不将该参考作为健康合格限值。颗粒物显示分段参考 [HJ 633—2026 表 3](https://www.mee.gov.cn/ywgz/fgbz/bz/bzwb/jcffbz/202602/t20260225_1144441.shtml)，瞬时值并非按标准计算的 AQI。
