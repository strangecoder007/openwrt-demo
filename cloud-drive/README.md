# 云盘一期（VPN 内 WebDAV 照片备份）部署存档

目标板：正点原子 i.MX6ULL（OpenWrt 19.07，内核 4.1.15），SD 卡 vfat 挂载在
`/mnt/sd`。手机 FolderSync 经 WireGuard 隧道把 `DCIM/Camera` 单向备份到
`/mnt/sd/backup/android/DCIM`。

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

## 证书续期自动化（Windows 计划任务）

- 脚本：本目录 `renew-cloud-cert.sh` → 复制到 `C:\Users\Administrator\scripts\`；
- 计划任务：`CloudCertRenew`（SYSTEM 账户，每天 03:30 + 开机，`StartWhenAvailable`
  错过后补跑）；
- 逻辑：`acme.sh --cron`（DNS-01，凭据在 `~/.acme.sh/account.conf`，经本机
  `127.0.0.1:7890` 代理访问 Let's Encrypt）→ `fullchain.cer` 当天更新过才合并
  key+fullchain 为 `server.pem` 推板子并 `lighttpd restart`；
- 日志：`C:\Users\Administrator\.acme.sh\cron.log`；下次续期窗口 2026-10-18。
