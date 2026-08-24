/*
 * tcp_reasm.c - 每条 TCP 连接的“首记录”重装器。
 *
 * 为什么只做“首记录”：
 *   我们想从一条连接里得到的是 TLS ClientHello（里面有 SNI），或者那段 HTTP
 *   请求的 Host 头。这些信息都在流的最开头，只要能拼出“第一条记录”就够了，
 *   没必要把整条流缓存起来。所以这里只保留流的前几个 KB，用 REASM_MAX_LEN
 *   封顶，内存占用可控（板子上不会因为一条长视频连接把几 MB 都攒在内存里）。
 *
 * 解决的三个问题：
 *   1) 记录被拆成多个 TCP 段（MTU 限制 / 大 ClientHello / 被分段）：
 *      -> 用“偏移域”把各段按 seq 位置拼起来。
 *   2) 乱序到达（后一个段先到、缺前面一段）：
 *      -> off 有缺口的段先塞进 pending，等缺口闭合再并进前缀。
 *   3) 重传/重复段：
 *      -> off 落在已有连续区间内时直接丢弃（去重），避免重复记账、重复拷贝。
 *
 * Copyright (C) 2024
 * Licensed under GPL-2.0
 */

#include "tcp_reasm.h"
#include <stdlib.h>
#include <string.h>

void tcp_reasm_init(tcp_reasm *r)
{
    /* 全清零：base_seq 未定、buf 为空、used_len=0、pending 为空、done=0。 */
    memset(r, 0, sizeof(*r));
}

void tcp_reasm_free(tcp_reasm *r)
{
    if (!r) return;
    /* 释放连续前缀缓冲和所有 pending 段（防御性清理，非必须）。 */
    free(r->buf);
    r->buf = NULL;
    r->used_len = 0;
    r->cap = 0;
    struct tcp_seg *s = r->pending;
    while (s) {
        struct tcp_seg *next = s->next;
        free(s->data);
        free(s);
        s = next;
    }
    r->pending = NULL;
}

void tcp_reasm_set_isn(tcp_reasm *r, uint32_t isn)
{
    /* 从 SYN 拿到初始序号 ISN。SYN 本身占一个序号，所以第一个数据字节的
     * seq = ISN + 1。我们把 base_seq 设成 ISN+1，这样后续 offset = seq-base_seq
     * 的第一个数据段正好是 0。只在还没定 base 时设一次（SYN 先于数据到达）。 */
    if (!r->have_base) {
        r->base_seq = isn + 1;
        r->have_base = 1;
    }
}

static int ensure_cap(tcp_reasm *r, size_t need)
{
    /* 按需给 buf 扩容（翻倍），但绝不越过 REASM_MAX_LEN。 */
    if (need > REASM_MAX_LEN) need = REASM_MAX_LEN;
    if (need <= r->cap) return 0;

    size_t new_cap = r->cap ? r->cap : 256;
    while (new_cap < need && new_cap < REASM_MAX_LEN)
        new_cap <<= 1;
    if (new_cap > REASM_MAX_LEN) new_cap = REASM_MAX_LEN;

    uint8_t *nb = realloc(r->buf, new_cap);
    if (!nb) return -1;
    r->buf = nb;
    r->cap = new_cap;
    return 0;
}

static void free_seg(struct tcp_seg *s)
{
    /* 释放一个 pending 段：先 free 其 data 副本，再 free 节点本身。 */
    if (!s) return;
    free(s->data);
    free(s);
}

/* 往 pending 链表插入一个乱序段。
 * 规则：
 *   - 按 off 升序插入（方便 merge 时从左到右扫）；
 *   - 先查重：若已有 off 相同且长度 >= 本段的 pending 段，说明这段数据已
 *     经在手上（重传），直接返回，不重复缓存。 */
static void add_pending(tcp_reasm *r, uint32_t off, const uint8_t *payload,
                        size_t len)
{
    for (struct tcp_seg *s = r->pending; s; s = s->next) {
        if (s->off == off && s->len >= len)
            return; /* already have this data */
    }

    struct tcp_seg *node = calloc(1, sizeof(*node));
    if (!node) return;
    node->data = malloc(len);
    if (!node->data) {
        free(node);
        return;
    }
    memcpy(node->data, payload, len);
    node->off = off;
    node->len = len;

    /* Insert sorted ascending by off */
    struct tcp_seg **pp = &r->pending;
    while (*pp && (*pp)->off < off)
        pp = &(*pp)->next;
    node->next = *pp;
    *pp = node;
}

/* 尝试把 pending 里“能接到连续前缀后面”的段并进去。
 * 因为前缀一定从 0 连续到 used_len，所以凡是 off <= used_len 的段都属于
 * “缺口已闭合”的范围，可以拼到前缀上。漏网的是还在等待更靠前的段（off >
 * used_len），它们继续留在 pending。*/
