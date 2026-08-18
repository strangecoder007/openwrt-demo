# 云盘（Android 手机照片备份）设计文档

- 日期：2026-08-18
- 状态：已确认（方案三：分两期，先 VPN 内 WebDAV，后公网 HTTPS）
- 设备：正点原子 i.MX6ULL，OpenWrt 19.07 用户态 + 正点原子 4.1.15 内核

## 1. 背景与目标

在板子上搭建一个"手机照片/文件备份中心"：Android 手机把相机照片单向备份到板子 SD 卡。
重点是一致、可恢复、可观察，而不是双向同步。

成功标准：

1. 一期：手机连 WireGuard 后，FolderSync 通过 `http://10.10.10.1:8080/dav` 完成备份。
2. 二期：手机在 4G/5G 下直接通过 `https://cy.gcaiyy.xyz/dav` 完成备份。
3. 数据落盘路径可预测，支持以后扩展多设备。

## 2. 环境与现状

- 板子：br-lan = 192.168.100.1/24；WAN = eth1 DHCP（当前 192.168.1.2，家里路由器）。
- WireGuard（wireguard-go）：wg0 = 10.10.10.1/24 + fd00:10:10:10::1/64，监听 51820，
  对端（手机/PC）主动连入，已在运行。
- ddns-go 运行中，域名 cy.gcaiyy.xyz，公网环境为真公网 IPv4 + 公网 IPv6（A 类环境）。
- SD 卡：/dev/mmcblk0p1（FAT32，约 29GB），已自动挂载到 /mnt/sd
  （/etc/hotplug.d/block/20-sd-share + /etc/rc.local 兜底），挂载参数
  `rw,utf8,umask=000`。
- samba36-server 已运行，共享 [sd] = /mnt/sd（guest 可读写，仅内网）。
- 可用 feeds：lighttpd 1.4.54 + lighttpd-mod-webdav（依赖 libsqlite3、libuuid、libxml2）、
  acme（Let's Encrypt 客户端）、nginx（备选，不采用）。

## 3. 范围

一期（本次实施）：

- 编译并安装 lighttpd + lighttpd-mod-webdav 及依赖。
- 配置 WebDAV：根 = /mnt/sd，URL 前缀 /dav，Basic 认证（htpasswd）。
- 监听 wg0（10.10.10.1:8080）与 br-lan（192.168.100.1:8080），不监听 WAN。
- 目录规划 backup/<设备名>/；手机 FolderSync 联通验收。

二期（后续）：

- acme 证书 + lighttpd TLS 443（IPv4 转发 + IPv6 入站放行）。
- 公网访问验收 + 安全加固（认证日志、防爆破、密码更换）。
- 可选：磁盘/挂载状态监控。

不做（明确 Out of scope）：双向同步、Web 文件管理器、多用户隔离、
ext4 迁移、公网暴露 Samba。

## 4. 总体架构

```
Android FolderSync
   │  一期: WireGuard 隧道 → http://10.10.10.1:8080/dav
   │  二期: 公网 HTTPS     → https://cy.gcaiyy.xyz/dav
   ▼
lighttpd (mod_auth + mod_authn_file + mod_webdav)
   │  Basic 认证（一期 VPN 内明文；二期 TLS 加密）
   ▼
/mnt/sd/backup/<设备名>/DCIM...
   │
   ▼
SD 卡 vfat（umask=000，无真实 unix 权限，认证即访问控制）
```

## 5. 组件设计

### 5.1 WebDAV 服务端：lighttpd 1.4.54 + mod_webdav

- 来源：feeds/packages/net/lighttpd，构建命令（构建主机）：
  `make package/feeds/packages/lighttpd/compile V=s`，menuconfig 里选中
  lighttpd（含 SSL 支持）与 lighttpd-mod-webdav。
- 板子上安装：scp ipk → `opkg install --force-reinstall`。
- 关键配置（/etc/lighttpd/lighttpd.conf）：
  - server.modules：mod_auth、mod_authn_file、mod_alias、mod_webdav、
    mod_accesslog、mod_dirlisting、mod_indexfile、mod_staticfile。
  - webdav.root = "/mnt/sd"。
  - alias.url += ("/dav/" => "/mnt/sd/")。
  - 一期监听两个 socket：10.10.10.1:8080 与 192.168.100.1:8080
    （lighttpd 多 socket：server.bind/port + $SERVER["socket"] 条件块），
    不监听 WAN 地址。
  - mod_webdav 的 LOCK/属性锁在 OpenWrt 编译时被禁用（`--without-webdav-locks`，
    老 sqlite3 段错误问题），FolderSync 单向上传不受影响。

### 5.2 认证

- 后端：mod_authn_file，密码文件 /etc/lighttpd/webdav.passwd，格式 crypt(3)。
- 生成方式：构建主机上 `openssl passwd -crypt`（或 Python `crypt`）生成哈希，
  scp 到板子；文件属主 root，权限 600。
- auth.require 限定 /dav/ 路径，realm = "Cloud"，用户 = backup。
- 一期密码为强密码即可（VPN 内明文可接受）；二期上 TLS 后更换。

### 5.3 数据目录

```
/mnt/sd/
├── backup/
│   └── <设备名>/
│       ├── DCIM/      # 手机相机照片（FolderSync 任务目标）
│       └── Files/     # 手动放置文件
└── （samba 共享根仍是 /mnt/sd，backup 子目录对 SMB 同样可见）
```

