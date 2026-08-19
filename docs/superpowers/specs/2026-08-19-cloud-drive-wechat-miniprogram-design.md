# 云盘微信小程序（一期）设计

> 状态：已与用户确认（2026-08-19）。前置公网 HTTPS 与小程序开发会拆进同一份实施计划。

> **修订（2026-08-19，方案 B 最小切片）**：微信真机 `wx.request` 方法白名单
> 不含 `PROPFIND`/`MKCOL`（开发者工具碰巧放行，真机直接 fail，报
> `network argv error`）。一期在板子上新增最小 C CGI **dav-bridge**
> （`package/network/services/dav-bridge/`）：`GET /cgi-bin/dav-bridge.cgi?op=ls|mkdir`
> 返回 JSON，Basic 认证由 lighttpd mod_auth 完成（与 WebDAV 同账号）；
> `PROPFIND`/`MKCOL` 语义由该桥承担，`PUT`/`GET`/`DELETE` 仍直连
> lighttpd mod_webdav。涉及第 3、5、6、9 节的相关描述以本修订为准。
>
> **修订（2026-08-19 二改）**：公网入口端口定为 **34443**（光猫拦截 80/443 等
> 小端口）。微信官方确认合法域名可配置端口（如 `https://域名:8080`），配置后
> 请求必须严格带该端口。小程序后台 request/downloadFile 合法域名、登录页默认
> baseUrl、防火墙、lighttpd socket 均使用 `https://cy.gcaiyy.xyz:34443`。

## 1. 背景与目标

板子（i.MX6ULL / OpenWrt 19.07）已跑通 lighttpd 1.4.54 + mod_webdav：

- `10.10.10.1:8443`（WireGuard 内 HTTPS，自签名）与 `192.168.100.1:8080`（LAN HTTP）；
- URL 前缀 `/dav`（`/dav` 与 `/dav/` 均兼容）→ `/mnt/sd/`；
- Basic 认证用户 `backup`；
- 手机 FolderSync 已把照片备份到 `/mnt/sd/backup/android/DCIM/`。

用户对 FolderSync 体验不满意，希望改为**微信小程序**：打开小程序手动勾选照片/视频上传到板子，按时间线浏览、预览、删除。体验版可接受（域名 `cy.gcaiyy.xyz` 已 ICP 备案）。

## 2. 范围

### 一期做

- 手动批量上传：`wx.chooseMedia` 多选照片（一次最多 9 张）或视频（一次 1 个），逐文件 `PUT`；
- 时间线浏览：按“年-月”目录分组，主页列出月份，月视图网格展示；
- 预览/播放：图片下载到本地后 `wx.previewImage`，视频下载后用 `video` 组件播放；
- 删除：编辑模式下二次确认后 `DELETE`；
- 登录：服务器地址 + 账号 + 密码，Basic 认证，记住密码；
- 错误处理：401 重新登录、网络重试、50MB 限制提示。

### 一期不做（YAGNI）

- 多用户、分享、重命名、移动；
- 自动/定时同步（微信小程序无后台常驻能力）；
- 分块上传/断点续传（视频 ≤50MB 单次 PUT）；
- 服务端缩略图（网格直接用下载到本地的原图；后续可走 API 层演进）；
- 微信云开发（违背自建目标）。

## 3. 总体架构

```
手机微信小程序（原生框架，无云开发）
   │  HTTPS：wx.request / wx.downloadFile
   ▼
lighttpd (mod_webdav + mod_auth + mod_openssl)   ← 板子，后端零改动
   │  Basic 认证（backup）
   ▼
/mnt/sd/backup/android/DCIM/YYYY-MM/xxx.jpg
```

小程序讲 WebDAV 语义（`PROPFIND` / `MKCOL` / `PUT` / `GET` / `DELETE`），其中
`PROPFIND`/`MKCOL` 经板子上的 dav-bridge CGI 转成 GET JSON，`PUT`/`GET`/`DELETE`
直连 lighttpd mod_webdav。lighttpd 除 443 公网证书外，仅新增 mod_cgi 与一条
alias/auth 配置。

## 4. 目录与数据组织

- 上传目标：`/dav/backup/android/DCIM/YYYY-MM/<原始文件名>`；
- 月份取 `wx.chooseMedia` 返回的 `time` 字段（拍摄时间，比文件名可靠）；
- 月目录不存在则 `MKCOL` 创建；已存在（405/301）忽略，幂等；
- 文件名保留原名；上传前对目标路径 `PROPFIND`，已存在则追加 `-1`、`-2` 后缀后重试（避免 vfat 同名覆盖）；
- 月份列表与文件列表均按名字倒序（最新在前）。
- 既有 FolderSync 备份（`DCIM/` 根下的 19 张）不在月目录里，一期不迁移、小程序不展示；如需展示，实施时按拍摄时间一次性归位到对应月目录（可选任务）。

## 5. 小程序端设计

### 5.1 页面

1. **登录页**：服务器地址（默认 `https://cy.gcaiyy.xyz`）、账号（默认 `backup`）、密码；“记住密码”存 `wx.setStorageSync`；登录 = 发一次 `PROPFIND /dav/backup/android/DCIM/`，207 通过、401 提示。
2. **主页（月份列表）**：`PROPFIND` depth 1 拉 `DCIM/` 下的月目录；显示“2026-08（12 个文件）”；下拉刷新；右上角“上传”。
3. **月视图**：`PROPFIND` depth 1 拉该月目录；网格展示；图片/视频混合；编辑模式（复选框）+ 删除（二次确认）。
4. **上传流程**：`wx.chooseMedia` 多选 → 校验大小（视频 ≤50MB）→ 逐个读文件为 ArrayBuffer（`wx.getFileSystemManager().readFile`）→ `wx.request` PUT（带 `Authorization: Basic ...`、`Content-Type`）→ 按“第 x/y 个完成”更新进度 → 完成后刷新月列表。

