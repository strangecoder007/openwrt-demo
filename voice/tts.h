/*
 * tts.h —— 语音合成接口
 */
#ifndef TTS_H
#define TTS_H

#include <stddef.h>

typedef enum {
    TTS_BACKEND_MOCK = 0,       /* 本地生成提示音 */
    TTS_BACKEND_ALIYUN,         /* 阿里云 TTS */
    TTS_BACKEND_TENCENT,        /* 腾讯云 TTS */
} tts_backend_t;

/*
 * 合成参数（阿里云 TTS 用）
 */
typedef struct {
    const char *voice;      /* 音色，默认 "xiaoyun" */
    int         rate;       /* 采样率，默认 16000 */
    int         volume;     /* 0-100，默认 50 */
} tts_opts_t;

int  tts_init(tts_backend_t backend);

/*
 * 把文本合成为 PCM（S16_LE 单声道）。
 *   text     输入文本
 *   pcm_out  输出缓冲
 *   cap      缓冲大小
 *   out_len  实际写入字节数
 *   rate     输出采样率
 * 返回 0 成功，-1 失败。
 */
int  tts_synthesize(const char *text,
                    void *pcm_out, size_t cap,
                    size_t *out_len, unsigned *rate);

void tts_deinit(void);
const char *tts_backend_name(void);

/* 设置音色参数 */
void tts_set_opts(const tts_opts_t *opts);

#endif /* TTS_H */
