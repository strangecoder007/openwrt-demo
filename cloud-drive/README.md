# 云盘一期（VPN 内 WebDAV 照片备份）部署存档

目标板：正点原子 i.MX6ULL（OpenWrt 19.07，内核 4.1.15），SD 卡 vfat 挂载在
`/mnt/sd`。手机 FolderSync 经 WireGuard 隧道把 `DCIM/Camera` 单向备份到
`/mnt/sd/backup/android/DCIM`。

## 本次更新（2026-08-20 会话汇总）

- **登录/上传链路增强**：小程序 `utils/wxreq.js` 网络层失败自动重试（从无
  IPv6 网络切到有 IPv6 的网络后不再必挂）、登录页“记住密码”（`davSavedCred`）、
  视频上限 500MB；
- **大视频 500 修复**：lighttpd
  `server.upload-dirs = ( "/mnt/sd/.upload", "/tmp" )` +
  `server.stream-request-body = 1`（本目录 `lighttpd.conf` 已同步），
  300MB 实测通过；
- **视频压缩版**：手机端 `wx.compressVideo`（medium）本地压缩 `.preview.mp4`
  回传，点视频优先播放压缩版、老视频回退原片；dav-bridge 过滤 `.preview.mp4`
  （PKG_RELEASE 8），小程序 v20260820.6；**板子待部署**（重编重装 + 上传
  体验版）；
- **性能与健壮性优化（dav-bridge 9 / 小程序 v20260820.7）**：
  - `op=register` 请求体在 `malloc` 前按 4KB 上限拦截（原来会先按
    Content-Length 分配最多 512MB 再拒绝，异常请求可打爆板子内存）；
  - 中断上传残留的 `<name>.part` 不再进目录列举；同名 `.part` 被占时 CGI
    清理超过 1 小时的残骸后重试同名，不会永久占住名字；
  - `op=ls` 每项带 `hasThumb/hasPreview/hasPreviewVideo` 标记（服务端
    `stat` 派生兄弟得出），月视图缩略图/预览图、点视频探测不再每张图发
    一次 `depth=0` 请求；
  - 首页月份计数改为并发 4 路拉取，不再 N+1 串行；
  - 旧版桥没有新字段时客户端自动回退到原探测逻辑（兼容）。
  **板子待部署**：重编 dav-bridge 9 + 上传小程序体验版 v20260820.7。
- **月视图预览返回不再整屏刷新（小程序 v20260820.8）**：`wx.previewImage`
  关闭会触发本页 `onShow`，原逻辑无条件 `loadFiles()` 导致全部缩略图闪
  “加载中…”；现改为预览前置 `_fromPreview` 标志、`onShow` 跳过重载；
  且重载时沿用已有缩略图/状态（仅新文件与失败项重新拉取），下拉刷新/删除
  后返回也不会整屏闪白。
- **ddns-go IPv4 已关闭**（光猫无管理员、无法端口转发），公网仅 IPv6。
  ⚠️ 现场核对 `ipv6.enable` 也被置为 `false`，待恢复（否则 AAAA 不再刷新）。

## 监听与认证

- `10.10.10.1:8080`（wg0，HTTP）
- `192.168.100.1:8080`（br-lan，HTTP）
- `10.10.10.1:8443`（wg0，**HTTPS**，FolderSync 用）
- URL 前缀 `/dav`（`/dav` 与 `/dav/` 均兼容）→ `/mnt/sd/`
- Basic 认证用户 `backup`（密码见设计稿；**仓库是公开的，上公网前必须轮换**）

## 文件清单（板子上）

- `/etc/lighttpd/lighttpd.conf` — 本目录同名文件
- `/etc/lighttpd/server.pem` — 自签名证书（RSA-2048，SAN
  `IP:10.10.10.1,DNS:cy.gcaiyy.xyz`，有效期到 2028-11）。私钥**不入库**。
- `/etc/lighttpd/webdav.passwd` — 0644（lighttpd 以 http 用户运行，0600 读不了）
- `/etc/init.d/lighttpd` — 本目录 `lighttpd.init`，改动点：`START=99` +
  等 `10.10.10.1` 出现（wireguard-go 异步建接口，S50 同优先级下 lighttpd 字母序
  在前会 bind 失败）。**注意：opkg 重装 lighttpd 会还原 init 脚本，需重新应用。**

