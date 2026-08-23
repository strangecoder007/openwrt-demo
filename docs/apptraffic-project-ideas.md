# 项目点子研发计划（供逐个研究）

> 说明：本文档只做本地研究用，**不要同步到 `../my-openwrt-demo`（该仓库是公开的，会推到 GitHub）**。
> 所有结论都基于本树 `package/network/utils/apptraffic/` 的实际源码，不是照搬 README。

## 目标与背景

你的主线目标是：在手头这块 i.MX6ULL + OpenWrt 19.07 + 4.1.15 内核的家底下，攒一个**有深度、面试能讲出名堂、能写进简历的网络项目**。

四个候选方向按“对求职的冲击力 + 复用什么 + 上手难度”排序。前两个是“路由器/网络设备厂商”最吃的数据面与选路，第三个是“路由协议”纯知识面，第四个偏完整闭环但技术含量略低。

| # | 项目 | 一句话 | 难度 | 最吃香的公司岗位 |
|----|------|--------|------|------------------|
| 1 | apptraffic 升级成“真正 DPI” | 从“端口/SNI 识别”进化到“协议指纹 + 会话关联”，并补齐 IPv6/重组/QUIC 短板 | med-high | 数据面/安全产品/流量分析 |
| 2 | 家用 SD-WAN（多线选路 + QoS） | 把板子做成真正会“做决策”的智能网关 | medium | 路由器/网关/CPE 开发 |
| 3 | FRR 路由协议实验台 | 用 FRR + 虚拟机搭真实收敛网络，学 OSPF/BGP | 知识量偏大 | 路由协议/网络设备软件 |
| 4 | 自组 mesh + 统一管理 | wireguard-go 版 ZeroTier-lite，多节点自动组网 | medium | 组网/云网/overlay |

---

## 项目一（详细）：把 apptraffic 从“可识别”升级成“真正的 DPI”

### 1.1 先盘点：你其实已经做了什么

现有 `src/main.c`（约 61KB，单文件）已经实现了这些，README 描述基本属实：

- **DNS 捕获**：`parse_dns_query()` / `parse_dns_response()`，解析 A/AAAA，经 `on_dns_response` → `mapping_add_dns()` 建立 **IP→域名** 哈希表（`dns_hash`，65536 槽，带过期）。
- **TLS SNI 提取**：`parse_tls_sni()` 从 TCP 443 的 ClientHello 里解出 `server_name`，回调 `on_sni_hostname`。
- **HTTP Host 提取**：`pcap_callback()` 里对 TCP 80/8080 的 `GET/POST/HEAD`，用 `strcasestr` 找 `\r\nHost: `。
- **流统计**：`conntrack_update()` 读 `/proc/net/nf_conntrack`，按五元组累计字节/包数，算增量（`prev_*` 记录上次值）。
- **存储与输出**：sqlite WAL 存 `traffic` 表；CLI `-g app/domain/host/category` + `-c json/csv`；LuCI 前端四个 Tab。
- **域名→应用映射**：`app-mapping.txt`（glob 通配 + 优先级），`mapping_lookup_app()` 归类。

所以“第一步识别”你已经做完了，这一步在简历里只能算“流量统计 + 简易分类”。下一个段位是把它做成**能被面试官追问的 DPI 引擎**。

### 1.2 短板诊断（都有源码依据，不是拍脑袋）

直接从 `pcap_callback()` 和相邻代码看，真实短板如下：

