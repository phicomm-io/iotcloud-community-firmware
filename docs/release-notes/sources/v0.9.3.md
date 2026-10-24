首个签名 OTA 版本，覆盖 TC1 A1、M1、A1。

- `.ota.bin.signed`：P-256/SHA-256 签名包，用于新固件 Web 默认上传及 MQTT/URL 安装。
- `.ota.bin.raw`：原生 OTA。旧固件首次过渡请通过可信本地 Web 上传；新固件勾选“允许未签名的固件”后即可上传，无二次弹窗。
- `.all.bin`：完整原生烧录镜像，不能把签名外壳交给 bootloader。

CI 私钥位于仓库外，并只在受 tag 限制的 GitHub 签名环境中使用；仓库只含公钥。设备擦除前验证头部签名，Flash 读回 SHA-256 和兼容性检查通过后才启用镜像。

M1 真机已验证 raw 过渡、Web raw/signed 上传、MQTT 坏签名/raw 拒绝及 signed 安装和重启恢复。TC1/A1 真机、完整资源峰值及断电/回退仍待验收。本版本允许重装和降级，URL 下载使用直接 HTTP 200 + Content-Length。

设计与验收记录：localdocs/ota-signed-update-v1.md；实现 PR #26。

发布后已下载三型号 Release 产物，逐一验证 P-256 签名、签名摘要及 SHA256SUMS。M1 另通过 MQTT 安装了本 Release 的 CI 签名包，重启后运行 0.9.3-d50e93，MQTT 与传感器数据恢复正常。
