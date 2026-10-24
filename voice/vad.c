/*
 * vad.c —— 基于短时能量的语音活动检测（自适应底噪）
 *
 * 设计要点：
 *
 * 1. ★ 自适应噪声基线（最重要）
 *    板子所在环境可能有持续底噪（风扇/电源/USB 干扰），实测 RMS 稳定在 300~400。
 *    固定阈值 200 比底噪还低 → VAD 一启动就误判"在说话"，永远等不到静音。
 *    解法：启动前 300ms 只采样不判断，算出 baseline，然后
 *      start_rms = baseline * 3
 *      end_rms   = baseline * 1.5
 *
 * 2. 双阈值（滞回）
 *    起始阈值高于结束阈值。只用单阈值时，能量在边界抖动会导致状态反复切换。
 *
 * 3. 前置缓冲（lead-in）
    *    从「检测到能量」到「确认在说话」有几十毫秒延迟，
 *    这段时间的第一个字会被吞，所以 WAIT_START 阶段的数据也要写入缓冲。
 *
 * 4. 最短语音时长过滤
 *    关门声、键盘敲击是短促尖峰，能量很高但只有几十毫秒。
 *    要求 SPEECH 持续超过 min_speech_ms 才算有效。
 */
#include "vad.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* 自适应的倍数：起始 3x 底噪，结束 1.5x 底噪 */
#define START_FACTOR  3
#define END_FACTOR    15   /* /10 */

int vad_frame_rms(const short *pcm, size_t frames)
{
    long long sum = 0;
    size_t i;

    if (frames == 0) return 0;

    for (i = 0; i < frames; i++) {
        long long v = pcm[i];
        sum += v * v;
    }

    return (int)sqrt((double)sum / (double)frames);
}

int vad_init(vad_t *v, unsigned sr, int start_rms, unsigned silence_ms)
{
    if (!v) return -1;
    memset(v, 0, sizeof(*v));

    v->sample_rate   = sr ? sr : 16000;
    v->frame_ms      = 20;
    v->silence_ms    = silence_ms ? silence_ms : 800;
    v->min_speech_ms = 200;
    v->max_speech_ms = 15000;
    v->lead_in_ms    = 5000;

    if (start_rms > 0) {
        /* 固定阈值模式（调试用） */
        v->start_rms   = start_rms;
        v->end_rms     = start_rms * 6 / 10;
        v->calibrate_ms = 0;
        v->baseline_rms = 0;
    } else {
        /* ★ 自适应模式：前 300ms 校准 */
        v->calibrate_ms = 300;
        v->baseline_rms = 0;
        v->start_rms    = 0;    /* 校准完才填 */
        v->end_rms      = 0;
    }

    v->state = VAD_WAIT_START;
    return 0;
}

int vad_process(vad_t *v, const short *pcm, size_t frames)
{
    int rms;

    if (!v) return VAD_DONE;
    if (v->state == VAD_DONE)
        return VAD_DONE;

    rms = vad_frame_rms(pcm, frames);
    if (rms > v->peak_rms) v->peak_rms = rms;
    v->sum_rms += rms;

    /* ---------- 阶段 0：噪声校准 ---------- */
    if (v->calibrate_ms > 0) {
        v->noise_sum += rms;
        v->noise_samples++;
        v->wait_frames++;

        if (v->wait_frames * v->frame_ms >= v->calibrate_ms) {
            int base = (int)(v->noise_sum / v->noise_samples);
            if (base < 20) base = 20;      /* 防止除零/过低 */

            v->baseline_rms = base;
            v->start_rms    = base * START_FACTOR;
            v->end_rms      = base * END_FACTOR / 10;

            /* 上限保护：别超过满量程一半，否则永远判不出语音 */
            if (v->start_rms > 8000) v->start_rms = 8000;
            if (v->end_rms   > 4000) v->end_rms   = 4000;

            /* ★ 校准只做一次：清零后不再进入这个分支 */
            v->calibrate_ms = 0;
            v->wait_frames  = 0;    /* 重新计等待时长 */

            fprintf(stderr, "[vad] 校准完成: 底噪=%d  起始阈值=%d  结束阈值=%d\n",
                    v->baseline_rms, v->start_rms, v->end_rms);
        }
        /* 校准期间不判断状态 */
        return v->state;
    }

    /* ---------- 正式判断 ---------- */
    switch (v->state) {
    case VAD_WAIT_START:
        v->wait_frames++;
        if (rms >= v->start_rms) {
            v->state = VAD_SPEECH;
            v->speech_frames = 1;
            v->silence_frames = 0;
        } else if (v->wait_frames * v->frame_ms >= v->lead_in_ms) {
            v->state = VAD_DONE;
        }
        break;

    case VAD_SPEECH:
        v->speech_frames++;

        if (rms < v->end_rms) {
            v->silence_frames++;
            if (v->silence_frames * v->frame_ms >= v->silence_ms) {
                unsigned voiced_ms = (v->speech_frames - v->silence_frames)
                                     * v->frame_ms;
                if (voiced_ms >= v->min_speech_ms) {
                    v->state = VAD_DONE;
                } else {
                    /* 太短，当成噪声，回到等待 */
                    v->state = VAD_WAIT_START;
                    v->speech_frames = 0;
                    v->silence_frames = 0;
                }
            }
        } else {
            v->silence_frames = 0;
        }

        if (v->speech_frames * v->frame_ms >= v->max_speech_ms) {
            v->state = VAD_DONE;
        }
        break;

    default:
        break;
    }

    return v->state;
}

void vad_dump_stats(const vad_t *v)
{
    unsigned total_frames = v->wait_frames + v->speech_frames;
    unsigned avg_rms = total_frames ? (unsigned)(v->sum_rms / total_frames) : 0;

    fprintf(stderr,
            "[vad] 状态=%d  底噪=%d  等待=%ums  语音=%ums  静音=%ums  "
            "峰值RMS=%d  均值RMS=%u  阈值=%d/%d\n",
            v->state, v->baseline_rms,
            v->wait_frames * v->frame_ms,
            v->speech_frames * v->frame_ms,
            v->silence_frames * v->frame_ms,
            v->peak_rms, avg_rms,
            v->start_rms, v->end_rms);
}
