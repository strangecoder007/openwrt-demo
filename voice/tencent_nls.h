/*
 * tencent_nls.h —— 腾讯云语音识别 / 语音合成客户端
 *
 * ASR: https://asr.tencentcloudapi.com/   Action=SentenceRecognition
 * TTS: https://tts.tencentcloudapi.com/   Action=TextToVoice
 *
 * 鉴权：TC3-HMAC-SHA256
 */
#ifndef TENCENT_NLS_H
#define TENCENT_NLS_H

#include <stddef.h>

typedef struct {
    char secret_id[128];
    char secret_key[256];
    char app_id[32];        /* 腾讯云 APPID（数字） */
    char region[32];        /* 默认 ap-shanghai */
} tencent_nls_t;

int tc_init(tencent_nls_t *tc, const char *secret_id, const char *secret_key,
            const char *app_id, const char *region);

/*
 * 一句话识别。
 *   pcm/pcm_len  裸 PCM（S16_LE 单声道 16kHz）
 *   out          识别文本
 * 返回 0 成功。
 */
int tc_asr(tencent_nls_t *tc, const void *pcm, size_t pcm_len,
           char *out, size_t outlen);

/*
 * 语音合成。
 *   text      输入文本
 *   pcm_out   输出 PCM（S16_LE 单声道 16kHz，驱动端会再包装）
 *   cap       缓冲大小
 *   out_len   实际长度
 * 返回 0 成功。
 */
int tc_tts(tencent_nls_t *tc, const char *text,
           void *pcm_out, size_t cap, size_t *out_len);

#endif /* TENCENT_NLS_H */
