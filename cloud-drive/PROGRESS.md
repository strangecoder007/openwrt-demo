# 云盘项目总结（2026-08-20）

## 一、项目目标

把手机里的照片/视频备份到家里的 OpenWrt 板子（正点原子 i.MX6ULL）SD 卡，
通过微信小程序实现浏览、上传、下载、删除；公网（IPv6）可访问。

## 二、总体架构

| 层 | 组件 | 说明 |
| --- | --- | --- |
| 存储 | lighttpd 1.4.54 + mod_webdav | WebDAV 根目录 `/mnt/sd`，SD 卡 vfat |
| 桥接 | dav-bridge（C CGI） | 微信 `wx.request` 不支持 PROPFIND/MKCOL，桥接为 GET；另提供上传、注册 |
| 认证 | lighttpd mod_auth（htpasswd） | `/etc/lighttpd/webdav.passwd`，多账号共用云盘 |
| 客户端 | 微信原生小程序 | pages：login / register / home / month / upload |
| 公网 | IPv6 + 高位端口 34443 | 无公网 IPv4（光猫无管理员）；域名 `cy.gcaiyy.xyz` 由 ddns-go 更新 |
| 证书 | Let's Encrypt + acme.sh | DNS-01（阿里云），板子 cron 每天 03:00 自动续期 |

## 三、数据与派生图约定

- 照片/视频按“年-月”目录存放：`/mnt/sd/backup/android/DCIM/<YYYY-MM>/IMG_xxx.jpg`
- 派生图约定（同目录同名、扩展名替换）：
  - `.thumb.jpg`：480px 缩略图（网格用，几 KB～几十 KB）
  - `.preview.jpg`：1280px 预览图（全屏预览用，100～300KB）
- 上传图片时客户端本地压缩两张派生图一并回传；老文件首次浏览/预览时下载原图、
  压缩生成回传一次，之后秒开。
- dav-bridge 目录列举过滤派生图（`is_derived_name`），客户端列表同步过滤，
  避免文件数量翻倍。

## 四、认证与账号

- lighttpd `auth.require`：`/dav`、`/dav/`、`/cgi-bin/dav-bridge.cgi` 均为
  `valid-user`（htpasswd 里所有账号权限相同、共用同一云盘）。
- 注册流程：小程序注册页填“管理员账号密码 + 新用户名/密码” → POST
  `dav-bridge?op=register`（form-urlencoded）→ CGI 校验 `REMOTE_USER == backup`
  （`ADMIN_USER` 常量，其他账号 403）→ 校验格式（用户名 3-32 位
  `[A-Za-z0-9._-]`、密码 6-128 位非控制字符、不可重名）→
  `openssl passwd -apr1` 生成哈希追加 htpasswd → 注册成功即自动登录。
- `webdav.passwd` 属主 `http:root` 640（CGI 以 http 用户追加写入）。
- 密码/Token 一律不入库（本仓库公开）。

## 五、小程序功能清单

1. **登录**：Basic 认证，服务器地址可配（默认 `https://cy.gcaiyy.xyz:34443`）。
2. **注册账号**：见上。
3. **主页**：月份列表（按 `DCIM/<YYYY-MM>`）。
4. **月视图**：
   - 按天分组网格，缩略图加载（`.thumb.jpg`）；
   - 本地持久缓存 `USER_DATA_PATH/thumbcache/`（上限 100MB/500 项，LRU 淘汰，
     命中零网络请求）；
   - 下拉刷新（带防重入）；
   - 编辑模式多选：批量下载到相册（确认框显示文件数与总大小，进度显示
     “已保存 Y MB”）、批量删除（连带服务端派生图与本地缓存）；
   - 点图片：按天加载 `.preview.jpg`（并发 3，带进度；老图无预览图时首次
     下载原图压缩生成回传）→ `wx.previewImage` 左右滑动；
   - 点视频：下载后播放（`.thumb.jpg` 封面 + ▶ 角标）；
   - 缩略图失败时格子直接显示具体错误原因（诊断友好）。
5. **上传**：`wx.uploadFile`（multipart）到 `dav-bridge?op=upload`，有进度；
   图片回传双派生图、视频回传封面；单文件上限 50MB（视频）；重名自动加
   `-1`/`-2` 后缀。

## 六、关键技术决策与已解决的坑

