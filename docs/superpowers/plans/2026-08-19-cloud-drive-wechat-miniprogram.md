# 云盘微信小程序（一期）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让手机微信小程序通过公网 HTTPS（`https://cy.gcaiyy.xyz/dav/`）把照片/小视频备份到板子 SD 卡，并按“年-月”时间线浏览、预览、删除。

**Architecture:** 小程序只讲 WebDAV（`PROPFIND`/`MKCOL`/`PUT`/`GET`/`DELETE` + Basic 认证），后端复用现有 lighttpd 1.4.54 mod_webdav，零代码改动；前置任务把公网 80/443 打通的准备工作做掉（uhttpd 让出 80、acme 证书、防火墙、微信后台域名）。小程序代码放 demo 仓库 `wechat-miniprogram/`，纯逻辑单元用 Node 跑单测，页面交互在微信开发者工具 + 真机验收。

> **修订（2026-08-19）**：微信真机 `wx.request` 方法白名单不含
> `PROPFIND`/`MKCOL`（开发者工具碰巧放行，真机直接 `fail`，报
> `network argv error`）。一期新增板子端最小 C CGI **dav-bridge**
> （Task 12）：文件列表/建目录走 `GET /cgi-bin/dav-bridge.cgi?op=ls|mkdir`
> （返回 JSON，Basic 认证由 lighttpd mod_auth 完成，与 WebDAV 同账号）；
> 上传/下载/删除仍直连 mod_webdav。“后端零代码改动”约束相应放宽为
> “mod_webdav 不动，仅新增独立 CGI 桥”。

**Tech Stack:** 微信小程序原生（JS/WXML/WXSS）、lighttpd 1.4.54 mod_webdav/mod_openssl、acme.sh（Let's Encrypt）、Node（仅测试）、无 npm 依赖。

**Spec:** [docs/superpowers/specs/2026-08-19-cloud-drive-wechat-miniprogram-design.md](\\192.168.100.181\chao\openwrt-19.07\openwrt\docs\superpowers\specs\2026-08-19-cloud-drive-wechat-miniprogram-design.md)

## Global Constraints

- 后端零代码改动；仅 lighttpd 配置与证书。
- 公网入口一律 HTTPS；`/` 根目录不得暴露 SD 卡内容（`server.document-root = /var/www`，`/dav/` 经 alias 到 `/mnt/sd/`）。
- 目录约定：`/dav/backup/android/DCIM/YYYY-MM/<文件名>`；月份取 `wx.chooseMedia` 的 `time` 字段。
- 视频单文件 ≤ 50MB，单次 `PUT`，不做分块。
- 图片一次最多 9 张、视频一次 1 个（微信限制）。
- Basic 认证用户 `backup`；密码与私钥**不得写入任何 git 仓库**。
- 小程序域名：request 与 downloadFile 合法域名均配 `https://cy.gcaiyy.xyz`。
- 代码仓库：demo 仓库 `wechat-miniprogram/`；AppID 先用 `touristappid`，拿到正式 AppID 后替换。
- 板子 SSH 必须带兼容参数（见 AGENTS.md）；Windows 无法交叉编译。

---

### Task 1: 公网前置 —— 防火墙放行 + uhttpd 让出 80 + 密码轮换

**Files:**
- Modify（板子）：`/etc/config/firewall`
- Modify（板子）：`/etc/config/uhttpd`
- Modify（板子）：`/etc/lighttpd/webdav.passwd`

**Interfaces:**
- Produces：wan 放行 tcp 80/443；uhttpd 移到 `0.0.0.0:8081`（LuCI 仍可用）；backup 新密码（只在本地台账记录，不进仓库）。

- [ ] **Step 1: 确认 wan zone 默认 input**

```bash
grep -A5 "config zone" /etc/config/firewall | grep -A4 "option name 'wan'"
# 预期含 option input 'REJECT'
```

- [ ] **Step 2: 放行 wan→80/443**

```bash
uci add firewall rule
uci set firewall.@rule[-1].name='Allow-HTTP-WAN'
uci set firewall.@rule[-1].src='wan'
uci set firewall.@rule[-1].proto='tcp'
uci set firewall.@rule[-1].dest_port='80'
uci set firewall.@rule[-1].target='ACCEPT'
uci add firewall rule
uci set firewall.@rule[-1].name='Allow-HTTPS-WAN'
uci set firewall.@rule[-1].src='wan'
uci set firewall.@rule[-1].proto='tcp'
uci set firewall.@rule[-1].dest_port='443'
uci set firewall.@rule[-1].target='ACCEPT'
uci commit firewall
/etc/init.d/firewall restart
```

预期：无报错；`iptables -L -n | grep 80` 与 `ip6tables -L -n | grep 443` 能看到 ACCEPT 规则。

- [ ] **Step 3: uhttpd 让出 80（LuCI 移到 8081）**

```bash
uci set uhttpd.main.listen_http='0.0.0.0:8081'
uci commit uhttpd
/etc/init.d/uhttpd restart
netstat -tlnp | grep 8081   # uhttpd 在 8081
netstat -tlnp | grep :80    # 80 已空
```

- [ ] **Step 4: 轮换 backup 密码**

在本机（Windows）生成新 apr1 哈希（不要写进任何仓库文件）：

```powershell
openssl passwd -apr1 -salt cloud202 '<新密码>'
```

写回板子（`sh -s` 管道，去 CR）：

```bash
printf 'backup:<新apr1哈希>\n' > /etc/lighttpd/webdav.passwd
chmod 644 /etc/lighttpd/webdav.passwd
```

预期：`head -c 20 /etc/lighttpd/webdav.passwd` 输出 `backup:$apr1$...`；旧密码立即失效（FolderSync 若还挂着会 401，属预期）。

- [ ] **Step 5: 验证 AAAA 记录指向当前 WAN IPv6**

```powershell
Resolve-DnsName cy.gcaiyy.xyz -Type AAAA
```

预期：IP 与板子 `ip -6 addr show eth1` 的全局地址一致（当前 `2408:8266:ba01:48bc:a614:4bff:fe83:79cc`，ddns-go 自动维护）。

- [ ] **Step 6: Commit（仅记录规则说明，不含密码）**

```bash
git -C ../my-openwrt-demo commit --allow-empty -m "cloud-drive: prep public HTTPS (firewall/uhttpd/password rotation)"
```

---

### Task 2: lighttpd 改安全根目录 + 80 监听 + 外部可达性验证

**Files:**
- Modify（板子）：`/etc/lighttpd/lighttpd.conf`
- Create（板子）：`/var/www/`、`/var/www/probe.txt`

**Interfaces:**
- Consumes：Task 1 的防火墙放行。
- Produces：公网 `http://cy.gcaiyy.xyz/probe.txt` 可访问；`/dav/` 仍 401；根路径不暴露 SD。

- [ ] **Step 1: 建安全根目录并写探针文件**

