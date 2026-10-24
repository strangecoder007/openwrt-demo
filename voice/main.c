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
#include "profile.h"
#include "trigger.h"

#define PCM_BUF_SIZE   (16000 * 2 * 10)   /* 10 秒 16kHz 单声道 */
#define TTS_BUF_SIZE   (16000 * 2 * 30)   /* 30 秒 */

static unsigned char g_pcm[PCM_BUF_SIZE];
static unsigned char g_tts[PCM_BUF_SIZE * 3];

static volatile int g_quit = 0;

/* 长期记忆 */
static profile_t g_profile;
static int g_rounds_since_extract = 0;

/*
 * 提炼周期：不能超过短期记忆容量（HIST_MAX=8），
 * 否则被环形缓冲覆盖的对话永远提炼不到。
 */
#define EXTRACT_EVERY_ROUNDS  8

static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
    audio_stop();
}

/* 触发一次长期记忆提炼 */
static void maybe_extract_profile(int force)
{
    char dialog[8192];
    char summary[512];
    int before = g_profile.fact_count;

    if (!force && g_rounds_since_extract < EXTRACT_EVERY_ROUNDS)
        return;
    if (llm_history_count() == 0)
        return;

    llm_dump_dialog(dialog, sizeof(dialog), EXTRACT_EVERY_ROUNDS);

    fprintf(stderr, "\n[profile] 开始提炼（%d 轮对话）...\n",
            llm_history_count());

    if (profile_extract(dialog, &g_profile) == 0) {
        g_profile.total_rounds += g_rounds_since_extract;
        {
            time_t t = time(NULL);
            struct tm tmv;
            localtime_r(&t, &tmv);
            strftime(g_profile.last_seen, sizeof(g_profile.last_seen),
                     "%Y-%m-%dT%H:%M:%S", &tmv);
        }
        profile_save(&g_profile);

        /* 更新注入到 llm 的摘要 */
        profile_to_prompt(&g_profile, summary, sizeof(summary));
        llm_set_profile(summary);

        fprintf(stderr, "[profile] 提炼完成（事实 %d -> %d 条）\n",
                before, g_profile.fact_count);
    }

    g_rounds_since_extract = 0;
    llm_reset_history();
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
    unsigned rec_ms = 0;
    long pcm_bytes;
    double t0, t1, t2, t3, t4;
    const char *mode = getenv("VOICE_VAD");
    int use_vad = (mode && strcmp(mode, "0") != 0);
    int start_rms = 0;          /* 0 = 自适应底噪（默认） */
    unsigned silence_ms = 800;

    {
        const char *v = getenv("VOICE_VAD_RMS");
        if (v) start_rms = atoi(v);      /* >0 固定阈值，0 自适应 */
        v = getenv("VOICE_VAD_SILENCE");
        if (v && atoi(v) > 0) silence_ms = (unsigned)atoi(v);
    }

    t0 = now_ms();

    if (use_vad) {
        printf("\n>>> 请说话（检测到 %ums 静音即停）...\n", silence_ms);
        fflush(stdout);
        pcm_bytes = audio_record_vad(g_pcm, sizeof(g_pcm), AUDIO_RATE,
                                     AUDIO_CHANNELS, start_rms, silence_ms,
                                     secs ? secs : 15, &rec_ms);
    } else {
        printf("\n>>> 请说话（固定 %u 秒）...\n", secs);
        fflush(stdout);
        pcm_bytes = audio_record_buf(g_pcm, sizeof(g_pcm), AUDIO_RATE,
                                     AUDIO_CHANNELS, secs);
        rec_ms = (unsigned)(pcm_bytes / (AUDIO_RATE * 2) * 1000);
    }

    t1 = now_ms();
    if (pcm_bytes <= 0) {
        if (use_vad)
            fprintf(stderr, "  没有检测到语音（%u ms 内无有效输入）\n", rec_ms);
        else
            fprintf(stderr, "录音失败\n");
        return -1;
    }
    printf("  [录音] %.2f 秒音频, %ld 字节, 耗时 %.0fms\n",
           rec_ms / 1000.0, pcm_bytes, t1 - t0);
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

    /* 一轮成功，计数并检查是否需要提炼长期记忆 */
    g_rounds_since_extract++;
    maybe_extract_profile(0);

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
    printf("  %s profile                  查看长期记忆\n", p);
    printf("  %s forget                   清除长期记忆\n", p);
    printf("  %s trigger [选项]           常驻监听唤醒\n", p);
    printf("      -u /dev/ttymxc1           监听串口（ASRPRO）\n");
    printf("      -b 115200                 波特率\n");
    printf("      -k WAKE                   关键字\n");
    printf("      -g 42                     监听 GPIO（sysfs 号）\n");
    printf("      -a 1                      GPIO 高电平触发（0=低）\n");
    printf("      -d 3000                   防抖毫秒\n");
    printf("      -s 15                     触发后录多少秒\n");
    printf("      -v                        打印原始数据\n");
    printf("\n环境变量:\n");
    printf("  ASR_BACKEND = mock|aliyun|tencent\n");
    printf("  TTS_BACKEND = mock|aliyun|tencent\n");
    printf("  LLM_BACKEND = mock|openai\n");
    printf("  VOICE_VAD = 1（默认）|0       说话才录，说完即停\n");
    printf("  VOICE_VAD_RMS = 0(自适应)|数字 起始能量阈值\n");
    printf("  VOICE_VAD_SILENCE = 800      静音多少毫秒算说完\n");
    printf("\n长期记忆:\n");
    printf("  存放 /var/lib/voice/profile.conf\n");
    printf("  每 %d 轮自动提炼一次，退出时也会提炼\n", EXTRACT_EVERY_ROUNDS);
    printf("\n腾讯云凭据:\n");
    printf("  TENCENT_SECRET_ID / TENCENT_SECRET_KEY / TENCENT_APP_ID\n");
}

