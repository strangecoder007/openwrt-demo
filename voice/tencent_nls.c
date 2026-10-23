/*
 * tencent_nls.c —— 腾讯云 ASR / TTS 实现
 */
#include "tencent_nls.h"
#include "tencent_sign.h"
#include "http.h"
#include "crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

/* ---------------- JSON 取值 ---------------- */
static int json_get_str(const char *json, const char *key,
                        char *out, size_t outlen)
{
    char pat[64];
    const char *p, *q;
    size_t n;

    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n')) p++;
    if (*p != ':') return -1;
    p++;
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n')) p++;
    if (*p != '"') return -1;
    p++;
    q = strchr(p, '"');
    if (!q) return -1;
    n = (size_t)(q - p);
    if (n >= outlen) n = outlen - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return 0;
}

/* JSON 字符串转义（UTF-8 直接透传） */
static void json_escape(const char *src, char *dst, size_t dstlen)
{
    size_t i = 0;
    for (; *src && i + 8 < dstlen; src++) {
        unsigned char c = (unsigned char)*src;
        switch (c) {
        case '"':  dst[i++]='\\'; dst[i++]='"';  break;
        case '\\': dst[i++]='\\'; dst[i++]='\\'; break;
        case '\n': dst[i++]='\\'; dst[i++]='n';  break;
        case '\r': dst[i++]='\\'; dst[i++]='r';  break;
        case '\t': dst[i++]='\\'; dst[i++]='t';  break;
        default:
            if (c < 0x20) i += (size_t)snprintf(dst + i, dstlen - i, "\\u%04x", c);
            else          dst[i++] = (char)c;
        }
    }
    dst[i] = 0;
}

/* ---------------- 通用调用 ---------------- */
static int tc_call(tencent_nls_t *tc,
                   const char *service, const char *host,
                   const char *action, const char *version,
                   const char *payload, size_t payload_len,
                   char *resp, size_t resp_len)
{
    char auth[1024];
    char hdr[2048];
    char url[256];
    char date[16];
    long ts = (long)time(NULL);
    int rc;
    struct tm tmv;
    time_t t = (time_t)ts;

    gmtime_r(&t, &tmv);
    strftime(date, sizeof(date), "%Y-%m-%d", &tmv);

    if (tc3_sign(tc->secret_id, tc->secret_key, service, host,
                 action, version, tc->region,
                 payload, payload_len, ts,
                 auth, sizeof(auth)) < 0) {
        fprintf(stderr, "[tc] 签名失败\n");
        return -1;
    }

    snprintf(url, sizeof(url), "https://%s/", host);

    /* 注意：Content-Type 必须与签名时完全一致（application/json，不带 charset）。
     * 带 charset 会导致腾讯云签名校验失败。 */
    snprintf(hdr, sizeof(hdr),
             "Authorization: %s\n"
             "Content-Type: application/json\n"
             "Host: %s\n"
             "X-TC-Action: %s\n"
             "X-TC-Version: %s\n"
             "X-TC-Timestamp: %ld",
             auth, host, action, version, ts);

    fprintf(stderr, "[tc] %s %s (%zu 字节 payload)\n", service, action, payload_len);

    rc = http_post(url, hdr, payload, payload_len, resp, resp_len);
    if (rc < 0) {
        fprintf(stderr, "[tc] 请求失败\n");
        return -2;
    }
    return 0;
}

/* ---------------- 初始化 ---------------- */
int tc_init(tencent_nls_t *tc, const char *secret_id, const char *secret_key,
            const char *app_id, const char *region)
{
    if (!tc) return -1;
    memset(tc, 0, sizeof(*tc));
    snprintf(tc->secret_id, sizeof(tc->secret_id), "%s", secret_id ? secret_id : "");
    snprintf(tc->secret_key, sizeof(tc->secret_key), "%s", secret_key ? secret_key : "");
    snprintf(tc->app_id, sizeof(tc->app_id), "%s", app_id ? app_id : "");
    snprintf(tc->region, sizeof(tc->region), "%s", region && region[0] ? region : "ap-shanghai");
    return 0;
}