- FolderSync 远端路径：/dav/backup/<设备名>/DCIM。
- 设备名约定：小写字母/数字/连字符，例如 android-mi9。

### 5.4 挂载与存储

- 复用现有 /etc/hotplug.d/block/20-sd-share + /etc/rc.local 兜底挂载。
- 卡未挂载时 WebDAV 请求返回 404/500，lighttpd error log 可查。
- FAT32 单文件上限 4GB，照片/视频单个不会超过，暂不迁移 ext4。

### 5.5 手机端（Android）

- 首选 FolderSync：免费、支持 WebDAV、定时任务、单向上传、可设仅 WiFi。
- 账户类型 WebDAV；一期 URL http://10.10.10.1:8080/dav（VPN 内），
  二期切换 https://cy.gcaiyy.xyz/dav。
- 任务：本地 DCIM/Camera → 远端 /dav/backup/<设备名>/DCIM，单向上传。

### 5.6 网络与端口

一期：

- 监听：10.10.10.1:8080（wg0）、192.168.100.1:8080（br-lan）。
- 公网零暴露；Samba 445 不对外。

二期：

- lighttpd TLS 监听 443（IPv4 + IPv6），TLS 由 mod_openssl 提供。
- IPv4：家里路由器转发 TCP 80/443 → 板子（80 仅用于 acme http-01 challenge）。
- IPv6：路由器防火墙只放行入站 80/443 到板子；8080/445 不放行。
- WireGuard（UDP 51820）保留为管理/回退通道。

### 5.7 证书（二期）

- 用 feeds 里的 acme 包（/etc/init.d/acme + UCI）。
- 域名 cy.gcaiyy.xyz 已由 ddns-go 解析 A/AAAA。
- challenge 用 http-01：acme webroot 指向 lighttpd 的
  /.well-known/acme-challenge/ 目录，需要 80 可达。
- 证书输出：/etc/lighttpd/ssl/cy.gcaiyy.xyz.crt / .key（实现时按 acme 包约定调整）。
- 续期：acme 默认周期；失败时 logread 可查。

## 6. 数据流

1. FolderSync 发起 PROPFIND/MKCOL/PUT 到 /dav/backup/<设备名>/DCIM/。
2. lighttpd mod_auth 校验 Basic 凭证；失败返回 401 并记录日志。
3. mod_webdav 将请求映射到 /mnt/sd/backup/<设备名>/DCIM/。
4. 文件经 vfat 落盘；返回 200/201/204。
5. 断点/续传由 FolderSync 按 WebDAV 语义重试。

## 7. 错误处理

| 场景 | 表现 | 处理 |
| --- | --- | --- |
| 卡未挂载 | 404/500 + error log | 检查 /proc/mounts 与 hotplug 日志 |
| 磁盘满 | 507 Insufficient Storage | FolderSync 报错；清理或换卡 |
| 认证失败 | 401 + access log | 二期根据日志做防爆破/告警 |
| 证书过期 | 手机 SSL 错误 | logread 查 acme，手动续期或修配置 |
| 拔卡/断电 | hotplug 卸载 | 重新插入自动恢复；数据完整性靠 FAT 校验 |

## 8. 安全设计

- 一期：WebDAV 只绑内网/wg 地址，公网无监听。
- 二期：仅 443 暴露；80 只给 acme challenge；8080/445 不对公网。
- 认证：唯一用户 backup，密码文件 600；二期换强密码。
- 管理：SSH + WireGuard 通道；Samba 保持内网。
- 弱 CPU 上的 TLS 握手开销可接受（照片备份为低频传输）。

## 9. 测试与验收

一期：

1. 板子上：
   `curl -u backup:<pw> -X MKCOL http://10.10.10.1:8080/dav/backup/test`
2. 上传再下载校验：
   `curl -u backup:<pw> -T file http://10.10.10.1:8080/dav/backup/test/`
   后下载比对 md5。
3. 手机 FolderSync 实机备份一批照片。
4. 重启板子：挂载恢复、lighttpd 自启、数据完好。

二期：

1. `https://cy.gcaiyy.xyz/dav` 公网可访问，证书链有效。
2. 手机 4G 下 FolderSync 备份成功。
3. 认证失败在日志中可查。

## 10. 已知限制

- mod_webdav 无 LOCK（编译期禁用），单用户备份不受影响。
- FAT32 单文件 4GB 上限、无真实权限（认证即访问控制）。
- 一期 Basic 认证在 HTTP 上明文，仅限 VPN/LAN 使用。
- i.MX6ULL 弱 CPU：TLS 握手与加密有开销，照片备份场景可接受。

## 11. 实施顺序（概述，详细计划另出）

1. 构建主机：编译 lighttpd + lighttpd-mod-webdav + libsqlite3/libuuid/libxml2，
   生成 ipk；板子安装。
2. 板子：写 lighttpd.conf（多 socket、webdav、auth），生成密码文件，开机自启。
3. 板子：mkdir backup 目录结构；curl 验收。
4. 手机：FolderSync 配置与实机备份验收。
5. 二期：acme 证书 → TLS 443 → 路由器放行 → 公网验收 → 安全加固。

## 12. 相关文件

- feeds/packages/net/lighttpd（1.4.54，含 lighttpd-mod-webdav）
- feeds/packages/net/acme（二期）
- /etc/lighttpd/lighttpd.conf（板子）
- /etc/lighttpd/webdav.passwd（板子）
- /etc/hotplug.d/block/20-sd-share、/etc/rc.local（已存在，不动）
- /etc/config/samba（已存在，不动）
