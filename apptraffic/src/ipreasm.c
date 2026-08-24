/*
 * ipreasm.c - 有界、只按顺序的 IPv4/IPv6 分片重组器。
 *
 * 目的：DNS 响应里 A/AAAA 一堆记录时可能超过一个 MTU 被 IP 层分片。要让
 * DNS 解析器能拿到完整的数据报，需要把分片按偏移拼回来。
 *
 * 设计取舍：
 *   - 用一个固定小表（IPFRAG_MAX=128）缓存“正在重组中的各条数据报”，
 *     以 (src, dst, id, proto) 为键区分不同数据报。IPv6 里 id 来自分片头
 *     的 identification（否则不同 ID 会串）。
 *   - 只支持“按顺序”的分片（偏移依次递增、无缺口）。UDP/DNS 大响应在
 *     实践中基本都按序到达，所以这里**不缓存乱序分片**，直接丢弃。这样省掉
 *     一个 pending 链表，代码和内存都简单很多；代价是极端乱序时可能拼不出来
 *     （可接受，因为 DNS 响应几乎总是按序送达）。
 *   - 每条数据报存一个“连续前缀” buf[0..used_len)，只有偏移正好接上的分片
 *     才拷进去；一旦最后一片（MF=0）给出了 total 长度，且 used_len >= total，
 *     就判定组装完成，把结果拷给调用方并释放本条目。
 *   - 超过 IPFRAG_TIMEOUT(30s) 没完成的条目会被 ipfrag_age() 清掉。
 *
 * Copyright (C) 2024
 * Licensed under GPL-2.0
 */

#include "ipreasm.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define IPFRAG_MAX      128
#define IPFRAG_TIMEOUT  30

struct ipfrag {
    int      in_use;     /* 槽位是否被占用 */
    int      is_v6;      /* 1=IPv6（地址 16 字节）/0=IPv4（4 字节） */
    uint8_t  src[16];    /* 源地址（网络序） */
    uint8_t  dst[16];    /* 目的地址（网络序） */
    uint32_t id;         /* IP 标识（v4 id / v6 fragment id） */
    uint8_t  proto;      /* 最终传输协议，参与键 */

    uint8_t *buf;        /* 已连续的 payload 前缀 [0, used_len) */
    size_t   used_len;   /* 前缀长度 */
    size_t   cap;        /* buf 容量 */

    int      have_last;  /* 是否已见到最后一片（MF=0），从而知道 total */
    size_t   total;      /* 整条数据报的 payload 总长（由最后一片给出） */
    time_t   last_seen;  /* 最近喂分片时间，用于超时淘汰 */
};

static struct ipfrag g_table[IPFRAG_MAX];

static int key_equal(const struct ipfrag *f, int is_v6,
                     const uint8_t *src, const uint8_t *dst, uint32_t id,
                     uint8_t proto)
{
    /* 用 (is_v6, id, proto, src, dst) 判断两条分片是否属于同一条数据报。 */
    if (f->is_v6 != is_v6 || f->id != id || f->proto != proto)
        return 0;
    int len = is_v6 ? 16 : 4;
    return memcmp(f->src, src, len) == 0 && memcmp(f->dst, dst, len) == 0;
}

static struct ipfrag *find_or_create(int is_v6, const uint8_t *src,
                                     const uint8_t *dst, uint32_t id,
                                     uint8_t proto)
{
    time_t now = time(NULL);
    struct ipfrag *oldest = NULL;
    for (int i = 0; i < IPFRAG_MAX; i++) {
        struct ipfrag *f = &g_table[i];
        /* 优先用空槽。 */
        if (!f->in_use) {
            memset(f, 0, sizeof(*f));
            f->in_use = 1;
            f->is_v6 = is_v6;
            f->id = id;
            f->proto = proto;
            int len = is_v6 ? 16 : 4;
            memcpy(f->src, src, len);
            memcpy(f->dst, dst, len);
            f->last_seen = now;
            return f;
        }
        /* 命中已有同 key 条目。 */
        if (key_equal(f, is_v6, src, dst, id, proto))
            return f;
        /* 顺手记下最久没用的一条，供表满时淘汰。 */
        if (!oldest || f->last_seen < oldest->last_seen)
            oldest = f;
    }
    /* 表满了：淘汰最旧条目（释放其 buf），复用其槽位。 */
    if (!oldest) return NULL;
    free(oldest->buf);
    memset(oldest, 0, sizeof(*oldest));
    oldest->in_use = 1;
    oldest->is_v6 = is_v6;
    oldest->id = id;
    oldest->proto = proto;
    int len = is_v6 ? 16 : 4;
    memcpy(oldest->src, src, len);
    memcpy(oldest->dst, dst, len);
    oldest->last_seen = now;
    return oldest;
}

