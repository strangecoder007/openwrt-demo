/*
 * main.c —— 语音助手主程序
 *
 * 命令：
 *   voice info                    设备信息
 *   voice record <f> [s] [r]      录音
 *   voice play <f>                播放
 *   voice loop [s]                录音+回放
 *   voice ask [s]                 一轮完整对话（录音→ASR→LLM→TTS→播放）
 *   voice chat                    交互式多轮对话
 *   voice selftest                自检：跑一遍全部 mock 链路
 *
 * 环境变量：
 *   ASR_BACKEND=mock|aliyun
 *   LLM_BACKEND=mock|dashscope
 *   TTS_BACKEND=mock|aliyun
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

#include "audio.h"
#include "asr.h"
#include "llm.h"
#include "tts.h"

#define PCM_BUF_SIZE   (16000 * 2 * 10)   /* 10 秒 16kHz 单声道 */
#define TTS_BUF_SIZE   (16000 * 2 * 30)   /* 30 秒 */

static unsigned char g_pcm[PCM_BUF_SIZE];
static unsigned char g_tts[PCM_BUF_SIZE * 3];

static volatile int g_quit = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
    audio_stop();
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* ---------- 后端选择 ---------- */
static asr_backend_t pick_asr(void)
{
    const char *v = getenv("ASR_BACKEND");
    if (v && !strcmp(v, "aliyun"))  return ASR_BACKEND_ALIYUN;
    if (v && !strcmp(v, "tencent")) return ASR_BACKEND_TENCENT;
    return ASR_BACKEND_MOCK;
}

static llm_backend_t pick_llm(void)
{
    const char *v = getenv("LLM_BACKEND");
    if (v && (!strcmp(v, "openai") || !strcmp(v, "dashscope") ||
              !strcmp(v, "hunyuan") || !strcmp(v, "tencent")))
        return LLM_BACKEND_OPENAI;
    return LLM_BACKEND_MOCK;
}

static tts_backend_t pick_tts(void)
{
    const char *v = getenv("TTS_BACKEND");
    if (v && !strcmp(v, "aliyun"))  return TTS_BACKEND_ALIYUN;
    if (v && !strcmp(v, "tencent")) return TTS_BACKEND_TENCENT;
    return TTS_BACKEND_MOCK;
}

/* ---------- 一轮对话 ---------- */
static int one_round(unsigned secs, int verbose)
{
    char text[ASR_TEXT_MAX] = {0};
    char reply[LLM_REPLY_MAX] = {0};
    size_t tts_bytes = 0;
    unsigned tts_rate = 0;
    long pcm_bytes;
    double t0, t1, t2, t3, t4;

    printf("\n>>> 请说话（%u 秒）...\n", secs);
    fflush(stdout);

    /* 1. 录音 */
    t0 = now_ms();
    pcm_bytes = audio_record_buf(g_pcm, sizeof(g_pcm), AUDIO_RATE,
                                 AUDIO_CHANNELS, secs);
    t1 = now_ms();
    if (pcm_bytes <= 0) {
        fprintf(stderr, "录音失败\n");
        return -1;
    }
    printf("  [录音] %.2f 秒音频, %ld 字节, 耗时 %.0fms\n",
           (double)pcm_bytes / (AUDIO_RATE * 2), pcm_bytes, t1 - t0);

    /* 2. 识别 */
    if (asr_recognize(g_pcm, (size_t)pcm_bytes, text, sizeof(text)) < 0) {
        fprintf(stderr, "识别失败\n");
        return -1;
    }
    t2 = now_ms();
    printf("  [ASR]  \"%s\"  (%.0fms)\n", text, t2 - t1);

    /* 3. 对话 */
    if (llm_chat(text, reply, sizeof(reply)) < 0) {
        fprintf(stderr, "对话失败\n");
        return -1;
    }
    t3 = now_ms();
    printf("  [LLM]  \"%s\"  (%.0fms)\n", reply, t3 - t2);

    /* 4. 合成 */
    if (tts_synthesize(reply, g_tts, sizeof(g_tts), &tts_bytes, &tts_rate) < 0) {
        fprintf(stderr, "合成失败\n");
        return -1;
    }
    t4 = now_ms();
    printf("  [TTS]  %zu 字节 @%uHz  (%.0fms)\n", tts_bytes, tts_rate, t4 - t3);

    /* 5. 播放 */
    if (tts_bytes > 0) {
        double p0 = now_ms();
        if (audio_play_pcm(g_tts, tts_bytes, tts_rate, 1) < 0) {
            fprintf(stderr, "播放失败\n");
            return -1;
        }
        printf("  [播放] 耗时 %.0fms\n", now_ms() - p0);
    }

    if (verbose) {
        printf("  ─────────────────────────────\n");
        printf("  总耗时: %.0f ms\n", now_ms() - t0);
    }
    return 0;
}

