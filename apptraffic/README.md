# AppTraffic - 应用流量分析工具

## 概述

AppTraffic 是一款面向 OpenWrt 19.07 的深度流量分析工具，能够**识别局域网用户正在使用哪些应用和网站**，并以直观的 Web 图表展示流量分布。

它不仅仅统计带宽用量，而是通过多层协议解析技术，把网络流量"翻译"成用户可理解的**应用名称**（如 YouTube、微信、TikTok、Steam 等）。

---

## 目录结构

```
openwrt-19.07/openwrt/
├── package/network/utils/apptraffic/       ← 后台守护进程（C 语言）
│   ├── Makefile                            # OpenWrt 编译规则
│   ├── src/
│   │   ├── apptraffic.h                    # 头文件（数据结构 + 接口声明）
│   │   ├── main.c                          # 主程序（全部功能实现）
│   │   └── app-mapping.txt                 # 域名→应用映射数据库
│   └── files/
│       ├── apptraffic.init                 # procd 守护进程启动脚本
│       ├── apptraffic.config               # UCI 配置文件模板
│       └── apptraffic.hotplug              # 网口热插拔脚本
│
└── feeds/luci/applications/luci-app-apptraffic/  ← LuCI Web 界面（Lua + JS）
    ├── Makefile                            # LuCI 编译规则
    ├── luasrc/
    │   ├── controller/apptraffic.lua       # 路由控制器（URL 映射 + API）
    │   ├── model/cbi/apptraffic/config.lua # 配置页面（CBI 模型）
    │   └── view/apptraffic/display.htm     # 展示页面（HTML 模板）
    ├── htdocs/luci-static/resources/view/
    │   ├── apptraffic.js                   # 前端交互逻辑（图表 + 表格）
    │   └── apptraffic.css                  # 样式表
    ├── po/
    │   ├── templates/apptraffic.pot        # 翻译模板
    │   └── zh_Hans/apptraffic.po           # 简体中文翻译
    └── root/
        ├── etc/uci-defaults/40_luci-apptraffic    # 首次安装初始化脚本
        └── usr/share/rpcd/acl.d/luci-app-apptraffic.json  # RPC 权限
```

---

## 架构设计

```
┌─────────────────────────────────────────────────────┐
│                    用户浏览器                         │
│               LuCI Web 界面 (192.168.1.1)            │
│          ┌──────────┐  ┌──────────┐                 │
│          │ 饼状图    │  │ 数据表格  │                 │
│          │ 应用/域名 │  │ Top N    │                 │
│          └─────┬────┘  └────┬─────┘                 │
└────────────────┼─────────────┼───────────────────────┘
                 │ AJAX/JSON   │
┌────────────────┼─────────────┼───────────────────────┐
│     LuCI 后端  │             │                       │
│  controller/apptraffic.lua   │                       │
│  ├─ /data          → JSON/CSV 导出                  │
│  ├─ /top_apps      → 按应用统计                     │
│  ├─ /top_domains   → 按域名统计                     │
│  ├─ /top_hosts     → 按设备IP统计                   │
│  └─ /live          → 实时数据                       │
└────────────────┼─────────────────────────────────────┘
                 │ 调用 CLI
┌────────────────┼─────────────────────────────────────┐
│     apptraffic 守护进程 (C)                           │
│                                                      │
│  ┌──────────────┐  ┌──────────────┐  ┌────────────┐ │
│  │ DNS 捕获模块  │  │ TLS SNI 模块 │  │HTTP Host   │ │
│  │ libpcap:53   │  │ libpcap:443  │  │libpcap:80  │ │
│  │ A/AAAA 解析  │  │ ClientHello  │  │Host header │ │
│  └──────┬───────┘  └──────┬───────┘  └─────┬──────┘ │
│         │                 │                 │        │
│         └─────────┬───────┴─────────────────┘        │
│                   ▼                                  │
│          ┌────────────────┐                          │
│          │ IP → 域名 映射  │  内存哈希表 (65536 槽)    │
│          └───────┬────────┘                          │
│                  ▼                                   │
│          ┌────────────────┐                          │
│          │ 域名 → 应用 映射 │  app-mapping.txt        │
│          │ 通配符匹配      │  glob 模式匹配           │
│          └───────┬────────┘                          │
│                  │                                   │
│  ┌───────────────┼────────────────────┐              │
│  │  Conntrack 流量采集模块             │              │
│  │  /proc/net/nf_conntrack            │              │
│  │  源IP/目标IP/端口/字节/包数         │              │
│  └───────────────┬────────────────────┘              │
│                  ▼                                   │
│          ┌────────────────┐                          │
│          │   SQLite 数据库  │  /var/lib/apptraffic/   │
│          │   WAL 模式       │  traffic.db            │
│          └────────────────┘                          │
└──────────────────────────────────────────────────────┘
```