static int ensure_cap(struct ipfrag *f, size_t need)
{
    /* 给 buf 扩容（翻倍），不超过 IPFRAG_MAX_LEN。 */
    if (need > IPFRAG_MAX_LEN) need = IPFRAG_MAX_LEN;
    if (need <= f->cap) return 0;
    size_t nc = f->cap ? f->cap : 512;
    while (nc < need && nc < IPFRAG_MAX_LEN)
        nc <<= 1;
    if (nc > IPFRAG_MAX_LEN) nc = IPFRAG_MAX_LEN;
    uint8_t *nb = realloc(f->buf, nc);
    if (!nb) return -1;
    f->buf = nb;
    f->cap = nc;
    return 0;
}

int ipfrag_feed(int is_v6, const uint8_t *src, const uint8_t *dst, uint32_t id,
                uint8_t proto, const uint8_t *payload, int payload_len,
                int offset_bytes, int mf, uint8_t *outbuf, size_t outbuf_cap,
                size_t *outlen)
{
    /* 找到（或新建）这条数据报的重组条目。 */
    struct ipfrag *f = find_or_create(is_v6, src, dst, id, proto);
    if (!f) return 0;

    /* 越界/空分片直接忽略。 */
    if (offset_bytes >= IPFRAG_MAX_LEN)
        return 0;
    if (payload_len > (int)(IPFRAG_MAX_LEN - offset_bytes))
        payload_len = IPFRAG_MAX_LEN - offset_bytes;
    if (payload_len <= 0)
        return 0;

    /* 最后一片（MF=0）会自带“本片把数据报补到哪”的信息：
     * total = offset_bytes + payload_len，即整条数据报的 payload 总长。
     * 在此之前 total 未知，无法判定组装完成。 */
    if (!mf) {
        f->have_last = 1;
        f->total = (size_t)offset_bytes + (size_t)payload_len;
    }

    /* 只接受“能接到前缀后面”的分片：offset <= used_len（无缝衔接）。
     * 若 offset > used_len 说明中间有缺口（乱序），直接丢弃——见文件头说明。 */
    if ((size_t)offset_bytes > f->used_len)
        return 0;

    /* 把该分片超出 used_len 的新数据拷到前缀末尾，前缀往后撑。 */
    size_t end = (size_t)offset_bytes + (size_t)payload_len;
    if (end > f->used_len) {
        if (ensure_cap(f, end)) return 0;
        size_t copy_off = f->used_len - (size_t)offset_bytes;
        memcpy(f->buf + f->used_len, payload + copy_off, end - f->used_len);
        f->used_len = end;
    }

    f->last_seen = time(NULL);

    /* 组装完成的条件：见过最后一片（知道 total），且连续前缀已经把整条数据报
     * 覆盖（used_len >= total），且没超上限。满足就拷贝给调用方、释放条目。 */
    if (f->have_last && f->total > 0 && f->used_len >= f->total &&
        f->total <= IPFRAG_MAX_LEN) {
        size_t n = 0;
        if (outbuf && f->total <= outbuf_cap) {
            memcpy(outbuf, f->buf, f->total);
            n = f->total;
        }
        free(f->buf);
        memset(f, 0, sizeof(*f));
        *outlen = n;
        return n > 0 ? 1 : 0;
    }

    return 0;
}

void ipfrag_age(void)
{
    /* 周期清理：凡是超过 IPFRAG_TIMEOUT 没等齐分片的条目都释放，防止表被
     * 半成品占满。由 daemon 主循环每 300s 调一次。 */
    time_t now = time(NULL);
    for (int i = 0; i < IPFRAG_MAX; i++) {
        struct ipfrag *f = &g_table[i];
        if (f->in_use && now - f->last_seen > IPFRAG_TIMEOUT) {
            free(f->buf);
            memset(f, 0, sizeof(*f));
        }
    }
}
