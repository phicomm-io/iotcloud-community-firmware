# 斐讯设备社区固件

为斐讯 TC1 智能排插、M1 空气检测仪和 A1 空气净化器提供本地网页控制、MQTT 与 Home Assistant 接入。

## 支持的设备

| 设备 | 主要功能 | 使用与构建 |
| --- | --- | --- |
| **TC1（A1 硬件版本）** | 六路插口与总开关、功率及电量统计、定时任务、童锁、按键设置、MQTT / HA、网页 OTA | [TC1](TC1/README.md) |
| **M1 空气检测仪** | 温度、湿度、PM2.5、甲醛读数，屏幕亮度与时间设置、MQTT / HA、与净化器绑定联动 | [M1](M1/README.md) |
| **A1 空气净化器** | 风机调速、触摸面板、RGB 灯带、童锁、定时与睡眠、多 M1 联动、MQTT / HA | [A1](A1/README.md) |

`tc1(a1)` 指 **TC1 的 A1 硬件版本**，不是 A1 空气净化器。TC1 A2 硬件不支持；M1 和 A1 空气净化器适用于 EMW3080B / MK3080B 原装 Wi-Fi 模块。

## 下载

固件版本由 Git tag 自动生成，例如 tag `v0.9.0` 对应设备显示 `0.9.0-abcdef`；这不表示所有源码构建均有对应 Release。请在 [Releases](https://github.com/FUjr/phicomm-community-firmware/releases) 中查看版本说明，下载与型号一致的文件：

| 设备 | 本社区固件网页升级文件 | 下载入口 |
| --- | --- | --- |
| TC1（A1 硬件版本） | `phicomm_tc1_a1.ota.bin` | [TC1 固件](https://github.com/FUjr/phicomm-community-firmware/releases) |
| M1 | `phicomm_m1.ota.bin` | [M1 固件](https://github.com/FUjr/phicomm-community-firmware/releases) |
| A1 空气净化器 | `phicomm_a1.ota.bin` | [A1 固件](https://github.com/FUjr/phicomm-community-firmware/releases) |

下载后按 Release 中的 `SHA256SUMS` 校验。`.all.bin` 为完整镜像，不能上传到网页 OTA，也不是通用首刷包。原厂或其他第三方固件的升级入口不保证接受上述 OTA 文件。

## 构建

在 Linux x86_64 上安装各型号构建说明中的系统依赖，然后执行：

```sh
./build.sh
```

根脚本依次构建 TC1、M1 和 A1，产物分别位于 `build/tc1-release/`、`build/m1-release/` 和 `build/a1-release/`。单独构建可运行对应目录内的 `build.sh`。工具链及 SDK 见 [构建依赖归档](https://github.com/FUjr/phicomm-community-firmware/releases/tag/0.0.0)。

## 文档

- [刷机与失败恢复](https://github.com/FUjr/phicomm-community-firmware/wiki/刷机)：支持的升级方式、文件选择和首次安装限制。
- 各型号的功能、构建和使用方法见上表。
- [M1 / A1 绑定与联动配置](common/README.md)。
- [MQTT 平台账号主题、配置迁移和验证方法](docs/mqtt-account.md)。

### 构建版本

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

## TMCP 平台

MQTT 服务与网页控制台位于 [platform/](platform/README.md)，与固件共同管理。账号迁移和回退流程见 [平台迁移文档](platform/docs/MIGRATION_TMCP.md)。