1. **选型**：轻量 WebDAV（lighttpd mod_webdav）而非 ksmbd/Samba——认证与
   小程序统一（Basic）、CGI 可扩展、OpenWrt 上开销小。
2. **64KB PUT 落盘 0 字节**：lighttpd 1.4.54 mod_webdav 上游 bug，backport
   1.4.56 修复；SD vfat 下走拷贝回退路径。
3. **真机 `wx.request` 无 PROPFIND/MKCOL**：`network argv error`，开发工具
   碰巧放行。方案 B 落地 dav-bridge CGI（`op=ls`/`op=mkdir`）。
4. **二进制上传损坏**：`readFile(encoding:'binary')` + PUT 字符串被 UTF-8
   膨胀；改 `wx.uploadFile` 流式上传（有进度）。
5. **mod_cgi 顺序**：必须排在 mod_staticfile 之前，否则 CGI 被当静态文件
   直接吐出 ELF 本体。
6. **列表数量翻倍**：派生图必须两端（客户端 + dav-bridge）同步过滤。
7. **previewImage 不能带 Authorization**：先下载本地再预览；按“天”分组 +
   1280px 预览图控制流量（方案 B）。
8. **SMB 共享上微信工具不识别新增文件**：报 `module not defined`；重启工具
   或本地磁盘开发。
9. **新账号登录 401**：登录验证走 dav-bridge，CGI 认证也要 `valid-user`；
   注册权限下沉 CGI 内部（`REMOTE_USER == backup`）。
10. **缩略图下载失败**：lighttpd 日志无 GET /dav/ 请求，客户端
    `wx.downloadFile` 被拦（合法域名）；补配 downloadFile 域名后恢复，
    失败原因已透传到界面便于自诊断。
11. **busybox wget 不认 `--header`**：板子 acme.sh 无法工作；编 GNU wget
    1.20.3 替换后证书续期迁到板子（详见下）。

## 七、证书续期（已迁到板子）

- 板子 `/root/.acme.sh`（DNS-01，阿里云凭据在 `account.conf`，不入库）；
- 板子 cron 每天 03:00 跑 `acme.sh --cron`，日志
  `/root/.acme.sh/cron.log`；
- 安装钩子：续期成功部署 `/etc/lighttpd/fullchain.pem` + `privkey.pem`，
  合并 `server.pem` 并 `lighttpd restart`；
- 前置：GNU wget 1.20.3 + libustream-openssl/ca-bundle/openssl-util；
- 回退：Windows 计划任务 `CloudCertRenew` 已停用但保留脚本
  `renew-cloud-cert.sh`（`Enable-ScheduledTask` 可恢复）。

## 八、运维操作

- **dav-bridge 更新**：Linux 构建主机
  `make package/network/services/dav-bridge/compile V=s && make package/index V=s`
  → 板子 `opkg install --force-reinstall /tmp/dav-bridge_*.ipk`。
- **添加账号**：小程序注册页（需 backup 管理员密码）；或直接改 htpasswd
  （`openssl passwd -apr1` 生成哈希追加）。
- **端口现状**：uhttpd/LuCI 80（仅内网）；lighttpd 443（内网/公网）、
  8080（LAN/WG）、8443（WG TLS）、34443（公网 IPv6 唯一入口）。

## 九、已知限制与后续方向

- 所有账号共用同一云盘目录，无 per-user 隔离；二期可做 `/dav/<user>` 分目录
  + dav-bridge 按 REMOTE_USER 围栏。
- 注册哈希经 `openssl` CLI 生成，密码 argv 短暂可见；可改 libcrypto 实现
  apr1 消除 exec。
- 老文件首次浏览/预览需下载原图生成派生图（一次）。
- 手机端无法截视频帧，老视频无封面（服务端无 ffmpeg）。
- lighttpd 1.4 认证为目录级，无按用户读写 ACL。
- 小程序本地缓存 100MB，照片量大时 LRU 会淘汰旧图（回源重下）。

## 十、当前状态（2026-08-20）

- 多账号注册（backup + yanzi）、共享云盘、月视图缓存/下拉刷新/左右滑动预览、
  下载显示大小、证书板子自动续期，全部验证正常；
- dav-bridge 新版（含 `ADMIN_USER` 管理员检查）已装板子：普通账号调注册
  返回 403，管理员可注册；
- 板子 `webdav.passwd` 当前用户：backup、yanzi。
