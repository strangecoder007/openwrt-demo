/*
 * audio.h —— ALSA 录音/播放封装
 */
#ifndef AUDIO_H
#define AUDIO_H

#include <stddef.h>

#define AUDIO_CARD      "hw:0,0"
#define AUDIO_RATE      16000   /* ASR 标准采样率 */
#define AUDIO_CHANNELS  1

/*
 * 录音到 WAV 文件。
 *   path   输出文件
 *   rate   采样率（会被就近调整）
 *   ch     声道数
 *   secs   时长（秒）；<=0 表示录到 g_audio_stop() 为止
 * 返回实际录到的帧数，负值为错误。
 */
long audio_record(const char *path, unsigned rate, unsigned ch, unsigned secs);

/* 直接录到内存缓冲（供 ASR 上传用），返回字节数 */
long audio_record_buf(unsigned char *buf, size_t cap,
                      unsigned rate, unsigned ch, unsigned secs);

/* 播放 WAV 文件 */
int audio_play(const char *path);

/* 播放裸 PCM 数据（S16_LE，给定 rate/ch） */
int audio_play_pcm(const void *data, size_t bytes,
                   unsigned rate, unsigned ch);

/* 请求停止当前的录音/播放 */
void audio_stop(void);

/* 是否已被请求停止 */
int audio_stopped(void);

/* 打印设备能力 */
void audio_info(void);

#endif /* AUDIO_H */
