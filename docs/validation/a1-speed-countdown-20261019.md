# A1 灯效速度与关闭倒计时

日期：2026-10-19。状态：本地实现、主机回归、浏览器验证及 A1 构建 PASS；公开发布、物理 A1 验证 NOT_RUN。

## 功能

- 呼吸与彩虹跑马灯支持 1–10 档变化速度，默认 5，速度为原来的 0.2–2 倍。保存后持久化；旧配置的保留字节为零时继续使用原速度。页面预览同步使用所选速度。
- 定时任务新增“倒计时关闭”，网页可设 1 分钟至 24 小时，接口支持 1–86400 整数秒。仅执行 `fan.set` / `{on:false}`，适用于手动及自动模式。
- 使用设备 uptime，关闭网页、校时前后或校时跳变均不改变计时。支持查看剩余时间、取消、删除、编辑和重新启用；编辑/重新启用从完整时长重新计时。
- 重启取消活动倒计时，标记 `cancelled`。到期先保存停用再执行，设备忙则标记 `busy`，不重复执行。与每周/指定日期任务共用原 8 个槽位。
- 保持 Flash 大小、既有字段偏移及旧任务语义不变。旧固件不展示新增控件。

## 验证

- `SANITIZE=1 bash tests/run.sh`：PASS。包含 A1/TC1 自动任务、三产品配置迁移、A1/M1 runtime，以及 ASan/UBSan 检查。新增覆盖校时独立性、时钟跳变、uptime 回绕、并行任务、编辑重置、取消/重启、存储失败、忙状态、设备重启取消及参数边界。
- 前端 `npm test --prefix frontend`：32 项 PASS；设备三产品 `tests/test_web.cjs`、语法检查及 Python 测试 PASS；灯效保存、倒计时、自动模式和滤芯专项回归 PASS。
- `tests/test_a1_effect_countdown_browser.cjs`：Chromium / WebKit，1280 / 390 px，四组 PASS。验证速度保存、创建倒计时、取消、重新计时、演示到期停机及横向溢出。修复任务列表手机布局。WebKit 使用本机缓存浏览器与临时解包依赖启动，启动适配仅位于忽略的 `build/local-webkit.cjs`，未更改系统或缓存浏览器文件。
- 三产品及平台演示 `tests/test_demos.cjs`：PASS。
- 中英文文档生成与翻译 hash 校验：15 项 PASS。
- `bash A1/build.sh`：PASS，包含固件尺寸、JSON 浮点链接及线程栈预算检查。

日志保存在本地 `build/evidence/a1-speed-countdown/`。浏览器截图位于前端仓库 `build/a1-countdown-{chromium,webkit}-{1280,390}.png`。

## 构建与发布边界

前端本地分支 `feat/a1-effect-speed-countdown`，commit `a35da8fb43ca906950e3f481df1d60866d8d05e0`。完整前端构建包 SHA-256：`2b346bc4ad24c7d3acdfeabf5b7d230fdbed392712f3f7874248cc7ede1cdebf`。固件锁文件已同步，并用本地归档通过校验后嵌入。

A1 本地候选产物：`build/a1-release/phicomm_a1.ota.bin`，709188 bytes，SHA-256 `5374d79ab2d7b090894be691e455898921e600aa4b53c2c5a313c71e7255a8e2`。构建基于固件 `193652f` 加本次改动，标记 `0.9.11-193652-dirty`，完整信息见同目录 `manifest.json`。历史构建备份位于验证目录的 `previous-release/`。

上述为首次本地验证时的候选构建记录，并非正式 Release 产物。正式发布在最新主分支整合后，由前端 CI 发布不可变归档，再将公开归档的完整 commit 和 SHA-256 写入固件锁文件。固件由 tag CI 重新构建并签名；以正式 Release 的校验和为准。

未升级、重启或操作物理设备；主机及演示中的停机测试不代表真实风机或光学效果验证。未部署 IotCloud 节点或公开文档。