/* ---------- 自检 ---------- */
static int selftest(void)
{
    const char *cases[] = {
        "你好",
        "今天天气怎么样",
        "给我讲个笑话",
        "你是谁",
    };
    int i, pass = 0, total = 0;
    char reply[LLM_REPLY_MAX];
    size_t tts_bytes;
    unsigned tts_rate;

    printf("=== 自检 ===\n");
    printf("ASR 后端: %s\n", asr_backend_name());
    printf("LLM 后端: %s\n", llm_backend_name());
    printf("TTS 后端: %s\n\n", tts_backend_name());

    /* LLM + TTS 链路测试（不需要录音） */
    for (i = 0; i < (int)(sizeof(cases)/sizeof(cases[0])); i++) {
        total++;
        printf("[%d] 输入: \"%s\"\n", i + 1, cases[i]);
        if (llm_chat(cases[i], reply, sizeof(reply)) < 0) {
            printf("    LLM 失败\n");
            continue;
        }
        printf("    回复: \"%s\"\n", reply);
        tts_bytes = 0;
        if (tts_synthesize(reply, g_tts, sizeof(g_tts), &tts_bytes, &tts_rate) < 0) {
            printf("    TTS 失败\n");
            continue;
        }
        printf("    合成: %zu 字节 @%uHz\n", tts_bytes, tts_rate);
        pass++;
    }
    printf("\n结果: %d/%d 通过\n", pass, total);
    return (pass == total) ? 0 : 1;
}

/* ---------- 回环测试：TTS → ASR ---------- */
/*
 * 合成几句已知文本，把 PCM 直接喂给 ASR，比对识别结果。
 * 不依赖麦克风和真人说话，验证 ASR/TTS 两端是否互通。
 */
static int loopback(void)
{
    static const char *cases[] = {
        "你好",
        "今天天气怎么样",
        "给我讲个笑话",
        "再见",
    };
    int i, pass = 0, total = 0;

    printf("=== 回环测试 TTS -> ASR ===\n");
    printf("ASR: %s   TTS: %s\n\n", asr_backend_name(), tts_backend_name());

    for (i = 0; i < (int)(sizeof(cases)/sizeof(cases[0])); i++) {
        char text[ASR_TEXT_MAX] = {0};
        size_t n = 0;
        unsigned rate = 0;

        total++;
        printf("[%d] 原始文本: \"%s\"\n", i + 1, cases[i]);

        if (tts_synthesize(cases[i], g_tts, sizeof(g_tts), &n, &rate) < 0) {
            printf("    TTS 失败\n");
            continue;
        }
        printf("    TTS: %zu 字节 @%uHz\n", n, rate);

        if (rate != 16000) {
            printf("    跳过：采样率不是 16000\n");
            continue;
        }

        if (asr_recognize(g_tts, n, text, sizeof(text)) < 0) {
            printf("    ASR 失败\n");
            continue;
        }
        printf("    ASR: \"%s\"\n", text);

        if (strstr(text, cases[i]) || (text[0] && strstr(cases[i], text))) {
            printf("    结果: 匹配 ✓\n\n");
            pass++;
        } else {
            printf("    结果: 不匹配（可能需要忽略标点/同音字）\n\n");
        }
    }

    printf("结果: %d/%d 匹配\n", pass, total);
    return 0;
}

/* ---------- 纯文本对话测试（不走录音/播放） ---------- */
static int llmtest(const char *text)
{
    char reply[LLM_REPLY_MAX] = {0};
    double t0, t1;

    printf("=== LLM 文本测试 ===\n");
    printf("后端: %s\n", llm_backend_name());
    printf("输入: %s\n", text);

    t0 = now_ms();
    if (llm_chat(text, reply, sizeof(reply)) < 0) {
        printf("失败\n");
        return 1;
    }
    t1 = now_ms();

    printf("回复: %s\n", reply);
    printf("耗时: %.0f ms\n", t1 - t0);
    return 0;
}

/* ---------- 全自动端到端：TTS 出题 -> ASR -> LLM -> TTS -> 播放 ---------- */
static int e2e(void)
{
    static const char *questions[] = {
        "你好",
        "你叫什么名字",
    };
    int i;

    printf("=== 端到端测试 ===\n");
    printf("ASR: %s   TTS: %s   LLM: %s\n\n",
           asr_backend_name(), tts_backend_name(), llm_backend_name());

    for (i = 0; i < (int)(sizeof(questions)/sizeof(questions[0])); i++) {
        char heard[ASR_TEXT_MAX] = {0};
        char reply[LLM_REPLY_MAX] = {0};
        size_t q_bytes = 0, a_bytes = 0;
        unsigned q_rate = 0, a_rate = 0;
        double t0, t1;

        printf("──── 第 %d 轮 ────\n", i + 1);
        printf("[问题] \"%s\"\n", questions[i]);

        /* 1. TTS 把问题变成语音（模拟人说话） */
        t0 = now_ms();
        if (tts_synthesize(questions[i], g_tts, sizeof(g_tts),
                           &q_bytes, &q_rate) < 0 || q_bytes == 0) {
            printf("  TTS 失败\n\n");
            continue;
        }
        printf("  TTS   : %zu 字节 @%uHz\n", q_bytes, q_rate);

        /* 2. ASR 识别这段语音 */
        if (asr_recognize(g_tts, q_bytes, heard, sizeof(heard)) < 0) {
            printf("  ASR 失败\n\n");
            continue;
        }
        printf("  ASR   : \"%s\"\n", heard);

        /* 3. 大模型回答 */
        if (llm_chat(heard, reply, sizeof(reply)) < 0) {
            printf("  LLM 失败\n\n");
            continue;
        }
        printf("  LLM   : \"%s\"\n", reply);

        /* 4. 合成回答 */
        if (tts_synthesize(reply, g_tts, sizeof(g_tts),
                           &a_bytes, &a_rate) < 0 || a_bytes == 0) {
            printf("  TTS(答) 失败\n\n");
            continue;
        }
        printf("  TTS(答): %zu 字节 @%uHz\n", a_bytes, a_rate);

        /* 5. 播放 */
        if (audio_play_pcm(g_tts, a_bytes, a_rate, 1) < 0) {
            printf("  播放失败\n\n");
            continue;
        }
        t1 = now_ms();
        printf("  播放完成，总耗时 %.0f ms\n\n", t1 - t0);
    }

    printf("=== 端到端测试结束 ===\n");
    return 0;
}