---

## 工作原理详解

### 1. 应用识别 - 三条路径并行

#### 路径 A：DNS 查询捕获（主力）
```
用户设备          路由器              DNS 服务器
   │                 │                    │
   ├─ DNS Query ────►├───────────────────►│  "youtube.com 的 IP 是什么？"
   │                 │                    │
   │                 │◄───────────────────┤  "142.250.80.46"
   │                 │                    │
   │     libpcap 捕获 DNS 响应包           │
   │     解析 A/AAAA 记录                  │
   │     记录: 142.250.80.46 → youtube.com │
```

#### 路径 B：TLS SNI 提取（HTTPS 流量）
```
用户设备                   路由器                      服务器
   │                         │                           │
   ├─ TLS ClientHello ──────►├──────────────────────────►│
   │  (SNI: youtube.com)     │                           │
   │                          │                           │
   │       libpcap 捕获 TCP SYN 后的第一个数据包            │
   │       解析 TLS 握手协议                               │
   │       提取 SNI 扩展字段: youtube.com                   │
   │       记录: 142.250.80.46 → youtube.com               │
```

SNI（Server Name Indication）是 TLS 握手中的必选字段，**明文传输**，即使是 HTTPS 也能看到目标域名。这解决了 DNS over HTTPS（DoH）时代 DNS 捕获失效的问题。

#### 路径 C：HTTP Host 头提取（明文 HTTP）
```
用户设备             路由器                   服务器
   │                   │                        │
   ├─ GET / HTTP/1.1 ─►├───────────────────────►│
   │  Host: bilibili.com                       │
   │                   │                        │
   │     libpcap 捕获 TCP 80 端口数据             │
   │     解析 HTTP 请求头                         │
   │     提取 Host: bilibili.com                 │
```

### 2. 域名 → 应用映射

捕获到域名后，通过通配符模式匹配将其归类到已知应用。映射文件 `app-mapping.txt` 格式如下：

```
# 格式: 域名模式,应用名,分类,优先级
*.youtube.com,YouTube,Video,100
*.googlevideo.com,YouTube,Video,95
*.facebook.com,Facebook,Social Media,100
*.whatsapp.com,WhatsApp,Messaging,100
*.steampowered.com,Steam,Gaming,100
```

匹配规则：
- `*` 匹配任意字符序列（包括 `.`）
- 优先级数值越高的规则优先匹配
- 未匹配的域名，自动提取其二级域名（如 `api.example.com` → `Example`）
- 支持 200+ 预置规则，涵盖社交、视频、购物、游戏、邮箱等 15 个分类

### 3. 流量统计

```
/proc/net/nf_conntrack 读取流程:
┌──────────────────────────────────────────────────┐
│ ipv4 2 tcp 6 300 ESTABLISHED                     │
│ src=192.168.1.100 dst=142.250.80.46              │
│ sport=54321 dport=443                            │
│ packets=150 bytes=12000     ← 上行数据            │
│ src=142.250.80.46 dst=192.168.1.100              │
│ sport=443 dport=54321                            │
│ packets=200 bytes=250000    ← 下行数据            │
│ [ASSURED]                                        │
└──────────────────────────────────────────────────┘

解析后:
  源IP: 192.168.1.100 (局域网设备)
  目标IP: 142.250.80.46 → DNS查表 → youtube.com
  目标端口: 443 → TLS SNI 也查到 → youtube.com
  应用: YouTube
  下载: 250000 字节, 上传: 12000 字节
```