```bash
mkdir -p /var/www
echo 'probe-ok' > /var/www/probe.txt
chmod 755 /var/www
```

- [ ] **Step 2: 写完整 lighttpd.conf（含 80 监听，document-root=/var/www）**

用 `sh -s` heredoc 写 `/etc/lighttpd/lighttpd.conf`（全文如下，注意全部行尾 LF）：

```text
server.modules = (
    "mod_auth",
    "mod_authn_file",
    "mod_alias",
    "mod_accesslog",
    "mod_dirlisting",
    "mod_indexfile",
    "mod_staticfile",
    "mod_openssl",
    "mod_webdav"
)

server.document-root        = "/var/www"
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
}

$SERVER["socket"] == "10.10.10.1:8443" {
    ssl.engine = "enable"
    ssl.pemfile = "/etc/lighttpd/server.pem"
}

$SERVER["socket"] == "0.0.0.0:80" {
}

alias.url += ( "/dav/" => "/mnt/sd/", "/dav" => "/mnt/sd/" )

$HTTP["url"] =~ "^/dav($|/)" {
    webdav.activate = "enable"
}

accesslog.filename           = "/var/log/lighttpd/access.log"

auth.backend                 = "htpasswd"
auth.backend.htpasswd.userfile = "/etc/lighttpd/webdav.passwd"
auth.require = ( "/dav/" => (
    "method"  => "basic",
    "realm"   => "Cloud",
    "require" => "user=backup"
),
    "/dav" => (
    "method"  => "basic",
    "realm"   => "Cloud",
    "require" => "user=backup"
) )

include "/etc/lighttpd/mime.conf"
```

- [ ] **Step 3: 语法检查并重启**

```bash
lighttpd -tt -f /etc/lighttpd/lighttpd.conf && /etc/init.d/lighttpd restart
netstat -tlnp | grep -e :80 -e 8080 -e 8443
```

预期：`0.0.0.0:80`、`192.168.100.1:8080`、`10.10.10.1:8080`、`10.10.10.1:8443` 都在。

- [ ] **Step 4: 本机（Windows）冒烟**

```powershell
curl.exe -s -o NUL -w '%{http_code}' http://192.168.100.1/probe.txt   # 200
curl.exe -s -o NUL -w '%{http_code}' http://192.168.100.1/dav/        # 401
curl.exe -s -o NUL -w '%{http_code}' http://192.168.100.1/            # 403/404，不得列出 SD 内容
```

- [ ] **Step 5: 用户从公网验证 IPv6 可达**

用户手机**关闭 WiFi（用流量）**访问 `http://cy.gcaiyy.xyz/probe.txt`，预期显示 `probe-ok`。失败则检查 ddns AAAA 与防火墙。

- [ ] **Step 6: Commit（demo 仓库 cloud-drive/lighttpd.conf 同步新配置）**

```bash
git -C ../my-openwrt-demo add cloud-drive/lighttpd.conf   # 先把本 Task Step 2 的完整配置写入 demo 仓库 cloud-drive/lighttpd.conf
git -C ../my-openwrt-demo commit -m "cloud-drive: secure document-root /var/www, add public :80"
git -C ../my-openwrt-demo push origin main
```

---

### Task 3: 签发正式证书 + lighttpd 443

**Files:**
- Install（板子）：acme.sh（`/root/.acme.sh/`）
- Create（板子）：`/etc/lighttpd/server.pem`（正式 fullchain+key，先备份自签名版）
- Modify（板子）：`/etc/lighttpd/lighttpd.conf`（加 443 socket）

**Interfaces:**
- Consumes：Task 2 的 80 监听与 `/var/www`。
- Produces：`https://cy.gcaiyy.xyz/dav/` 返回 401；证书在 `/etc/lighttpd/server.pem`。

- [ ] **Step 1: 板子安装 acme.sh（不依赖 opkg）**

```bash
cd /root
wget -q -O - https://get.acme.sh | sh
ln -sf /root/.acme.sh/acme.sh /usr/bin/acme.sh
acme.sh --version
```

若 wget 拉不到，改用构建主机下载后按“二进制传输法”传到板子。

- [ ] **Step 2: HTTP-01 签发（webroot=/var/www）**

```bash
acme.sh --issue -d cy.gcaiyy.xyz -w /var/www --server letsencrypt
```

预期：签发成功，证书位于 `/root/.acme.sh/cy.gcaiyy.xyz/`。失败（如 LE 无法经 IPv6 回源）则改用 DNS-01：

```bash
acme.sh --issue -d cy.gcaiyy.xyz --dns dns_<服务商> --server letsencrypt
# 需用户在板子 ~/.acme.sh/account.conf 或环境变量提供 DNS API Token（不入库）
```

- [ ] **Step 3: 合并正式证书并备份自签名**

```bash
cp /etc/lighttpd/server.pem /etc/lighttpd/server.pem.selfsigned.bak
cat /root/.acme.sh/cy.gcaiyy.xyz/cy.gcaiyy.xyz.key \
    /root/.acme.sh/cy.gcaiyy.xyz/fullchain.cer > /etc/lighttpd/server.pem
chmod 644 /etc/lighttpd/server.pem
```

- [ ] **Step 4: lighttpd 增加公网 443 socket**

在 `$SERVER["socket"] == "0.0.0.0:80" {}` 后追加：

```text
$SERVER["socket"] == "0.0.0.0:443" {
    ssl.engine = "enable"
    ssl.pemfile = "/etc/lighttpd/server.pem"
}
```

若需 IPv6 直连 443（手机走 IPv6 时），再追加（语法用 `lighttpd -tt` 验证，1.4.54 若不认 `[::]` 则跳过、靠 IPv4-mapped）：

```text
$SERVER["socket"] == "[::]:443" {
    ssl.engine = "enable"
    ssl.pemfile = "/etc/lighttpd/server.pem"
}
```

```bash
lighttpd -tt -f /etc/lighttpd/lighttpd.conf && /etc/init.d/lighttpd restart
netstat -tlnp | grep -e :443
```

- [ ] **Step 5: 验收**

```powershell
curl.exe -s -o NUL -w '%{http_code}' https://cy.gcaiyy.xyz/dav/          # 401
curl.exe -s -u backup:'<新密码>' -X PROPFIND -H 'Depth: 1' -o NUL -w '%{http_code}' https://cy.gcaiyy.xyz/dav/backup/android/DCIM/   # 207
curl.exe -s -o NUL -w '%{http_code}' https://cy.gcaiyy.xyz/              # 403/404，不暴露 SD
```

（本机若无公网 IPv6 直连，用用户手机流量复测。）

- [ ] **Step 6: 配置 acme 续期（cron 自动）**

```bash
crontab -l | grep acme.sh || { (crontab -l 2>/dev/null; echo '0 3 * * * /root/.acme.sh/acme.sh --cron --home /root/.acme.sh >/dev/null') | crontab -; }
```

- [ ] **Step 7: Commit**

