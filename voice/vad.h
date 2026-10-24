/*
 * vad.h —— 语音活动检测（Voice Activity Detection）
 *
 * 目的：不要固定录 N 秒。检测到说话就开始，检测到持续静音就停。
 *
 * 算法：基于短时能量的双阈值 + 静音计时
 *   - 每帧（20ms）计算 RMS 能量
 *   - 能量 > 起始阈值 → 判定为「有语音」，进入 SPEECH 状态
 *   - 进入 SPEECH 后，能量 < 结束阈值持续 silent_ms → 判定「说完了」
 *   - 双阈值（滞回）避免在阈值边界反复抖动
 *
 * 为什么不用模型：
 *   528MHz 单核跑神经网络不现实；能量 VAD 对近场麦克风足够，
 *   而且延迟低（逐帧判断，不需要缓冲整句）。
 */
#ifndef VAD_H
#define VAD_H

#include <stddef.h>

typedef struct {
    /* 配置 */
    unsigned sample_rate;      /* 采样率，如 16000 */
    unsigned frame_ms;         /* 帧长（ms），建议 20 */
    int      start_rms;        /* 起始阈值：超过它认为开始说话 */
    int      end_rms;          /* 结束阈值：低于它算静音（应 < start_rms） */
    unsigned silence_ms;       /* 连续静音多久算说完 */
    unsigned min_speech_ms;    /* 最短语音时长（低于此视为噪声，丢弃） */
    unsigned max_speech_ms;    /* 最长录音时长（兜底，防止无限录） */
    unsigned lead_in_ms;       /* 允许的最长起始等待（没人说话就放弃） */

    /* ★ 自适应噪声基线 */
    int      baseline_rms;     /* 估算出的环境底噪 */
    int      noise_samples;    /* 已采样帧数 */
    long long noise_sum;       /* 底噪累计 */
    unsigned calibrate_ms;     /* 校准时长（这段只测不判） */

    /* 运行状态 */
    int      state;            /* 0=WAIT_START 1=SPEECH 2=DONE */
    unsigned speech_frames;    /* 已录的语音帧数 */
    unsigned silence_frames;   /* 连续静音帧数 */
    unsigned wait_frames;      /* 起始等待帧数 */
    int      peak_rms;         /* 语音段内的峰值（调试用） */
    long long sum_rms;         /* 平均能量（调试用） */
} vad_t;

enum {
    VAD_WAIT_START = 0,
    VAD_SPEECH     = 1,
    VAD_DONE       = 2,
};

/*
 * 初始化。
 *   sr          采样率
 *   start_rms   ★ 0 = 自适应（推荐）；>0 = 固定阈值（调试用）
 *   silence_ms  静音多久算说完（建议 800）
 *
 * 自适应模式：前 calibrate_ms（默认 300ms）只采样不判断，
 * 用平均 RMS 作为底噪 baseline，然后
 *   start_rms = baseline * 3
 *   end_rms   = baseline * 1.5
 * 这样在有持续底噪的环境（风扇、电源）也能工作。
 */
int vad_init(vad_t *v, unsigned sr, int start_rms, unsigned silence_ms);

/*
 * 处理一帧 PCM（S16_LE 单声道）。
 *   pcm   帧数据
 *   frames 帧内采样点数
 * 返回当前状态（VAD_WAIT_START / VAD_SPEECH / VAD_DONE）。
 *
 * 调用方逻辑：
 *   - WAIT_START：丢弃数据（但可以保留一小段前置缓冲，避免吞掉第一个字）
 *   - SPEECH：写入录音缓冲
 *   - DONE：停止录音
 */
int vad_process(vad_t *v, const short *pcm, size_t frames);

/* 计算一帧的 RMS 能量（供外部调试用） */
int vad_frame_rms(const short *pcm, size_t frames);

/* 打印统计（调试用） */
void vad_dump_stats(const vad_t *v);

#endif /* VAD_H */
