# 云盘微信小程序（一期）

手机照片/小视频（≤50MB）经 WebDAV 备份到板子（lighttpd mod_webdav），按“年-月”时间线浏览、预览、删除。

> **板子依赖**：微信真机 `wx.request` 不支持 `PROPFIND`/`MKCOL`（开发者工具
> 碰巧放行，真机报 `network argv error`），所以文件列表与建目录走板子上的
> `dav-bridge` CGI（`GET /cgi-bin/dav-bridge.cgi?op=ls|mkdir`，返回 JSON，
> Basic 认证与 WebDAV 相同）；上传走 `wx.uploadFile`（有上传进度回调）到
> `POST /cgi-bin/dav-bridge.cgi?op=upload`（multipart）；下载/删除仍直连
> lighttpd mod_webdav。
> 部署需安装 `dav-bridge` 与 `lighttpd-mod-cgi` 两个 ipk，并把
> `cloud-drive/lighttpd.conf` 应用到板子 `/etc/lighttpd/`。

## 已踩的坑

- 曾用 `readFile(encoding:'binary')` + `wx.request` PUT 上传，字符串被 UTF-8 编码
  导致文件损坏（文件头 `FF D8` 变 `C3 BF C3 98`）。现改为 `wx.uploadFile` 直接
  传 `tempFilePath`（流式、有进度），不再在 JS 里读整文件。

## 打开方式

微信开发者工具 → 导入项目 → 选择本目录。AppID 已配置为 `wx4a39d6f5c1ac38a9`；如需换号，修改 `project.config.json` 的 `appid` 并把 `urlCheck` 改回 `true`。

## 合法域名

小程序后台 request/downloadFile 合法域名均配置 `https://cy.gcaiyy.xyz:34443`
（光猫拦截 80/443 等小端口，微信合法域名支持配置端口；配置后请求 URL 必须
严格带 `:34443`，登录页默认值已带）。

## 结构

- `utils/`：auth（Basic 认证）、xml（WebDAV PROPFIND 解析，备用）、dav（WebDAV 客户端，列表/建目录经 dav-bridge）、uploader（上传逻辑）
- `pages/`：login / home（月份列表）/ month（文件网格）/ upload
- `tests/`：Node 单测（纯逻辑，不依赖微信环境）

## 缩略图约定

图片上传时用 `wx.compressImage` 本地压缩一张 `.thumb.jpg`（同目录同名、扩展名
替换），月视图优先下载缩略图（几 KB～几十 KB）；老文件没有缩略图时首次浏览会
下载原图并自动压缩回传一张，之后秒开。视频暂无服务端缩略图，网格显示 ▶ 占位。
