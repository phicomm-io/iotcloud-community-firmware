# 斐讯设备社区固件

为斐讯 **TC1 智能排插、M1 空气检测仪和 A1 空气净化器**维护社区固件，让设备通过本地网页继续使用，也能接入 MQTT、Home Assistant 和远程管理平台。固件免费，无需原厂云服务；仅在局域网使用时，不需要部署平台。

**[项目主页与下载](https://phicomm-io.github.io/) · [使用文档](https://phicomm-io.github.io/#docs) · [在线演示](https://phicomm-io.github.io/#demos) · [发布说明](https://phicomm-io.github.io/guides/release-log.html)**

## 项目提供什么

- **本地控制**：手机或电脑浏览器直接访问设备，完成配网、日常操作与固件升级。
- **智能家居接入**：支持 MQTT 与 Home Assistant 自动发现，也可通过 TMCP 管理平台集中查看和控制设备。
- **设备联动**：M1 提供空气质量数据，A1 可按绑定的 M1 读数自动调整风机。
- **统一网页体验**：设备 WebUI、管理平台和在线演示由独立前端仓库维护；固件构建锁定指定前端 Release。

## 支持的设备

| 设备 | 主要功能 | 刷机教程 | 使用指南 |
| --- | --- | --- | --- |
| **TC1 智能排插（A1 硬件版本）** | 六路插口与总开关、功率和电量统计、定时、童锁、按键设置 | [J-Link 刷机](https://phicomm-io.github.io/guides/tc1-flash.html) | [TC1 操作](https://phicomm-io.github.io/guides/tc1-guide.html) |
| **M1 空气检测仪** | 温湿度、PM2.5、甲醛读数，屏幕亮度、时间设置及净化器联动 | [J-Link / Bootloader 刷机](https://phicomm-io.github.io/guides/a1-m1-flash.html) | [M1 操作](https://phicomm-io.github.io/guides/m1-guide.html) |
| **A1 空气净化器** | 风机调速、触摸面板、RGB 灯环、童锁、定时、夜间模式及多 M1 联动 | [J-Link / Bootloader 刷机](https://phicomm-io.github.io/guides/a1-m1-flash.html) | [A1 操作](https://phicomm-io.github.io/guides/a1-guide.html) |

**TC1 的 A1 硬件版本与 A1 空气净化器是两个产品，固件不能混用。** TC1 A2 硬件不支持；M1 和 A1 空气净化器适用于 EMW3080B / MK3080B 原装 Wi-Fi 模块。

## 开始使用

1. 阅读[刷机前准备](https://phicomm-io.github.io/guides/flash-prep.html)，确认型号、接线方式与供电要求。
2. 从[下载页](https://phicomm-io.github.io/#releases)选择对应产品的固件，按附带的 `SHA256SUMS` 校验，并依照上表教程刷入。
3. 按[快速开始](https://phicomm-io.github.io/guides/quick-start.html)连接设备热点、完成配网，再通过浏览器访问设备。
4. 按需配置 [MQTT、Home Assistant 与设备联动](https://phicomm-io.github.io/guides/mqtt-link.html)。后续升级见[固件更新](https://phicomm-io.github.io/guides/firmware-update.html)。

网页升级使用对应产品的 `.ota.bin` 签名包；`.all.bin` 用于完整镜像刷写，不能上传到网页 OTA。原厂或其他第三方固件不保证接受本项目 OTA 包，首次安装请遵循对应教程。

## 项目仓库

| 仓库 | 负责内容 |
| --- | --- |
| [phicomm-community-firmware](https://github.com/phicomm-io/phicomm-community-firmware)（本仓库） | TC1 / M1 / A1 固件、构建脚本与使用文档 |
| [phicomm-community-frontend](https://github.com/phicomm-io/phicomm-community-frontend) | 设备 WebUI、平台管理界面及在线演示的前端源码与 Release |
| [phicomm-community-mqtt](https://github.com/phicomm-io/phicomm-community-mqtt) | TMCP 平台服务、MQTT 接入及部署配置 |

## 构建与参与

在 Linux x86_64 上按[构建依赖](docs/public/build-deps.md)安装工具链与依赖后，执行：

```sh
python3 -m pip install -r requirements-build.txt
./build.sh
```

该脚本依次构建 TC1、M1 和 A1，产物分别位于 `build/tc1-release/`、`build/m1-release/` 和 `build/a1-release/`。单独构建见 [TC1](TC1/README.md)、[M1](M1/README.md)、[A1](A1/README.md)；工具链与 SDK 见[依赖归档](https://github.com/phicomm-io/phicomm-community-firmware/releases/tag/0.0.0)。

固件通过 `frontend.lock.json` 锁定前端 Release 的完整 commit 和 SHA-256，下载验证后嵌入对应页面，固件侧不需要 Node。离线准备可运行 `python3 tools/frontend_release.py --archive /path/frontend.tar.gz`，压缩包必须与锁文件匹配。接口约定见 [WebUI 契约](docs/webui-contract.md)。

文档源位于 `docs/public/`，目录由 `site/pages.py` 维护。修改后运行 `python3 -m unittest discover -s tests -p test_site_docs.py` 校验。设备绑定见 [M1 / A1 联动](common/README.md)，平台账号与迁移见 [MQTT 配置](docs/mqtt-account.md)。

<details>
<summary>构建版本规则</summary>

版本不依赖 `.version` 文件。推送 `vMAJOR.MINOR.PATCH` tag 会触发 CI 构建和发布；
发布构建校验该 tag 指向 HEAD 且源码干净。main、PR 和手动构建仍然保留。
普通构建使用 HEAD 历史中最近的合法版本 tag，拼接六位 commit ID；
存在已暂存、未暂存或未忽略的新增文件时追加 `-dirty`。
没有可达版本 tag 时基础版本为 `0.0.0`。非 Git 源码目录或 Git 不可用时，
固定使用 `0.0.0-000000-dirty`，不会使用父目录仓库的版本。

构建脚本自动生成 `common/generated/build_version.h` 和 `version.json`，
统一供设备状态、网页、MQTT、日志和 SDK 固件版本字段使用。
打包读取同一份版本快照，并将版本、完整 commit、tag 和 dirty 状态写入 manifest。
这些生成文件不提交；版本变化会更新头文件，以触发依赖它的源码重新编译。

</details>

