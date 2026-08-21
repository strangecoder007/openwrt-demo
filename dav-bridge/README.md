# dav-bridge

微信小程序 `wx.request` 的方法白名单里没有 `PROPFIND` / `MKCOL`
（真机会直接 `fail`，报 `network argv error`；只有开发者工具碰巧放行）。
本包是一个极小的 C CGI，把小程序需要的这两个 WebDAV 操作转成普通 `GET`
并返回 JSON：

- `GET /cgi-bin/dav-bridge.cgi?op=ls&path=<url-encoded>&depth=0|1`
  - `depth=1`：列出目录下的子项；`depth=0`：只返回该资源本身
  - 资源不存在 → `404 {"ok":false,"error":"not_found"}`
  - 正常 → `200 {"ok":true,"items":[{href,contentLength,contentType,lastModified,isDir,hasThumb,hasPreview,hasPreviewVideo}, ...]}`
  - 目录列举会**跳过派生文件**（`.thumb.jpg` / `.preview.jpg` / `.preview.mp4`
    结尾）和上传中间文件（`.part`；深度 0 的单文件查询不受影响，小程序加载
    缩略图/压缩视频仍可用）；避免派生文件被当成普通文件、被再次生成链条。
  - 每个文件项额外带 `hasThumb` / `hasPreview` / `hasPreviewVideo` 布尔标记
    （服务端 `stat` 派生兄弟文件得出）：小程序拿到列表即可决定下载哪个派生图，
    省掉每张图一次 `depth=0` 存在性探测请求。
- `GET /cgi-bin/dav-bridge.cgi?op=mkdir&path=<url-encoded>`
  - 创建成功 → `201`；已存在 → `405`（与 WebDAV `MKCOL` 语义一致）
- `POST /cgi-bin/dav-bridge.cgi?op=upload&path=<url-encoded>`
  - multipart/form-data，文件字段名 `file`（微信 `wx.uploadFile` 默认字段名）
  - 创建成功 → `201 {"ok":true,"items":[],"path":"/dav/.../<最终文件名>","size":<字节>,"md5":"<32hex>"}`；
    超过 512MB → `413`；格式错误 → `400`
  - 可选完整性参数 `md5=<32hex>` 与 `size=<字节>`（客户端 `wx.getFileInfo`
    提供）：服务端边写边算，大小/哈希不一致 → `400 size mismatch` /
    `400 checksum mismatch`（temp 直接删除不发布，损坏文件不会出现在列表）；
    `size` 超 512MB → `413`
  - **服务端原子命名（PKG_RELEASE=5）**：目标名以 `O_EXCL` 方式创建
    `<name>.part`，重名自动加 `-1`、`-2` 后缀（插在扩展名之前），再
    `renameat2(RENAME_NOREPLACE)` 发布；并发的同名上传不会互相覆盖，
    `path` 字段返回最终落盘路径，小程序据此派生缩略图/预览图
  - **流式写盘**：multipart 边收边写（固定 64KB 缓冲），不再把整个请求体
    读进内存，并发大文件上传不会把板子内存吃满
  - 半截文件不会以正式文件名出现（写 `.part` 期间列表不可见）
- `DELETE /cgi-bin/dav-bridge.cgi?op=delete&path=<url-encoded>`
  - 删除主文件并**连带删除派生文件**（`.thumb.jpg` / `.preview.jpg` /
    `.preview.mp4`），一次请求代替客户端原来逐张发的 4 次 DELETE
  - 成功 → `204`；不存在 → `404`（客户端按幂等成功处理）；目录 → `405`
  - 删除后 `fsync` 目录，目录项修改落盘

上传/下载/删除：上传走桥（`wx.request` 无上传进度回调，`wx.uploadFile` 有
`onProgressUpdate`，但它只支持 multipart，所以桥提供 `op=upload`）；
下载仍是小程序合法方法（`GET`），直接由 lighttpd mod_webdav 处理；删除从
PKG_RELEASE=10 起也走桥（一次删主文件 + 派生文件）。

## 上传落盘一致性（PKG_RELEASE=10）

- 发布前 `fsync(fd)`：数据真正落盘后才把 `.part` rename 成正式名，掉电不会
  把截断文件发布成正式名；
- 发布后 `fsync` 父目录：目录项（rename/unlink）落盘，掉电后数据与目录项
  一致；
- MD5 由 libcrypto 计算（新增依赖 `libopenssl`，板子已有：openssl-util 依赖
  它），`make` 时 `-lcrypto` 链接。

## 中断上传的 `.part` 残骸（PKG_RELEASE=9）

- 上传先写 `<name>.part` 再原子发布；CGI 被 kill / 板子掉电 / lighttpd 超时杀
  进程时，`.part` 会残留在 SD 卡上。
- 目录列举已过滤 `.part`，不会出现半截文件或让文件数翻倍；
- 同名 `.part` 被占时，CGI 会检查它的 mtime：超过 1 小时视为上次中断的残骸，
  清掉后重试同名，否则（并发上传正在写）跳到下一个 `-N` 后缀。残留的 `.part`
  不会永久占住名字。

## 认证与安全

- Basic 认证由 lighttpd mod_auth 在 CGI 执行前完成（`auth.require` 里对
  `/cgi-bin/dav-bridge.cgi` 配置与 `/dav/` 相同的用户 `backup`），CGI 只校验
  `REMOTE_USER` 非空；
