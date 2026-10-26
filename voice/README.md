# voice —— 板载语音助手

板载麦克风录音 → 云端 ASR → 云端大模型 → 云端 TTS → 板载喇叭放音。
全部跑在 **i.MX6ULL（Cortex-A7 528MHz 单核 / 512MB / OpenWrt 19.07 / 内核 4.1.15-v3）** 上，
**没有额外硬件**，用的是板载 WM8960 codec。

## 快速开始

```sh
# 1. 复制配置模板，填入凭据
cp voice.conf.example /etc/voice.conf
chmod 600 /etc/voice.conf
vi /etc/voice.conf

# 2. 部署
sh deploy.sh ./voice /etc/voice.conf ./voicectl ./voice-init

# 3. 验证（不用麦克风，全自动）
voicectl e2e

# 4. 真人对讲
voicectl ask 5
voicectl chat
```

## 命令

| 命令 | 作用 |
| --- | --- |
| `voicectl ask [秒]` | 一轮对话（录音→ASR→LLM→TTS→播放） |
| `voicectl chat` | 多轮对话，Ctrl-C 退出；**连续 2 次无语音自动结束** |
| `voicectl e2e` | 全自动端到端（TTS出题→ASR→LLM→TTS→播放，**不用麦克风**） |
| `voicectl loopback` | TTS→ASR 回环（验证识别准确率） |
| `voicectl llmtest "文本"` | 纯文本测大模型延迟 |
| `voicectl selftest` | 离线自检（mock 后端，不联网） |
| `voicectl info` | 声卡能力 + 后端配置 + 用户档案 |
| `voicectl profile` | 查看长期记忆 |
| `voicectl forget` | 清除长期记忆 |
| `voicectl trigger [选项]` | **常驻监听唤醒**（UART / GPIO） |
| `voicectl record/play/loop` | 录音 / 播放 / 回放 |

`voicectl` 读 `/etc/voice.conf` 后 export 环境变量，再调 `/usr/bin/voice`。

## VAD 自动断句（默认启用）

不用固定录 N 秒：**检测到说话才开始，说完（持续静音）就停。**

```sh
# 默认行为
VOICE_VAD=1                # 1=启用（默认），0=固定时长
VOICE_VAD_SILENCE=800      # 连续静音多少毫秒算说完
VOICE_VAD_RMS=0            # 0=自适应底噪（推荐），>0=固定阈值
```

**自适应底噪是关键**：板子所在环境可能有持续底噪（实测 RMS 稳定在 350~410，
可能是电源/USB 干扰）。固定阈值 200 比底噪还低 → VAD 会误判"一直在说话"。

自适应模式的做法：
```
启动前 300ms 只采样不判断 → 算出 baseline
start_rms = baseline * 3
end_rms   = baseline * 1.5
```

实测日志：
```
[vad] 校准完成: 底噪=412  起始阈值=1236  结束阈值=618
[vad] 状态=2  底噪=412  等待=5000ms  语音=0ms  静音=0ms  峰值RMS=1575  均值RMS=354
```

**行为对比（无人说话时）**：

| 模式 | 行为 | 耗时 |
| --- | --- | --- |
| 固定 5 秒 | 无差别录满，发空音频给 ASR（浪费流量） | 5.0s + ASR 0.6s |
| **VAD** | 5 秒判定无语音 → **直接返回，不调 ASR** | 5.0s |

有人说话时：说 0.5 秒 + 静音 0.8 秒判定 → **录音仅 1.3 秒**（而不是固定 5 秒）。

## 长期记忆（用户档案）

**存提炼出的事实，不存对话原文。**

```
/var/lib/voice/profile.conf   ← key=value 平面文件，人类可读
  name=小明
  city=成都
  job=做嵌入式开发
  fact0=养了一只橘猫叫大橘
  total_rounds=8
```

**为什么不用 JSON**：板子没有 JSON 库，profile 结构固定且无嵌套，平面文件 30 行解析搞定。

**怎么用**：生成一句摘要拼进 system prompt（约 50 token）：

```
你是中文语音助手。用户信息：叫小明；住在成都；做嵌入式开发；
养了一只橘猫叫大橘；回答控制在40字内，适合朗读。
```