### 4. 数据存储

使用 SQLite 存储，开启 WAL（Write-Ahead Logging）模式：

| 字段 | 说明 |
|------|------|
| timestamp | Unix 时间戳 |
| src_ip | 局域网设备 IP |
| dst_ip | 目标服务器 IP |
| src_port / dst_port | 端口号 |
| protocol | TCP / UDP |
| domain | 解析出的域名 |
| app_name | 映射后的应用名 |
| app_category | 应用分类 |
| rx_bytes / tx_bytes | 下载/上传字节数 |
| rx_packets / tx_packets | 下载/上传包数 |

### 5. 前端展示

LuCI Web 界面提供四个 Tab：

| Tab | 内容 | 数据来源 |
|-----|------|----------|
| **Applications** | 按应用统计流量，饼图 + Top 排行 | `apptraffic -g app` |
| **Domains** | 按域名统计，显示域名→应用映射关系 | `apptraffic -g domain` |
| **Devices** | 按局域网 IP 统计，看到哪个设备流量最大 | `apptraffic -g host` |
| **Categories** | 按分类聚合（视频/社交/游戏/购物…） | 前端对 app 数据二次聚合 |

### 6. 数据流总结

```
网络数据包 (pcap)
     │
     ▼
┌─────────────┐    ┌──────────────┐    ┌───────────────┐
│ DNS 响应解析  │    │ TLS SNI 提取  │    │ HTTP Host 提取 │
│ port 53     │    │ port 443     │    │ port 80       │
└──────┬──────┘    └──────┬───────┘    └──────┬────────┘
       └──────────┬───────┴──────────────────┘
                  ▼
        ┌─────────────────┐
        │ IP → 域名 哈希表  │
        └────────┬────────┘
                 ▼
        ┌─────────────────┐
        │ 域名 → 应用 匹配  │
        └────────┬────────┘
                 │
┌────────────────┼────────────────┐
│ /proc/net/nf_conntrack          │
│ 流量字节数/包数/连接数            │
└────────────────┬────────────────┘
                 ▼
        ┌─────────────────┐
        │ SQLite 持久化存储 │
        └────────┬────────┘
                 ▼
        ┌─────────────────┐
        │ CLI JSON/CSV 输出│
        └────────┬────────┘
                 ▼
        ┌─────────────────┐
        │ LuCI Web 图表展示 │
        └─────────────────┘
```

---

## 编译和安装

### 编译

```bash
cd ~/openwrt-19.07/openwrt

# 确保依赖已勾选
make menuconfig
#   Libraries → libpcap        [*]
#   Libraries → libsqlite3      [*]
#   Kernel modules → Netfilter Extensions → kmod-nf-conntrack       [*]
#   Kernel modules → Netfilter Extensions → kmod-nf-conntrack-netlink [*]
#   LuCI → Applications → luci-app-apptraffic  [*]
#   Network → apptraffic                        [*]

# 编译
make V=s
```

### 在路由器上安装

```bash
# 传输 ipk 到路由器
scp bin/packages/arm_cortex-a9_neon/packages/apptraffic_1.0.0-1_arm_cortex-a9_neon.ipk root@192.168.1.1:/tmp/
scp bin/packages/arm_cortex-a9_neon/luci/luci-app-apptraffic_1.0.0-1_all.ipk root@192.168.1.1:/tmp/

# SSH 到路由器安装
ssh root@192.168.1.1
opkg install /tmp/apptraffic_1.0.0-1_arm_cortex-a9_neon.ipk
opkg install /tmp/luci-app-apptraffic_1.0.0-1_all.ipk
```

### 使用

