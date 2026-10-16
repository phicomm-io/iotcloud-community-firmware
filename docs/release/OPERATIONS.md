# OTA 发布操作

## 来源与密钥

境外源：`http://phicomm.fjrcn.com`；境内缓存：`http://phicomm.fjrcn.cn`。
浏览器访问元数据使用相应 HTTPS 入口，避免 HTTPS Pages 的混合内容限制。

本机签名私钥：`/home/fjr/.config/phicomm-ota/release-p256-key-1.pem`（0600）。
GitHub Environment `signed-ota-production` 的 `OTA_SIGNING_KEY_PEM` 负责 CI 签名。
本机 Cloudflare 环境文件：`/home/fjr/.config/phicomm-ota/cloudflare.env`（0600）。
私钥及 API Token 不进入源码、Wiki、Pages 或构建产物。

## 准备新版本

1. 编辑 `docs/public/` 下对应的页面（公开文档的唯一来源），提交并合并。
2. 在私有 Wiki `Release-Log.md` 编辑唯一的 `## vMAJOR.MINOR.PATCH` 章节并提交、推送 Wiki。
3. 运行 `python3 tools/release/notes.py vMAJOR.MINOR.PATCH --wiki /path/to/wiki`，把 `docs/release/Release-Log.md` 和 `wiki-snapshot.json` 一起提交。
4. 测试、审核并合并主分支。确认新版本严格递增后才在该主分支提交上创建 tag。
5. tag CI 构建三个型号、测试并签名。发布任务先创建草稿，上传不可变版本目录，通过国内外下载哈希及签名验证后才公开 GitHub Release，并更新一个 `stable.json`。
6. 最后同步公开 Pages 的明确文件白名单。Pages 的历史归档读取 `/releases/index.json`，保留所有已导入历史版本。

发布说明缺失、重复、未完成或快照哈希不匹配会阻止发布。相同版本目录中的不同内容会被拒绝覆盖；重试应使用已上传的签名产物，不能重新签名后强行覆盖。所有生产发布共用 Actions concurrency group，禁止手工并发运行发布脚本。

## 公开文档的单一来源

`site/pages.py` 定义公开文档的页面清单（分组、文件名、标题、摘要、对应 Wiki 页面），两个公开界面都从这里生成，不再各维护一份：

| 产物 | 生成方式 |
| --- | --- |
| 公开站点 | `python3 site/build.py build/pages` 读取 `docs/public/*.md` 与 `site/pages.py` |
| GitHub Wiki | `python3 tools/release/wiki_sync.py <wiki clone>` 从同一批 markdown 重写链接后写出 |

因此页面标题、导航顺序和链接只需改一处。新增或改名页面时同步修改 `site/pages.py`，再运行 `tests/test_site_docs.py` 校验：

```sh
python3 -m unittest discover -s tests -p test_site_docs.py
```

该测试会阻止两类静默错误：清单里有条目但没有对应 markdown，以及 `docs/public/` 有页面但清单没有收录（后者会导致页面在站点和 Wiki 上都不出现）。

**`Release-Log.md` 是例外**，它从 Wiki 流向仓库（步骤 2、3），不由 `wiki_sync.py` 生成，以免覆盖作者刚写的版本章节。

### 预发布

改动站点后先在本地构建并预览，确认无误再推送：

```sh
python3 site/build.py build/pages
(cd build/pages && python3 -m http.server 18767)   # 打开 http://127.0.0.1:18767/
```

需要可分享的地址时推送到 `preview` 分支（只影响预览，不动线上 `main`）：

```sh
tools/release/pages.sh --preview
```

线上 `main` 只在发布流程末尾由 `tools/release/pages.sh --publish` 更新。

## 术语

公开文档统一使用设备的完整名称。注意区分两个容易混淆的概念：

- **本地 WebUI**：设备自身在局域网提供的网页，直连设备。
- **MQTT 远程集中管理平台**：`phicomm-community-mqtt` 提供的账号与设备集中管理服务，经 MQTT 接入。首次出现写全称，其后可用「集中管理平台」简称。

## 元数据

- `/ota/{tc1,m1,a1}/latest.json`：型号、版本、commit、大小、sha256、相对下载路径和发布说明路径。
- `/releases/index.json`：按数字 SemVer 倒序的版本归档；`stable` 指定正式最新版。
- `/releases/vX.Y.Z/`：原始固件、签名 OTA、完整镜像、SHA256SUMS、notes.md、index.html、release.json。

`.ota.bin.signed` 是签名升级包；`.ota.bin.raw` 是原始 OTA；`.all.bin` 是完整有线刷机镜像。旧版本可能只有 `.ota.bin`，归档按原文件保留，不补签、不重建。

## 境内缓存与 1Panel

1Panel 正式站点 `phicomm.fjrcn.cn`（站点 ID 25），监听 60080；主机 nginx 的域名专用 80 端口虚拟主机转发到它。站点文件位于 `/opt/1panel/www/sites/phicomm.fjrcn.cn`，缓存最多 2 GB。

上游固定为 HTTPS `phicomm.fjrcn.com`，启用 SNI 与证书验证。固件按版本缓存一年；最新元数据和索引缓存 60 秒。失败不缓存、不以过期最新元数据兜底。响应的 `X-Cache` 可检查 MISS/HIT。

原主机 nginx、OpenResty 配置与数据库备份在服务器 `/root/phicomm-ota-backup`。修改代理前备份 `proxy/root.conf`，运行 `docker exec web_gateway nginx -t` 通过后再 reload；主机配置同样先 `nginx -t`。

Cloudflare 原先的全区 Always Use HTTPS 已由单一重定向替代：除 OTA 域名外的 HTTP 请求仍跳 HTTPS，保留路径与查询参数。回退时先恢复 Always Use HTTPS，再删除替代规则。部署前快照在本机 `~/.config/phicomm-ota/cloudflare-backup-*.json`。

## 回退

上一正式指针保存在 R2 `rollback/stable.json`。确认目标版本及签名后将该对象恢复到 `stable.json`；不删除版本归档。元数据最多等待 60 秒刷新。若使用国内缓存急救，清理仅 OTA 站点的缓存，不能清理其他站点。

每次上传前读取账户 R2 总用量；超过 10 GB 停止上传，先由维护者删除旧对象。无需修改已有的其他 bucket、Worker 或站点。

## GitHub 静态固件归档

Pages 默认从本站 `/releases/` 下载历史固件，不依赖个人域名或服务器。
`tools/release/archive_pages.py` 从现有发布目录复制文件，逐个核对固件大小与 SHA-256 后写入索引；不重建、不补签旧包。发布时保留上一次 Pages 归档，上游停机时继续使用已有归档。新版本发布仍需配置签名、存储及跨仓库发布凭据。

固件内置 OTA 检查和 MQTT 远程集中管理服务仍使用各自的服务端；静态归档支持下载后本地上传升级。
