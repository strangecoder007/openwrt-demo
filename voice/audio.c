/*
 * audio.c —— ALSA 录音/播放实现
 */
#include "audio.h"

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
    size_t frame_bytes = ch * 2;
    const unsigned char *p = data;
    snd_pcm_sframes_t n;

    g_stop = 0;

    pcm = pcm_open(AUDIO_CARD, SND_PCM_STREAM_PLAYBACK, &rate, &ch);
    if (!pcm)
        return -1;

    snd_pcm_prepare(pcm);

    size_t remain_frames = bytes / frame_bytes;
    while (remain_frames > 0 && !g_stop) {
        n = snd_pcm_writei(pcm, p, remain_frames);
        if (n == -EPIPE) { snd_pcm_prepare(pcm); continue; }
        if (n < 0) {
            fprintf(stderr, "[audio] writei: %s\n", snd_strerror(n));
            snd_pcm_close(pcm);
            return -1;
        }
        p += n * frame_bytes;
        remain_frames -= (size_t)n;
    }

    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
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
