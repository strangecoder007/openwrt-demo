/*
 * aliyun_nls.h —— 阿里云智能语音交互（NLS）客户端
 *
 * 流程：
 *   1. 用 AccessKey 换取 Token（24 小时有效）
 *       POST https://nls-meta.cn-shanghai.aliyuncs.com/
 *       Action=CreateToken
 *   2. 一句话识别
 *       POST https://nls-gateway-cn-shanghai.aliyuncs.com/stream/v1/asr
 *       ?appkey=...&format=pcm&sample_rate=16000
 *       Header: X-NLS-Token: <token>
 *       Body:   裸 PCM
 *   3. 语音合成
 *       POST https://nls-gateway-cn-shanghai.aliyuncs.com/stream/v1/tts
 *       JSON body: {appkey, token, text, format:"wav", sample_rate}
 *       返回 wav 二进制
 */
#ifndef ALIYUN_NLS_H
#define ALIYUN_NLS_H

#include <stddef.h>

typedef struct {
    char ak_id[128];
    char ak_secret[160];
    char appkey[64];
    char token[512];
    long token_expire;      /* 到期时间戳（秒） */
} aliyun_nls_t;

/* 初始化（只存凭据，不联网） */
int  nls_init(aliyun_nls_t *nls, const char *ak_id, const char *ak_secret,
              const char *appkey);

/*
 * 确保 token 有效（必要时刷新）。
 * 返回 0 成功。
 */
int  nls_ensure_token(aliyun_nls_t *nls);

/*
 * 一句话识别。
 *   pcm/pcm_len  裸 PCM（S16_LE 单声道 16kHz）
 *   out          识别文本
 * 返回 0 成功。
 */
int  nls_asr(aliyun_nls_t *nls, const void *pcm, size_t pcm_len,
             char *out, size_t outlen);

/*
 * 语音合成。
 *   text      输入文本
 *   wav_out   输出 WAV 数据
 *   cap       缓冲大小
 *   out_len   实际长度
 * 返回 0 成功。
 */
int  nls_tts(aliyun_nls_t *nls, const char *text,
             void *wav_out, size_t cap, size_t *out_len);

#endif /* ALIYUN_NLS_H */
