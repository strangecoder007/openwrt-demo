# shcmd — 板子上的白名单远程命令入口

给 OpenWrt 板子加一个「手机/脚本能敲一条命令」的 HTTP 口子，同时不把
shell 暴露出去。**核心设计：没有 shell。** 每条白名单命令是一个 argv
向量，请求参数作为独立 argv 槽位传给 `execvp`，从不拼接成字符串。

## 为什么不是「把 dsh 装到板子上」

板子是 i.MX6 / ARMv6 / musl / 512MB，跑不了 Node.js 18+（官方无 armv6
二进制，V8 也编不出来）。dsh 留在桌面机，板子这边只需要一个遥控器。

## 架构

```
手机 / curl / 脚本
        │  GET|POST  http://板IP:8099/r?n=<name>&k=<token>&a=<arg>
        ▼
   shcmd 守护进程（静态 61KB，常驻 RSS ~1MB）
        │  execvp(argv)  ← 无 shell，无注入面
        ▼
   /bin/uptime, /sbin/logread, /etc/init.d/xxx restart ...
```

LuCI 走另一条更安全的路径，**token 不下发浏览器**：

```
浏览器 ──session──> LuCI 控制器 ──loopback wget──> 127.0.0.1:8099 ──> shcmd
```

## 给程序/AI 用：`/schema` 和 `shc`

手写 `curl` 拼 URL 和 token 很啰嗦，而且调用方不知道参数边界（`channel`
只能取 1/6/11 这种约束原本埋在 shell 脚本里）。因此有两条路：

**1. `/schema` —— 机器可读的完整契约**

```sh
curl "http://板IP:8099/schema?k=TOKEN"
```

每个命令都带：URL 形态、每个参数的**请求参数名**（`a`/`a1`/`a2`…）、
`enum`、`range`、`required`、超时、以及响应的确切字段和全部错误码。
调用方一次就能写对，不必靠试错摸边界。

**2. `shc` —— 板上的薄封装**

```sh
shc ls                      # 命令清单，标 [danger] 和参数名
shc info uci-radio          # 单个命令的契约
shc run uci-radio a=channel a1=6
shc run logtail a=3
shc --json run uptime       # 原始 JSON，便于程序消费
shc schema                  # 完整契约
```

`shc run` 的参数用 `key=value`，键名就是 `shc ls` 显示的那个
（slot 1 是 `a`，slot 2 是 `a1`，依此类推）。

约束由 **daemon 自己强制执行**，不依赖 shell 脚本：

```
$ shc run traffic-period a=hour
ERROR: argument 1: 'hour' is not one of: today,yesterday,week,month

$ shc run logtail a=9999
ERROR: argument 1 must be an integer in 1-500
```

## 声明参数约束

`arg` 指令支持结构化选项，`/schema` 和校验都用它：

```
arg 1 "Line count" "40" "1-500" range=1-500 required
arg 1 "Period" "today" "..."    enum=today,yesterday,week,month required
arg 1 "Client MAC" "aa:bb:..."  "..." required
```

- `enum=a,b,c` —— 值必须是其中之一
- `range=N-M` —— 必须是 `[N,M]` 内的整数
- `required` —— 调用方必须提供

没有这些选项时，约束只在 shell 脚本里，daemon 无法提前拒绝。

## 安装

```sh
# 构建
ssh buildhost "cd ~/openwrt-19.07/openwrt && \
  make package/network/utils/shcmd/clean && \
  make package/network/utils/shcmd/compile V=s && \
  make package/network/utils/luci-app-shcmd/compile V=s && \
  make package/index V=s"

# 部署
opkg install --force-reinstall shcmd_0.1.0-1_arm_cortex-a9_neon.ipk
opkg install --force-reinstall luci-app-shcmd_1.0.0-1_arm_cortex-a9_neon.ipk
```

**装完必须改 token 再重启**（见下）。

## 配置 `/etc/shcmd.conf`

```
token "<随机的 16 字节 hex>"       # 必须换掉 CHANGE-ME，否则所有请求被拒
listen 0.0.0.0
port 8099
max_args 4                        # 每次请求最多几个额外参数（0..16）
allow 127.0.0.1/32                # 来源 CIDR，可重复
allow 192.168.100.0/24
command <名字> <超时秒> <程序> [参数...]
```

