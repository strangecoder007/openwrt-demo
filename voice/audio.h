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

/*
 * ★ VAD 录音：说话就开始，说完（持续静音）就停，不用固定时长。
 *
 *   buf/cap       输出缓冲
 *   rate/ch       采样率 / 声道
 *   start_rms     起始能量阈值（安静环境约 100~300）
 *   silence_ms    连续静音多久算说完（建议 800）
 *   max_secs      最长录音兜底（防止一直说不停）
 *   out_ms        [输出] 实际录到的音频时长（毫秒）
 *
 * 返回字节数（已包含前置缓冲，即第一个字不会被吞）。
 * 如果一直没检测到语音（超过 lead_in），返回 0。
 */
long audio_record_vad(unsigned char *buf, size_t cap,
                      unsigned rate, unsigned ch,
                      int start_rms, unsigned silence_ms,
                      unsigned max_secs, unsigned *out_ms);

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
