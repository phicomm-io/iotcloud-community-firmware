# 验证记录（2026-10-13）

实现依据为 phicomm-community-firmware 的源码提交 `e6c659d2df6ab0b9de53ec55a5e5d2cafa9e90a5`，以及 Mosquitto 2.0.22 官方 Dynamic Security 实现。

- `go test -race ./...`、`go vet ./...`：设备归属、并发独占绑定、失败后待同步与恢复、解绑再绑定、会话与 CSRF、管理员权限隔离、改密撤销会话、迁移输入与初始化保护。
- `npm test`：旧设备列表格式、MAC 规范化与去重、TC1 v2 命令大写、M1 新鲜度与零值、A1 灯光载荷、模型与主题隔离。
- `npm run build` 和两个 Compose 镜像构建通过；后端 Linux amd64 与 arm64 静态编译通过。
- `scripts/test-integration.sh`：实际 Mosquitto TLS 登录、错误密码拒绝、设备主题授权、其他账号拒绝、管理主题拒绝、非保留命令收发、解绑撤权、重新绑定、改密；重启 Broker 后三个型号的设备角色和 SQLite 列表保留，删除账号后不能登录。
- 迁移测试使用实际 `mosquitto_passwd` 生成 `$7$` 哈希，离线迁移重复 ACL 段后启动独立 Broker，原密码可以登录，设备角色正确。
- `scripts/test-browser-compose.sh`：独立 Compose 栈、随机本机端口和临时数据库，Chromium 三项流程通过：无 MAC 注册、云端跨浏览器恢复、账号隔离、保留访客列表；M1、A1、TC1 v2 实际 WebSocket MQTT 命令；管理员创建账号、绑定、解绑、删除。
- npm 依赖审计：零已知漏洞；构建保留一个体积提示，主要来自 MQTT 与二维码依赖，不影响构建成功。

测试使用隔离环境，没有部署到生产，没有发布镜像，没有刷写设备。MQTT 模拟客户端验证了消息与权限，尚未进行 TC1/M1/A1 实物设备硬件执行验收。
