# 设备 WebUI 压缩：构建与三台实机验证（2026-10-19）

压缩空间、签名升级、配置保留和页面功能检查通过；未观察到压缩新增的布局回归。并行状态读取窗口保留 FAIL，既有手机布局问题未修复；隔离读取复测见下文。

## 范围与可复现来源

仅优化构建侧内联 JS/CSS 和 gzip 编码，保留全部页面、功能、中英文、脚本顺序和全局名称。无设备内容裁剪，无局域网协议原型，无服务端压缩器。设备依旧直接发送 gzip 字节。

两个仓库均从本次刷新后的 `origin/main` 创建并推送 `perf/device-web-compression`：

- 固件基线：`4b34678e4f6c0a2a4ac43a10e78ee9a1e05d8099`。
- 固件构建提交：`ae2f59e847af2a4f94d7e3885b09d41f8c5fdc8c`，clean，运行版本 `0.9.13-ae2f59`。本验收文档在构建之后单独提交。
- 前端基线：`42a87f152e71153398fc29c0fff991e7144a5308`。
- 前端构建提交：`69e124321020dbccbb9138f159a7420316340937`，clean。
- 工具：esbuild `0.28.2`，Python zopfli `0.2.3.post1`，15 iterations；ARM GCC `5.4.1`，MiCO `6c465211d3ff8797cd835e400ec54a06530dd476`。
- 前端归档 SHA-256：`018a658ef4a14e5b7cb00f9e26e65bf12ca62d8ddd600fb42d4b8cb3fb4c9783`。

实机验收时尚未合并分支、未发布生产 Release；当时的前端归档保存在本地，固件使用相同完整 commit、SHA-256 与 manifest 进行离线校验后构建，未绕过锁文件验证。下方命令用于复现实机验收提交；后续 main 集成状态见文末。

```sh
# frontend worktree
python3 -m venv build/venv
build/venv/bin/pip install -r requirements-build.txt
npm ci --prefix frontend
build/venv/bin/python tools/build_release.py
# firmware worktree (after obtaining the matching archive)
python3 tools/frontend_release.py --archive ../phicomm-web-compression-frontend/build/frontend.tar.gz
bash A1/build.sh
bash M1/build.sh
bash TC1/build.sh
```

三款构建均执行了各自镜像/栈/JSON 浮点等脚本要求的检查。基线与候选均在本独立 worktree 构建；原工作区的未提交内容未引入。

## 实际固件空间

`.usr.bin` 大小，单位为字节。链接对齐使固件节省量略小于 gzip 字节差。

| 机型 | main 基线 | 压缩候选 | Flash 节省 | 单次签名上传响应 |
|---|---:|---:|---:|---:|
| A1 | 232,644 | 224,484 | 8,160 | 202 |
| M1 | 198,836 | 191,668 | 7,168 | 202 |
| TC1 | 245,120 | 238,272 | 6,848 | 202 |

对应页面 gzip 为 A1 `59,765 → 51,593 B`、M1 `52,944 → 45,765 B`、TC1 `52,419 → 45,548 B`。ELF `.data`/`.bss` 与基线逐款一致：A1 `316/38,652 B`、M1 `316/27,796 B`、TC1 `9,048/13,992 B`。压缩器只运行在构建宿主上。

## 自动回归与实机证明

- PASS：前端 Python 7 项、平台 Node 32 项、原始及压缩设备页面共用的功能回归（统一 API、导航、表单、自动控制、滤芯、灯效、倒计时、M1 CO2）。
- PASS：前端完整 bundle、设备/平台 demo；Chromium/WebKit 的 A1 灯效保存及倒计时创建/取消/重启/到期，三机型桌面/手机交互布局回归。
- PASS：固件 `bash tests/run.sh`、`bash tests/run_tc1.sh`。
- PASS：三个签名包均通过型号、payload SHA-256、提交、版本和可信公钥校验；每款仅上传一次，均返回 HTTP 202。
- PASS：升级前后真实 `device_id` 一致、运行版本匹配、uptime 回退证明重启、Wi-Fi/MQTT 恢复。A1 面板/风机、M1 UART/传感器恢复；config.get、绑定、定时、按键以及适用的联动配置投影比较通过。
- PASS：TC1 六路输出升级前后均为 `[1,1,1,1,1,1]`；没有发送插座控制命令。
- PASS：从三台实际 HTTP 服务取得的 gzip，与锁定前端发布候选字节完全一致，解压后与 HTML 相同。
- PASS：实机 Chromium/WebKit，1280px 英文与 390px 中文，首页和所有已展示设置路由均完成加载；未出现 JavaScript 异常、API HTTP 错误或意外写命令。

