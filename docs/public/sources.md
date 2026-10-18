# 资源来源

刷机教程中的图片与工具保存在本站 `guides/assets/` 目录。[SHA256SUMS](assets/SHA256SUMS) 包含图片与安装包校验和。

| 文件 | 来源 |
| --- | --- |
| `images/emw3080-pins.png` | [庆科官网 EMW3080 引脚原图](https://docs.mxchip.com/img/zh/1631842737419-34af0113-6807-499a-9ff9-129040293369.png)，保留原图内容，透明背景转为白色 |
| `images/emw3080-swd-pins.png` | [庆科 EMW3080 官方烧录说明](https://docs.mxchip.com/zh/foiwqd8gbgh23xu2/bwyo0fs5t2rquekz/antua5wkt8m3qx7l/lqdz2h5zpp117imt.html)中的 SWD 模块引脚原图 |
| `images/tc1-a1-module.png`、`images/tc1-a1-test-points.png` | 用户提供的 TC1 A1 模组与工厂测试点标注图 |
| `images/a1-jlink-points.png` | 用户提供的 A1 空气净化器新版 J-Link 接线标注图（DIO 上、CLK 下） |
| `images/m1-scd40-wiring.png` | 用户提供的 M1 外接 SCD40 实物接线图，手写 7 / 8 为 GPIO 信号名 |
| `tools/emw3080-openocd.zip`、`tools/emw3080-openocd/` | 本项目的 Python 3 校验、切分、烧录脚本与新版 MX1290 配置；不含第三方二进制与固件。烧录仍需 [zA1 原工具包](https://github.com/a2633063/zA1/blob/f4f67ee88d9287fd1badff55ccd7704bb0f748c4/Jlink烧录文档/3080_JLINK烧录.zip)内配套的编程器及脚本 |
| `images/jlink-swd.png` | [SEGGER 官方 SWD 接口图](https://www.segger.com/fileadmin/images/products/J-Link/Technology/Interface_Description/181129_SWD.svg)，由 SVG 转为白色背景 PNG |
| `images/flashplus-config.png` | [EMW3031 / FlashPlus 工具说明 PDF](https://github.com/IoTDevice/phicomm_tc1_a1/blob/master/工具和文档/EMW3031说明文档_flashplus.pdf)，第 2 页配置区域裁剪；仅用于识别芯片与 Flash 配置，擦除及校验选项以本项目教程为准 |
| `tools/FlashPlus_v1.0.10.msi` | Marvell FlashPlus v1.0.10，[IoTDevice 工具镜像](https://github.com/IoTDevice/phicomm_tc1_a1/blob/master/工具和文档/FlashPlus_v1.0.10.msi)，安装包保持原样 |

图片与第三方工具的版权属于原作者及供应商，保存副本不改变原有授权；FlashPlus 不属于本项目固件源码。**EMW3031 数据手册明确限制配图转载，因此教程仅链接原文。**

构建所需的工具链与 SDK 见[构建依赖](build-deps)。