int main(int argc, char **argv)
{
    unsigned secs = 3;
    profile_t prof;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    asr_init(pick_asr());
    llm_init(pick_llm());
    tts_init(pick_tts());

    /* 加载长期记忆，注入 system prompt */
    memset(&prof, 0, sizeof(prof));
    profile_load(&prof);
    g_profile = prof;
    {
        char summary[512];
        profile_to_prompt(&g_profile, summary, sizeof(summary));
        if (summary[0]) {
            llm_set_profile(summary);
            fprintf(stderr, "[main] 已加载用户档案\n");
        }
    }

    if (argc < 2) { usage(argv[0]); return 1; }

    const char *cmd = argv[1];

    if (!strcmp(cmd, "info")) {
        audio_info();
        printf("\nASR: %s  LLM: %s  TTS: %s\n",
               asr_backend_name(), llm_backend_name(), tts_backend_name());
        profile_dump(&g_profile);
        return 0;
    }

    if (!strcmp(cmd, "profile")) {
        profile_dump(&g_profile);
        return 0;
    }

    if (!strcmp(cmd, "forget")) {
        profile_clear(&g_profile);
        profile_save(&g_profile);
        llm_set_profile("");
        printf("已清除长期记忆\n");
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
        int rc = one_round(secs, 1);
        /* ask 模式单轮结束后也提炼（如果到达周期） */
        maybe_extract_profile(0);
        return rc == 0 ? 0 : 1;
    }

    if (!strcmp(cmd, "chat")) {
        int n = 0;
        int consecutive_fail = 0;
        const int MAX_FAIL = 2;

        printf("交互模式，Ctrl-C 退出；连续 %d 次无语音自动结束\n", MAX_FAIL);
        while (!g_quit) {
            printf("\n─── 第 %d 轮 ───", ++n);
            if (one_round(secs, 0) < 0) {
                printf("  本轮失败\n");
                if (++consecutive_fail >= MAX_FAIL) {
                    printf("\n连续 %d 次没有语音输入，自动结束\n", MAX_FAIL);
                    break;
                }
            } else {
                consecutive_fail = 0;
            }
        }

        /* 退出前：把本轮会话提炼进长期记忆 */
        if (llm_history_count() > 0)
            maybe_extract_profile(1);

        printf("\n退出\n");
        return 0;
    }

    if (!strcmp(cmd, "trigger")) {
        /*
         * voice trigger [选项]
         *   -u /dev/ttymxc1     监听串口
         *   -b 115200           波特率
         *   -k WAKE             关键字（默认 WAKE）
         *   -g 42               监听 GPIO 42（sysfs 号）
         *   -a 1                GPIO 高电平触发（0 = 低电平）
         *   -d 3000             防抖毫秒
         *   -s 15               触发后录多少秒
         *   -v                  打印原始数据
         */
        trigger_cfg_t tcfg;
        int i;

        memset(&tcfg, 0, sizeof(tcfg));
        tcfg.uart_dev = NULL;
        tcfg.uart_baud = 115200;
        tcfg.uart_keyword = "WAKE";
        tcfg.gpio_num = -1;
        tcfg.gpio_active = 1;
        tcfg.debounce_ms = 3000;
        tcfg.ask_secs = 15;
        tcfg.voicectl = "/usr/bin/voicectl";
        tcfg.verbose = 0;

        for (i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "-u") && i + 1 < argc)
                tcfg.uart_dev = argv[++i];
            else if (!strcmp(argv[i], "-b") && i + 1 < argc)
                tcfg.uart_baud = atoi(argv[++i]);
            else if (!strcmp(argv[i], "-k") && i + 1 < argc)
                tcfg.uart_keyword = argv[++i];
            else if (!strcmp(argv[i], "-g") && i + 1 < argc)
                tcfg.gpio_num = atoi(argv[++i]);
            else if (!strcmp(argv[i], "-a") && i + 1 < argc)
                tcfg.gpio_active = atoi(argv[++i]);
            else if (!strcmp(argv[i], "-d") && i + 1 < argc)
                tcfg.debounce_ms = (unsigned)atoi(argv[++i]);
            else if (!strcmp(argv[i], "-s") && i + 1 < argc)
                tcfg.ask_secs = (unsigned)atoi(argv[++i]);
            else if (!strcmp(argv[i], "-v"))
                tcfg.verbose = 1;
            else {
                fprintf(stderr, "未知参数: %s\n", argv[i]);
                return 1;
            }
        }

        if (!tcfg.uart_dev && tcfg.gpio_num < 0) {
            fprintf(stderr,
                "至少要指定一个触发源：\n"
                "  -u /dev/ttymxc1   监听串口（ASRPRO 唤醒模块）\n"
                "  -g 42             监听 GPIO\n");
            return 1;
        }

        return trigger_run(&tcfg);
    }

    usage(argv[0]);
    return 1;
}
