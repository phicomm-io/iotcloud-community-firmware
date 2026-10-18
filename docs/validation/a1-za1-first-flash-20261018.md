# A1 使用 zA1 工具首刷后无反应、无热点排查

日期：2026-10-18。用户确认：原厂固件直接刷入社区 `phicomm_a1.all.bin` 后无反应、无热点；尚未提供烧录 `log.txt`、Flash 回读或上电串口日志。

## 结论与证据边界

**确认存在首刷兼容缺陷**：保留的 MiCO 参数若 CRC 和系统标识有效，但其中应用数据不是社区 A1 布局，SDK 会照常加载，社区应用随后返回 `unsupported_layout`。此时尚未启动网络、HTTP、硬件和按键线程，能够解释无反应、无热点。主机测试已复现该退出路径，并验证修复。

**用户设备的最终根因尚未确认**：没有该机参数备份或启动日志，不能证明原厂参数恰好满足上述条件；烧录失败、供电和其他启动故障仍待设备证据区分。未连接、擦除或刷写任何实机。

## 教程、工具与镜像核对

- [zA1 固件烧录教程](https://github.com/a2633063/zA1/wiki/固件烧录) 的 J-Link 方法要求把完整镜像重命名为 `all.bin` 后先分割，再烧录。
- 实际下载并检查其 `Jlink烧录文档/3080_JLINK烧录.zip`，核对 `split.sh`、Windows 批处理、`sflash_write.tcl` 和 **`flash_prog.elf` 内的分区表**。
- 分割：Boot 为前 72 KiB；MiCO 从 76 KiB 开始取 724 KiB；ATE 从 832 KiB 开始最多取 256 KiB。
- ELF 内对应写入区域为 Boot `0x0..0x13000`、MiCO `0x13000..0xc8000`、ATE `0xd0000..0x110000`。批处理仅写分区 1/2/3，**保留 `0xc8000..0xd0000` 的两份参数**。ELF 的写入完成处理不会额外清空参数。
- v0.9.10 完整镜像共 1,077,968 字节，SHA256 为 `c5d58330efd82770d9a3bb71e046568c49ca122b60cc5ce32daaa3ec6c143727`，与 GitHub Release asset digest 一致。
- 从完整镜像中按应用头长度提取 OTA：703,428 字节，SHA256 为 `bc547724135ead2a943140ad0516f60217bfc6de481205da36e6b14408fe1048`，与发布的 `.ota.bin.raw` 一致；`A1/tools/check_image.py` 的内核哈希、应用 CRC、产品标识、ABI 和 MD5 检查通过。
- v0.9.10 应用结束位置 `0xbebb4`，小于 MiCO 分区末尾 `0xc8000`，未被旧分割脚本截断。
- 对本机已有的 zA1 v0.3.1 完整镜像逐字节比较：Boot、内核及 ATE 均与社区 v0.9.10 一致。没有证据支持这些公共启动组件或分割偏移不兼容。
- zA1 的教程要求搜索 `zA1_XXXX`；社区固件的配网热点为 `A1-AP-<MAC后六位>`。

工作区根目录同名 `phicomm_a1.all.bin` 实际为下载日志文本；本次核对的下载镜像是 `index.html?fname=phicomm_a1.all.bin`，其哈希与发布件一致。没有改动这些原始文件。

## 代码路径与修复

SDK 的 `mico_system_para_storage.c:MICOReadConfiguration()` 检查 MiCO 参数分区 CRC 和 `micoSystemConfig.magic_number`，不会检查应用自定义的 `A1_CONFIG_MAGIC`。CRC 有效的其他固件参数不会自动触发 `appRestoreDefault_callback()`。

原 `A1/main.c:application_start()` 将应用标识不匹配与社区配置版本不支持合并为同一个错误，直接返回。已核对当前工作区与 `v0.9.10` 标签均有此逻辑。

修复分开处理两种情况：

1. 应用标识不匹配：清空应用存储结构、生成社区默认值，并通过 SDK 保存。记录 `foreign_layout_reset` 后继续启动，需要重新配网。
2. 应用标识属于社区，但版本不支持：继续拒绝启动并保留原配置，避免降级时误清空未来版本数据。

只初始化应用数据，不调用整片擦除或 SDK 系统恢复。保存失败仍停止启动；v1–v6 迁移继续保留现有 Wi-Fi、名称、滤芯和任务数据。

## 验证

- PASS：新增测试先在旧代码上复现 `migrate()==0` 断言失败。
- PASS：修复后的生产配置代码测试，含其他固件数据、保存失败、再次启动不重复保存、不支持的社区版本原样保留及原有迁移；ASan/UBSan 通过。
- PASS：当前工作区改动复制到独立 `e9cda6e` worktree 后，`tests/run.sh` 完整通过。
- PASS：独立 worktree 的 A1 ARM GCC 5.4.1 构建、按钮/业务线程栈检查、镜像检查和浮点 JSON 链接检查通过。
- PASS：相同修复移植到独立 `v0.9.10` worktree 后，配置回归测试及 ASan/UBSan 通过。
- PASS：`v0.9.10` 修复件 A1 编译、栈检查、镜像和浮点 JSON 检查通过；修复逻辑与工作区逐字一致，Boot、内核及 ATE 未更换。
- NOT_RUN：实机首刷、原厂参数回读、冷启动串口和配网验收。
- 额外执行 `test_a1_*.py` 通配发现时，两个 MQTT 集成测试因系统 Python 缺少 `paho` 导入失败；其他三个测试通过。它们不属于此次已通过的 `tests/run.sh` 配置回归，不记作 MQTT 验证通过。

构建、工具解包、日志和两个独立 worktree 保存在 `/tmp/a1-boot-investigation-20261018/`。正式发布件未替换，未创建固件发布。

测试包：`build/a1-first-flash-fix-20261018/a1-first-flash-fix-v0.9.10-test.zip`。构建版本 `0.9.10-3592fc-dirty`，完整镜像仍为 1,077,968 字节，SHA256 为 `bb0b60a5f3adc973d21e13a44733cabf24ca9ef0bffec264e90bea1f16442601`。包内包含镜像、校验和、构建 manifest 与操作说明；属于未发布、未实机验收的测试件。

## 用户设备的下一步

优先取得烧录目录中的 `log.txt`，核对三个分区完整写入和校验结果。需要实机复现时，先保存该机完整 Flash（包含两份参数），再使用测试镜像验证原参数条件下能否冷启动并出现社区配网热点。修复镜像预期会重置非社区的应用配置；如仍无热点，则收集上电日志和镜像范围回读，不以整片擦除掩盖原因。
