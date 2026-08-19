# 云盘微信小程序（一期）

手机照片/小视频（≤50MB）经 WebDAV 备份到板子（lighttpd mod_webdav），按“年-月”时间线浏览、预览、删除。

> **板子依赖**：微信真机 `wx.request` 不支持 `PROPFIND`/`MKCOL`（开发者工具
> 碰巧放行，真机报 `network argv error`），所以文件列表与建目录走板子上的
> `dav-bridge` CGI（`GET /cgi-bin/dav-bridge.cgi?op=ls|mkdir`，返回 JSON，
> Basic 认证与 WebDAV 相同）；上传/下载/删除仍直连 lighttpd mod_webdav。
> 部署需安装 `dav-bridge` 与 `lighttpd-mod-cgi` 两个 ipk，并把
> `cloud-drive/lighttpd.conf` 应用到板子 `/etc/lighttpd/`。

## 已踩的坑

- `wx.getFileSystemManager().readFile` 传 `encoding: 'binary'` 返回的是**字符串**，
  `wx.request` PUT 会按 UTF-8 编码发送 → 所有 >127 的字节被膨胀成两字节，
  图片/视频落盘即损坏（文件头 `FF D8` 变成 `C3 BF C3 98`）。必须**不传 encoding**，
  让 `readFile` 返回 ArrayBuffer，`wx.request` 才会原样发送字节。

## 打开方式

微信开发者工具 → 导入项目 → 选择本目录。AppID 已配置为 `wx15b3015f0705ec62`；如需换号，修改 `project.config.json` 的 `appid` 并把 `urlCheck` 改回 `true`。

## 合法域名

小程序后台 request/downloadFile 合法域名均配置 `https://cy.gcaiyy.xyz:34443`
（光猫拦截 80/443 等小端口，微信合法域名支持配置端口；配置后请求 URL 必须
严格带 `:34443`，登录页默认值已带）。

## 结构

- `utils/`：auth（Basic 认证）、xml（WebDAV PROPFIND 解析，备用）、dav（WebDAV 客户端，列表/建目录经 dav-bridge）、uploader（上传逻辑）
- `pages/`：login / home（月份列表）/ month（文件网格）/ upload
- `tests/`：Node 单测（纯逻辑，不依赖微信环境）
