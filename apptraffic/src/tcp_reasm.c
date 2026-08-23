/*
 * tcp_reasm.c - per-connection first-record TCP reassembler.
 *
 * Only the beginning of the stream is kept: enough to reconstruct a single
 * TLS record (or the header prelude of an HTTP request). This bounds memory
 * on the small board while handling the common case where the ClientHello or
 * request spans multiple segments, arrives out of order, or is retransmitted.
 *
 * Copyright (C) 2024
 * Licensed under GPL-2.0
 */

#include "tcp_reasm.h"
#include <stdlib.h>
#include <string.h>

void tcp_reasm_init(tcp_reasm *r)
{
    memset(r, 0, sizeof(*r));
}

void tcp_reasm_free(tcp_reasm *r)
{
    if (!r) return;
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
    /* The first data byte has sequence number ISN+1 (SYN consumes one) */
    if (!r->have_base) {
        r->base_seq = isn + 1;
        r->have_base = 1;
    }
}

static int ensure_cap(tcp_reasm *r, size_t need)
{
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
    if (!s) return;
    free(s->data);
    free(s);
}

/* Insert an out-of-order segment, sorted ascending by offset, de-duplicating
 * a retransmission that exactly repeats an existing pending segment. */
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

/* Merge any pending segments that can now be appended to the contiguous
 * prefix (i.e. their offset is <= used_len). */
static void merge_pending(tcp_reasm *r)
{
    struct tcp_seg **pp = &r->pending;
    while (*pp) {
        struct tcp_seg *s = *pp;
        if (s->off > r->used_len)
            break;

        size_t seg_end = s->off + s->len;
        if (seg_end <= r->used_len) {
            /* fully covered -> drop */
            *pp = s->next;
            free_seg(s);
            continue;
        }

        size_t new_used = seg_end;
        if (ensure_cap(r, new_used)) {
            /* Out of memory: drop the whole pending list to avoid leaking */
            while (r->pending) {
                struct tcp_seg *n = r->pending->next;
                free_seg(r->pending);
                r->pending = n;
            }
            return;
        }
        size_t copy_off = r->used_len - s->off;
        memcpy(r->buf + r->used_len, s->data + copy_off,
               new_used - r->used_len);
        r->used_len = new_used;
        *pp = s->next;
        free_seg(s);
    }
}

int tcp_reasm_feed(tcp_reasm *r, uint32_t seq, const uint8_t *payload, int len)
{
    if (!r || r->done || len <= 0 || !payload)
        return 0;

    if (!r->have_base) {
        r->base_seq = seq;
        r->have_base = 1;
    }

    uint32_t off = seq - r->base_seq; /* wraps naturally for forward seq */
    if (off >= REASM_MAX_LEN)
        return 0;

    if ((size_t)len > REASM_MAX_LEN - off)
        len = REASM_MAX_LEN - off;

    if ((size_t)off + (size_t)len <= r->used_len)
        return 0; /* duplicate / already buffered */

    if ((size_t)off > r->used_len) {
        /* out of order -> park it; prefix did not grow */
        add_pending(r, off, payload, len);
        r->last_seen = time(NULL);
        return 0;
    }

    size_t new_used = (size_t)off + (size_t)len;
    if (ensure_cap(r, new_used)) {
        r->last_seen = time(NULL);
        return 0;
    }

    size_t copy_from = r->used_len - off; /* bytes already present */
    memcpy(r->buf + r->used_len, payload + copy_from, new_used - r->used_len);
    r->used_len = new_used;
    r->last_seen = time(NULL);

    merge_pending(r);
    return 1;
}

const uint8_t *tcp_reasm_data(const tcp_reasm *r, size_t *len)
{
    if (len) *len = r ? r->used_len : 0;
    return r ? r->buf : NULL;
}

void tcp_reasm_mark_consumed(tcp_reasm *r)
{
    if (!r) return;
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