```bash
git -C ../my-openwrt-demo add cloud-drive/lighttpd.conf
git -C ../my-openwrt-demo commit -m "cloud-drive: public HTTPS 443 with acme cert"
git -C ../my-openwrt-demo push origin main
```

---

### Task 4: 微信小程序后台配置（用户为主，Codex 放校验文件）

**Files:**
- Create（板子）：`/var/www/<微信校验文件名>`

**Interfaces:**
- Produces：AppID（替换 `touristappid`）；request/downloadFile 合法域名配置完成；域名归属校验通过。

- [ ] **Step 1: 注册小程序并拿 AppID（用户）**

微信公众平台注册小程序（个人主体免费）→ 开发管理 → 开发设置 → 复制 AppID。微信开发者工具安装到本机 Windows。

- [ ] **Step 2: 配置合法域名（用户）**

小程序后台 → 开发设置 → 服务器域名：request 合法域名填 `https://cy.gcaiyy.xyz`，downloadFile 合法域名填 `https://cy.gcaiyy.xyz`，并保存。

- [ ] **Step 3: 域名归属校验（Codex 放文件 + 用户点验证）**

后台下载校验文件（如 `xxx.txt`）→ 传到板子：

```bash
scp 校验文件 root@192.168.100.1:/var/www/
```

用户后台点“验证”。预期通过（`https://cy.gcaiyy.xyz/xxx.txt` 可访问）。

- [ ] **Step 4: Commit**

```bash
git -C ../my-openwrt-demo commit --allow-empty -m "cloud-drive: wechat domain verification done"
```

---

### Task 5: 小程序工程骨架

**Files:**
- Create：`../my-openwrt-demo/wechat-miniprogram/project.config.json`
- Create：`../my-openwrt-demo/wechat-miniprogram/app.json`
- Create：`../my-openwrt-demo/wechat-miniprogram/app.js`
- Create：`../my-openwrt-demo/wechat-miniprogram/app.wxss`
- Create：`../my-openwrt-demo/wechat-miniprogram/README.md`

**Interfaces:**
- Produces：`App()` 全局会话存取（`setSession/getSession`），页面注册表。

- [ ] **Step 1: project.config.json**

```json
{
  "description": "云盘微信小程序（一期）",
  "miniprogramRoot": "./",
  "appid": "touristappid",
  "compileType": "miniprogram",
  "setting": {
    "es6": true,
    "postcss": true,
    "minified": true,
    "urlCheck": false
  }
}
```

（拿到正式 AppID 后替换 `appid`，并把 `urlCheck` 改回 `true`。）

- [ ] **Step 2: app.json**

```json
{
  "pages": [
    "pages/login/login",
    "pages/home/home",
    "pages/month/month",
    "pages/upload/upload"
  ],
  "window": {
    "navigationBarTitleText": "云盘",
    "navigationBarBackgroundColor": "#07c160",
    "navigationBarTextStyle": "white",
    "backgroundColor": "#f6f6f6"
  },
  "style": "v2",
  "sitemapLocation": "sitemap.json"
}
```

同时创建 `sitemap.json`：`{"rules":[{"action":"disallow","page":"*"}]}`。

- [ ] **Step 3: app.js**

```js
const auth = require('./utils/auth');

App({
  globalData: { session: null },
  onLaunch() {
    this.globalData.session = auth.loadSession();
  },
  setSession(s) {
    this.globalData.session = s;
    auth.saveSession(s);
  },
  getSession() {
    return this.globalData.session;
  },
  clearSession() {
    this.globalData.session = null;
    auth.clearSession();
  }
});
```

- [ ] **Step 4: app.wxss**（全局基础样式）

```css
page { background: #f6f6f6; font-size: 28rpx; color: #333; }
.btn-primary { background: #07c160; color: #fff; border-radius: 12rpx; }
.card { background: #fff; border-radius: 12rpx; padding: 24rpx; margin: 20rpx; }
```

- [ ] **Step 5: README.md**（打开方式：微信开发者工具 → 导入项目 → 选择本目录；AppID 说明；合法域名说明）

- [ ] **Step 6: 验证与 Commit**

用户用微信开发者工具导入，能编译无报错（4 个空页面）。提交：

```bash
git -C ../my-openwrt-demo add wechat-miniprogram
git -C ../my-openwrt-demo commit -m "feat(miniprogram): scaffold project"
git -C ../my-openwrt-demo push origin main
```

---

### Task 6: utils 纯逻辑 + Node 单测（TDD）

**Files:**
- Create：`../my-openwrt-demo/wechat-miniprogram/utils/auth.js`
- Create：`../my-openwrt-demo/wechat-miniprogram/utils/xml.js`
- Create：`../my-openwrt-demo/wechat-miniprogram/utils/dav.js`
- Create：`../my-openwrt-demo/wechat-miniprogram/utils/uploader.js`
- Create：`../my-openwrt-demo/wechat-miniprogram/tests/test-auth.js`
- Create：`../my-openwrt-demo/wechat-miniprogram/tests/test-xml.js`
- Create：`../my-openwrt-demo/wechat-miniprogram/tests/test-dav.js`
- Create：`../my-openwrt-demo/wechat-miniprogram/tests/test-uploader.js`

**Interfaces:**
- Produces：`auth.base64Encode/makeAuthHeader/saveSession/loadSession/clearSession`；`xml.parsePropfind`；`dav.createDav({baseUrl, authHeader, request}) → {urlFor, propfind, mkcol, put, del}`；`uploader.{MAX_VIDEO_BYTES, monthDir, contentTypeFor, uniquePath, uploadFiles, makeFileName}`。

- [ ] **Step 1: 写失败测试（auth）**

`tests/test-auth.js`：

```js
const assert = require('assert');
const auth = require('../utils/auth');

function testBase64() {
  assert.strictEqual(auth.base64Encode(''), '');
  assert.strictEqual(auth.base64Encode('hello'), 'aGVsbG8=');
  const expected = Buffer.from('backup:Backup@2026', 'utf8').toString('base64');
  assert.strictEqual(auth.base64Encode('backup:Backup@2026'), expected);
  assert.strictEqual(auth.makeAuthHeader('u', 'p'), 'Basic ' + Buffer.from('u:p').toString('base64'));
}

testBase64();
console.log('test-auth OK');
```

- [ ] **Step 2: 运行确认失败（`Cannot find module`）→ 实现 auth.js**