### 5.2 微信平台约束与对策

- `wx.request` 无字节级上传进度（`onProgressUpdate` 仅 `wx.uploadFile` 有，而它是 multipart 不是 WebDAV）→ 进度做到**文件级**，单文件内显示加载态；
- `wx.previewImage` 不能带自定义头 → 先 `wx.downloadFile`（可带 `Authorization`）到本地临时文件，再预览/播放；
- `wx.chooseMedia` 图片单次最多 9 张、视频单次 1 个 → 多选分批；
- **`wx.request` 方法白名单不含 `PROPFIND`/`MKCOL`**（真机 `network argv error`）→
  文件列表/建目录改走板子 `dav-bridge` CGI（GET + JSON）；`utils/xml.js` 的
  PROPFIND 解析保留为备用实现；
- 合法域名：小程序后台需把 `https://cy.gcaiyy.xyz:34443` 配入 **request 合法域名**与 **downloadFile 合法域名**（两者分开配置）；域名已备案，正式/体验版均可使用；**配置带端口后请求 URL 必须带该端口**（登录页默认值已带）；
- 域名归属校验：按微信要求把校验文件放到 `https://cy.gcaiyy.xyz/` 根路径（前置任务处理）。

### 5.3 PROPFIND 响应解析

小程序无 DOMParser。引入 npm 包 **fast-xml-parser**（微信开发者工具支持 npm 构建），解析 `<D:multistatus>` 下的 `href`、`getcontentlength`、`getcontenttype`、`getlastmodified`。仅解析自己服务器的受控 XML，风险可控。

## 6. WebDAV 交互细节

| 操作 | 方法/URL | 说明 |
| --- | --- | --- |
| 验证/列月份 | `PROPFIND /dav/backup/android/DCIM/`，`Depth: 1` | 207；401 重新登录 |
| 建月目录 | `MKCOL /dav/backup/android/DCIM/2026-08` | 405/301 = 已存在，忽略 |
| 检查重名 | `PROPFIND /dav/.../xxx.jpg`，`Depth: 0` | 404 = 不存在 |
| 上传 | `PUT /dav/.../xxx.jpg`，body=ArrayBuffer | 201 新建；`Content-Type` 按文件类型 |
| 下载 | `wx.downloadFile` → 临时文件 | 预览/播放用 |
| 删除 | `DELETE /dav/.../xxx.jpg` | 204；月目录为空时也可删除 |

- 所有请求带 `Authorization: Basic base64(user:pass)`；
- 超时：`wx.request` timeout 60s；慢速网络下大文件可能超时 → 提示并自动重试 2 次；
- 状态码映射：401 重新登录、403 权限、404 资源不存在、507 磁盘满提示。

## 7. 安全

- Basic 认证仅走 HTTPS（443 正式证书）；
- 密码存本地 storage，自用体验版可接受；二期可演进为 token/扫码登录；
- 删除需二次确认；
- 不暴露明文 HTTP 到公网（LAN 8080 保留仅用于诊断）。

## 8. 前置任务：公网 HTTPS（实施计划最先做）

1. 证书：acme.sh（板子或构建主机）为 `cy.gcaiyy.xyz` 签发；优先 HTTP-01（IPv6 防火墙放行 80/443），DNS 服务商不支持时退 DNS-01；
2. 证书落到 `/etc/lighttpd/`，`server.pem` 换成正式证书（SAN 含 `cy.gcaiyy.xyz`），443 监听复用现有 `$SERVER["socket"]` + `ssl.engine` 配置段；自签名 8443 保留或下线；
3. 防火墙放行 IPv6 80/443；确认 ddns-go 的 AAAA 记录指向板子；
4. 微信小程序后台：配置 request/downloadFile 合法域名、完成域名归属校验；
5. 验收：`curl https://cy.gcaiyy.xyz/dav/` 返回 401；`curl -u backup:... -X PROPFIND` 返回 207；校验文件可访问。

## 9. 工程结构与落位

- 小程序代码放 demo 仓库新目录 `wechat-miniprogram/`（原生小程序，微信开发者工具直接打开；`project.config.json` 用用户自己的 AppID）；
- 板子侧新增 `dav-bridge` CGI（C，仅 libc，无新依赖），证书与 lighttpd 配置照旧；
- 本设计文档与后续实施计划同步到 demo 仓库 `docs/`。

## 10. 验收标准

开发者工具 + 真机（Android，移动网络 IPv6）：

- 登录成功/失败提示正确（401 处理）；
- 主页正确列出月份、月视图正确列出文件（数量与板子 `ls` 一致）；
- 上传照片与 ≤50MB 视频：板子侧文件存在、大小一致（md5 抽查）；同名文件得到 `-1` 后缀不覆盖；
- 图片预览、视频播放正常；
- 删除文件后板子侧消失、列表刷新；
- 断网/超时：有提示、重试行为符合预期；
- 50MB 以上视频被拦截并提示。

## 11. 风险与后续演进

- 慢速上行网络下单文件 60s 超时是最大体验风险；后续可上分块上传（需在板子加收分块的小端点，即 API 层演进）；
- 服务端缩略图/多用户/分享 → 方案 B 的 JSON API 层，另起计划；
- 公开仓库已含一期旧密码，公网开放前轮换。
