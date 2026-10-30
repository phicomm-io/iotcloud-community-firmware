# Phicomm workspace tooling

Use the shared toolchains and runtimes in `~/.cache/phicomm/`.
Read `~/.cache/phicomm/POLICY.md` before building or preparing an experiment.
Do not install another toolchain/Python/MiCoder copy inside this workspace.
This checkout retains its original GCC5 compiler gate: use
`source ~/.cache/phicomm/env.sh gcc5`. Use GCC14 only in a checkout whose
build scripts explicitly support it. Preserve the selected compiler/runtime
distinction and put compiler logs in the current run.

测试工作树用完必须及时回收：测试结束先保留独有补丁、必要固件、校验值和结果，
随后使用 Git 移除临时工作树并核对登记；有活跃任务或外部依赖时记录保留原因。
不要积累已结束的测试目录，也不要在工作树中重复安装工具链。

# 统一设备开发规范

- 新设备应复用 `phicomm-community-frontend` 的公共样式、页面布局与交互约定，提供同一套首页和设置导航；不另做独立风格的控制页，也不把框架原生管理页作为常规设置流程。允许适配制造商、型号和硬件能力；不显示没有实现的功能。公共前端资源必须记录来源 commit 和 SHA-256。
- HTTP 与网页鉴权遵循已有设备：未设置管理密码时直接进入页面、允许本地 HTTP 访问，不显示密码框、不消耗登录会话；设置密码后才要求鉴权。支持项目已有的 `admin` Basic 访问；使用会话的前端应复用有效会话，正确处理刷新、过期、退出和密码变更，不能因空密码或残留会话反复要求登录。此策略不取消 UDP 配对或 MQTT broker/ACL 鉴权。
- 固件版本使用 `tools/check_version.py` 的仓库规则：最近的 `vMAJOR.MINOR.PATCH` 标签、当前提交前六位及必要的 `-dirty`。发布标签构建必须验证标签指向 HEAD、源码干净。禁止另设产品专用语义版本或伪造干净版本。
- 新设备业务的 HTTP、MQTT、UDP 按项目 `openiot` 协议和 `phicomm-messagepack-lab` 的二进制契约实现，使用 MessagePack/CBOR；JSON 仅用于 Home Assistant，不新增旧 JSON 兼容层。已有设备协议的迁移需遵守对应任务授权。
- 对齐公共工程应直接复用公共 HTTPD、MQTT owner/session/queue、UDP 包解析及完整公共前端，再提供芯片 HAL/RTOS 和产品业务适配；不能只复制样式或另写一套近似服务。固定来源快照、记录适配补丁与文件校验值，并通过真实套接字和浏览器验证。
- 设备身份必须读取该板实际工厂参数分区的有效 MAC；校验地址和 TLV 边界，升级时修复旧版保存的默认地址。禁止硬编码某台设备的 MAC，禁止用尾部全零的 SDK 默认地址生成设备 ID、热点或 MQTT 主题。
- 对外声明的项目名为 `iotcloud`，协议名为 `openiot`。制造商、设备型号、模块和芯片使用真实名称；不能把模块或芯片型号当成整机型号。内部 TMCP 命令封装可单独标注，避免混淆项目协议名。
- 配网热点使用产品约定的型号前缀和 MAC 后六位大写十六进制，默认管理地址、网关为 `192.168.0.1`。本次 S30-V1B 按用户明确指定保留热点前缀 `S30M_V1B_`；整机型号仍是 `S30-V1B`，模块 `BL2028N`，芯片 `BK7231N`，计量 `HLW8032`。
- DHCP hostname 使用项目名、真实型号及有效原厂 MAC 后六位，只含合法主机名字符；本次 S30-V1B 为 `iotcloud-s30-v1b-<后六位小写>`。升级必须修复旧框架默认名称，不能继续对外广播 OpenBK 默认身份。灯效按产品确认的电源、异常与配网含义实现并记录优先级；本次蓝灯表示插座输出，红灯表示 Wi-Fi/已配置 MQTT 异常，未配网红蓝交替。
- 默认重置手势：按住至少 5 秒、不足 10 秒后松开，只清除 Wi-Fi（含备用网络），保留产品设置、MQTT、管理密码、配对和累计值；按住至少 10 秒后松开，恢复设备出厂设置，清除网络、产品设置、认证/配对和累计值。到达阈值给出不同灯效提示，松手执行，不能在 5 秒时先行重启而阻断 10 秒手势。恢复手势不受童锁屏蔽，消抖、边界和长按不误触发短按应验证。
- 本设备仅使用 OpenBeken 的运行框架和 BK7231N SDK/HAL/RTOS；不混入 MiCO、ESPHome、Arduino 运行框架。所有芯片、固件和运行验证需明确对应到实际设备。