1. 浏览器打开 `http://192.168.1.1`，登录 LuCI
2. 左侧菜单 → **App Traffic** → **Traffic Analysis** 查看流量图表
3. 左侧菜单 → **App Traffic** → **Configuration** 配置参数

### 命令行使用

```bash
# 启动守护进程
/etc/init.d/apptraffic start

# 查询数据（JSON 格式，按应用分组）
apptraffic -c json -g app

# 查询今天的数据（CSV 格式，按域名分组）
apptraffic -c csv -g domain -t today

# 查询最近一周
apptraffic -c json -g app -t week

# 导出 CSV
apptraffic -c csv -g host -t month
```

---

## 配置说明

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| enabled | 1 | 是否启用守护进程 |
| interface | br-lan | 抓包网口，`any` 表示所有网口 |
| database | /var/lib/apptraffic | 数据库存储目录 |
| commit_interval | 60 | 数据提交间隔（秒），越短数据越安全但闪存磨损越大 |
| dns_timeout | 3600 | IP→域名映射缓存时间（秒） |
| flow_timeout | 300 | 空闲流超时删除时间（秒） |
| retention_days | 30 | 每条流每天只保留一条汇总记录，超过该天数的数据自动删除，防止数据库无限增长把 /tmp 写满 |

---

## 技术选型说明

### 为什么用 libpcap 而不是 nflog？

libpcap 可以直接抓取网络包，不需要额外内核模块。对于 DNS、TLS SNI、HTTP Host 这些只需要解析几百字节的协议头，libpcap 的性能完全够用，且兼容性更好。

### 为什么用 conntrack 而不是 nfqueue？

nf_conntrack 是内核自带的连接跟踪模块，不需要额外把包从内核态拷贝到用户态。直接读取 `/proc/net/nf_conntrack` 可以零拷贝获取所有流的统计数据。

### 为什么不用 nDPI？

nDPI 虽然准确度最高，但：
1. 需要交叉编译整个 C 库，OpenWrt 19.07 feeds 中没有现成包
2. 运行时内存占用较大（通常 > 50MB），不适合家用路由器
3. DNS + SNI + HTTP 三条路径已经能覆盖 95% 以上的流量识别需求

### 为什么是单文件 C 程序？

把捕获、解析、映射、存储、输出全部放在一个文件里，减少了编译依赖和运行时开销，更适合 OpenWrt 嵌入式环境。整个程序编译后约 30-50KB。

---

## 预置应用识别列表（200+）

| 分类 | 预置应用 |
|------|----------|
| 社交 | Facebook, Instagram, Twitter/X, TikTok, Snapchat, Reddit, LinkedIn, Pinterest, 微博 |
| 消息 | WhatsApp, Telegram, Signal, Discord, Slack, WeChat, LINE, KakaoTalk |
| 视频 | YouTube, Netflix, Bilibili, iQIYI, Youku, Twitch, Disney+, Hulu, HBO Max |
| 搜索 | Google, Baidu, Bing, DuckDuckGo, Yahoo, Yandex |
| 购物 | Amazon, Taobao, TMall, JD.com, eBay, Etsy, Shopee, Lazada, SHEIN |
| 云存储 | Dropbox, OneDrive, Google Drive, iCloud, Box, MEGA |
| 邮箱 | Gmail, Outlook, Yahoo Mail, Proton Mail, QQ Mail |
| 游戏 | Steam, Epic Games, Roblox, Minecraft, EA, Ubisoft, Riot Games, Blizzard |
| 新闻 | BBC, CNN, NYT, WSJ, Reuters, 新浪, 搜狐, 头条, 知乎 |
| 音乐 | Spotify, Apple Music, Pandora, SoundCloud, 网易云音乐 |
| 会议 | Zoom, Google Meet, Microsoft Teams, Skype, Webex |
| 金融 | PayPal, Stripe, Alipay, Venmo, Coinbase, Binance |
| 开发 | GitHub, GitLab, Stack Overflow, npm, Docker |
| 系统 | Windows Update, Apple, Akamai CDN, Cloudflare CDN |