生成 token：

```sh
head -c 16 /dev/urandom | hexdump -v -e '/1 "%02x"'; echo
```

### 需要管道的命令，由**你**写死在配置里

```
command myreport 30 sh -c "logread -e apptraffic | grep ERROR | tail -20"
```

那串脚本是你的，不是调用方的。请求只能选名字、追加位置参数，
**永远无法构造新的管道或重定向**。

## 用法

```sh
# 列出可用命令
curl "http://192.168.100.1:8099/c?k=TOKEN"

# 执行
curl "http://192.168.100.1:8099/r?n=uptime&k=TOKEN"
curl "http://192.168.100.1:8099/r?n=logread&k=TOKEN"

# 带参数（a 是第一个，a1..aN 依次跟在后面）
curl "http://192.168.100.1:8099/r?n=ping&k=TOKEN&a=8.8.8.8"

# 也可以放在 header 里
curl -H "Authorization: Bearer TOKEN" "http://192.168.100.1:8099/r?n=uptime"
```

返回：

```json
{"ok":true,"command":"uptime","status":0,"timed_out":false,
 "truncated":false,"output":" 13:05:57 up 12:11,  load average: 0.03\n"}
```

失败时 `ok:false` 且带 `error`：`400` 缺参数、`401` token 错、
`403` 来源不允许、`404` 未知命令、`405` 非 GET/POST、`503` 守护进程不可达。

## LuCI 界面

「系统 → Remote Commands」：

- **Run** 页：每个白名单命令一个按钮，点开填参数，输出在深色终端框里，
  带 `exit N`、耗时、`TIMED OUT`、`truncated` 标记
- **Settings** 页：改 token / 监听地址 / 端口 / max_args / allow 列表

**命令白名单故意不放进 Web UI**——加命令是安全决策，应该要求 shell 访问。

## 安全边界（务必理解）

| 属性 | 说明 |
|---|---|
| 无 shell | 参数是独立 argv 槽位，`;id`、`$(id)`、`` `id` `` 都是字面量 |
| token | 常数时间比较，支持 query `k=` 和 `Authorization: Bearer` |
| 来源 | CIDR 白名单（v4/v6）；**为空 = 接受任何来源**，别留空 |
| 超时 | 子进程独立进程组，超时 `kill(-pgid, SIGKILL)`，不留僵尸 |
| 输出 | 上限 64KB，超了标记 `truncated` 并继续排空管道（避免 SIGPIPE） |
| 命令名再校验 | LuCI 控制器在拼 URL 前用磁盘上的白名单复核，防 `&` 走私 |

### ⚠️ 空密码风险

这台板子 root 是空密码，LuCI 空密码即可登录。**任何能连到内网的人
都能打开 Run 页执行白名单里的所有命令**（包括 `restart-apptraffic`）。
两种加固方式：

1. 给 root 设密码（`passwd`），或
2. 把 shcmd 改成 `listen 127.0.0.1`，让外部必须先登 LuCI

## 测试

```sh
# 主机侧行为测试（24 项，含认证/注入/超时/输出上限）
gcc -std=gnu99 -Wall -Wextra -O2 -static -o /tmp/shcmd_host src/shcmd.c
sh tests/run-tests.sh /tmp/shcmd_host
```

板端脚本（`tests/board-*.sh`）：隔离测试、hpyhen 词法、LuCI 端到端、
负向状态码。它们用 `sh` 跑，避免 PowerShell → ssh 的引号坑。

## 踩过的坑

1. **引号 token 首字符被吞**：旧 `char *d = ++p; tok = ++p - 1;` 两个指针
   起始同位，第一轮把第 2 字符写到第 1 字符上，`test-token` → `est-token`。
   症状是「永远认证失败」，而 prefix/suffix 否定测试恰好通过，把 bug 藏住。
2. **`$(TARGET_STRIP)` 不存在**：19.07 里叫 `$(STRIP)`，未定义时那行退化
   成执行 ARM 二进制，报 `Exec format error`。
