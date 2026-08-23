#ifndef TCP_REASM_H
#define TCP_REASM_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

/* Maximum number of stream bytes we keep for the first data record */
#define REASM_MAX_LEN 8192

/* Out-of-order segment waiting for a gap to close */
struct tcp_seg {
    uint32_t        off;
    uint8_t        *data;
    size_t          len;
    struct tcp_seg *next;
};

/*
 * Per-connection "first data record" reassembler.
 *
 * base_seq maps to stream offset 0. It is taken from the SYN (ISN+1) when
 * available, otherwise from the first data segment seen (assumes the capture
 * starts at the beginning of the record). Only the contiguous prefix is kept
 * in buf; out-of-order segments are parked in pending and merged lazily.
 */
typedef struct tcp_reasm {
    uint32_t        base_seq;
    int             have_base;
    uint8_t        *buf;      /* contiguous prefix [0, used_len) */
    size_t          used_len;
    size_t          cap;
    struct tcp_seg *pending;
    int             done;     /* first record already consumed */
    time_t          last_seen;
} tcp_reasm;

void tcp_reasm_init(tcp_reasm *r);
void tcp_reasm_free(tcp_reasm *r);

/* Record the server-side initial sequence number learned from a SYN */
void tcp_reasm_set_isn(tcp_reasm *r, uint32_t isn);

/*
 * Feed one client->server data segment. Returns 1 if the contiguous prefix
 * grew (caller may re-check for a complete record), 0 otherwise.
 */
int tcp_reasm_feed(tcp_reasm *r, uint32_t seq, const uint8_t *payload, int len);

/* Return pointers to the current contiguous prefix */
const uint8_t *tcp_reasm_data(const tcp_reasm *r, size_t *len);

/* Stop buffering and release the buffers (first record handled) */
void tcp_reasm_mark_consumed(tcp_reasm *r);

#endif /* TCP_REASM_H */