- 路径必须先以 `/dav` 或 `/dav/` 开头；拒绝 `..` 段；`ls` 用 `realpath`、
  `mkdir` 用“最深已存在祖先的 realpath”双重确认解析结果仍在 `/mnt/sd` 之下，
  防 symlink 逃逸；
- 拒绝 `%00`，路径参数上限 1024 字节；
- `ls`/`mkdir` 只接受 `GET`，`upload` 只接受 `POST`，`delete` 只接受 `DELETE`；
  `REMOTE_USER` 为空直接 401。
- 上传请求体上限 512MB 由 CGI 自身保证（超限 413），客户端（小程序）限制
  500MB/视频；lighttpd 1.4.54 的 `server.max-request-size` 默认 0（不限），
  Web 服务器不构成瓶颈。
- `op=register` 请求体上限 4KB（`MAX_FORM_BYTES`），且在 `malloc` 之前就按
  `Content-Length` 拦截（PKG_RELEASE=9），恶意/异常的超大表单不会先把板子
  内存吃满再被拒绝。

## 并发说明（2026-08-20）

- 多个用户共用账号=多个并发 HTTP 连接，Basic 认证无会话锁，服务端不会互踢；
- 并发上传**不同文件**安全；并发上传**同名文件**由服务端 `O_EXCL` +
  `-N` 后缀保证不覆盖（旧版是先查后写，存在竞态）；
- 删除与上传并发仍有窗口：A 删除时 B 恰好传完同名文件，A 的删除会删掉 B
  的新文件（删除操作本身幂等，界面刷新后以实际结果为准）。

## 大文件上传（2026-08-20 实测）

**坑**：lighttpd 1.4.54 收到 POST 请求体会先把 body 写入
`server.upload-dirs` 的临时文件，再喂给 CGI stdin；OpenWrt 默认
`server.upload-dirs = ( "/tmp" )` 而 /tmp 是 tmpfs（本板 248MB），
427MB 视频传到 ~240MB 时写临时文件失败，lighttpd 直接回 500
（error.log：`write() temp-file /tmp/lighttpd-upload-XXXX failed`），
我们 CGI 的流式写盘根本跑不到。

**修复（已在板子与 `cloud-drive/lighttpd.conf` 生效）**：

```text
server.upload-dirs         = ( "/mnt/sd/.upload", "/tmp" )
server.stream-request-body = 1
```

- `/mnt/sd/.upload` 挂在 SD 卡（大 body 落到有空间的盘），/tmp 兜底；
- `server.stream-request-body = 1` 让 lighttpd 把请求体直接流式转发给
  CGI（mod_cgi 1.4.54 支持），大文件不再产生完整临时文件；
- 300MB LAN 实测通过：201 + 最终路径，/tmp 占用不涨，md5 一致，无
  `.part`/临时文件残留。

## lighttpd 配置（板子 `/etc/lighttpd/lighttpd.conf`）

```text
server.modules = ( ..., "mod_webdav", "mod_cgi" )

alias.url += ( "/cgi-bin/" => "/usr/lib/cgi-bin/" )
cgi.assign = ( ".cgi" => "" )

auth.require = ( ...,
    "/cgi-bin/dav-bridge.cgi" => (
        "method"  => "basic",
        "realm"   => "Cloud",
        "require" => "user=backup"
    ) )
```

安装 `lighttpd-mod-cgi` 后模块 `.so` 位于 `/usr/lib/lighttpd/`，本桥配置不依赖
OpenWrt 默认 conf.d（板子自定义 conf 不 include conf.d）。

## 构建与安装（在 Linux 构建主机）

```sh
# 启用 lighttpd-mod-cgi（板子当前没装）
sed -i 's/# CONFIG_PACKAGE_lighttpd-mod-cgi is not set/CONFIG_PACKAGE_lighttpd-mod-cgi=y/' .config
make package/feeds/packages/lighttpd/compile V=s
make package/network/services/dav-bridge/compile V=s
make package/index V=s
```

ipk 输出（路径随目标板架构变化）：

```text
bin/targets/imx6/generic/packages/lighttpd-mod-cgi_1.4.54-2_arm_cortex-a9_neon.ipk
bin/targets/imx6/generic/packages/dav-bridge_1_arm_cortex-a9_neon.ipk
```

板子上：

```sh
opkg install --force-reinstall /tmp/lighttpd-mod-cgi_*.ipk /tmp/dav-bridge_*.ipk
# 应用新 lighttpd.conf（demo 仓库 cloud-drive/lighttpd.conf），然后：
lighttpd -tt -f /etc/lighttpd/lighttpd.conf && /etc/init.d/lighttpd restart
```

冒烟（Windows 本机，密码为板子当前 WebDAV 密码）：

```powershell
curl.exe -s -u backup:'<密码>' "http://192.168.100.1/cgi-bin/dav-bridge.cgi?op=ls&path=%2Fdav%2Fbackup%2Fandroid%2FDCIM%2F&depth=1"
curl.exe -s -u backup:'<密码>' "https://cy.gcaiyy.xyz/cgi-bin/dav-bridge.cgi?op=ls&path=%2Fdav%2F&depth=0"
curl.exe -s -o NUL -w '%{http_code}' "http://192.168.100.1/cgi-bin/dav-bridge.cgi?op=ls&path=%2Fetc%2F&depth=1"  # 400
curl.exe -s -o NUL -w '%{http_code}' "http://192.168.100.1/cgi-bin/dav-bridge.cgi?op=ls&path=%2Fdav%2F..%2Fetc%2F&depth=1"  # 400
```