1. **只做 IPv4**：`pcap_callback()` 只认以太网类型 `0x0800` 和 `0x8100`（VLAN+IPv4），**完全不处理 `0x86DD`（IPv6）**。你这板子 IPv6 流量很大（WireGuard `fd00::`、云盘公网只有 IPv6），这一块现在全丢。
2. **无 TCP 流重组**：`parse_tls_sni()` 拿的是**单个 pcap 包**的 payload，直接假定 ClientHello 整段都在这一个包里。真实网络里 ClientHello 可能跨多个 segment（窗口小、被 MTU 拆、TCP 分段），一旦被拆就解不出 SNI；TCP 重传/乱序也没处理。
3. **无 IP 分片重组**：大 DNS 响应（A+AAAA 一坨）可能被 IP 分片，只解析首个分片，后面的 record 直接丢。
4. **无 QUIC（UDP 443）**：`pcap` 过滤串是 `"port 53 or port 80 or port 8080 or port 443 or port 8443"`，BPF 里 `port 443` 同时命中 TCP/UDP，但 `pcap_callback()` 的 UDP 分支只判 `dst/src_port == 53`，**UDP 443 包到了却被静默丢弃**。HTTP/3、越来越多 App 的 QUIC 全识别不出。
5. **没有 TLS 以外的协议指纹**：QQ 专有 UDP、各类游戏、P2P（BitTorrent）、SSH/RDP… 走自己的端口/协议，不走 80/443 明文区，只能落到 `Unknown`。这就是“DIP”和“DPI”的分水岭。
6. **IP→域名是“单映射”**：`dns_hash` 一个 IP 只存一个域名。CDN / 多域名共用 IP 时，谁最后被 DNS 写进去谁赢，会误标。SNI 其实能给出**该连接真正的主机**，但目前只是个普通写入，没有置信度/优先级概念。
7. **conntrack 用 `/proc` 轮询**：每 5 秒读一次 `/proc/net/nf_conntrack`。依赖里其实已经打了 `kmod-nf-conntrack-netlink`（AGENTS.md 也列了），但代码**没用 netlink**，是纯轮询，CPU 白耗，也拿不到连接建/删事件。

### 1.3 升级路线（建议分批，每批都能单独交付/验证）

#### 阶段 A：正确性补强（先让“现有识别”变准）

这是性价比最高、也最容易出活的一步，建议第一个做：

- **补 IPv6**：`pcap_callback()` 增加 `0x86DD` 分支（解析 IPv6 基础头 + 扩展头链 + 传输层），DNS 支持 AAAA、SNI 在 IPv6 下同样解。要处理 IPv6 分片（`next header = 44 Frag`）。
- **TCP 首段重组**：按五元组维护一个小缓冲（几 KB），从 SYN 记录初始序列号，用 `<seq - iseq>` 算出流内偏移，把第一个数据段及后续段拼起来，直到拿到**完整的 ClientHello**（TLS record 头 5 个字节给出总长，够长就收手），解完即释放缓冲。处理乱序（按偏移插入）与重传（按序列号去重）。
- **QUIC SNI**：识别 UDP 443 的长头 Initial 包，剥掉 QUIC 长头（`0x80|0x40` 固定位 + 版本 + DCID/SCID + token 长度 + length），取出 CRYPTO frame，里面是 TLS 1.3 ClientHello（`0x16 0x03 ...`），复用 `parse_tls_sni` 的解析逻辑。要区分 QUIC v1（version `0x00000001`）与 v2（RFC 9369，版本 `0x6b3343cf`），长头 type 位不一样。
- **方向/首包判定**：SNI 只在“客户端→服务器”且是流起点时解析。用 SYN 的 ISN 和 TCP seq 判断是否处于流起始，避免把应答包/重传当 ClientHello。

#### 阶段 B：真正的 L7 指纹（这是“DPI”的灵魂）

在 DNS/SNI/HTTP 之外，加一个**自研协议签名引擎**（不引入 nDPI，理由见下）：

- 规则表（文本文件，仿 `app-mapping.txt`）：每条规则含 `协议 + 方向 + 端口(可空) + 偏移 + 特征(magic/glob) + 启发式`。
- 示例特征：BitTorrent 握手 `\x13BitTorrent protocol`；SSH `SSH-2.0-`；QUIC 的固定位；某些游戏 UDP 的固定魔数 + 包大小/频率；QQ 的 `OICQ` 魔数；RDP `\x03\x00\x00\x13\x0e` …
- 打分：DNS 命名、SNI 主机、HTTP Host、载荷指纹各自给出**置信度**，多证据融合（SNI 对主机最权威，DNS 次之，指纹判定“协议”）。结果落到 `app_name` + `app_category` + `confidence`。
- **不引 nDPI** 的理由（现有 README 也写了）：交叉编译整库、内存 >50MB、19.07 feeds 无现成包。但你可以**参考 nDPI 的分类思路**（哪些端口/魔数属于哪个 app），用轻量自研替代，这正是能讲的技术点，也是面试官想听的“你怎么做 trade-off”。

#### 阶段 C：会话级关联 + 实时分析

