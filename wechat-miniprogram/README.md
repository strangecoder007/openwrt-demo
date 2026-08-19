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
- `utils/format.js`：字节数格式化（下载进度/确认框显示大小）
- `pages/`：login / register（注册账号）/ home（月份列表）/ month（文件网格）/ upload
- `tests/`：Node 单测（纯逻辑，不依赖微信环境）

## 账号注册

登录页可进入注册页：填新用户名/密码 + **管理员账号密码**（默认 `backup`）。
管理员 Basic 认证由 lighttpd 在 CGI 前完成，`dav-bridge?op=register` 校验
格式后把 `openssl passwd -apr1` 生成的哈希追加进 `/etc/lighttpd/webdav.passwd`。
新账号与管理员共用同一个云盘（`/dav/` 认证为 `valid-user`），注册成功即登录。
部署要求：dav-bridge 新版 ipk（含 register）装到板子，`webdav.passwd`
属主为 `http`，lighttpd conf 已同步（见 `cloud-drive/lighttpd.conf`）。

## 缩略图约定

图片上传时用 `wx.compressImage` 本地压缩两张派生图（同目录同名、扩展名替换）：
- `.thumb.jpg`：480px 缩略图（几 KB～几十 KB），月视图网格用；
- `.preview.jpg`：1280px 预览图（100～300KB），点击全屏预览用，比原图小一个量级。

老文件没有派生图时首次使用会自动下载原图、压缩回传一张，之后秒开。
**视频**：`wx.chooseMedia` 自带封面（`thumbTempFilePath`），上传时一并传为
`.thumb.jpg` 封面，网格显示封面 + ▶ 角标；老视频没有封面时显示 ▶ 占位
（手机端无法截帧，服务端无 ffmpeg）。

## 下载与保存

- 网格缩略图：优先命中本地持久缓存（`USER_DATA_PATH/thumbcache/`，命中零网络
  请求；上限 100MB/500 项，LRU 淘汰最旧）；未命中才下载服务端 `.thumb.jpg`
  并写入缓存。老图首次浏览仍会下载原图压缩一张回传服务端，之后秒开。
- 点图片：下载**当天**全部 `.preview.jpg`（并发 3，显示“加载预览 x/y”；老图
  没有预览图时首次下载原图压缩生成回传）→
  `wx.previewImage` 全屏预览，**可左右滑动**；
- 点视频：下载临时文件 → 播放；
- 月视图支持下拉刷新（重新拉列表 + 缩略图，带防重入）；
- 编辑模式多选 → “下载(n)”：把选中的原图/视频**保存到手机相册**
  （`wx.saveImageToPhotosAlbum` / `wx.saveVideoToPhotosAlbum`，首次需授权相册权限）；
  确认框显示文件数与总大小，保存过程显示“保存中 x/n · 已保存 Y MB”；
  删除时服务端 `.thumb.jpg`/`.preview.jpg` 与本地缓存一并清除。