实测效果（注入后问「我叫什么名字？」）：
```
回复: 你叫小明，住在成都。
回复: 你养的猫叫大橘呀。
```

请求体从 366 字节涨到 516 字节（+150 = 用户档案），符合预期。

**提炼时机**（`EXTRACT_EVERY_ROUNDS = 8`）：
- 每 8 轮对话提炼一次
- **退出时（Ctrl-C 或自动结束）也提炼一次**

⚠️ **周期不能超过短期记忆容量**：`HIST_MAX=8` 是环形缓冲大小，
如果用 20 轮提炼，中间 12 轮在被提炼前就被覆盖了。

**成本**：一次提炼约 2000 token，每 8 轮一次可忽略。

## 唤醒触发（voice-trigger）

常驻监听 UART / GPIO，收到唤醒信号就跑一次完整对话。

```sh
# 串口触发（ASRPRO 唤醒模块）
voice trigger -u /dev/ttymxc1 -k WAKE -v

# GPIO 触发
voice trigger -g 100 -a 1

# 同时监听
voice trigger -u /dev/ttymxc1 -g 100
```

参数：

| 选项 | 含义 | 默认 |
| --- | --- | --- |
| `-u DEV` | 串口设备 | 不监听 |
| `-b BAUD` | 波特率 | 115200 |
| `-k KEYWORD` | 关键字过滤 | `WAKE` |
| `-g N` | GPIO sysfs 编号 | 不监听 |
| `-a 0/1` | 触发电平 | 1（高） |
| `-d MS` | 防抖窗口 | 3000 |
| `-s SECS` | 触发后录多久 | 15 |
| `-v` | 打印原始数据 | 关 |

**核心设计**（`trigger.c`）：

```c
while (!g_quit) {
    reap_child(&st);                  // waitpid(WNOHANG)，不阻塞
    r = poll(fds, nfds, 500);         // 500ms 超时，可被信号打断
    if (r < 0 && errno == EINTR) continue;
    if (r == 0) continue;             // 超时，空转
    // UART: 读数据 → 行缓冲 → 关键字 → 防抖 → 状态检查 → fire()
    // GPIO: 边沿检测 → 防抖 → 状态检查 → fire()
}
```

**四层保护**：

1. **poll 而非阻塞 read** —— 阻塞 read 收不到 SIGTERM（`SA_RESTART` 会吞掉），
   procd 要等 30 秒超时才 SIGKILL。实测 `poll` + `sa_flags=0` → **SIGTERM 0.2 秒响应**。
2. **行缓冲** —— 串口是字节流，`read` 可能只给 `"WA"`，要累积到 `\n` 才算一条消息。
3. **防抖 3 秒** —— ASRPRO 可能连发两次。
4. **状态机** —— `LISTENING` 期间忽略新触发，防止两个 `voicectl` 抢麦克风。

**服务化**（`voice-trigger.init`，S97）：

```sh
/etc/init.d/voice-trigger enable
/etc/init.d/voice-trigger start
```

配置在 `/etc/voice.conf`：

```sh
VOICE_TRIGGER_UART="/dev/ttymxc1"
VOICE_TRIGGER_BAUD=115200
VOICE_TRIGGER_KEYWORD="WAKE"
VOICE_TRIGGER_GPIO=""
VOICE_TRIGGER_DEBOUNCE=3000
VOICE_TRIGGER_ASK_SECS=15
VOICE_TRIGGER_VERBOSE=0
```

⚠️ **init 脚本里不要收紧 nofile**（如 `limits nofile="64 64"`）——
procd.sh 用 fd 1000 做 flock，子进程继承 64 上限就开不出来，
**所有 init 脚本的 restart 都会返回 1 且服务不重启**。

### ASRPRO 模块接线

天问 ASRPRO 5.0（约 26 元）是**离线命令词识别**芯片，只做唤醒，不做通用识别：

| ASRPRO 排针 | 板子 TTL 座（ttymxc1 / UART2） |
| --- | --- |
| TX | RXD |
| RX | TXD |
| GND | GND |
| — | 供电用 ASRPRO 自己的 USB（最稳） |