3. **`luci.fs` 模块不存在**：19.07 没有这个模块，`require` 失败会让整个
   控制器加载失败、uhttpd 对所有路由返回 500。只用 `luci.sys`。
   **注意会有两份文件踩这个坑**：控制器和 CBI 模型各 require 一次。
4. **配置用的是 UCI 的 `Map` + `TypedSection`**：`/etc/shcmd.conf` 里没有
   `config <type>` 行，CBI 认定「尚无任何配置」，所有字段都不渲染。
   必须用 **`SimpleForm`**，且每个字段自己实现 `cfgvalue` / `write`。
5. **`SimpleForm` 必须用 `form()` 挂载**：挂在 `cbi()` 上会报
   `Model ... returns SimpleForm but is dispatched via cbi()`，
   页面能显示但保存被静默跳过。
6. **procd 不会因改配置而重启**：`/etc/init.d/shcmd start` 在已运行时是
   空操作，旧实例仍持有旧 token。改配置后必须 `restart`。
7. **`signal()` 会隐式带 `SA_RESTART`**：`accept()` 被自动重启，主循环
   永远看不到 EINTR，`g_stop` 不起作用，procd 只能升级到 `SIGKILL`。
   必须用 `sigaction()` 且 `sa_flags = 0`。
8. **busybox 的 `cat -v` / `od` / `base64` / `chpasswd` 都没有**，
   `sleep` 不支持小数，`wget` 对非 2xx 静默丢 body（要用 `-S` 看状态码）。
9. **PowerShell → ssh 的引号坑**：内嵌引号会被吞、`2>/dev/null` 会被
   PowerShell 当成自己的重定向。复杂脚本一律写成文件 scp 过去再 `sh` 执行。
10. **URL 拼接漏了 `?`**：`"http://host/$1&k=..."` 在 `$1` 是裸路径
    （如 `c`）时会得到 `/c&k=...`，daemon 按第一个 `?` 切分，于是整串被当
    成路径，token 永远解析不到，表现是 **401/空响应**。Lua 控制器和 `shc`
    里各犯过一次——凡是要拼 query 的地方都要判断有没有 `?`。
11. **`wget -q` 会丢弃 4xx 的响应体**：所有被拒绝的请求（enum 不匹配、
    缺参数）都显示成空白，看起来像服务没响应。`shc` 必须加
    `--content-on-error` 才能拿到 `{"ok":false,"error":...}`。
12. **`math.random` 在 Lua 5.1 里不播种**：同一进程里每次调用序列相同。
    用它生成上传文件名时，同秒内两次上传得到同一路径，**第二次静默覆盖
    第一次**。改用 时间戳-序号-PID，并在落盘前检查文件是否已存在。

## 文件

```
package/network/utils/shcmd/
├── Makefile              # package.mk + 手写 install（与 wifi-probe 同约定）
├── src/shcmd.c           # 单文件守护进程（/r /c /schema）
├── files/shcmd.conf      # 模板（CHANGE-ME 占位）
├── files/shcmd.init      # procd init 脚本
├── files/shc             # 板上 CLI 封装
└── tests/                # 主机侧 + 板端测试

feeds/luci/applications/luci-app-shcmd/
└── luasrc/controller/shcmd.lua   # 代理控制器 + ipk 上传（token 不下发浏览器）
```

## LuCI 上传 ipk

「Run」页有上传区：选 ipk → 上传 → 列表出现新文件 → 点 `Install` 自动切到
`install-ipk` 并填好路径。

服务端硬约束（都在 `action_upload` 里）：

- 只接受 `.ipk` 结尾（防止往 `/tmp` 放可执行文件）
- 上限 8 MB
- **落盘文件名由服务端生成**，客户端给的 `filename` 只做记录：
  `../../etc/passwd.ipk` 会存成 `/tmp/upload-<ts>-<seq>-<pid>.ipk`
- 写到 `/tmp`（tmpfs），重启即失

注意：本项目的 LuCI 应用要**同时**放在 `package/network/utils/luci-app-shcmd/`
（构建系统实际扫描的实体目录），`feeds/` 下那份是备份副本。
