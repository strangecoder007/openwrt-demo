# 云盘微信小程序（一期）

手机照片/小视频（≤50MB）经 WebDAV 备份到板子（lighttpd mod_webdav），按“年-月”时间线浏览、预览、删除。

## 打开方式

微信开发者工具 → 导入项目 → 选择本目录。AppID 已配置为 `wx15b3015f0705ec62`；如需换号，修改 `project.config.json` 的 `appid` 并把 `urlCheck` 改回 `true`。

## 合法域名

小程序后台 request/downloadFile 合法域名均配置 `https://cy.gcaiyy.xyz`。

## 结构

- `utils/`：auth（Basic 认证）、xml（PROPFIND 解析）、dav（WebDAV 客户端）、uploader（上传逻辑）
- `pages/`：login / home（月份列表）/ month（文件网格）/ upload
- `tests/`：Node 单测（纯逻辑，不依赖微信环境）
