# 云盘一期（VPN 内 WebDAV 备份）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在板子上跑通 Android 手机（FolderSync）经 WireGuard 隧道把照片备份到 SD 卡（WebDAV，lighttpd + mod_webdav，监听 wg0/LAN，不暴露公网）。

**Architecture:** lighttpd 1.4.54 + mod_webdav 提供 WebDAV，根目录 /mnt/sd、URL 前缀 /dav、Basic 认证（htpasswd）；一期只监听 10.10.10.1:8080（wg0）和 192.168.100.1:8080（br-lan）两个地址；手机 FolderSync 单向上传照片到 backup/<设备名>/DCIM。

**Tech Stack:** lighttpd 1.4.54（feeds/packages）、mod_webdav、mod_authn_file、FolderSync（Android）、WireGuard（已有）。

**Spec:** [docs/superpowers/specs/2026-08-18-cloud-drive-design.md](\\192.168.100.181/chao/openwrt-19.07/openwrt/docs/superpowers/specs/2026-08-18-cloud-drive-design.md)

## Global Constraints

- 一期只监听 `10.10.10.1:8080`（wg0）与 `192.168.100.1:8080`（br-lan），不监听 WAN。
- `webdav.root = /mnt/sd`，URL 前缀 `/dav/`，alias 到 `/mnt/sd/`。
- 认证：用户 `backup`，初始密码 `Backup@2026`（二期上 TLS 后必须更换）。
- 目录约定：`/mnt/sd/backup/<设备名>/{DCIM,Files}`，一期设备名用 `android`。
- 不改动 `/etc/config/samba`、`/etc/hotplug.d/block/20-sd-share`、`/etc/rc.local`。
- 编译必须在 Linux 构建主机执行（Windows 无法交叉编译）；板子操作走 SSH。
- 本地文件修改用 apply_patch；板子远程文件用 heredoc 写入（去 CR）。
- 板子 SSH 必须带兼容参数：
  `ssh -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa -o KexAlgorithms=+diffie-hellman-group14-sha1,diffie-hellman-group1-sha1,diffie-hellman-group-exchange-sha1 root@192.168.100.1`

---

### Task 1: 构建主机编译 lighttpd 全家桶（用户执行）

**Files:**
- 构建主机上的 OpenWrt 树（`.config`、`feeds/packages/net/lighttpd`）

**Interfaces:**
- 产出：`bin/targets/imx6/generic/packages/` 下的 ipk：
  `lighttpd_1.4.54-*_arm_cortex-a9_neon.ipk`、`lighttpd-mod-webdav_1.4.54-*`、
  `libsqlite3_*`、`libuuid_*`、`libxml2_*`、`libpcre_*`（如板子缺）。

- [ ] **Step 1: 选中 lighttpd 及 webdav 模块**

```bash
make menuconfig
#  Network -> Web Servers/Proxies -> <*> lighttpd
#                                   <*> lighttpd-mod-webdav
#                                   <*> lighttpd-mod-auth
#                                   <*> lighttpd-mod-authn_file
#                                   <*> lighttpd-mod-alias
#                                   <*> lighttpd-mod-accesslog
# 保存退出（依赖 libsqlite3/libuuid/libxml2/libpcre 会自动选中）
```

- [ ] **Step 2: 确认选中**

```bash
grep -E 'CONFIG_PACKAGE_lighttpd|CONFIG_PACKAGE_libsqlite3|CONFIG_PACKAGE_libuuid|CONFIG_PACKAGE_libxml2' .config
# 应看到 =y
```

- [ ] **Step 3: 编译**

```bash
make package/feeds/packages/lighttpd/compile V=s
make package/index V=s
```

- [ ] **Step 4: 确认 ipk 生成（作为本任务验收）**

```bash
ls -l bin/targets/imx6/generic/packages/lighttpd*.ipk \
      bin/targets/imx6/generic/packages/libsqlite3*.ipk \
      bin/targets/imx6/generic/packages/libuuid*.ipk \
      bin/targets/imx6/generic/packages/libxml2*.ipk \
      bin/targets/imx6/generic/packages/libpcre*.ipk
```

预期：lighttpd、lighttpd-mod-webdav、lighttpd-mod-auth、lighttpd-mod-authn_file、
lighttpd-mod-alias、lighttpd-mod-accesslog 以及依赖库全部列出
（libpcre 若板子已有可跳过）。

---

### Task 2: 板子安装 lighttpd（Codex 执行）

**Files:**
- 修改（板子）：opkg 数据库（安装 ipk）

**Interfaces:**
- 消费：Task 1 的 ipk（从共享盘拷贝到板子 /tmp）
- 产出：`/usr/sbin/lighttpd`、`/usr/lib/lighttpd/mod_webdav.so`、`/etc/init.d/lighttpd`

