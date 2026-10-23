/*
 * aliyun_nls.c —— 阿里云 NLS 客户端实现
 */
#include "aliyun_nls.h"
#include "aliyun_sign.h"
#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* 极简 JSON 取值：找 "key":"value" 或 "key":value */
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

static int json_get_num(const char *json, const char *key, long *out)
{
    char pat[64];
    const char *p;

    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n')) p++;
    if (*p != ':') return -1;
    p++;
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n')) p++;
    *out = strtol(p, NULL, 10);
    return 0;
}

/* ---------------- 初始化 ---------------- */
int nls_init(aliyun_nls_t *nls, const char *ak_id, const char *ak_secret,
             const char *appkey)
{
    if (!nls) return -1;
    memset(nls, 0, sizeof(*nls));
    snprintf(nls->ak_id, sizeof(nls->ak_id), "%s", ak_id ? ak_id : "");
    snprintf(nls->ak_secret, sizeof(nls->ak_secret), "%s", ak_secret ? ak_secret : "");
    snprintf(nls->appkey, sizeof(nls->appkey), "%s", appkey ? appkey : "");
    return 0;
}

/* ---------------- 取 Token ---------------- */
int nls_ensure_token(aliyun_nls_t *nls)
{
    long now = (long)time(NULL);
    char params[1024];
    char sig[256];
    char url[1600];
    char resp[4096];
    char token[512];
    long expire = 0;
    int rc;

    /* token 还有 5 分钟以上有效期就直接用 */
    if (nls->token[0] && nls->token_expire > now + 300) {
        return 0;
    }

    /*
     * 构造签名的规范查询串（参数按 key 字典序）：
     *   AccessKeyId, Action, Format, RegionId, SignatureMethod,
     *   SignatureNonce, SignatureVersion, Timestamp, Version
     */
    char nonce[64];
    char ts[64];
    struct tm tmv;
    time_t t = now;

    gmtime_r(&t, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    snprintf(nonce, sizeof(nonce), "%ld-%d", now, (int)(rand() % 100000));

    snprintf(params, sizeof(params),
             "AccessKeyId=%s"
             "&Action=CreateToken"
             "&Format=JSON"
             "&RegionId=cn-shanghai"
             "&SignatureMethod=HMAC-SHA1"
             "&SignatureNonce=%s"
             "&SignatureVersion=1.0"
             "&Timestamp=%s"
             "&Version=2019-02-28",
             nls->ak_id, nonce, ts);

    if (aliyun_sign_rpc("POST", params, nls->ak_secret,
                        sig, sizeof(sig)) < 0) {
        fprintf(stderr, "[nls] 签名失败\n");
        return -1;
    }

    /* 签名本身也要 encode */
    char sig_enc[512];
    aliyun_percent_encode(sig, sig_enc, sizeof(sig_enc));

    snprintf(url, sizeof(url),
             "https://nls-meta.cn-shanghai.aliyuncs.com/?Signature=%s&%s",
             sig_enc, params);

    fprintf(stderr, "[nls] 请求 Token...\n");
    rc = http_get(url, NULL, resp, sizeof(resp));
    if (rc < 0) {
        fprintf(stderr, "[nls] Token 请求失败\n");
        return -2;
    }

    if (json_get_str(resp, "Id", token, sizeof(token)) < 0) {
        fprintf(stderr, "[nls] 响应里没找到 Token:\n%s\n", resp);
        return -3;
    }
    json_get_num(resp, "ExpireTime", &expire);

    snprintf(nls->token, sizeof(nls->token), "%s", token);
    nls->token_expire = expire;
    fprintf(stderr, "[nls] Token OK，有效期至 %ld\n", expire);
    return 0;
}

/* ---------------- 一句话识别 ---------------- */
int nls_asr(aliyun_nls_t *nls, const void *pcm, size_t pcm_len,
            char *out, size_t outlen)
{
    char url[1024];
    char hdr[768];
    char resp[8192];
    char text[1024];
    int rc;

    if (nls_ensure_token(nls) < 0)
        return -1;

    snprintf(url, sizeof(url),
             "https://nls-gateway-cn-shanghai.aliyuncs.com/stream/v1/asr"
             "?appkey=%s&format=pcm&sample_rate=16000&enable_punctuation_prediction=true"
             "&enable_inverse_text_normalization=true",
             nls->appkey);

    snprintf(hdr, sizeof(hdr),
             "X-NLS-Token: %s\nContent-Type: application/octet-stream",
             nls->token);

    fprintf(stderr, "[nls-asr] 上传 %zu 字节 PCM...\n", pcm_len);
    rc = http_post(url, hdr, pcm, pcm_len, resp, sizeof(resp));
    if (rc < 0) {
        fprintf(stderr, "[nls-asr] 请求失败\n");
        return -2;
    }

    fprintf(stderr, "[nls-asr] 响应: %.300s\n", resp);

    if (json_get_str(resp, "result", text, sizeof(text)) < 0) {
        char status[64] = "";
        json_get_str(resp, "status", status, sizeof(status));
        fprintf(stderr, "[nls-asr] 无 result，status=%s\n", status);
        return -3;
    }

    snprintf(out, outlen, "%s", text);
    return 0;
}

/* ---------------- 语音合成 ---------------- */
int nls_tts(aliyun_nls_t *nls, const char *text,
            void *wav_out, size_t cap, size_t *out_len)
{
    char url[512];
    char hdr[768];
    char body[2048];
    int rc;

    if (nls_ensure_token(nls) < 0)
        return -1;

    snprintf(url, sizeof(url),
             "https://nls-gateway-cn-shanghai.aliyuncs.com/stream/v1/tts");

    snprintf(hdr, sizeof(hdr),
             "X-NLS-Token: %s\nContent-Type: application/json",
             nls->token);

    snprintf(body, sizeof(body),
             "{\"appkey\":\"%s\",\"token\":\"%s\",\"text\":\"%s\","
             "\"format\":\"wav\",\"sample_rate\":16000,\"voice\":\"xiaoyun\","
             "\"volume\":50,\"speech_rate\":0,\"pitch_rate\":0}",
             nls->appkey, nls->token, text);

    fprintf(stderr, "[nls-tts] 合成 %zu 字符...\n", strlen(text));
    rc = http_post_binary(url, hdr, body, strlen(body),
                          wav_out, cap, out_len);
    if (rc < 0) {
        fprintf(stderr, "[nls-tts] 请求失败\n");
        return -2;
    }

    /* 检查是不是 WAV（RIFF 头） */
    if (*out_len < 44 || memcmp(wav_out, "RIFF", 4) != 0) {
        fprintf(stderr, "[nls-tts] 返回的不是 WAV，前 200 字节:\n%.200s\n",
                (char *)wav_out);
        return -3;
    }

    fprintf(stderr, "[nls-tts] OK，%zu 字节 WAV\n", *out_len);
    return 0;
}
