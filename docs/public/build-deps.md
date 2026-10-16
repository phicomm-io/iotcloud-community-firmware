# 构建依赖

本页说明如何获取工具链与 SDK，以及如何用本地副本构建三款固件。只使用预编译固件的用户不需要这一页，请直接[下载固件]({{SITE_URL}}#releases)。

在 [0.0.0 构建依赖归档](https://github.com/phicomm-io/phicomm-community-firmware/releases/tag/0.0.0) 下载工具链和 SDK，并使用随包 `SHA256SUMS` 校验：

```sh
sha256sum -c SHA256SUMS
```

## 依赖文件

| 文件 | 用途与来源 |
| --- | --- |
| [gcc-arm-none-eabi-5_4-2016q2-20160622-linux.tar.bz2](https://github.com/phicomm-io/phicomm-community-firmware/releases/download/0.0.0/gcc-arm-none-eabi-5_4-2016q2-20160622-linux.tar.bz2) | 三型号使用的 Linux x86_64 ARM GCC 5.4.1，[原下载页](https://launchpad.net/gcc-arm-embedded/5.0/5-2016-q2-update) |
| [gcc-arm-none-eabi-5_4-2016q2-20160622-src.tar.bz2](https://github.com/phicomm-io/phicomm-community-firmware/releases/download/0.0.0/gcc-arm-none-eabi-5_4-2016q2-20160622-src.tar.bz2) | 同版本工具链对应源码及构建脚本，保留原许可证 |
| [Python-2.7.18.tgz](https://github.com/phicomm-io/phicomm-community-firmware/releases/download/0.0.0/Python-2.7.18.tgz) | 旧 SDK 构建所需 Python 2.7.18 源码，[Python 官方下载](https://www.python.org/ftp/python/2.7.18/Python-2.7.18.tgz) |
| [mico-os-3.6.4.bundle](https://github.com/phicomm-io/phicomm-community-firmware/releases/download/0.0.0/mico-os-3.6.4.bundle) | 三型号共用的 SDK Git 副本，[MXCHIP/mico-os](https://github.com/MXCHIP/mico-os/tree/6c465211d3ff8797cd835e400ec54a06530dd476)，固定提交 `6c465211d3ff8797cd835e400ec54a06530dd476`，不含本项目补丁 |
| [tc1-mico-sdk.tar.gz](https://github.com/phicomm-io/phicomm-community-firmware/releases/download/0.0.0/tc1-mico-sdk.tar.gz) | 历史 TC1 MiCO 3.5.4 归档，仅用于构建旧版本；当前版本统一使用上面的 MiCO 3.6.4 |

工具链压缩包保留随包许可证；GCC 等组件的对应源码同时提供。Python 使用随源码的 PSF 许可证。MiCO SDK 包含多个不同授权的源码及二进制组件，按原文件声明适用，不采用本项目统一授权替代。原版权与许可证保留在归档内。

## 准备本地副本

创建有写权限的共享工具目录，将 GCC 压缩包解压到该目录，例如 `/opt/phicomm-tools`；将 Python 源码解压、配置并安装到该目录的 `python2/`：

```sh
tar -xjf gcc-arm-none-eabi-5_4-2016q2-20160622-linux.tar.bz2 -C /opt/phicomm-tools
tar -xzf Python-2.7.18.tgz
cd Python-2.7.18
./configure --prefix=/opt/phicomm-tools/python2
make -j4
make install
```

三型号的 SDK 来源和固定提交统一由仓库的 `common/mico-sdk.json` 定义。默认从上游下载一次，保存在 `build/mico-upstream.git`，再为各型号创建独立 checkout 并应用板级补丁。A1/M1 的 GPIO 初始化不同，不能共用已打补丁的工作目录。

使用本地 Git bundle 构建时，指定来源即可，无需分别下载：

```sh
export MICO_SDK_REPOSITORY=/下载路径/mico-os-3.6.4.bundle
./build.sh
```

`MICO_SDK_CACHE` 可以指定公共源缓存的绝对路径。每个 checkout 都会校验提交为 `6c465211d3ff8797cd835e400ec54a06530dd476`；来源不符或目录不完整会终止构建。默认 checkout 为 `build/tc1-sdk-3.6.4/mico-os`、`build/m1-sdk/mico-os` 和 `build/a1-sdk/mico-os`，也可以通过各型号的 `*_SDK_DIR` 指定同一提交的独立 Git checkout。

## 构建

在项目根目录运行：

```sh
TC1_TOOLS_DIR=/opt/phicomm-tools ./TC1/build.sh
M1_TOOLS_DIR=/opt/phicomm-tools ./M1/build.sh
A1_TOOLS_DIR=/opt/phicomm-tools ./A1/build.sh
```

脚本会应用仓库内各产品的 SDK 补丁。**工具目录已有正确版本的 GCC 与 Python 时，不再下载它们。**

仍需 Linux 系统的 `make`、Git、Perl、Python 3、bzip2、curl、C 编译环境及相关系统库；安装命令见[项目构建说明](https://github.com/phicomm-io/phicomm-community-firmware#readme)。

当前版本不再下载 TC1 的 3.5.4 归档。在项目根目录执行 `./build.sh` 可依次构建三个型号。
