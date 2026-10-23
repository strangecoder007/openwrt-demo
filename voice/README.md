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
| `voicectl chat` | 多轮对话，Ctrl-C 退出 |
| `voicectl e2e` | 全自动端到端（TTS出题→ASR→LLM→TTS→播放，**不用麦克风**） |
| `voicectl loopback` | TTS→ASR 回环（验证识别准确率） |
| `voicectl llmtest "文本"` | 纯文本测大模型延迟 |
| `voicectl selftest` | 离线自检（mock 后端，不联网） |
| `voicectl info` | 声卡能力 + 后端配置 |
| `voicectl record/play/loop` | 录音 / 播放 / 回放 |

`voicectl` 读 `/etc/voice.conf` 后 export 环境变量，再调 `/usr/bin/voice`。

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

- **录音固定 N 秒**，没有 VAD。说完话要干等，这是最大延迟来源
- **没有唤醒词**，靠命令行/SSH 触发
- **不能打断**（播放时无法用语音停止）
- **非流式**，ASR/TTS 都是整段处理
- **记忆只在进程内**，进程退出即失
- **LLM 不能上网**，没有实现 function calling 的本地执行层

## 相关文档

- 项目笔记：`Obsidian: 项目-voice语音助手`
- 踩坑记录：`坑-WM8960驱动没编进内核` / `坑-WM8960挂在错误的I2C总线` /
  `坑-mixer默认关闭导致DMA不动` / `坑-vfat分区sync不落盘` /
  `坑-TC3签名date与service之间是斜杠` / `坑-腾讯大模型接入三层权限` /
  `坑-推理模型的content为空`
