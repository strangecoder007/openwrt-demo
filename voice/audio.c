/*
 * audio.c —— ALSA 录音/播放实现
 */
#include "audio.h"
#include "vad.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <alsa/asoundlib.h>

static volatile int g_stop = 0;

void audio_stop(void)  { g_stop = 1; }
int  audio_stopped(void) { return g_stop; }

/* ---------------- WAV 头 ---------------- */
#pragma pack(push, 1)
struct wav_hdr {
    char     riff[4];
    unsigned int riff_size;
    char     wave[4];
    char     fmt[4];
    unsigned int fmt_size;
    unsigned short format;
    unsigned short channels;
    unsigned int rate;
    unsigned int byte_rate;
    unsigned short block_align;
    unsigned short bits;
    char     data[4];
    unsigned int data_size;
};
#pragma pack(pop)

static int wav_write_header(FILE *f, unsigned int data_bytes,
                            unsigned rate, unsigned channels, unsigned bits)
{
    struct wav_hdr h;
    memset(&h, 0, sizeof(h));
    memcpy(h.riff, "RIFF", 4);
    h.riff_size   = 36 + data_bytes;
    memcpy(h.wave, "WAVE", 4);
    memcpy(h.fmt,  "fmt ", 4);
    h.fmt_size    = 16;
    h.format      = 1;
    h.channels    = channels;
    h.rate        = rate;
    h.byte_rate   = rate * channels * bits / 8;
    h.block_align = channels * bits / 8;
    h.bits        = bits;
    memcpy(h.data, "data", 4);
    h.data_size   = data_bytes;
    return (fwrite(&h, sizeof(h), 1, f) == 1) ? 0 : -1;
}

static int wav_read_header(FILE *f, unsigned *rate, unsigned *channels,
                           unsigned *bits, unsigned int *data_bytes)
{
    struct wav_hdr h;
    if (fread(&h, sizeof(h), 1, f) != 1)
        return -1;
    if (memcmp(h.riff, "RIFF", 4) || memcmp(h.wave, "WAVE", 4))
        return -1;
    *rate = h.rate;
    *channels = h.channels;
    *bits = h.bits;
    *data_bytes = h.data_size;
    return 0;
}

