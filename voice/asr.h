/*
 * asr.h —— 语音识别接口
 */
#ifndef ASR_H
#define ASR_H

#include <stddef.h>

/* 识别结果上限 */
#define ASR_TEXT_MAX 1024

/* 后端类型 */
typedef enum {
    ASR_BACKEND_MOCK = 0,   /* 本地假实现，返回固定文本 */
    ASR_BACKEND_ALIYUN,     /* 阿里云智能语音（需 AccessKey） */
    ASR_BACKEND_TENCENT,    /* 腾讯云一句话识别（需 SecretId/SecretKey） */
} asr_backend_t;

/*
 * 初始化 ASR。
 *   backend  后端类型
 * 返回 0 成功。
 */
int asr_init(asr_backend_t backend);

/*
 * 识别一段 PCM（S16_LE 单声道 16kHz）。
 *   pcm     PCM 数据
 *   bytes   字节数
 *   out     输出文本缓冲
 *   outlen  缓冲大小
 * 返回 0 成功，-1 失败。
 */
int asr_recognize(const void *pcm, size_t bytes,
                  char *out, size_t outlen);

/* 释放资源 */
void asr_deinit(void);

/* 后端名称，用于日志 */
const char *asr_backend_name(void);

#endif /* ASR_H */