/* ---------- 用法 ---------- */
static void usage(const char *p)
{
    printf("用法:\n");
    printf("  %s info                     设备信息\n", p);
    printf("  %s record <f> [s] [r]       录音\n", p);
    printf("  %s play <f>                 播放\n", p);
    printf("  %s loop [s]                 录音+回放\n", p);
    printf("  %s ask [s]                  一轮对话（录音→ASR→LLM→TTS→播放）\n", p);
    printf("  %s chat                     交互式多轮对话\n", p);
    printf("  %s selftest                 自检（不录音）\n", p);
    printf("  %s loopback                 回环测试（TTS→ASR，不用麦克风）\n", p);
    printf("  %s llmtest \"文本\"            纯文本测试大模型\n", p);
    printf("  %s e2e                      全自动端到端（TTS→ASR→LLM→TTS→播放）\n", p);
    printf("\n环境变量:\n");
    printf("  ASR_BACKEND = mock|aliyun|tencent\n");
    printf("  TTS_BACKEND = mock|aliyun|tencent\n");
    printf("  LLM_BACKEND = mock|dashscope\n");
    printf("\n腾讯云凭据:\n");
    printf("  TENCENT_SECRET_ID / TENCENT_SECRET_KEY / TENCENT_APP_ID\n");
}

int main(int argc, char **argv)
{
    unsigned secs = 3;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    asr_init(pick_asr());
    llm_init(pick_llm());
    tts_init(pick_tts());

    if (argc < 2) { usage(argv[0]); return 1; }

    const char *cmd = argv[1];

    if (!strcmp(cmd, "info")) {
        audio_info();
        printf("\nASR: %s  LLM: %s  TTS: %s\n",
               asr_backend_name(), llm_backend_name(), tts_backend_name());
        return 0;
    }

    if (!strcmp(cmd, "selftest"))
        return selftest();

    if (!strcmp(cmd, "loopback"))
        return loopback();

    if (!strcmp(cmd, "llmtest")) {
        if (argc < 3) { usage(argv[0]); return 1; }
        return llmtest(argv[2]);
    }

    if (!strcmp(cmd, "e2e"))
        return e2e();

    if (!strcmp(cmd, "record")) {
        if (argc < 3) { usage(argv[0]); return 1; }
        if (argc >= 4) secs = (unsigned)atoi(argv[3]);
        long n = audio_record(argv[2], AUDIO_RATE, AUDIO_CHANNELS, secs);
        if (n < 0) return 1;
        printf("录音完成: %ld 帧 -> %s\n", n, argv[2]);
        return 0;
    }

    if (!strcmp(cmd, "play")) {
        if (argc < 3) { usage(argv[0]); return 1; }
        return audio_play(argv[2]) == 0 ? 0 : 1;
    }

    if (!strcmp(cmd, "loop")) {
        if (argc >= 3) secs = (unsigned)atoi(argv[2]);
        if (audio_record("/tmp/voice_loop.wav", AUDIO_RATE,
                         AUDIO_CHANNELS, secs) < 0)
            return 1;
        printf("--- 回放 ---\n");
        return audio_play("/tmp/voice_loop.wav") == 0 ? 0 : 1;
    }

    if (!strcmp(cmd, "ask")) {
        if (argc >= 3) secs = (unsigned)atoi(argv[2]);
        return one_round(secs, 1) == 0 ? 0 : 1;
    }

    if (!strcmp(cmd, "chat")) {
        int n = 0;
        printf("交互模式，Ctrl-C 退出\n");
        while (!g_quit) {
            printf("\n─── 第 %d 轮 ───", ++n);
            if (one_round(secs, 0) < 0)
                printf("  本轮失败\n");
        }
        printf("\n退出\n");
        return 0;
    }

    usage(argv[0]);
    return 1;
}