static void merge_pending(tcp_reasm *r)
{
    struct tcp_seg **pp = &r->pending;
    while (*pp) {
        struct tcp_seg *s = *pp;
        /* 仍离前缀有缺口，停止（链表按 off 升序，后面的只会更晚）。 */
        if (s->off > r->used_len)
            break;

        size_t seg_end = s->off + s->len;
        if (seg_end <= r->used_len) {
            /* 整段都落在已连续区间里（重叠/重复），直接扔掉。 */
            *pp = s->next;
            free_seg(s);
            continue;
        }

        size_t new_used = seg_end;
        if (ensure_cap(r, new_used)) {
            /* 内存不足：把整个 pending 清掉，宁可丢数据也别泄漏。 */
            while (r->pending) {
                struct tcp_seg *n = r->pending->next;
                free_seg(r->pending);
                r->pending = n;
            }
            return;
        }
        /* 该段有一部分已经在前缀里（s->off < used_len），只把【超出】的部分
         * 从 s->data 里拷到前缀末尾。 */
        size_t copy_off = r->used_len - s->off;
        memcpy(r->buf + r->used_len, s->data + copy_off,
               new_used - r->used_len);
        r->used_len = new_used;
        /* 这段并完了，移除并释放。 */
        *pp = s->next;
        free_seg(s);
    }
}

int tcp_reasm_feed(tcp_reasm *r, uint32_t seq, const uint8_t *payload, int len)
{
    /* 主入口：喂一个“客户端->服务器”方向的 TCP 数据段（payload 不含 TCP 头）。
     * 返回 1 表示连续前缀增长了（调用方可以再判断是否已凑齐完整记录），
     * 返回 0 表示这次没让前缀增长（重复段 / 乱序先入 pending / 超限 / 已 done）。 */
    if (!r || r->done || len <= 0 || !payload)
        return 0;

    if (!r->have_base) {
        /* 没抓到 SYN，用当前段 seq 当 base_seq（假定这就是流开头）。 */
        r->base_seq = seq;
        r->have_base = 1;
    }

    /* 关键：offset = seq - base_seq。TCP seq 是 32 位环形递增的，这里直接用
     * 无符号减法，向前自然回绕，不越界、不依赖比较大小。 */
    uint32_t off = seq - r->base_seq;
    if (off >= REASM_MAX_LEN)
        return 0;

    /* 越界的部分直接截掉（我们只关心 offset < REASM_MAX_LEN 的前缀）。 */
    if ((size_t)len > REASM_MAX_LEN - off)
        len = REASM_MAX_LEN - off;

    if ((size_t)off + (size_t)len <= r->used_len)
        return 0; /* 整段已在连续前缀内 -> 重复/重传，忽略 */

    if ((size_t)off > r->used_len) {
        /* 中间有缺口（这段比前缀还靠后）-> 先挂 pending，前缀没长。 */
        add_pending(r, off, payload, len);
        r->last_seen = time(NULL);
        return 0;
    }

    /* 到这里 off <= used_len < off+len：段头有部分和前缀重叠，但至少有新数据
     * 超出了 used_len，可以把前缀往后撑。 */
    size_t new_used = (size_t)off + (size_t)len;
    if (ensure_cap(r, new_used)) {
        r->last_seen = time(NULL);
        return 0;
    }

    size_t copy_from = r->used_len - off; /* 这段里已经在前缀内的字节数 */
    memcpy(r->buf + r->used_len, payload + copy_from, new_used - r->used_len);
    r->used_len = new_used;
    r->last_seen = time(NULL);

    /* 前缀撑大后，之前挂起的乱序段可能能接上了，尝试 merge。 */
    merge_pending(r);
    return 1;
}

const uint8_t *tcp_reasm_data(const tcp_reasm *r, size_t *len)
{
    /* 返回当前连续前缀数据（buf）和长度。调用方据此判断是否已经凑齐：
     *   - TLS：buf[0]==0x16 且 5 + record_len <= used_len； 凑齐后调
     *          proto_tls_parse_sni() 提取 SNI，再 mark_consumed()。
     *   - HTTP：buf 以 GET/POST... 开头且含 \r\nHost: ；
     *   - 到 cap 还没凑齐（不是我们要的消息）也 mark_consumed() 收手。 */
    if (len) *len = r ? r->used_len : 0;
    return r ? r->buf : NULL;
}

void tcp_reasm_mark_consumed(tcp_reasm *r)
{
    if (!r) return;
    /* 第一条记录已经处理完（或放弃），把缓冲全释放，置 done，让后续数据
     * 直接走 else-if（capture 里 done 的分支直接 return，不再缓存）。 */
    r->done = 1;
    free(r->buf);
    r->buf = NULL;
    r->used_len = 0;
    r->cap = 0;
    struct tcp_seg *s = r->pending;
    while (s) {
        struct tcp_seg *next = s->next;
        free_seg(s);
        s = next;
    }
    r->pending = NULL;
}