**TX/RX 必须交叉接**，GND 必须连（不共地则无参考电平）。
电平兼容（都是 3.3V），不需要转换。

⚠️ TTL 座上的 VCC/5V 是「板子对外输出」，**不要往那里接模块电源**。

天问 Block 里配：唤醒词 → 串口发送 `WAKE\n`（115200）。

### 板子的串口资源

| 内核名 | 硬件地址 | 状态 | 用途 |
| --- | --- | --- | --- |
| `ttymxc0` | 02020000 | 已注册 | **调试串口**（console），别碰 |
| **`ttymxc1`** | 021e8000 | 已注册 | **空闲可用**（板上 TTL 座） |
| ttymxc2~7 | 其他 | 未启用 | 需改 dts 启用 pinctrl |

⚠️ 板子上**没有 `stty` 命令**（opkg 也装不到 `coreutils-stty`），
所以 trigger 程序用 `tcsetattr()` 在代码里配串口，不依赖外部工具。


## 配置（`/etc/voice.conf`，权限 600）

```sh
# 腾讯云 ASR/TTS（TC3-HMAC-SHA256 签名）
TENCENT_SECRET_ID=...
TENCENT_SECRET_KEY=...
TENCENT_APP_ID=...
TENCENT_REGION=ap-shanghai
ASR_BACKEND=tencent
TTS_BACKEND=tencent

# 大模型（TokenHub，OpenAI 兼容，Bearer token）
LLM_BACKEND=openai
LLM_BASE_URL=https://tokenhub.tencentmaas.com/v1
LLM_API_KEY=sk-...
LLM_MODEL=hunyuan-role-latest
LLM_MAX_HISTORY=3

# 交互默认
VOICE_RECORD_SECS=5
```

**后端可以混搭**：ASR 用腾讯云、LLM 用阿里百炼、TTS 用腾讯云都行，互不干扰。
`ASR_BACKEND` / `TTS_BACKEND` 取 `mock|aliyun|tencent`，`LLM_BACKEND` 取 `mock|openai`。

## 架构

```
┌─────────────────────────────────────────────────┐
│ main.c   命令分发 / 对话编排 / 自检              │
└────────────┬────────────────────────────────────┘
    ┌────────┼────────┬──────────┐
    ↓        ↓        ↓          ↓
 audio.c  asr.c    llm.c      tts.c      ← 四个能力接口
    ↓        ↓        ↓          ↓
 alsa-lib  tencent_nls.c     tencent_nls.c
           aliyun_nls.c      aliyun_nls.c
              ↓                 ↓
           tencent_sign.c    aliyun_sign.c
              ↓                 ↓
           crypto_sha256.c   crypto.c
              └───────┬────────┘
                   http.c (wget)
```

**职责边界**：只有 `main.c` 知道全局，模块之间不互相依赖。
`audio.c` 不知道 `llm.c` 存在。

| 模块 | 只做一件事 |
| --- | --- |
| `audio.c` | PCM 进/出（ALSA） |
| `asr.c` | 字节 → 文本 |
| `llm.c` | 文本 → 文本（含多轮历史） |
| `tts.c` | 文本 → PCM |
| `*_nls.c` | 某个云厂商的协议 |
| `*_sign.c` | 纯签名算法 |
| `crypto*.c` | 纯算法，零依赖 |
| `http.c` | 发请求收响应（wget popen） |

### 为什么零加密库依赖

板子是 **musl**，而 `~/linux/IMX6ULL/tool/openssl` 那份 libcrypto 是 **glibc** 编的，
链接时报几十条 `undefined reference to xxx@GLIBC_2.4`。

自实现 400 行（SHA1 / SHA256 / HMAC / Base64 / MD5），换来编译无痛。

### 为什么用 wget 而不是 libcurl

板子上没有 libcurl，但有 GNU wget 1.20.3（带 TLS）。
代价是每次请求 fork 一个进程（~10ms），对语音场景可接受。

## 关键实现点

### 语音识别 / 合成