- 把 conntrack 从 `/proc` 轮询改成 **netlink 事件驱动**（`NFNL_SUBSYS_CTNETLINK`，订阅 NEW/DEL/GET），连接一建就更新，不再空转。
- 把“流”聚合成“会话/设备”：每台局域网设备 × 每个应用的**时间序列**（按时间桶），能看到“这台手机 20:00-21:00 在看视频”。
- 实时 Top-N / 异常告警：某设备突然暴涨、某应用持续大流量、WebDAV 上传中断等，供 LuCI 实时页展示。

#### 阶段 D：数据面性能与落地

- 抓包从 libpcap 默认 buffer 换成 **PACKET_MMAP**（减少 syscall），或至少调大 `pcap_set_buffer_size` 避免丢包。
- 内存优化：DNS hash、流表、重装缓冲都用内存池，避免实时路径上频繁 `malloc/free`。
- `snaplen` 现在 2048，ClientHello 可能超，需评估；QUIC/HTTP 抓包长度也影响识别。
- 数据库演进：`traffic` 表加 `confidence`、`flow_id`、`machine/app` 关联；加索引；按天/Host 聚合表（现在只按天一条汇总，粒度偏粗）。
- 打包成 ipk + LuCI 版本迭代，板子 `opkg install --force-reinstall` 实测。

### 1.4 关键技术细节（说清楚，面试就能讲）

**TCP 首段重组要点**：抓 SYN 拿初始序号 `iseq`，对每个 TCP 段算 `offset = seq - iseq`；把 `offset >= 0` 的载荷按偏移插入 `rewrite_buffer`；因为只需要 ClientHello，重组到“已拼出完整一条 TLS record（5 字节头 + length）”就停，或缓存到一定上限（如 8KB）防止内存泄漏；用 5 元组 + 方向做 key；解到即标记 done，清理缓冲。乱序段先放，靠 `[offset, offset+len)` 覆盖检查去重。

**QUIC Initial 解析**：首字节 `0x80|0x40` = 长头 + 固定位；type 位（v1：`00=Initial`，`01=0-RTT`，`10=Handshake`，`11=Retry`）；之后 Version(4) + DCID Len(1) + DCID + SCID Len(1) + SCID + Token Len(varint) + Token + Length(varint) + Packet Number；payload 第一个 frame 是 CRYPTO，frame 头给出 offset/length，里面就是 TLS 1.3 ClientHello，可交给现有 ClientHello 解析。v2 的长头 type 位和版本号不同，需要按版本分支。

**自研签名引擎**：一张规则表，匹配方式可以是“固定偏移 magic（memcmp）”、“子串（含通配）”、“包长/方向/频率启发式”。匹配命中的规则返回“协议名 + 置信度”；DNS/SNI 给出“域名/应用名”；二者按置信度融合，输出 `app_name`。要处理“同一个目标 IP 多种协议”与“CDN 大 IP 多域名”的冲突。

**DNS/SNI 置信度融合**：一个 IP 可能对应多个域名（CDN/共享主机）。方案：`dns_hash` 从“单域名”改成**该 IP 的域名集合（小列表 + LRU）**；SNI 是精确的，用它做连接级主键；DNS 做兜底。这样能显著降低误标率。

**conntrack netlink vs /proc**：`/proc` 是全量轮询，每次遍历所有连接；netlink 订阅事件能拿到“连接建立/销毁/更新”的增量，省 CPU、实时。代价是要处理 `NFNL_SUBSYS_CTNETLINK` 的头部/属性解析（可用 `libmnl`/`libnftnl` 或裸 `recv`）。既然依赖已经开了，正好用上。

### 1.5 数据模型演进建议

现表 `traffic` 大概字段：`timestamp, src_ip, dst_ip, src_port, dst_port, protocol, domain, app_name, app_category, rx_bytes, tx_bytes, rx_packets, tx_packets`。

建议加：

- `confidence`（TINYINT）：4=SNI/指纹确证，3=SNI，2=DNS，1=仅端口，0=Unknown。
- `flow_id` / `ct_id`：关联 conntrack 连接，便于“一次会话”聚合。
- `device_label`：把 `src_ip` 关联到 dhcp/主机名/MAC。
- `session_start/end`、`app_detail`（比如 SNI 或指纹的具体协议）。
- 聚合表：按 `day × host × app` 预聚合，查询更快；现在“每条流每天一条”粒度偏粗。

### 1.6 难点与大坑（提前知道）