- [ ] **Step 1: 拷贝 ipk 到板子并安装**

```powershell
# 本机（Windows）：从共享盘 scp 到板子
$src = '\\192.168.100.181\chao\openwrt-19.07\openwrt\bin\targets\imx6\generic\packages'
scp "$src\lighttpd*.ipk" "$src\libsqlite3*.ipk" "$src\libuuid*.ipk" "$src\libxml2*.ipk" root@192.168.100.1:/tmp/
# 板子上
opkg install --force-reinstall /tmp/lighttpd*.ipk /tmp/libsqlite3*.ipk /tmp/libuuid*.ipk /tmp/libxml2*.ipk
```

- [ ] **Step 2: 缺 libpcre 则补装**

```bash
opkg list-installed | grep libpcre || opkg install /tmp/libpcre*.ipk
```

- [ ] **Step 3: 验收**

```bash
lighttpd -v
ls -l /usr/lib/lighttpd/mod_webdav.so /usr/lib/lighttpd/mod_authn_file.so
ls -l /etc/init.d/lighttpd
```

预期：lighttpd/1.4.54 版本号输出；三个文件都在。

---

### Task 3: 生成密码文件与目录（部分用户执行）

**Files:**
- 创建（板子）：`/etc/lighttpd/webdav.passwd`（0600）
- 创建（板子）：`/var/log/lighttpd/`（init 会自动建，如缺则手动）
- 创建（板子）：`/mnt/sd/backup/android/DCIM`、`/mnt/sd/backup/android/Files`

**Interfaces:**
- 产出：`/etc/lighttpd/webdav.passwd` 内容为 `backup:<apr1哈希>`，供 Task 4 的 auth 使用。

- [ ] **Step 1: 构建主机生成 apr1 哈希（用户执行，把输出贴回来）**

```bash
openssl passwd -apr1 -salt cloud202 'Backup@2026'
```

预期输出形如 `$apr1$cloud202$xxxxxxxxxxxxxxxxxxxxx`。Codex 拿到后写入板子文件；
若用户不想贴出，可在板子上自行执行
`echo 'backup:<上一步输出>' > /etc/lighttpd/webdav.passwd && chmod 600 /etc/lighttpd/webdav.passwd`。

- [ ] **Step 2: 写入密码文件并建目录（Codex 执行）**

```bash
mkdir -p /etc/lighttpd /mnt/sd/backup/android/DCIM /mnt/sd/backup/android/Files
echo 'backup:<apr1哈希>' > /etc/lighttpd/webdav.passwd
chmod 600 /etc/lighttpd/webdav.passwd
```

- [ ] **Step 3: 验收**

```bash
ls -l /etc/lighttpd/webdav.passwd
head -c 20 /etc/lighttpd/webdav.passwd
ls -ld /mnt/sd/backup/android/DCIM /mnt/sd/backup/android/Files
```

预期：权限 600；内容以 `backup:$apr1$` 开头；两个目录存在。

---

### Task 4: 写 lighttpd.conf（Codex 执行）

**Files:**
- 覆盖（板子）：`/etc/lighttpd/lighttpd.conf`

**Interfaces:**
- 消费：Task 2 的模块、Task 3 的密码文件与目录
- 产出：监听 `10.10.10.1:8080` 与 `192.168.100.1:8080` 的 WebDAV 服务

- [ ] **Step 1: 写入配置**

```bash
cat > /etc/lighttpd/lighttpd.conf <<'EOF'
server.modules = (
    "mod_auth",
    "mod_authn_file",
    "mod_alias",
    "mod_accesslog",
    "mod_dirlisting",
    "mod_indexfile",
    "mod_staticfile",
    "mod_webdav"
)

server.document-root        = "/mnt/sd"
server.upload-dirs          = ( "/tmp" )
server.errorlog             = "/var/log/lighttpd/error.log"
server.pid-file             = "/var/run/lighttpd.pid"
server.username             = "http"
server.groupname            = "www-data"

index-file.names            = ( "index.html" )
dir-listing.activate        = "disable"

server.bind                 = "10.10.10.1"
server.port                 = 8080

$SERVER["socket"] == "192.168.100.1:8080" {
    # 同一份配置的第二个监听地址（global 配置对两个 socket 都生效）
}

alias.url += ( "/dav/" => "/mnt/sd/" )

webdav.root                  = "/mnt/sd"
webdav.log-file              = "/var/log/lighttpd/webdav.log"

auth.backend                 = "htpasswd"
auth.backend.htpasswd.userfile = "/etc/lighttpd/webdav.passwd"
auth.require = ( "/dav/" => (
    "method"  => "basic",
    "realm"   => "Cloud",
    "require" => "user=backup"
) )

include "/etc/lighttpd/mime.conf"
EOF
```