- 采样率 **16kHz / 16bit / 单声道**（ASR/TTS 的公共格式）
- 板子支持 8k / 16k / 32k / 48k，**不支持 44.1kHz**
  （`imx-wm8960.c` 里硬编码的 `imx_wm8960_rates[]`）
- 腾讯云 ASR 是 RESTful：PCM 裸数据 POST 到 `/stream/v1/asr`，返回 JSON
- 腾讯云 TTS 是 RESTful：JSON body POST 到 `/stream/v1/tts`，返回 WAV

### 多轮历史（环形缓冲）

```c
#define HIST_MAX 8
static char g_hist_user[HIST_MAX][512];
static char g_hist_asst[HIST_MAX][512];
static int  g_hist_count, g_hist_head;
```

每次请求把 `system prompt + 历史 + 当前问题` 重新组装成 `messages` 数组。
**进程内有效，进程退出即失**（`voicectl ask` 每次是新进程）。

### 推理模型的坑

`hy3` / `hy4` 会先输出 `reasoning_content`（思考），再输出 `content`（回答）。
`max_tokens` 是两者共享的预算，给小了会被思考过程吃光
（`finish_reason: "length"` 而 `content: ""`）。

解析时必须跳过 `reasoning_content`：
```c
while ((q = strstr(q, "\"content\"")) != NULL) {
    if (q >= p + 12 && strncmp(q - 10, "reasoning_", 10) == 0) {
        q += 9; continue;      // 这是 reasoning_content，跳过
    }
    break;
}
```

**语音场景建议用非推理模型**（`hunyuan-role-latest` 942ms vs hy3 3140ms）。

### function calling

**`hy3` 完整支持** OpenAI 标准（`tools` → `tool_calls` → 回传 → 继续生成），
`hunyuan-role-latest` **不支持**（报 400）。

```json
// 请求里带 tools
{"tools": [{"type":"function","function":{
  "name": "get_weather",
  "parameters": {"type":"object","properties":{"city":{"type":"string"}},"required":["city"]}
}}]}

// 模型返回
{"finish_reason":"tool_calls","message":{"content":"","tool_calls":[{
  "function":{"name":"get_weather","arguments":"{\"city\": \"成都\"}"},
  "id":"chatcmpl-tool-xxx","type":"function"}]}}

// 本地执行后回传
{"role":"tool","tool_call_id":"chatcmpl-tool-xxx","content":"成都：晴，18~25°C"}

// 模型继续生成
{"finish_reason":"stop","message":{"content":"成都今天天气晴朗，气温18到25摄氏度…"}}
```

**代价**：两轮 LLM + 工具执行时间，延迟从 2.3 秒涨到 6~9 秒。

## 实测性能

单轮对话（`hunyuan-role-latest`）：

| 环节 | 耗时 |
| --- | --- |
| 录音固定 5 秒 | **5000ms**（纯等待） |
| ASR | ~700ms |
| LLM | ~950ms |
| TTS | ~1100ms |
| 播放 | ~2500ms |
| **总耗时** | 6100~6400ms |
| **用户感知延迟** | **约 2.3 秒**（含 4 秒空等） |

**注意最大的浪费是录音固定 5 秒**：说完话还要干等。改成 VAD 能省一半。

## 模型选型（TokenHub 实测）

| 模型 | 耗时 | 类型 | tools |
| --- | --- | --- | --- |
| **hunyuan-role-latest** | **942ms** | 非推理 | ❌ |
| hunyuan-t1-vision-20250916 | 1867ms | 非推理 | ? |
| hy3 | 3140~4000ms | 推理 | ✅ |
| hy4-preview | 30.97s | 推理 | ? |

## 构建

```sh
ST=$HOME/openwrt-19.07/openwrt/staging_dir/toolchain-arm_cortex-a9+neon_gcc-7.5.0_musl_eabi
export STAGING_DIR=$HOME/openwrt-19.07/openwrt/staging_dir
CC=$ST/bin/arm-openwrt-linux-gcc
ALSA=$HOME/build/alsa/install        # 自己交叉编译的 alsa-lib 1.1.8

$CC -O2 -Wall -I$ALSA/include -o voice \
    main.c audio.c asr.c llm.c tts.c http.c crypto.c crypto_sha256.c \
    aliyun_sign.c aliyun_nls.c tencent_sign.c tencent_nls.c \
    -L$ALSA/lib -lasound -lpthread -lm -ldl
```