```js
const KEY = 'davSession';

function utf8Bytes(str) {
  const enc = unescape(encodeURIComponent(str));
  const bytes = new Uint8Array(enc.length);
  for (let i = 0; i < enc.length; i++) bytes[i] = enc.charCodeAt(i);
  return bytes;
}

function base64Encode(str) {
  const b64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  const bytes = utf8Bytes(str);
  let out = '';
  for (let i = 0; i < bytes.length; i += 3) {
    const b0 = bytes[i];
    const b1 = bytes[i + 1] || 0;
    const b2 = bytes[i + 2] || 0;
    out += b64[b0 >> 2];
    out += b64[((b0 & 3) << 4) | (b1 >> 4)];
    out += i + 1 < bytes.length ? b64[((b1 & 15) << 2) | (b2 >> 6)] : '=';
    out += i + 2 < bytes.length ? b64[b2 & 63] : '=';
  }
  return out;
}

function makeAuthHeader(user, pass) {
  return 'Basic ' + base64Encode(user + ':' + pass);
}

function saveSession(s) { if (typeof wx !== 'undefined') wx.setStorageSync(KEY, s); }
function loadSession() { return typeof wx !== 'undefined' ? (wx.getStorageSync(KEY) || null) : null; }
function clearSession() { if (typeof wx !== 'undefined') wx.removeStorageSync(KEY); }

module.exports = { base64Encode, makeAuthHeader, saveSession, loadSession, clearSession };
```

运行 `node tests/test-auth.js` 预期 PASS。

- [ ] **Step 3: 写失败测试（xml）→ 实现 xml.js**

`tests/test-xml.js`（样本取自板子真实 PROPFIND 响应，仅截取关键结构）：

```js
const assert = require('assert');
const { parsePropfind } = require('../utils/xml');

const SAMPLE = `<?xml version="1.0" encoding="utf-8"?>
<D:multistatus xmlns:D="DAV:" xmlns:ns0="urn:uuid:c2f41010-65b3-11d1-a29f-00aa00c14882/">
<D:response><D:href>/dav/backup/android/DCIM/</D:href><D:propstat><D:prop><D:resourcetype><D:collection/></D:resourcetype></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>
<D:response><D:href>/dav/backup/android/DCIM/2026-08/</D:href><D:propstat><D:prop><D:resourcetype><D:collection/></D:resourcetype></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>
<D:response><D:href>/dav/backup/android/DCIM/2026-08/a.jpg</D:href><D:propstat><D:prop><D:getcontentlength>12345</D:getcontentlength><D:getcontenttype>image/jpeg</D:getcontenttype></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>
</D:multistatus>`;

const items = parsePropfind(SAMPLE);
assert.strictEqual(items.length, 3);
assert.strictEqual(items[1].href, '/dav/backup/android/DCIM/2026-08/');
assert.strictEqual(items[1].isDir, true);
assert.strictEqual(items[2].isDir, false);
assert.strictEqual(items[2].contentLength, 12345);
assert.strictEqual(items[2].contentType, 'image/jpeg');
console.log('test-xml OK');
```

`utils/xml.js`（注：自研轻量解析替代 spec 5.3 的 fast-xml-parser，避免 npm 构建步骤；spec 的“仅解析受控 XML”前提成立，后续要支持任意 WebDAV 服务端再换库）：

```js
function parsePropfind(xml) {
  const out = [];
  const resp = /<(?:\w+:)?response>([\s\S]*?)<\/(?:\w+:)?response>/g;
  let m;
  while ((m = resp.exec(xml))) {
    const block = m[1];
    const href = block.match(/<(?:\w+:)?href>([\s\S]*?)<\/(?:\w+:)?href>/);
    if (!href) continue;
    const len = block.match(/<(?:\w+:)?getcontentlength>(\d+)<\/(?:\w+:)?getcontentlength>/);
    const type = block.match(/<(?:\w+:)?getcontenttype>([\s\S]*?)<\/(?:\w+:)?getcontenttype>/);
    const lm = block.match(/<(?:\w+:)?getlastmodified[^>]*>([\s\S]*?)<\/(?:\w+:)?getlastmodified>/);
    const isDir = /<(?:\w+:)?resourcetype>[\s\S]*?<(?:\w+:)?collection\s*\/>/.test(block);
    out.push({
      href: href[1].trim(),
      contentLength: len ? parseInt(len[1], 10) : null,
      contentType: type ? type[1].trim() : '',
      lastModified: lm ? lm[1].trim() : '',
      isDir
    });
  }
  return out;
}

module.exports = { parsePropfind };
```

- [ ] **Step 4: 写失败测试（dav）→ 实现 dav.js**

`tests/test-dav.js`：

```js
const assert = require('assert');
const { createDav } = require('../utils/dav');

function mockRequest(handler) {
  return function request(opts) {
    return Promise.resolve(handler(opts));
  };
}

async function main() {
  const calls = [];
  const dav = createDav({
    baseUrl: 'https://cy.gcaiyy.xyz',
    authHeader: 'Basic eDp4',
    request: mockRequest((opts) => {
      calls.push(opts);
      return { statusCode: opts.method === 'PROPFIND' ? 207 : 201, data: '<D:multistatus/>' };
    })
  });

  await dav.propfind('/dav/backup/android/DCIM/', 1);
  assert.strictEqual(calls[0].method, 'PROPFIND');
  assert.strictEqual(calls[0].url, 'https://cy.gcaiyy.xyz/dav/backup/android/DCIM/');
  assert.strictEqual(calls[0].header.Authorization, 'Basic eDp4');
  assert.strictEqual(calls[0].header.Depth, '1');

  await dav.put('/dav/a.jpg', new Uint8Array([1, 2, 3]).buffer, 'image/jpeg');
  assert.strictEqual(calls[1].method, 'PUT');
  assert.strictEqual(calls[1].header['Content-Type'], 'image/jpeg');

  const missing = createDav({ baseUrl: 'https://x', authHeader: 'B', request: mockRequest(() => ({ statusCode: 404, data: '' })) });
  assert.strictEqual(await missing.propfind('/nope', 0), null);

  const unauth = createDav({ baseUrl: 'https://x', authHeader: 'B', request: mockRequest(() => ({ statusCode: 401, data: '' })) });
  let threw = null;
  try { await unauth.propfind('/dav/', 1); } catch (e) { threw = e; }
  assert.strictEqual(threw.code, 401);
  console.log('test-dav OK');
}

main();
```

`utils/dav.js`：

```js
const { parsePropfind } = require('./xml');

function createDav({ baseUrl, authHeader, request }) {
  function urlFor(path) {
    const base = baseUrl.replace(/\/+$/, '');
    return base + (path.charAt(0) === '/' ? path : '/' + path);
  }

  async function call(method, path, opts = {}) {
    const res = await request({
      url: urlFor(path),
      method,
      header: Object.assign({ Authorization: authHeader }, opts.header || {}),
      data: opts.data,
      timeout: opts.timeout || 60000
    });
    return res;
  }

  function httpError(code) {
    const err = new Error('HTTP ' + code);
    err.code = code;
    return err;
  }

  async function propfind(path, depth) {
    const res = await call('PROPFIND', path, { header: { Depth: String(depth) } });
    if (res.statusCode === 207) return parsePropfind(res.data);
    if (res.statusCode === 404) return null;
    throw httpError(res.statusCode);
  }

  async function mkcol(path) {
    const res = await call('MKCOL', path);
    if (res.statusCode === 201 || res.statusCode === 405 || res.statusCode === 301) return true;
    throw httpError(res.statusCode);
  }

  async function put(path, arrayBuffer, contentType) {
    const res = await call('PUT', path, { data: arrayBuffer, header: { 'Content-Type': contentType } });
    if (res.statusCode === 201 || res.statusCode === 204) return res.statusCode;
    throw httpError(res.statusCode);
  }

  async function del(path) {
    const res = await call('DELETE', path);
    if (res.statusCode === 204 || res.statusCode === 404) return true;
    throw httpError(res.statusCode);
  }

  return { urlFor, propfind, mkcol, put, del };
}

module.exports = { createDav };
```