- **内存**：板子小，重装缓冲、DNS hash、流表都要设上限并做过期/淘汰，防止长时间运行 OOM。
- **定向包抓全**：`snaplen=2048` + 默认 buffer，流量一大 libpcap 丢包 → SNI 漏识别；QUIC/大包会截断。
- **方向判定错误**：在网关上同时看到客户→服务器、服务器→客户，必须分清哪边是客户端（用 SYN 方向和源/目的归属）。
- **DoH/DoT 逃逸**：DNS 走 853/443 加密后 DNS 路径失效，但 SNI 仍能看到目标域名（这是 SNI 价值所在，也是“识别为主机而非应用”的边界——要诚实告诉面试官这一点）。
- **QUIC 版本演进**：v1/v2 地址位不同，不按版本分支会误解析。
- **DB 增长**：必须保留 retention / 聚合策略，避免把 `/tmp`（或 SD）写满（现有 retention_days 已有，但聚合粒度要合理）。
- **并发/线程安全**：`capture_run` 是独立线程，`dns_hash` 已加 `dns_mutex`，但流表、数据库写入要确保锁正确，避免实时路径与提交路径竞争。

### 1.7 与求职的关联（可讲的硬点）

- 你能讲清楚“**端口识别 vs 协议指纹识别 vs 主机识别**”三者的区别与互补（这是 DPI 面试常考）。
- 你能说明在**嵌入式资源受限平台**上如何做 trade-off（不引 nDPI、用首段重组、用小缓冲）。
- 你能谈 **TCP 重组 + 包方向 + QUIC 版本** 这些“看着简单做起来有坑”的细节。
- 你能结合 **netlink vs /proc** 讲数据面性能优化。
- 产出一套“**置信度分级的应用识别 + 每设备时序 + 实时告警**”的完整闭环，比“统计流量”高一档。

### 1.8 落地建议（第一步怎么动）

1. **先做阶段 A 的 IPv4 重叠重组 + 方向判定**，因为它直接改善现有准确率，改动集中、易验证。
2. **再补 IPv6**（你板子大量 IPv6，反馈最明显）。
3. **再上 QUIC**（识别 HTTP/3）。
4. 这三步做完，拿一通真实抓包（用 `tcpdump` 在板子抓，或本机抓）比对“SNI 应识别到但当前漏掉”的样本，确认命中率提升。
5. 阶段 B 的通信指纹引擎再单独立项，规则文件可迭代，不影响存量。

验证方式：先在 Linux 构建主机 `make package/network/utils/apptraffic/compile V=s` 编译 → `opkg install --force-reinstall` 到板子 → `logread | grep apptraffic` + LuCI 页面对比。用 `argparse` 级别的自测集合（构造几段 ClientHello / QUIC Initial 字节流喂给解析函数）做单元验证最稳。

---

## 项目二（概要）：家用 SD-WAN（多线智能选路 + QoS）

把板子做成真正做决策的网关。它天然“多上行”：eth1 DHCP + 手机 USB 网络共享 + WireGuard，正好当多 WAN。

- **策略路由**：`ip rule` + `ip route` + `fwmark`，按流量类别（视频/游戏/透传）走不同链路；配 conntrack 做状态关联。
- **链路健康探测**：ICMP/traceroute 探活 + `tc` 上加 `netem`/`cake`/`fq_codel` 主动探测带宽/抖动/丢包；故障自动切换（mwan3 思路，自己实现）。
- **QoS**：DSCP 标记 + SQM（`cake`），拥塞管理，给实时流量优先。
- LuCI 做“链路状态 + 当前决策 + 切换历史”面板。

难点：切换抖动、状态一致性、路由表维护。产出对应“路由器/网关设备/CPE”岗位，最对口你的路由器软件出身。

---

## 项目三（概要）：FRR 路由协议实验台

在板子跑 FRR，VM（QEMU/VPP）造小拓扑，组真实收敛网络。

- **OSPF**：LSA/LSU、DR/BDR 选举、Area 边界、优先级。
- **BGP**：状态机 Idle→…→Established、AS 关系、路由通告/抑制、与 OSPF 重分发。
- 可展示**报文在协议栈里真实流转**，比背 RFC 深刻。

最对口成都做“路由/交换/网络设备软件”的公司（迈普通信、新华三成都研究院、华为成都所等）。配置不难，难点是理解每条 LSA 为何出现、一条路由为何收敛。

---

## 项目四（概要）：自组 mesh + 统一管理（ZeroTier-lite）