/* ---------------- PCM 打开 ---------------- */
static snd_pcm_t *pcm_open(const char *dev, snd_pcm_stream_t stream,
                           unsigned *rate, unsigned *channels)
{
    snd_pcm_t *pcm = NULL;
    snd_pcm_hw_params_t *hw = NULL;
    int err;
    unsigned r = *rate, c = *channels;

    err = snd_pcm_open(&pcm, dev, stream, 0);
    if (err < 0) {
        fprintf(stderr, "[audio] snd_pcm_open(%s): %s\n", dev, snd_strerror(err));
        return NULL;
    }

    snd_pcm_hw_params_alloca(&hw);
    if ((err = snd_pcm_hw_params_any(pcm, hw)) < 0) goto fail;
    if ((err = snd_pcm_hw_params_set_access(pcm, hw,
                     SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) goto fail;
    if ((err = snd_pcm_hw_params_set_format(pcm, hw,
                     SND_PCM_FORMAT_S16_LE)) < 0) goto fail;
    if ((err = snd_pcm_hw_params_set_rate_near(pcm, hw, &r, 0)) < 0) goto fail;
    if ((err = snd_pcm_hw_params_set_channels_near(pcm, hw, &c)) < 0) goto fail;
    if ((err = snd_pcm_hw_params(pcm, hw)) < 0) goto fail;

    *rate = r;
    *channels = c;
    return pcm;

fail:
    fprintf(stderr, "[audio] hw_params: %s\n", snd_strerror(err));
    snd_pcm_close(pcm);
    return NULL;
}

/* ---------------- 录音到缓冲 ---------------- */
long audio_record_buf(unsigned char *buf, size_t cap,
                      unsigned rate, unsigned ch, unsigned secs)
{
    snd_pcm_t *pcm;
    snd_pcm_uframes_t period = 1024;
    size_t frame_bytes = ch * 2;
    unsigned long total_frames, done = 0;
    unsigned char *tmp;
    int err;

    g_stop = 0;

    pcm = pcm_open(AUDIO_CARD, SND_PCM_STREAM_CAPTURE, &rate, &ch);
    if (!pcm)
        return -1;

    total_frames = (unsigned long)rate * secs;
    if (total_frames * frame_bytes > cap)
        total_frames = cap / frame_bytes;

    tmp = malloc(period * frame_bytes);
    if (!tmp) { snd_pcm_close(pcm); return -1; }

    snd_pcm_prepare(pcm);

    while (done < total_frames && !g_stop) {
        snd_pcm_uframes_t want = period;
        if ((unsigned long)want > total_frames - done)
            want = total_frames - done;

        err = snd_pcm_readi(pcm, tmp, want);
        if (err == -EPIPE) { snd_pcm_prepare(pcm); continue; }
        if (err == -EAGAIN) continue;
        if (err < 0) {
            fprintf(stderr, "[audio] readi: %s\n", snd_strerror(err));
            break;
        }
        if (err > 0) {
            memcpy(buf + done * frame_bytes, tmp, err * frame_bytes);
            done += err;
        }
    }

    free(tmp);
    snd_pcm_drop(pcm);
    snd_pcm_close(pcm);

    return (long)(done * frame_bytes);
}

/* ---------------- ★ VAD 录音 ---------------- */
/*
 * 说话就开始，说完（持续静音）就停。
 *
 * 关键：WAIT_START 阶段不直接丢数据，而是写入缓冲。
 * 这样「检测到能量 → 确认在说话」之间那几十毫秒的第一个字不会被吞。
 * 代价是如果全程没人说话，缓冲区里有几 KB 噪声——反正会返回 0，调用方忽略。
 */
long audio_record_vad(unsigned char *buf, size_t cap,
                      unsigned rate, unsigned ch,
                      int start_rms, unsigned silence_ms,
                      unsigned max_secs, unsigned *out_ms)
{
    snd_pcm_t *pcm;
    snd_pcm_uframes_t period = 512;      /* 512 帧 @16kHz = 32ms */
    size_t frame_bytes = ch * 2;
    unsigned char *tmp;
    vad_t vad;
    unsigned long max_frames, written = 0;
    int err, state;
    int saw_speech = 0;

    g_stop = 0;
    if (out_ms) *out_ms = 0;

    pcm = pcm_open(AUDIO_CARD, SND_PCM_STREAM_CAPTURE, &rate, &ch);
    if (!pcm)
        return -1;

    if (vad_init(&vad, rate, start_rms, silence_ms) < 0) {
        snd_pcm_close(pcm);
        return -1;
    }
    if (max_secs) vad.max_speech_ms = max_secs * 1000;

    max_frames = max_secs ? (unsigned long)rate * max_secs : (unsigned long)rate * 15;
    if (max_frames * frame_bytes > cap)
        max_frames = cap / frame_bytes;

    tmp = malloc(period * frame_bytes);
    if (!tmp) { snd_pcm_close(pcm); return -1; }

    snd_pcm_prepare(pcm);

    while (!g_stop) {
        snd_pcm_sframes_t got;

        got = snd_pcm_readi(pcm, tmp, period);
        if (got == -EPIPE) { snd_pcm_prepare(pcm); continue; }
        if (got == -EAGAIN) continue;
        if (got < 0) {
            fprintf(stderr, "[audio] readi: %s\n", snd_strerror(got));
            break;
        }
        if (got == 0) continue;

        /* VAD 判定 */
        state = vad_process(&vad, (const short *)tmp, (size_t)got);

        if (state == VAD_SPEECH) saw_speech = 1;

        /* WAIT_START 也写入——保住第一个字 */
        if (state != VAD_DONE) {
            if (written + (unsigned long)got > max_frames) {
                /* 触顶 */
                break;
            }
            memcpy(buf + written * frame_bytes, tmp, (size_t)got * frame_bytes);
            written += (unsigned long)got;
        } else {
            /* DONE：如果检测到过语音，停止；否则（等太久）也停止 */
            break;
        }
    }

    free(tmp);
    snd_pcm_drop(pcm);
    snd_pcm_close(pcm);

    vad_dump_stats(&vad);

    if (!saw_speech) {
        /* 全程没检测到语音 */
        if (out_ms) *out_ms = 0;
        return 0;
    }

    /*
     * 去掉尾部多余的静音。
     * vad.silence_frames 是「连续静音帧数」，每帧 period 个采样点。
     * 保留 silence_ms 的量（听起来自然），其余裁掉。
     */
    {
        unsigned long total_silence_frames =
            (unsigned long)vad.silence_frames * period;
        unsigned long keep_frames = (unsigned long)rate * silence_ms / 1000;
        if (total_silence_frames > keep_frames) {
            unsigned long drop = total_silence_frames - keep_frames;
            if (drop < written)
                written -= drop;
        }
    }

    if (out_ms)
        *out_ms = (unsigned)((written * 1000) / rate);

    return (long)(written * frame_bytes);
}

/* ---------------- 录音到文件 ---------------- */
long audio_record(const char *path, unsigned rate, unsigned ch, unsigned secs)
{
    snd_pcm_t *pcm;
    FILE *f;
    snd_pcm_uframes_t period = 1024;
    size_t frame_bytes = ch * 2;
    unsigned long total_frames, done = 0;
    unsigned char *buf;
    int err;

    g_stop = 0;

    pcm = pcm_open(AUDIO_CARD, SND_PCM_STREAM_CAPTURE, &rate, &ch);
    if (!pcm)
        return -1;

    total_frames = (unsigned long)rate * secs;

    f = fopen(path, "wb");
    if (!f) { perror(path); snd_pcm_close(pcm); return -1; }
    wav_write_header(f, 0, rate, ch, 16);

    buf = malloc(period * frame_bytes);
    if (!buf) { fclose(f); snd_pcm_close(pcm); return -1; }

    snd_pcm_prepare(pcm);

    while (done < total_frames && !g_stop) {
        snd_pcm_uframes_t want = period;
        if ((unsigned long)want > total_frames - done)
            want = total_frames - done;

        err = snd_pcm_readi(pcm, buf, want);
        if (err == -EPIPE) { snd_pcm_prepare(pcm); continue; }
        if (err == -EAGAIN) continue;
        if (err < 0) {
            fprintf(stderr, "[audio] readi: %s\n", snd_strerror(err));
            break;
        }
        if (err > 0) {
            fwrite(buf, frame_bytes, err, f);
            done += err;
        }
    }

    free(buf);
    snd_pcm_drop(pcm);
    snd_pcm_close(pcm);

    unsigned int data_bytes = (unsigned int)(done * frame_bytes);
    fseek(f, 0, SEEK_SET);
    wav_write_header(f, data_bytes, rate, ch, 16);
    fclose(f);

    return (long)done;
}

/* ---------------- 播放裸 PCM ---------------- */
int audio_play_pcm(const void *data, size_t bytes, unsigned rate, unsigned ch)
{
    snd_pcm_t *pcm;
    size_t frame_bytes;
    const unsigned char *p = data;
    unsigned char *upsample = NULL;
    snd_pcm_sframes_t n;

    g_stop = 0;

    /*
     * ★ 单声道必须先展开成双声道再送 ALSA。
     *
     * 板载喇叭只接在右声道（SPK_R）。i.MX6ULL 的 SAI 在 channels=1 时
     * 每个 I2S 帧只填 slot0（左），右声道 slot 没有任何数据 —— 结果就是
     * /proc/asound/.../status 一切正常、hw_ptr 正常推进，但喇叭完全没声。
     * 用 plughw 也救不回来：hw 参数本身声明支持 1ch（CHANNELS [1 2]），
     * plug 层判定「无需转换」直接直通，SAI 仍然只发一个 slot。
     * 实测证据：SAI 的 slot 掩码寄存器 mono=0xfffffffe / stereo=0xfffffffc。
     */
    if (ch == 1) {
        size_t frames = bytes / 2, i;
        const short *src = (const short *)data;
        short *dst = malloc(frames * 4);

        if (!dst)
            return -1;
        for (i = 0; i < frames; i++) {
            dst[i * 2]     = src[i];
            dst[i * 2 + 1] = src[i];
        }
        upsample = (unsigned char *)dst;
        p = upsample;
        bytes = frames * 4;
        ch = 2;
    }

    pcm = pcm_open(AUDIO_CARD, SND_PCM_STREAM_PLAYBACK, &rate, &ch);
    if (!pcm) {
        free(upsample);
        return -1;
    }

    /* 以 pcm_open 实际协商出来的声道数为准（它可能改写 ch） */
    frame_bytes = ch * 2;

    snd_pcm_prepare(pcm);

    size_t remain_frames = bytes / frame_bytes;
    while (remain_frames > 0 && !g_stop) {
        n = snd_pcm_writei(pcm, p, remain_frames);
        if (n == -EPIPE) { snd_pcm_prepare(pcm); continue; }
        if (n < 0) {
            fprintf(stderr, "[audio] writei: %s\n", snd_strerror(n));
            snd_pcm_close(pcm);
            free(upsample);
            return -1;
        }
        p += n * frame_bytes;
        remain_frames -= (size_t)n;
    }

    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    free(upsample);
    return 0;
}

/* ---------------- 播放 WAV ---------------- */
int audio_play(const char *path)
{
    FILE *f;
    unsigned rate, ch, bits;
    unsigned int data_bytes;
    unsigned char *buf;
    int ret;

    f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }

    if (wav_read_header(f, &rate, &ch, &bits, &data_bytes) < 0) {
        fprintf(stderr, "[audio] 不是合法 WAV: %s\n", path);
        fclose(f);
        return -1;
    }
    if (bits != 16) {
        fprintf(stderr, "[audio] 只支持 16bit，实际 %u\n", bits);
        fclose(f);
        return -1;
    }

    buf = malloc(data_bytes);
    if (!buf) { fclose(f); return -1; }

    if (fread(buf, 1, data_bytes, f) != data_bytes) {
        fprintf(stderr, "[audio] 读文件不完整\n");
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    ret = audio_play_pcm(buf, data_bytes, rate, ch);
    free(buf);
    return ret;
}

/* ---------------- 设备信息 ---------------- */
void audio_info(void)
{
    int card = -1;
    printf("=== ALSA 声卡 ===\n");
    while (snd_card_next(&card) >= 0 && card >= 0) {
        char *name = NULL;
        if (snd_card_get_name(card, &name) == 0) {
            printf("  card %d: %s\n", card, name);
            free(name);
        }
    }

    printf("\n=== %s 能力 ===\n", AUDIO_CARD);
    snd_pcm_t *pcm;
    if (snd_pcm_open(&pcm, AUDIO_CARD, SND_PCM_STREAM_PLAYBACK,
                     SND_PCM_NONBLOCK) < 0) {
        printf("  打不开\n");
        return;
    }
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    if (snd_pcm_hw_params_any(pcm, hw) >= 0) {
        unsigned rmin, rmax, cmin, cmax;
        snd_pcm_hw_params_get_rate_min(hw, &rmin, 0);
        snd_pcm_hw_params_get_rate_max(hw, &rmax, 0);
        snd_pcm_hw_params_get_channels_min(hw, &cmin);
        snd_pcm_hw_params_get_channels_max(hw, &cmax);
        printf("  采样率: %u - %u Hz\n", rmin, rmax);
        printf("  声道:   %u - %u\n", cmin, cmax);
        printf("  格式:   S16_LE\n");
    }
    snd_pcm_close(pcm);
}