- [ ] **Step 5: 写失败测试（uploader）→ 实现 uploader.js**

`tests/test-uploader.js`：

```js
const assert = require('assert');
const { MAX_VIDEO_BYTES, monthDir, contentTypeFor, uniquePath, uploadFiles, makeFileName } = require('../utils/uploader');

function testMonthDir() {
  assert.strictEqual(monthDir(new Date(2026, 7, 18, 10, 30).getTime()), '2026-08');
  assert.strictEqual(monthDir(new Date(2027, 0, 1).getTime()), '2027-01');
}

function testContentType() {
  assert.strictEqual(contentTypeFor('a.JPG'), 'image/jpeg');
  assert.strictEqual(contentTypeFor('v.mp4'), 'video/mp4');
  assert.strictEqual(contentTypeFor('x.unknownext'), 'application/octet-stream');
}

function testMakeFileName() {
  const t = new Date(2026, 7, 18, 10, 30, 5).getTime();
  assert.strictEqual(makeFileName('image', t), 'IMG_20260818_103005.jpg');
  assert.strictEqual(makeFileName('image', t, 'png'), 'IMG_20260818_103005.png');
  assert.strictEqual(makeFileName('video', t, 'mov'), 'VID_20260818_103005.mov');
  assert.strictEqual(makeFileName('video', t), 'VID_20260818_103005.mp4');
}

function testUniquePath() {
  let n = 0;
  const dav = { propfind: async () => (n++ < 1 ? [{ href: 'x' }] : null) };
  return uniquePath(dav, '/dav/m', 'a.jpg').then((p) => {
    assert.strictEqual(p, '/dav/m/a-1.jpg');
  });
}

async function testUploadFiles() {
  const log = [];
  const dav = {
    mkcol: async (p) => { log.push('mkcol:' + p); },
    propfind: async () => null,
    put: async (p) => { log.push('put:' + p); }
  };
  const progress = [];
  const files = [
    { name: 'a.jpg', type: 'image', size: 100, time: new Date(2026, 7, 18).getTime(), arrayBuffer: new Uint8Array(1).buffer },
    { name: 'v.mp4', type: 'video', size: 1024, time: new Date(2026, 7, 19).getTime(), arrayBuffer: new Uint8Array(1).buffer }
  ];
  await uploadFiles({ dav, files, onProgress: (i, t) => progress.push(i + '/' + t) });
  assert.deepStrictEqual(log, [
    'mkcol:/dav/backup/android/DCIM/2026-08',
    'put:/dav/backup/android/DCIM/2026-08/a.jpg',
    'mkcol:/dav/backup/android/DCIM/2026-08',
    'put:/dav/backup/android/DCIM/2026-08/v.mp4'
  ]);
  assert.deepStrictEqual(progress, ['1/2', '2/2']);

  const big = { name: 'b.mp4', type: 'video', size: MAX_VIDEO_BYTES + 1, time: Date.now(), arrayBuffer: new Uint8Array(1).buffer };
  let err = null;
  try { await uploadFiles({ dav, files: [big], onProgress: () => {} }); } catch (e) { err = e; }
  assert.strictEqual(err.code, 'TOO_LARGE');
}

testMonthDir(); testContentType(); testMakeFileName();
testUniquePath().then(testUploadFiles).then(() => console.log('test-uploader OK'));
```

`utils/uploader.js`：

```js
const MAX_VIDEO_BYTES = 50 * 1024 * 1024;

function pad(n) { return n < 10 ? '0' + n : '' + n; }

function monthDir(timeMs) {
  const d = new Date(timeMs);
  return d.getFullYear() + '-' + pad(d.getMonth() + 1);
}

function makeFileName(type, timeMs, ext) {
  const d = new Date(timeMs);
  const ts = '' + d.getFullYear() + pad(d.getMonth() + 1) + pad(d.getDate()) + '_' + pad(d.getHours()) + pad(d.getMinutes()) + pad(d.getSeconds());
  const suffix = ext || (type === 'video' ? 'mp4' : 'jpg');
  return (type === 'video' ? 'VID_' : 'IMG_') + ts + '.' + suffix;
}

function contentTypeFor(name) {
  const ext = name.split('.').pop().toLowerCase();
  const map = {
    jpg: 'image/jpeg', jpeg: 'image/jpeg', png: 'image/png', gif: 'image/gif',
    heic: 'image/heic', webp: 'image/webp',
    mp4: 'video/mp4', mov: 'video/quicktime', m4v: 'video/mp4', mkv: 'video/x-matroska'
  };
  return map[ext] || 'application/octet-stream';
}

async function uniquePath(dav, dirPath, name) {
  let candidate = name;
  let n = 1;
  for (;;) {
    const found = await dav.propfind(dirPath + '/' + candidate, 0);
    if (!found || found.length === 0) return dirPath + '/' + candidate;
    const dot = name.lastIndexOf('.');
    const base = dot > 0 ? name.slice(0, dot) : name;
    const ext = dot > 0 ? name.slice(dot) : '';
    candidate = base + '-' + n + ext;
    n += 1;
  }
}

async function uploadFiles({ dav, files, onProgress }) {
  const total = files.length;
  for (let i = 0; i < total; i++) {
    const f = files[i];
    if (f.type === 'video' && f.size > MAX_VIDEO_BYTES) {
      throw Object.assign(new Error('视频超过 50MB 限制: ' + f.name), { code: 'TOO_LARGE', file: f });
    }
    const dir = monthDir(f.time);
    await dav.mkcol('/dav/backup/android/DCIM/' + dir);
    const path = await uniquePath(dav, '/dav/backup/android/DCIM/' + dir, f.name);
    await dav.put(path, f.arrayBuffer, contentTypeFor(f.name));
    if (onProgress) onProgress(i + 1, total, f);
  }
  return total;
}

module.exports = { MAX_VIDEO_BYTES, monthDir, makeFileName, contentTypeFor, uniquePath, uploadFiles };
```

- [ ] **Step 6: 全部单测通过并提交**

```bash
node tests/test-auth.js
node tests/test-xml.js
node tests/test-dav.js
node tests/test-uploader.js
```

预期 4 个文件均打印 `OK`。提交：

