/*
 * tts.c —— 语音合成实现
 *
 * MOCK  : 本地提示音（按文本长度给音节）
 * ALIYUN: 阿里云 NLS 语音合成（返回 WAV）
 */
#include "tts.h"
#include "aliyun_nls.h"
#include "tencent_nls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static tts_backend_t g_backend = TTS_BACKEND_MOCK;
static int g_init = 0;
static tts_opts_t g_opts = { "xiaoyun", 16000, 50 };
static aliyun_nls_t  g_nls;
static int g_nls_ready = 0;
static tencent_nls_t g_tc;
static int g_tc_ready = 0;

/* ================= MOCK ================= */
static int mock_synthesize(const char *text, void *pcm_out, size_t cap,
                           size_t *out_len, unsigned *rate)
{
    const unsigned R = 16000;
    const int AMP = 8000;
    const int SYL_MS = 110;
    const int GAP_MS = 45;
    short *out = (short *)pcm_out;
    size_t max_samples = cap / 2;
    size_t pos = 0;
    int text_len, syls, s, i;

    if (!text || !*text) { *out_len = 0; *rate = R; return 0; }

    text_len = (int)strlen(text);
    syls = text_len / 3 + 1;
    if (syls > 30) syls = 30;

    for (s = 0; s < syls; s++) {
        int syl_samples = R * SYL_MS / 1000;
        int gap_samples = R * GAP_MS / 1000;
        double freq = 650.0 + (s % 5) * 60.0;

        for (i = 0; i < syl_samples; i++) {
            if (pos >= max_samples) goto done;
            double env = 1.0;
            int edge = syl_samples / 8;
            if (i < edge) env = (double)i / edge;
            else if (i > syl_samples - edge) env = (double)(syl_samples - i) / edge;
            out[pos++] = (short)(AMP * env * sin(2 * M_PI * freq * i / R));
        }
        for (i = 0; i < gap_samples; i++) {
            if (pos >= max_samples) goto done;
            out[pos++] = 0;
        }
    }

done:
    *out_len = pos * 2;
    *rate = R;
    fprintf(stderr, "[tts-mock] \"%s\" -> %d 音节, %zu 字节 @%uHz\n",
            text, syls, *out_len, R);
    return 0;
}

/* ================= 阿里云 ================= */
static int aliyun_synthesize(const char *text, void *pcm_out, size_t cap,
                             size_t *out_len, unsigned *rate)
{
    unsigned char *wav;
    size_t wav_len = 0;
    int rc;

    *out_len = 0;
    *rate = 16000;

    if (!g_nls_ready) {
        fprintf(stderr, "[tts-aliyun] 缺少 AK/AppKey\n");
        return -1;
    }

    /* NLS TTS 返回 WAV，先收到临时缓冲 */
    wav = malloc(cap);
    if (!wav) return -1;

    rc = nls_tts(&g_nls, text, wav, cap, &wav_len);
    if (rc < 0) {
        free(wav);
        return -2;
    }

    /* 解析 WAV 头，把 PCM 部分交给调用方（audio_play_pcm 要裸 PCM） */
    if (wav_len < 44) {
        free(wav);
        return -3;
    }

    /* 简单解析：找 "data" chunk */
    unsigned char *pcm = NULL;
    size_t pcm_len = 0;
    unsigned r = 16000, ch = 1;

    /* 标准 44 字节头 */
    unsigned char *p = wav + 12;
    while (p + 8 < wav + wav_len) {
        unsigned chunk_len = p[4] | (p[5] << 8) | (p[6] << 16) | ((unsigned)p[7] << 24);
        if (!memcmp(p, "fmt ", 4)) {
            ch = p[10] | (p[11] << 8);
            r  = p[12] | (p[13] << 8) | (p[14] << 16) | ((unsigned)p[15] << 24);
        } else if (!memcmp(p, "data", 4)) {
            pcm = p + 8;
            pcm_len = chunk_len;
            if (pcm_len > (size_t)(wav + wav_len - pcm))
                pcm_len = (size_t)(wav + wav_len - pcm);
            break;
        }
        p += 8 + chunk_len + (chunk_len & 1);
    }

    if (!pcm || pcm_len == 0) {
        free(wav);
        return -4;
    }

    if (pcm_len > cap) pcm_len = cap;
    memcpy(pcm_out, pcm, pcm_len);

    *out_len = pcm_len;
    *rate = r;

    fprintf(stderr, "[tts-aliyun] OK: %zu 字节 PCM @%uHz %uch\n",
            pcm_len, r, ch);

    free(wav);
    return 0;
}

/* ================= 腾讯云 ================= */
static int tencent_synthesize(const char *text, void *pcm_out, size_t cap,
                              size_t *out_len, unsigned *rate)
{
    *out_len = 0;
    *rate = 16000;

    if (!g_tc_ready) {
        fprintf(stderr, "[tts-tencent] 缺少 SecretId/SecretKey\n");
        return -1;
    }

    return tc_tts(&g_tc, text, pcm_out, cap, out_len);
}

/* ================= 公共 ================= */
int tts_init(tts_backend_t backend)
{
    const char *note = "";

    g_backend = backend;

    /* 阿里云 */
    {
        const char *ak_id  = getenv("ALIYUN_AK_ID");
        const char *ak_sec = getenv("ALIYUN_AK_SECRET");
        const char *appkey = getenv("ALIYUN_APPKEY");
        if (ak_id && ak_sec && appkey && ak_id[0] && ak_sec[0] && appkey[0]) {
            nls_init(&g_nls, ak_id, ak_sec, appkey);
            g_nls_ready = 1;
        }
    }

    /* 腾讯云 */
    {
        const char *id  = getenv("TENCENT_SECRET_ID");
        const char *key = getenv("TENCENT_SECRET_KEY");
        const char *app = getenv("TENCENT_APP_ID");
        const char *rg  = getenv("TENCENT_REGION");
        if (id && key && id[0] && key[0]) {
            tc_init(&g_tc, id, key, app ? app : "", rg ? rg : "ap-shanghai");
            g_tc_ready = 1;
        }
    }

    g_init = 1;
    if (g_backend == TTS_BACKEND_ALIYUN && !g_nls_ready) note = " (凭据缺失)";
    if (g_backend == TTS_BACKEND_TENCENT && !g_tc_ready) note = " (凭据缺失)";
    fprintf(stderr, "[tts] 初始化: %s%s\n", tts_backend_name(), note);
    return 0;
}

int tts_synthesize(const char *text, void *pcm_out, size_t cap,
                   size_t *out_len, unsigned *rate)
{
    if (!g_init) return -1;
    if (!text || !pcm_out || !out_len || !rate) return -1;

    switch (g_backend) {
    case TTS_BACKEND_MOCK:    return mock_synthesize(text, pcm_out, cap, out_len, rate);
    case TTS_BACKEND_ALIYUN:  return aliyun_synthesize(text, pcm_out, cap, out_len, rate);
    case TTS_BACKEND_TENCENT: return tencent_synthesize(text, pcm_out, cap, out_len, rate);
    default:                  return -1;
    }
}

void tts_deinit(void) { g_init = 0; g_nls_ready = 0; g_tc_ready = 0; }

void tts_set_opts(const tts_opts_t *opts) { if (opts) g_opts = *opts; }

const char *tts_backend_name(void)
{
    switch (g_backend) {
    case TTS_BACKEND_MOCK:    return "mock";
    case TTS_BACKEND_ALIYUN:  return "aliyun";
    case TTS_BACKEND_TENCENT: return "tencent";
    default:                  return "unknown";
    }
}