## 关键坑（已解决）

1. **64KB PUT 落盘 0 字节**：lighttpd 1.4.54 mod_webdav 上游 bug
   （论坛 9273）。修复在 1.4.56（commit 8b4abaf498 + 3a766d3d02），已 backport
   到 1.4.54：见 `feeds-patch/`，构建时 `PKG_RELEASE=2`。SD 是 vfat 时
   O_TMPFILE/linkat 快速路径失败走拷贝回退，chunk 长度没更新导致 0 字节。
2. **FolderSync 强制 HTTPS**：新版禁明文 HTTP（Android cleartext 策略，官方 FAQ
   明确不可配置），所以 WG 侧加了 8443 自签名 TLS，FolderSync 账户勾选
   “允许自签名证书”。
3. **开机 lighttpd 起不来**：`S50lighttpd` 在 `S50wireguard-go` 之前执行，
   且 wireguard-go 建接口是异步的（wrapper 有 8s setconf 等待），bind
   `10.10.10.1` 失败。修复：init 脚本 `START=99` + 等待 wg0 地址，并删除
   残留 `S50lighttpd` 软链。

## 二期（公网 HTTPS）

- acme 证书 + lighttpd 443（IPv4 转发 + IPv6 防火墙放行 80/443），复用现有
  `ssl.engine` 配置段。
- **轮换密码**（公开仓库已含旧密码）。
- 认证日志与防爆破（mod_auth 限速/失败锁定）。

## 端口现状（2026-08-19）

- uhttpd（LuCI）回到 **80**：`http://192.168.100.1/`（`rfc1918_filter=1`，仅内网
  可达；uhttpd 的 https 监听已删除、`redirect_https=0`，避免与 lighttpd 抢 443）；
- lighttpd **不再监听 80**；公网 80 防火墙规则（`Allow-HTTP-WAN`）已删除；
- 公网 HTTPS 入口只剩 **34443**；lighttpd 监听 443 / 8080 / 8443(WG) / 34443。

## 证书续期自动化（2026-08-19 起迁到板子）

- **板子侧（现行）**：`/root/.acme.sh`（DNS-01，阿里云凭据在
  `account.conf`，不入库）；板子 cron 每天 03:00 跑
  `acme.sh --cron`，日志 `/root/.acme.sh/cron.log`；
- 安装钩子：fullchain/key 直接部署到 `/etc/lighttpd/`，reloadcmd 合并
  `server.pem` 并 `lighttpd restart`（下次续期窗口 2026-10-18）；
- **前置条件（已装）**：GNU wget 1.20.3（busybox wget 不认 `--header`，acme.sh
  会失败）+ libustream-openssl/ca-bundle/openssl-util；
- **回退**：Windows 计划任务 `CloudCertRenew` 已停用但保留；脚本
  `renew-cloud-cert.sh`（`C:\Users\Administrator\scripts\`）重启用
  `Enable-ScheduledTask -TaskName CloudCertRenew`。

## 公网双栈与 DNS（2026-08-20）

- ddns-go（`/root/.ddns_go_config.yaml`，init 脚本 `/etc/init.d/ddns-go`）
  维护 **AAAA**（刷新周期 `-f 60`）：
  - **IPv4 已关闭**（2026-08-20 按用户要求：光猫无管理员、无法端口转发），
    A 记录不再维护，`cy.gcaiyy.xyz` 仅解析 AAAA；
  - IPv6：`gettype: netInterface`（eth1），域名 `cy.gcaiyy.xyz` →
    板子 WAN 全局 IPv6；
  - ⚠️ **现场核对（2026-08-20）**：`ipv6.enable` 当前也是 `false`，11:30
    重启后日志已无 IPv6 检查；当前 AAAA 恰好仍等于板子地址，IPv6 前缀一变
    就会失效。待恢复 `enable: true` 并重启 ddns-go。
- 公网 HTTPS 入口仅 `[IPv6]:34443`；IPv6 直连依赖上游对板子 WAN 地址的入站
  放行，无 IPv6 的网络无法访问（IPv4 兜底已放弃）。
- 小程序端已对网络层失败（不可达/超时）自动重试，切网后首次登录不再必挂。