```bash
git -C ../my-openwrt-demo add wechat-miniprogram/utils wechat-miniprogram/tests
git -C ../my-openwrt-demo commit -m "feat(miniprogram): webdav utils with unit tests"
git -C ../my-openwrt-demo push origin main
```

---

### Task 7: 登录页 + 全局会话

**Files:**
- Create：`../my-openwrt-demo/wechat-miniprogram/utils/wxreq.js`
- Create：`../my-openwrt-demo/wechat-miniprogram/pages/login/*`（js/wxml/wxss/json）

**Interfaces:**
- Consumes：Task 6 的 `auth.*`、`dav.createDav`。
- Produces：登录成功后 `app.setSession({baseUrl,user,pass})` 并跳转 `pages/home/home`。

- [ ] **Step 1: utils/wxreq.js（wx.request 适配为 Promise）**

```js
function wxRequest(opts) {
  return new Promise((resolve, reject) => {
    wx.request({
      url: opts.url,
      method: opts.method,
      data: opts.data,
      header: opts.header,
      timeout: opts.timeout,
      success: (res) => resolve({ statusCode: res.statusCode, data: res.data }),
      fail: (err) => reject(new Error(err.errMsg || 'network error'))
    });
  });
}

module.exports = { wxRequest };
```

- [ ] **Step 2: login.js**

```js
const auth = require('../../utils/auth');
const { createDav } = require('../../utils/dav');
const { wxRequest } = require('../../utils/wxreq');

Page({
  data: {
    baseUrl: 'https://cy.gcaiyy.xyz',
    user: 'backup',
    pass: '',
    loading: false
  },
  onLoad() {
    const s = getApp().getSession();
    if (s) this.setData({ baseUrl: s.baseUrl, user: s.user });
  },
  onInput(e) {
    this.setData({ [e.currentTarget.dataset.field]: e.detail.value });
  },
  async onSubmit() {
    const { baseUrl, user, pass } = this.data;
    if (!baseUrl || !user || !pass) {
      wx.showToast({ title: '请填写完整', icon: 'none' });
      return;
    }
    this.setData({ loading: true });
    try {
      const authHeader = auth.makeAuthHeader(user, pass);
      const dav = createDav({ baseUrl, authHeader, request: wxRequest });
      await dav.propfind('/dav/backup/android/DCIM/', 1);
      getApp().setSession({ baseUrl, user, pass });
      wx.showToast({ title: '登录成功', icon: 'success' });
      wx.reLaunch({ url: '/pages/home/home' });
    } catch (e) {
      const msg = e.code === 401 ? '账号或密码错误' : (e.message || '连接失败');
      wx.showToast({ title: msg, icon: 'none' });
    } finally {
      this.setData({ loading: false });
    }
  }
});
```

- [ ] **Step 3: login.wxml**

```xml
<view class="card">
  <input placeholder="服务器地址" value="{{baseUrl}}" data-field="baseUrl" bindinput="onInput" />
  <input placeholder="账号" value="{{user}}" data-field="user" bindinput="onInput" />
  <input placeholder="密码" password value="{{pass}}" data-field="pass" bindinput="onInput" />
  <button class="btn-primary" bindtap="onSubmit" disabled="{{loading}}">登录</button>
</view>
```

`login.wxss` 提供输入框间距；`login.json`：`{"navigationBarTitleText":"登录"}`。

- [ ] **Step 4: 验证与 Commit**

用户开发者工具编译，登录页显示；用 `https://cy.gcaiyy.xyz` + 新密码登录（依赖 Task 3/4 完成）；401 提示正确。提交：

```bash
git -C ../my-openwrt-demo add wechat-miniprogram/utils/wxreq.js wechat-miniprogram/pages/login
git -C ../my-openwrt-demo commit -m "feat(miniprogram): login page"
git -C ../my-openwrt-demo push origin main
```

---

### Task 8: 主页（月份列表）

**Files:**
- Create：`../my-openwrt-demo/wechat-miniprogram/pages/home/*`

**Interfaces:**
- Consumes：Task 7 的会话；Task 6 的 `dav`/`wxreq`。
- Produces：`home.data.months = [{name:'2026-08', count:12}]`；上传按钮跳 `pages/upload/upload`。

- [ ] **Step 1: home.js**

```js
const { createDav } = require('../../utils/dav');
const { wxRequest } = require('../../utils/wxreq');

function getDav() {
  const s = getApp().getSession();
  return createDav({ baseUrl: s.baseUrl, authHeader: 'Basic ' + require('../../utils/auth').base64Encode(s.user + ':' + s.pass), request: wxRequest });
}

Page({
  data: { months: [], loading: false },
  onShow() {
    const s = getApp().getSession();
    if (!s) { wx.reLaunch({ url: '/pages/login/login' }); return; }
    this.loadMonths();
  },
  async loadMonths() {
    this.setData({ loading: true });
    try {
      const dav = getDav();
      const root = '/dav/backup/android/DCIM/';
      const items = await dav.propfind(root, 1) || [];
      const months = [];
      for (const it of items) {
        if (!it.isDir || it.href === root) continue;
        const name = it.href.split('/').filter(Boolean).pop();
        const files = await dav.propfind(it.href, 1) || [];
        const count = files.filter((f) => !f.isDir).length;
        months.push({ name, count, path: it.href });
      }
      months.sort((a, b) => (a.name < b.name ? 1 : -1));
      this.setData({ months });
    } catch (e) {
      if (e.code === 401) { wx.reLaunch({ url: '/pages/login/login' }); return; }
      wx.showToast({ title: e.message || '加载失败', icon: 'none' });
    } finally {
      this.setData({ loading: false });
    }
  },
  onPullDownRefresh() {
    this.loadMonths().finally(() => wx.stopPullDownRefresh());
  },
  onTapMonth(e) {
    wx.navigateTo({ url: '/pages/month/month?path=' + encodeURIComponent(e.currentTarget.dataset.path) });
  },
  onUpload() {
    wx.navigateTo({ url: '/pages/upload/upload' });
  }
});
```

- [ ] **Step 2: home.wxml**

```xml
<view wx:if="{{months.length === 0 && !loading}}" class="empty">还没有备份，点右上角上传</view>
<view class="card" wx:for="{{months}}" wx:key="name" data-path="{{item.path}}" bindtap="onTapMonth">
  <text>{{item.name}}（{{item.count}} 个文件）</text>
</view>
<button class="btn-primary fixed-upload" bindtap="onUpload">上传</button>
```

`home.json`：`{"enablePullDownRefresh": true}`；`home.wxss`：`.empty` 居中、`.fixed-upload` 底部固定。

- [ ] **Step 3: 验证与 Commit**

真机/工具验证：登录后能列出月份与计数（与板子 `ls /mnt/sd/backup/android/DCIM/` 一致）。提交：

```bash
git -C ../my-openwrt-demo add wechat-miniprogram/pages/home
git -C ../my-openwrt-demo commit -m "feat(miniprogram): month list home page"
git -C ../my-openwrt-demo push origin main
```