- [ ] **Step 2: 配置语法校验**

```bash
lighttpd -tt -f /etc/lighttpd/lighttpd.conf
```

预期：无输出、退出码 0（与 init 脚本 validate_conf 一致）。

---

### Task 5: 启动服务并 curl 验收（Codex 执行）

**Files:**
- 修改（板子）：/var/log/lighttpd/ 下的日志

**Interfaces:**
- 消费：Task 4 的配置
- 产出：可用的 `http://10.10.10.1:8080/dav/` 与 `http://192.168.100.1:8080/dav/`

- [ ] **Step 1: 启动并设开机自启**

```bash
/etc/init.d/lighttpd enable
/etc/init.d/lighttpd start
```

预期：无报错；`logread | grep lighttpd` 无 validation failed。

- [ ] **Step 2: 检查监听地址**

```bash
netstat -tlnp | grep 8080
```

预期：包含 `10.10.10.1:8080` 与 `192.168.100.1:8080`，**不包含** WAN 地址。

- [ ] **Step 3: 未认证访问应 401**

```bash
curl -s -o /dev/null -w '%{http_code}\n' http://10.10.10.1:8080/dav/
```

预期：401。

- [ ] **Step 4: 认证后建目录、传文件、下载比对**

```bash
curl -u backup:'Backup@2026' -X MKCOL http://10.10.10.1:8080/dav/backup/test
echo 'phase1-ok' > /tmp/cloud-test.txt
curl -u backup:'Backup@2026' -T /tmp/cloud-test.txt http://10.10.10.1:8080/dav/backup/test/cloud-test.txt
curl -u backup:'Backup@2026' -o /tmp/cloud-dl.txt http://10.10.10.1:8080/dav/backup/test/cloud-test.txt
md5sum /tmp/cloud-test.txt /tmp/cloud-dl.txt
```

预期：MKCOL 返回 201；两次 md5 一致；文件落在 `/mnt/sd/backup/test/cloud-test.txt`。

- [ ] **Step 5: 清理测试目录并确认日志**

```bash
curl -u backup:'Backup@2026' -X DELETE http://10.10.10.1:8080/dav/backup/test
tail -5 /var/log/lighttpd/access.log
```

预期：DELETE 204；access.log 有 PROPFIND/PUT 记录。

---

### Task 6: 手机 FolderSync 实机备份（用户执行，Codex 协助验收）

**Files:**
- 修改：手机 FolderSync 配置（用户操作）

**Interfaces:**
- 消费：Task 5 的 WebDAV 服务
- 产出：`/mnt/sd/backup/android/DCIM/` 下的真实照片文件

- [ ] **Step 1: 手机连 WireGuard**（已有的手机端配置，确认 wg0 通：`ping 10.10.10.1`）

- [ ] **Step 2: FolderSync 添加 WebDAV 账户**

```
账户类型: WebDAV
服务器地址: http://10.10.10.1:8080/dav
用户名: backup
密码: Backup@2026
```

- [ ] **Step 3: 新建同步对（Sync Pair）**

```
本地文件夹: /storage/emulated/0/DCIM/Camera（或手机实际相机目录）
远端文件夹: /backup/android/DCIM
同步方式: 仅上传（Upload only）
```

- [ ] **Step 4: 手动执行一次并验收（Codex 在板子侧确认）**

```bash
find /mnt/sd/backup/android/DCIM -type f | wc -l
ls -lh /mnt/sd/backup/android/DCIM | head
```

预期：文件数 > 0，尺寸与手机照片一致。

---

### Task 7: 重启恢复验收（Codex 执行）

**Files:**
- 无（仅验证）

- [ ] **Step 1: 重启板子**

```bash
reboot
```

- [ ] **Step 2: 重启后验证挂载与服务**

```bash
mount | grep mmcblk0p1
netstat -tlnp | grep 8080
curl -u backup:'Backup@2026' -s -o /dev/null -w '%{http_code}\n' http://10.10.10.1:8080/dav/backup/
ls /mnt/sd/backup/android/DCIM | head
```

预期：挂载在 /mnt/sd；8080 监听两个地址；curl 200；照片文件仍在。

---

## 二期（不在本计划内，后续单独计划）

- acme 证书 + lighttpd TLS 443（IPv4 转发 + IPv6 防火墙放行 80/443）。
- 公网 `https://cy.gcaiyy.xyz/dav` 验收与安全加固（更换密码、认证日志、防爆破）。
- 可选：磁盘/挂载状态监控。
