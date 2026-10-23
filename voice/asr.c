/*
 * asr.c —— 语音识别实现
 *
 * MOCK     : 本地假实现（按能量分档）
 * ALIYUN   : 阿里云 NLS 一句话识别
 * TENCENT  : 腾讯云一句话识别
 */
#include "asr.h"
#include "aliyun_nls.h"
#include "tencent_nls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static asr_backend_t g_backend = ASR_BACKEND_MOCK;
static int g_init = 0;

static aliyun_nls_t  g_ali;
static int g_ali_ready = 0;

static tencent_nls_t g_tc;
static int g_tc_ready = 0;

/* ================= MOCK ================= */
static int mock_recognize(const void *pcm, size_t bytes, char *out, size_t outlen)
{
    const short *s = (const short *)pcm;
    size_t n = bytes / 2;
    long long energy = 0;
    size_t i;

    if (n == 0) { snprintf(out, outlen, "(无音频)"); return 0; }

    for (i = 0; i < n; i++) {
        long long v = s[i];
        energy += (v < 0 ? -v : v);
    }
    energy /= (long long)n;

    if (energy < 50)        snprintf(out, outlen, "(静音)");
    else if (energy < 300)  snprintf(out, outlen, "今天天气怎么样");
    else if (energy < 1500) snprintf(out, outlen, "帮我放一首歌");
    else                    snprintf(out, outlen, "你好，我是语音助手");

    fprintf(stderr, "[asr-mock] %zu 字节, 平均能量 %lld -> \"%s\"\n",
            bytes, energy, out);
    return 0;
}

/* ================= 阿里云 ================= */
static int aliyun_recognize(const void *pcm, size_t bytes, char *out, size_t outlen)
{
    if (!g_ali_ready) {
        snprintf(out, outlen, "(阿里云未配置)");
        fprintf(stderr, "[asr-aliyun] 缺少 AK/AppKey\n");
        return -1;
    }
    return nls_asr(&g_ali, pcm, bytes, out, outlen);
}

/* ================= 腾讯云 ================= */
static int tencent_recognize(const void *pcm, size_t bytes, char *out, size_t outlen)
{
    if (!g_tc_ready) {
        snprintf(out, outlen, "(腾讯云未配置)");
        fprintf(stderr, "[asr-tencent] 缺少 SecretId/SecretKey\n");
        return -1;
    }
    return tc_asr(&g_tc, pcm, bytes, out, outlen);
}

/* ================= 公共 ================= */
int asr_init(asr_backend_t backend)
{
    const char *note = "";

    g_backend = backend;

    /* 阿里云 */
    {
        const char *id = getenv("ALIYUN_AK_ID");
        const char *sc = getenv("ALIYUN_AK_SECRET");
        const char *ak = getenv("ALIYUN_APPKEY");
        if (id && sc && ak && id[0] && sc[0] && ak[0]) {
            nls_init(&g_ali, id, sc, ak);
            g_ali_ready = 1;
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
    if (g_backend == ASR_BACKEND_ALIYUN && !g_ali_ready) note = " (凭据缺失)";
    if (g_backend == ASR_BACKEND_TENCENT && !g_tc_ready) note = " (凭据缺失)";
    fprintf(stderr, "[asr] 初始化: %s%s\n", asr_backend_name(), note);
    return 0;
}

int asr_recognize(const void *pcm, size_t bytes, char *out, size_t outlen)
{
    if (!g_init) return -1;
    if (!out || outlen == 0) return -1;

    switch (g_backend) {
    case ASR_BACKEND_MOCK:    return mock_recognize(pcm, bytes, out, outlen);
    case ASR_BACKEND_ALIYUN:  return aliyun_recognize(pcm, bytes, out, outlen);
    case ASR_BACKEND_TENCENT: return tencent_recognize(pcm, bytes, out, outlen);
    default:                  return -1;
    }
}

void asr_deinit(void)
{
    g_init = 0;
    g_ali_ready = 0;
    g_tc_ready = 0;
}

const char *asr_backend_name(void)
{
    switch (g_backend) {
    case ASR_BACKEND_MOCK:    return "mock";
    case ASR_BACKEND_ALIYUN:  return "aliyun";
    case ASR_BACKEND_TENCENT: return "tencent";
    default:                  return "unknown";
    }
}