本地浏览器环境使用已存在的隔离 WebKit 动态库与 WPE 启动脚本。最终归档、构建和上述测试均使用已提交的源码；宿主依赖/脚本修复及构建过程的失败尝试保留在本地日志中。

## 保留的失败与边界

1. **并行读取窗口 FAIL**：120.47 秒内，A1 19/25 次状态读取成功、6 次 4 秒超时；M1、TC1 各 25/25 成功。成功样本中 uptime 连续递增，未观察到意外重启。该窗口与多个 A1 浏览器/基线对比请求重叠；未取得未压缩固件在同等流量下的对照，故不能将超时归因于压缩或宣布并发稳定性通过。
2. **既有手机布局问题**：初次 A1 390px 页面溢出检查失败。后续采用同一组冻结的实机只读 API 响应、相同引擎/语言/视口/导航顺序，对 main 原始 HTML 与压缩 HTML 作受控比较：无新增布局回归；WebKit 的绑定页面 A1 `486px`、M1 `413px`，固件页面三款均 `411px`，两版一致。前次仅比较不同导航顺序/时刻的宽度曾产生假回归（539 vs 486），该结果保留并由受控对照澄清。上述既有溢出未在本压缩分支修复。
3. 不包含长时间稳定性、物理按键、光学显示、TC1 带载计量、断电中断、远端 CI 或生产 Release 验收。

隔离状态读取复测：**PASS**。停止浏览器流量后，120.88 秒内三台各 25/25、合计 75/75 次严格读取通过，4 秒超时且无自动重试；身份、版本、Wi-Fi/MQTT、各机型外设、uptime 连续性及 TC1 六路状态均通过。证据位于 `build/compression-device-evidence/isolated/`。该结果不覆盖首次并行窗口 FAIL，也不证明长期/并发稳定性。

## 产物与验证范围

- 三机产物、manifest、SHA256SUMS、签名包：`build/{a1,m1,tc1}-release/`；未压缩对照：`build/compression-baseline/`。
- 前端归档：前端 checkout 的 `build/frontend.tar.gz`、`build/frontend.lock.json`。
- 受控布局比较记录每条路由的宽度及分类；配置、设备快照和浏览器截图保留于私有验收材料，不随源码发布。

上述结果对应本文列出的构建提交与归档校验值；正式发布状态以公开 Release 和 CI 为准。

## main 集成补充（2026-10-19）

前端 [PR #11](https://github.com/phicomm-io/phicomm-community-frontend/pull/11) 已合并，main 提交 `c557fea8d70dbf517da26156d09c52b078a53c46` 与实测前端提交的文件树完全一致。[main CI](https://github.com/phicomm-io/phicomm-community-frontend/actions/runs/39198294488) 全部通过，并自动发布按提交寻址的前端构建包。

固件锁文件改为该 [main 前端构建包](https://github.com/phicomm-io/phicomm-community-frontend/releases/tag/frontend-c557fea8d70dbf517da26156d09c52b078a53c46)，归档 SHA-256 为 `8c1a21df918d7cbda16d911e1becafae9da81913f657d9a69fd4a441002e8fae`。发布包已通过完整锁文件/manifest/逐文件校验，三款 `devices/*/index.html` 及 `index.html.gz` 均与上述已实测资源逐字节一致。归档的提交元数据因此更新；设备页面内容与 Flash 节省量不变。

此锁文件支持直接在线获取公开前端构建包，无需本地验收归档。不发布新的生产固件版本，不重复升级实机；三台实机仍对应上文的 `ae2f59e` 构建，保留的并行超时和手机溢出结论不变。