**alsa-lib 要自己编**（staging_dir 是空的，sysroot 里没有 libasound）：

```sh
./configure --host=arm-openwrt-linux --prefix=$HOME/build/alsa/install \
    --disable-python --disable-static --enable-shared \
    CC=arm-openwrt-linux-gcc
make -j16 && make install
```

⚠️ 编译时 `export PATH` 里**不要只保留 toolchain 路径**，否则 `ls`/`cp`/`tail`
全部 `not found`。

## 测试

### 离线自检（不联网）

```sh
voicectl selftest      # mock 后端跑 LLM+TTS
```

### 加密与签名单元测试

```sh
# 阿里云签名（10 项：SHA1 / HMAC-SHA1 / Base64 / percent-encode / RPC 签名 / MD5）
$CC -static -O2 -o cryptotest tests/cryptotest.c crypto.c aliyun_sign.c
./cryptotest

# 腾讯云 TC3（6 项：SHA256 / HMAC-SHA256 / TC3 签名，含真实密钥验证的 cvm 用例）
$CC -static -O2 -o tc3test tests/tc3test.c crypto_sha256.c tencent_sign.c
./tc3test
```

**参考向量必须用独立实现生成**：

```sh
python tests/sign_ref.py    # 阿里云
python tests/tc3_ref.py     # 腾讯云 TC3
python tests/keyderive.py   # TC3 派生密钥链
```

⚠️ **不要用被测实现自己生成测试向量** —— Python 和 C 会犯同一个错，
互相验证「通过」但真实接口全 401。`tc3test` 第一版就是这么错的。

### 端到端

```sh
voicectl loopback      # TTS→ASR，验证识别准确率（4/4）
voicectl e2e           # 全链路，不用麦克风
```

## 已知限制

已解决的（历史记录）：
- ~~录音固定 N 秒，没有 VAD~~ → **已实现自适应 VAD**，说完 800ms 静音即停
- ~~没有唤醒词，靠命令行触发~~ → **已实现 trigger 常驻监听**（UART/GPIO），
  ASRPRO 模块到货即可接线
- ~~记忆只在进程内~~ → **已实现长期记忆**（profile.conf）+ 8 轮提炼

仍未做：
- **不能打断**（播放时无法用语音停止）
- **非流式**，ASR/TTS 都是整段处理。语音场景延迟主要在播放本身，流式收益有限
- **LLM 不能上网**，没有实现 function calling 的本地执行层
  （`hy3` 已实测支持标准 function calling，但需要工具执行层）
- **唤醒依赖外部模块**。板子 528MHz 单核跑不动本地 KWS 模型

## 踩过的坑（本项目特有）

| 坑 | 一句话 |
| --- | --- |
| WM8960 驱动没编进内核 | v2 内核缺 `SND_SOC_WM8960`/`IMX_WM8960`/`FSL_SAI`；`<<ABSENT>>` 的符号 `scripts/config --enable` 无效 |
| WM8960 挂在错误的 I2C 总线 | EVK dts 放 `&i2c2`（UART5 引脚），ALPHA V2.4 原理图接 `I2C1_SCL/SDA`（UART4 引脚）；`-EIO` = No ACK |
| mixer 默认关闭导致 DMA 不动 | DAPM 无活跃 widget 时不 trigger 播放流，`hw_ptr` 永远 0，报 I/O error |
| vfat 分区 sync 不落盘 | `cp`+`sync`+`reboot` 后内核没变；必须 `umount` 再 `mount` 验 md5 |
| TC3 签名 date/service 是斜杠 | `CredentialScope = date/service/tc3_request`，写成换行 100% 401 |
| TokenHub 端点文档里没有 | `tokenhub.tencentmaas.com`，只在控制台示例代码里 |
| 推理模型 content 为空 | `max_tokens` 被 `reasoning_content` 吃光，`finish_reason: "length"` |
| VAD 固定阈值失效 | 环境底噪 RMS 350~410 高于阈值 200 → 一启动就误判"在说话"；改自适应 |
| poll 被 SA_RESTART 吞信号 | `signal()` 隐式带 `SA_RESTART`，procd 停服务要等 30 秒；用 `sigaction` + `sa_flags=0` |
| 喇叭只接右声道，单声道播放全哑 | 板载喇叭接 SPK_R，而 SAI 在 `channels=1` 时每个 I2S 帧只填左 slot |