---

### Task 9: 月视图（网格、预览/播放、删除）

**Files:**
- Create：`../my-openwrt-demo/wechat-miniprogram/pages/month/*`

**Interfaces:**
- Consumes：Task 8 的 `getDav()`（复制同款工厂到本页）；Task 6 的 `dav`。
- Produces：`month.data.files=[{name,size,type,path}]`；删除后列表刷新。

- [ ] **Step 1: month.js**

```js
const { createDav } = require('../../utils/dav');
const { wxRequest } = require('../../utils/wxreq');

function getDav() {
  const s = getApp().getSession();
  return createDav({ baseUrl: s.baseUrl, authHeader: 'Basic ' + require('../../utils/auth').base64Encode(s.user + ':' + s.pass), request: wxRequest });
}

function authHeader() {
  const s = getApp().getSession();
  return 'Basic ' + require('../../utils/auth').base64Encode(s.user + ':' + s.pass);
}

Page({
  data: { dirPath: '', files: [], editing: false, videoSrc: '', playingName: '' },
  onLoad(q) {
    this.setData({ dirPath: decodeURIComponent(q.path) });
  },
  onShow() { this.loadFiles(); },
  async loadFiles() {
    try {
      const dav = getDav();
      const items = await dav.propfind(this.data.dirPath, 1) || [];
      const files = items
        .filter((f) => !f.isDir)
        .map((f) => {
          const name = f.href.split('/').filter(Boolean).pop();
          return {
            name,
            size: f.contentLength,
            type: f.contentType.startsWith('video/') ? 'video' : 'image',
            path: f.href
          };
        })
        .sort((a, b) => (a.name < b.name ? 1 : -1));
      this.setData({ files });
    } catch (e) {
      wx.showToast({ title: e.message || '加载失败', icon: 'none' });
    }
  },
  onToggleEdit() { this.setData({ editing: !this.data.editing }); },
  async onTapFile(e) {
    const f = e.currentTarget.dataset.file;
    if (this.data.editing) { this.onDelete(e); return; }
    wx.showLoading({ title: '下载中' });
    try {
      const s = getApp().getSession();
      const res = await new Promise((resolve, reject) => {
        wx.downloadFile({
          url: s.baseUrl.replace(/\/+$/, '') + f.path,
          header: { Authorization: authHeader() },
          success: resolve,
          fail: reject
        });
      });
      wx.hideLoading();
      if (res.statusCode !== 200) throw new Error('下载失败 ' + res.statusCode);
      if (f.type === 'image') {
        wx.previewImage({ urls: [res.tempFilePath] });
      } else {
        this.setData({ videoSrc: res.tempFilePath, playingName: f.name });
      }
    } catch (e) {
      wx.hideLoading();
      wx.showToast({ title: e.message, icon: 'none' });
    }
  },
  async onDelete(e) {
    const f = e.currentTarget.dataset.file;
    const ok = await new Promise((resolve) => wx.showModal({ title: '删除', content: '确认删除 ' + f.name + '？', success: (r) => resolve(r.confirm) }));
    if (!ok) return;
    try {
      await getDav().del(f.path);
      this.loadFiles();
    } catch (err) {
      wx.showToast({ title: err.message, icon: 'none' });
    }
  }
});
```

- [ ] **Step 2: month.wxml（网格 + 删除 + 视频浮层）**

```xml
<view class="grid">
  <view wx:for="{{files}}" wx:key="name" class="cell" data-file="{{item}}" bindtap="onTapFile">
    <view wx:if="{{item.type === 'image'}}" class="thumb-placeholder">{{item.name}}</view>
    <view wx:else class="thumb-placeholder video">▶ {{item.name}}</view>
  </view>
</view>
<button class="btn-primary" bindtap="onToggleEdit">{{editing ? '完成' : '编辑'}}</button>
<video wx:if="{{videoSrc}}" src="{{videoSrc}}" controls class="video-overlay"></video>
```

（编辑模式下点单元格 = 删除并二次确认；缩略图 v1 用占位，后续可演进为下载小图。）

- [ ] **Step 3: 验证与 Commit**

真机验证：网格列出文件、图片可预览、视频可播放、删除生效。提交：

```bash
git -C ../my-openwrt-demo add wechat-miniprogram/pages/month
git -C ../my-openwrt-demo commit -m "feat(miniprogram): month view with preview/delete"
git -C ../my-openwrt-demo push origin main
```

---

### Task 10: 上传流程

**Files:**
- Create：`../my-openwrt-demo/wechat-miniprogram/pages/upload/*`

**Interfaces:**
- Consumes：Task 6 的 `uploader.*`、Task 8 的 `getDav()` 工厂。
- Produces：上传完成返回主页触发刷新。

- [ ] **Step 1: upload.js**

```js
const { createDav } = require('../../utils/dav');
const { wxRequest } = require('../../utils/wxreq');
const uploader = require('../../utils/uploader');

function getDav() {
  const s = getApp().getSession();
  return createDav({ baseUrl: s.baseUrl, authHeader: 'Basic ' + require('../../utils/auth').base64Encode(s.user + ':' + s.pass), request: wxRequest });
}

Page({
  data: { files: [], done: 0, uploading: false, log: '' },
  pickImage() { this.pick(['image'], 9); },
  pickVideo() { this.pick(['video'], 1); },
  pick(mediaType, count) {
    wx.chooseMedia({
      count,
      mediaType,
      success: (res) => {
        const files = res.tempFiles.map((t) => ({ path: t.tempFilePath, size: t.size, type: t.fileType, time: Date.now() }));
        this.setData({ files: this.data.files.concat(files), done: 0 });
      }
    });
  },
  async startUpload() {
    const list = this.data.files;
    if (!list.length) { wx.showToast({ title: '先选择文件', icon: 'none' }); return; }
    this.setData({ uploading: true, done: 0, log: '' });
    try {
      const fs = wx.getFileSystemManager();
      const prepared = [];
      for (const f of list) {
        const buf = await new Promise((resolve, reject) => {
          fs.readFile({ filePath: f.path, encoding: 'binary', success: (r) => resolve(r.data), fail: reject });
        });
        const ext = (f.path.split('.').pop() || '').toLowerCase() || (f.type === 'video' ? 'mp4' : 'jpg');
        const name = uploader.makeFileName(f.type, f.time, ext);
        prepared.push({ name, type: f.type, size: f.size, time: f.time, arrayBuffer: buf });
      }
      const dav = getDav();
      const total = prepared.length;
      await uploader.uploadFiles({
        dav,
        files: prepared,
        onProgress: (i, n, f) => {
          this.setData({ done: i, log: '正在上传 ' + i + '/' + n + '：' + f.name });
        }
      });
      wx.showToast({ title: '上传完成 ' + total + ' 个', icon: 'success' });
      setTimeout(() => wx.navigateBack(), 800);
    } catch (e) {
      wx.showToast({ title: (e && e.message) || '上传失败', icon: 'none' });
    } finally {
      this.setData({ uploading: false });
    }
  }
});
```