你已经把 wireguard-go 跑通，再进一步就是一个小型 ZeroTier-lite。

- 小控制器管理多个板子节点组 full-mesh / hub-spoke。
- 自动交换密钥、下发路由、健康检查、拓扑可视化。
- LuCI 页面增删 peer、看拓扑。

胜在“完整闭环 + 看得见”，适合当对外演示项目，但技术冲击力不如前三个。

---

## 怎么逐个研究 / 下一步

建议先专注**项目一阶段 A（IPv4 重叠重组 + 方向判定）**，因为：

1. 改动最集中、收益最直接；
2. 能立刻体现“SNI 漏识别 → 命中率提升”的可量化进步；
3. 是后续 IP 重组、QUIC、L7 指纹的共同地基。

你要是定了先搞哪个，我可以帮你把：

- 模块拆解 / 目录建议（是否继续单文件，还是拆 `tcp_reasm.c`、`quic.c`、`lp7.c`）；
- 对应的数据结构与接口设计；
- IP 重组/QUIC/L7 规则的**单元测试用例**（构造字节流喂解析函数）；

一步步落到代码上。

---

## 落地进度（2026-08-23，供对照）

- 阶段 A 已做完并部署：单文件拆成 `capture/proto/tcp_reasm/ipreasm/dnsmap`，
  补齐 TCP 首段重组、方向/首包判定、IPv6、IP 分片；单测 10/10，`PKG_RELEASE` 已到 5。
- **顺带修掉一个上一版遗留的 conntrack 统计 bug**：协议自 `token[4]`（timeleft）
  误读，导致“同一连接每次读都当新流、字节被重复累加”（曾把板子自身 IP 显示成
  “192.168.100.1 / General / 537GB”）。改为 `token[3]`（协议号），并把“首见加全量/
  再见加增量”改为用 `found` 判断。
- **本机服务识别已做**：目的地是板子自身 IP 时按端口归类（443/8080/8443→
  CloudDriveWebDAV、**8081→MJPGStreamer(视频监控)**、80→LuCI、53→DNS、22→SSH、
  445→Samba、51820→WireGuard、其余→Local）；`mapping_lookup_app` 加了 IP 字面量防呆。
- **DNS/SNI 置信度融合已做**（阶段 B 准确率部分）：`IP→域名` 带置信度（DNS=1、
  SNI/HTTP=2），查询取最高；新增**按五元组（5-tuple）的 SNI 主机缓存**，每个连接以
  自己的 SNI 为准、DNS 兜底，解决 CDN/多域名共用 IP 误标。单测扩到 13 项。
- **本机流量不进 domain 视图**：本机服务流写库时 `domain` 置空，`-g domain` 查询
  过滤 `domain != ''`；App/Device/Category 视图仍显示本地服务（如 MJPGStreamer）。
- **噪声过滤已做**：丢弃 IPv4 组播(224.0.0.0/4)、回环(127.0.0.0/8)、有限广播
  (255.255.255.255) 及各接口子网广播（getifaddrs `ifa_broadaddr`），domain 视图不再
  被 `224.x / 239.x / xx.255 / 127.x` 刷屏；单测扩到 19 项。
- **L7 协议指纹已做**：自研轻量签名引擎 `l7.c`（不引 nDPI），规则表 `l7-rules.txt`
  `proto,dstport,offset,hexmagic,app,category`（支持仅端口启发式）；抓包侧对新增
  L7 端口（22/3389/8000/4000）首个数据包做匹配，结果按五元组存 `flow_app` 缓存，
  归流时优先级：本机服务 → L7指纹 → 五元组SNI → IP缓存。初始规则 SSH/RDP/QQ；
  单测扩到 25 项。SSH/RDP 用魔数、QQ 用端口启发式；BT 等随机端口需整流量捕获（可选）。
- **时序 + 实时告警已做**：把 `traffic` 表按流改为 **5 分钟桶**（`day` 列存
  `last_seen/TS_BUCKET`），提供 `-g ts`（每设备×应用按桶时序 JSON）、
  `-g alert -t <secs> -A <MB>`（窗口内超阈值告警，HAVING>threshold）、实时 Top-N
  用 `-g app -t <secs>`。LuCI 前端可挂这三个查询。
- 备注：QUIC 解密（Initial 的 SNI 已加密，需 HKDF+AES-GCM+HP）暂缓，单独立项。