### 喇叭只接右声道 → 单声道播放必然全哑（2026-10-26）

这是最隐蔽的一个：**mixer 全开、`hw_ptr` 正常推进、返回码 0、DAPM 也起来了，但喇叭一声没有。**

硬件上，ALPHA 底板那颗喇叭只接在 **SPK_R（右声道）**，SPK_L 是空着的。
软件上，`main.c` 的播放调用是硬编码单声道的：

```c
audio_play_pcm(g_tts, tts_bytes, tts_rate, 1)   /* ← ch = 1 */
```

`hw:0,0` 的 hw_params 声明 `CHANNELS: [1 2]`，**真的接受 1ch**。于是 i.MX6ULL 的 SAI
以单声道运行，每个 I2S 帧只填 slot0（左），右声道 slot 一个采样都没有。

关键证据：

| 检查项 | 结果 |
| --- | --- |
| 播放 1ch 时 `pcm0p/sub0/hw_params` | `channels: 1`，确实按单声道打开 |
| SAI slot 掩码寄存器 | mono `0xfffffffe`（1 slot） / stereo `0xfffffffc`（2 slot） |
| `plughw:0,0` 能否救 | 不能。hw 本身支持 1ch，plug 层判定无需转换直接直通 |
| 麦克风回录同一段 1kHz | 旧路径幅度 **3.0** → 补丁后 **10035**（3300 倍） |
| 实播 TTS 回录 | peak 856 / rms 121（全底噪） → peak 32768 / rms 1404（两轮对话清晰） |

修复在 `audio_play_pcm()` 入口：`ch == 1` 时先展开成 `L = R` 的立体声再打开 PCM；
顺带把 `frame_bytes` 从 `pcm_open()` 之前挪到协商之后（`pcm_open` 可能改写 `ch`，
原来按入参算会错位）。

**复核手法**（不需要人耳朵）：板子 `card0` 的 playback/capture 共用同一个 SAI 不能同时开，
但板上有 USB 摄像头 = `card1`，拿它当耳朵：

```sh
arecord -D hw:1,0 -f S16_LE -r 48000 -c 1 -d 30 /tmp/rec.wav &
voicectl e2e
wait
# 拉回 PC 算 1kHz 幅度 / 看 rms
```

**两个连带坑**：

1. `/sys/kernel/debug/regmap/0-001a/registers` 直接 `cat` 是空的（wm8960 寄存器被标成不可读），
   要 `echo 1 > cache_only` 才出值；**用完必须 `echo 0`**，否则寄存器只进 cache 不落硬件，
   下次会看到「配置全对但没声」的假象。想看声道配置直接读 `202c000.sai` 的 regmap 更快。
2. 混用 16k 录音 + 48k 播放会把 SAI 时钟状态搞坏（dmesg: `unmatched rate symmetry: 48000 - 16000`），
   之后所有 `aplay` 都 `pcm_write: I/O error`，软复位救不回来，**重启板子即恢复**。
   voice 全链路都是 16k，正常不会触发。

完整的根因分析见 Obsidian 笔记库 `D:\Notes`。

## 相关文档

- 项目笔记：`Obsidian: 项目-voice语音助手`
- 踩坑记录：`坑-WM8960驱动没编进内核` / `坑-WM8960挂在错误的I2C总线` /
  `坑-mixer默认关闭导致DMA不动` / `坑-vfat分区sync不落盘` /
  `坑-TC3签名date与service之间是斜杠` / `坑-腾讯大模型接入三层权限` /
  `坑-推理模型的content为空` / `坑-vad固定阈值失效` / `坑-poll与SA_RESTART`