- [ ] **Step 2: upload.wxml**

```xml
<button class="btn-primary" bindtap="pickImage" disabled="{{uploading}}">选照片（最多 9 张）</button>
<button class="btn-primary" bindtap="pickVideo" disabled="{{uploading}}">选视频（1 个，≤50MB）</button>
<view class="card">
  <view wx:for="{{files}}" wx:key="index">{{index + 1}}. {{item.type}}（{{item.size}}B）</view>
</view>
<view wx:if="{{uploading}}">进度：{{done}}/{{files.length}}　{{log}}</view>
<button class="btn-primary" bindtap="startUpload" disabled="{{uploading || files.length === 0}}">开始上传</button>
```

- [ ] **Step 3: 验证与 Commit**

真机：选 9 张照片 + 1 个 ≤50MB 视频上传；板子核对 `find /mnt/sd/backup/android/DCIM -type f | wc -l` 与 md5；>50MB 视频被拦截。提交：

```bash
git -C ../my-openwrt-demo add wechat-miniprogram/pages/upload
git -C ../my-openwrt-demo commit -m "feat(miniprogram): upload flow"
git -C ../my-openwrt-demo push origin main
```

---

### Task 11: 端到端验收与收尾

**Files:**
- Modify（demo 仓库）：`README.md`、`docs/`

**Interfaces:**
- Consumes：全部前置任务。

- [ ] **Step 1: 板子侧核对上传结果**

```bash
find /mnt/sd/backup/android/DCIM -type f | wc -l
ls -lh /mnt/sd/backup/android/DCIM/2026-08 | head
```

抽查 md5：板子上 `md5sum <远端文件>`，与小程序所选原图（用户手机）核对。

- [ ] **Step 2: 安全回归**

```powershell
curl.exe -s -o NUL -w '%{http_code}' https://cy.gcaiyy.xyz/            # 403/404
curl.exe -s -o NUL -w '%{http_code}' https://cy.gcaiyy.xyz/dav/        # 401
curl.exe -s -o NUL -w '%{http_code}' https://cy.gcaiyy.xyz/dav/backup/android/DCIM/2026-08/x.jpg   # 未认证 401
```

- [ ] **Step 3: 文档同步**

把本计划与 spec 复制到 demo 仓库 `docs/` 并提交推送：

```bash
git -C ../my-openwrt-demo add docs wechat-miniprogram/README.md
git -C ../my-openwrt-demo commit -m "docs: cloud drive wechat miniprogram phase1 complete"
git -C ../my-openwrt-demo push origin main
```

- [ ] **Step 4: 收尾告知**

向用户交付：公网 HTTPS 地址、AppID 替换位置、合法域名配置状态、密码轮换记录（仅本地台账）、续期 cron、后续演进（分块上传/API 层）。

### Task 12: dav-bridge CGI 桥（微信 PROPFIND/MKCOL 受限的对策）

**Files:**
- Create（主树）：`package/network/services/dav-bridge/{Makefile,src/dav-bridge.c,README.md}`
- Modify（demo 仓库）：`wechat-miniprogram/utils/dav.js`、`tests/test-dav.js`、`README.md`
- Modify（demo 仓库）：`cloud-drive/lighttpd.conf`（加 `mod_cgi`、`cgi.assign`、`/cgi-bin/dav-bridge.cgi` 的 auth.require）
- Modify（板子）：`/etc/lighttpd/lighttpd.conf`（安装模块后应用同款改动）

**Interfaces:**
- Consumes：Task 1 的防火墙/密码、Task 3 的 443、用户先在 Linux 构建主机编出
  `lighttpd-mod-cgi`（`.config` 需 `CONFIG_PACKAGE_lighttpd-mod-cgi=y`）。
- Produces：小程序 `propfind`/`mkcol` 经 GET JSON 桥完成，`put`/`del`/`downloadFile`
  行为不变；安全上路径越界（`..`、symlink 逃逸）被拒。

- [x] **Step 1: 源码与单测（controller 已完成）** —— dav-bridge 包（C 无依赖，
  仅 libc；`REMOTE_USER` 校验 + `realpath` 根目录围栏 + `%00`/`..` 拒绝）；
  dav.js 的 `propfind`/`mkcol` 改走桥；test-dav.js 更新，4 个 Node 单测全过。
- [ ] **Step 2: 用户构建（Linux 构建主机）**

```sh
sed -i 's/# CONFIG_PACKAGE_lighttpd-mod-cgi is not set/CONFIG_PACKAGE_lighttpd-mod-cgi=y/' .config
make package/feeds/packages/lighttpd/compile V=s
make package/network/services/dav-bridge/compile V=s
make package/index V=s
```

产出 `bin/targets/imx6/generic/packages/{lighttpd-mod-cgi_1.4.54-2_arm_cortex-a9_neon.ipk,dav-bridge_1_arm_cortex-a9_neon.ipk}`。

- [ ] **Step 3: 板子安装 + 应用新 lighttpd.conf**

```sh
opkg install --force-reinstall /tmp/lighttpd-mod-cgi_*.ipk /tmp/dav-bridge_*.ipk
# 用 demo 仓库 cloud-drive/lighttpd.conf 覆盖 /etc/lighttpd/lighttpd.conf
lighttpd -tt -f /etc/lighttpd/lighttpd.conf && /etc/init.d/lighttpd restart
```

- [ ] **Step 4: 验收**

```powershell
curl.exe -s -u backup:'<密码>' "https://cy.gcaiyy.xyz/cgi-bin/dav-bridge.cgi?op=ls&path=%2Fdav%2Fbackup%2Fandroid%2FDCIM%2F&depth=1"   # 200 JSON 列表
curl.exe -s -o NUL -w '%{http_code}' "https://cy.gcaiyy.xyz/cgi-bin/dav-bridge.cgi?op=ls&path=%2Fetc%2F&depth=1"                     # 400
curl.exe -s -o NUL -w '%{http_code}' "https://cy.gcaiyy.xyz/cgi-bin/dav-bridge.cgi?op=ls&path=%2Fdav%2F..%2Fetc%2F&depth=1"          # 400
curl.exe -s -o NUL -w '%{http_code}' "https://cy.gcaiyy.xyz/cgi-bin/dav-bridge.cgi?op=ls&path=%2Fdav%2F&depth=1"                     # 401（无认证）
```

真机复测登录/月份列表/月视图/上传；上传/下载/删除路径不受桥影响。

- [ ] **Step 5: Commit 并推送 demo 仓库（controller）** —— dav-bridge 包目录、
  wechat-miniprogram（含此前漏提交的 utils/tests 与 AppID）、lighttpd.conf、docs。

## 二期（不在本计划）

- 分块上传/断点续传（板子加收分块端点）；
- 缩略图、多用户、分享（JSON API 层）；
- token 登录替代本地密码。