/* ---------------- ASR ---------------- */
int tc_asr(tencent_nls_t *tc, const void *pcm, size_t pcm_len,
           char *out, size_t outlen)
{
    char *b64;
    size_t b64_cap;
    char *payload;
    size_t payload_cap;
    char resp[16384];
    char text[1024];
    int rc;

    if (!tc->secret_id[0] || !tc->secret_key[0]) {
        fprintf(stderr, "[tc-asr] 缺少 SecretId/SecretKey\n");
        return -1;
    }

    /* base64 编码 PCM */
    b64_cap = (pcm_len + 2) / 3 * 4 + 8;
    b64 = malloc(b64_cap);
    if (!b64) return -1;
    if (base64_encode(pcm, pcm_len, b64, b64_cap) < 0) {
        free(b64);
        return -1;
    }

    payload_cap = b64_cap + 512;
    payload = malloc(payload_cap);
    if (!payload) { free(b64); return -1; }

    snprintf(payload, payload_cap,
             "{\"EngSerViceType\":\"16k_zh\",\"SourceType\":1,"
             "\"VoiceFormat\":\"pcm\",\"Data\":\"%s\",\"DataLen\":%zu,"
             "\"FilterDirty\":0,\"FilterModal\":0,\"ConvertNumMode\":1}",
             b64, pcm_len);

    fprintf(stderr, "[tc-asr] PCM %zu 字节 -> base64 %zu 字节\n", pcm_len, strlen(b64));

    rc = tc_call(tc, "asr", "asr.tencentcloudapi.com",
                 "SentenceRecognition", "2019-06-14",
                 payload, strlen(payload), resp, sizeof(resp));

    free(b64);
    free(payload);

    if (rc < 0) return -2;

    fprintf(stderr, "[tc-asr] 响应: %.400s\n", resp);

    if (json_get_str(resp, "Result", text, sizeof(text)) < 0) {
        char errmsg[256] = "";
        json_get_str(resp, "Message", errmsg, sizeof(errmsg));
        fprintf(stderr, "[tc-asr] 无 Result，Message=%s\n", errmsg);
        return -3;
    }

    snprintf(out, outlen, "%s", text);
    return 0;
}

/* ---------------- TTS ---------------- */
int tc_tts(tencent_nls_t *tc, const char *text,
           void *pcm_out, size_t cap, size_t *out_len)
{
    char esc[2048];
    char payload[4096];
    char resp[65536 * 4];
    char audio_b64[65536 * 4];
    char sid[64];
    int rc;
    long now = (long)time(NULL);

    *out_len = 0;

    if (!tc->secret_id[0] || !tc->secret_key[0]) {
        fprintf(stderr, "[tc-tts] 缺少 SecretId/SecretKey\n");
        return -1;
    }

    json_escape(text, esc, sizeof(esc));
    snprintf(sid, sizeof(sid), "voice%ld", now);

    snprintf(payload, sizeof(payload),
             "{\"Text\":\"%s\",\"SessionId\":\"%s\","
             "\"Volume\":5,\"Speed\":0,\"ProjectId\":0,"
             "\"ModelType\":1,\"VoiceType\":1001,\"PrimaryLanguage\":1,"
             "\"SampleRate\":16000,\"Codec\":\"pcm\"}",
             esc, sid);

    fprintf(stderr, "[tc-tts] 合成 %zu 字符...\n", strlen(text));

    rc = tc_call(tc, "tts", "tts.tencentcloudapi.com",
                 "TextToVoice", "2019-08-23",
                 payload, strlen(payload), resp, sizeof(resp));
    if (rc < 0) return -2;

    if (json_get_str(resp, "Audio", audio_b64, sizeof(audio_b64)) < 0) {
        char errmsg[256] = "";
        json_get_str(resp, "Message", errmsg, sizeof(errmsg));
        fprintf(stderr, "[tc-tts] 无 Audio，Message=%s\n响应: %.300s\n", errmsg, resp);
        return -3;
    }

    /* base64 解码 PCM */
    {
        static const int8_t dtab[256] = {
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
            52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
            -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
            15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
            -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
            41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
        };
        size_t i, o = 0;
        unsigned int buf = 0;
        int bits = 0;

        for (i = 0; audio_b64[i] && audio_b64[i] != '='; i++) {
            int v = dtab[(unsigned char)audio_b64[i]];
            if (v < 0) continue;
            buf = (buf << 6) | (unsigned)v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                if (o < cap)
                    ((unsigned char *)pcm_out)[o++] = (unsigned char)((buf >> bits) & 0xFF);
            }
        }
        *out_len = o;
    }

    fprintf(stderr, "[tc-tts] OK: %zu 字节 PCM @16000Hz\n", *out_len);
    return (*out_len > 0) ? 0 : -4;
}
