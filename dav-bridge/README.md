# dav-bridge

微信小程序 `wx.request` 的方法白名单里没有 `PROPFIND` / `MKCOL`
（真机会直接 `fail`，报 `network argv error`；只有开发者工具碰巧放行）。
本包是一个极小的 C CGI，把小程序需要的这两个 WebDAV 操作转成普通 `GET`
并返回 JSON：

- `GET /cgi-bin/dav-bridge.cgi?op=ls&path=<url-encoded>&depth=0|1`
  - `depth=1`：列出目录下的子项；`depth=0`：只返回该资源本身
  - 资源不存在 → `404 {"ok":false,"error":"not_found"}`
  - 正常 → `200 {"ok":true,"items":[{href,contentLength,contentType,lastModified,isDir}, ...]}`
  - 目录列举会**跳过 `.thumb.jpg` 结尾的缩略图文件**（深度 0 的单文件查询不受影响，
    小程序加载缩略图仍可用）；避免缩略图被当成普通文件、被再次生成链条。
- `GET /cgi-bin/dav-bridge.cgi?op=mkdir&path=<url-encoded>`
  - 创建成功 → `201`；已存在 → `405`（与 WebDAV `MKCOL` 语义一致）
- `POST /cgi-bin/dav-bridge.cgi?op=upload&path=<url-encoded>`
  - multipart/form-data，文件字段名 `file`（微信 `wx.uploadFile` 默认字段名）
  - 创建成功 → `201`；超过 64MB → `413`；格式错误 → `400`
  - 先写 `<target>.part` 再 rename，半截文件不会以正式文件名出现

上传/下载/删除：上传走桥（`wx.request` 无上传进度回调，`wx.uploadFile` 有
`onProgressUpdate`，但它只支持 multipart，所以桥提供 `op=upload`）；
下载/删除仍是小程序合法方法（`GET`/`DELETE`），直接由 lighttpd mod_webdav 处理。

## 认证与安全

- Basic 认证由 lighttpd mod_auth 在 CGI 执行前完成（`auth.require` 里对
  `/cgi-bin/dav-bridge.cgi` 配置与 `/dav/` 相同的用户 `backup`），CGI 只校验
  `REMOTE_USER` 非空；
- 路径必须先以 `/dav` 或 `/dav/` 开头；拒绝 `..` 段；`ls` 用 `realpath`、
  `mkdir` 用“最深已存在祖先的 realpath”双重确认解析结果仍在 `/mnt/sd` 之下，
  防 symlink 逃逸；
- 拒绝 `%00`，路径参数上限 1024 字节；
- `ls`/`mkdir` 只接受 `GET`，`upload` 只接受 `POST`；`REMOTE_USER` 为空直接 401。
- 上传请求体上限 64MB 由 CGI 自身保证（超限 413）；lighttpd 1.4.54 默认请求体
  上限足够（不认 `server.max-request-body-size` 配置键，会告警忽略）。

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
