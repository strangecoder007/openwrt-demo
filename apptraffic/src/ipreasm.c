/*
 * ipreasm.c - bounded in-order IPv4/IPv6 fragment reassembler.
 *
 * Keeps a small table of in-flight datagrams keyed by (src, dst, id, proto).
 * Fragments are accumulated by byte offset. IP fragments for a UDP/DNS reply
 * arrive in order in practice, so we only handle the contiguous (in-order)
 * case; out-of-order fragments are dropped rather than cached. When the final
 * fragment (MF=0) announces the total length and every byte is present, the
 * reassembled payload is copied to the caller and the entry is released.
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
    int      in_use;
    int      is_v6;
    uint8_t  src[16];
    uint8_t  dst[16];
    uint32_t id;
    uint8_t  proto;

    uint8_t *buf;       /* contiguous prefix [0, used_len) */
    size_t   used_len;
    size_t   cap;

    int      have_last;
    size_t   total;
    time_t   last_seen;
};

static struct ipfrag g_table[IPFRAG_MAX];

static int key_equal(const struct ipfrag *f, int is_v6,
                     const uint8_t *src, const uint8_t *dst, uint32_t id,
                     uint8_t proto)
{
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
        if (key_equal(f, is_v6, src, dst, id, proto))
            return f;
        if (!oldest || f->last_seen < oldest->last_seen)
            oldest = f;
    }
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
    struct ipfrag *f = find_or_create(is_v6, src, dst, id, proto);
    if (!f) return 0;

    if (offset_bytes >= IPFRAG_MAX_LEN)
        return 0;
    if (payload_len > (int)(IPFRAG_MAX_LEN - offset_bytes))
        payload_len = IPFRAG_MAX_LEN - offset_bytes;
    if (payload_len <= 0)
        return 0;

    if (!mf) {
        f->have_last = 1;
        f->total = (size_t)offset_bytes + (size_t)payload_len;
    }

    /* Only accept a fragment that is contiguous with what we already have.
     * (offset 0 then increasing, no gaps.) Out-of-order is dropped. */
    if ((size_t)offset_bytes > f->used_len)
        return 0;

    size_t end = (size_t)offset_bytes + (size_t)payload_len;
    if (end > f->used_len) {
        if (ensure_cap(f, end)) return 0;
        size_t copy_off = f->used_len - (size_t)offset_bytes;
        memcpy(f->buf + f->used_len, payload + copy_off, end - f->used_len);
        f->used_len = end;
    }

    f->last_seen = time(NULL);

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
    time_t now = time(NULL);
    for (int i = 0; i < IPFRAG_MAX; i++) {
        struct ipfrag *f = &g_table[i];
        if (f->in_use && now - f->last_seen > IPFRAG_TIMEOUT) {
            free(f->buf);
            memset(f, 0, sizeof(*f));
        }
    }
}
