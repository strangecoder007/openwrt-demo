# 云盘微信小程序（一期）

手机照片/小视频（≤50MB）经 WebDAV 备份到板子（lighttpd mod_webdav），按“年-月”时间线浏览、预览、删除。

> **板子依赖**：微信真机 `wx.request` 不支持 `PROPFIND`/`MKCOL`（开发者工具
> 碰巧放行，真机报 `network argv error`），所以文件列表与建目录走板子上的
> `dav-bridge` CGI（`GET /cgi-bin/dav-bridge.cgi?op=ls|mkdir`，返回 JSON，
> Basic 认证与 WebDAV 相同）；上传/下载/删除仍直连 lighttpd mod_webdav。
> 部署需安装 `dav-bridge` 与 `lighttpd-mod-cgi` 两个 ipk，并把
> `cloud-drive/lighttpd.conf` 应用到板子 `/etc/lighttpd/`。

## 打开方式

微信开发者工具 → 导入项目 → 选择本目录。AppID 已配置为 `wx15b3015f0705ec62`；如需换号，修改 `project.config.json` 的 `appid` 并把 `urlCheck` 改回 `true`。

## 合法域名

小程序后台 request/downloadFile 合法域名均配置 `https://cy.gcaiyy.xyz`。

## 结构

- `utils/`：auth（Basic 认证）、xml（WebDAV PROPFIND 解析，备用）、dav（WebDAV 客户端，列表/建目录经 dav-bridge）、uploader（上传逻辑）
- `pages/`：login / home（月份列表）/ month（文件网格）/ upload
- `tests/`：Node 单测（纯逻辑，不依赖微信环境）
