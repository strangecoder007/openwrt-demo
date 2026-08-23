#ifndef IPREASM_H
#define IPREASM_H

#include <stdint.h>
#include <stddef.h>

/* Maximum reassembled IP payload we hold per datagram */
#define IPFRAG_MAX_LEN 8192

/*
 * Feed one IP fragment. When the datagram is fully reassembled the bytes are
 * copied into outbuf and 1 is returned; otherwise 0 (caller keeps waiting).
 */
int ipfrag_feed(int is_v6,
                const uint8_t *src, const uint8_t *dst, uint32_t id,
                uint8_t proto,
                const uint8_t *payload, int payload_len,
                int offset_bytes, int mf,
                uint8_t *outbuf, size_t outbuf_cap, size_t *outlen);

/* Drop entries idle for more than a few seconds (call from the daemon loop) */
void ipfrag_age(void);

#endif /* IPREASM_H */
