/*
 * proto.c - protocol header parsers (pure functions, no pcap dependency).
 *
 * Each function takes a byte buffer and returns extracted fields so the
 * parsers can be unit-tested in isolation and reused for QUIC/HTTP3 later.
 *
 * Copyright (C) 2024
 * Licensed under GPL-2.0
 */

#define _GNU_SOURCE

#include "proto.h"
#include <string.h>
#include <strings.h>

/* Byte-accurate substring search for buffers that are not NUL-terminated. */
static const uint8_t *mem_find(const uint8_t *hay, size_t hay_len,
                               const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0 || nlen > hay_len) return NULL;
    for (size_t i = 0; i + nlen <= hay_len; i++) {
        if (memcmp(hay + i, needle, nlen) == 0)
            return hay + i;
    }
    return NULL;
}

static int matches_method(const uint8_t *d, int len, const char *m)
{
    size_t ml = strlen(m);
    if ((size_t)len < ml) return 0;
    return strncasecmp((const char *)d, m, ml) == 0;
}

/* Read a DNS domain name at offset, handling compression pointers.
 * Returns the byte position after the name (after 0x00 or compression
 * pointer). On error returns -1. */
static int dns_read_name(const uint8_t *data, int data_len, int offset,
                         char *buf, int buf_len)
{
    int pos = offset;
    int buf_pos = 0;
    int jumped = 0;
    int return_pos = 0;
    int max_jumps = 10; /* prevent infinite pointer loops */

    while (max_jumps-- > 0) {
        if (pos >= data_len) return -1;

        uint8_t label_len = data[pos];

        if (label_len == 0) {
            if (!jumped) return_pos = pos + 1;
            break;
        }

        if ((label_len & 0xC0) == 0xC0) {
            if (pos + 2 > data_len) return -1;
            uint16_t ptr = ((label_len & 0x3F) << 8) | data[pos + 1];
            if (ptr >= (uint16_t)data_len) return -1;
            if (!jumped) {
                return_pos = pos + 2;
                jumped = 1;
            }
            pos = ptr;
            continue;
        }

        if (label_len > 63 || pos + 1 + label_len > data_len) return -1;

        if (buf_pos > 0 && buf_pos < buf_len - 1)
            buf[buf_pos++] = '.';

        pos++;
        for (int i = 0; i < label_len && pos < data_len; i++) {
            if (buf_pos < buf_len - 1)
                buf[buf_pos++] = data[pos];
            pos++;
        }
    }

    if (buf_pos < buf_len)
        buf[buf_pos] = '\0';
    else
        buf[buf_len - 1] = '\0';

    return jumped ? return_pos : (buf_pos > 0 ? return_pos : -1);
}

int proto_dns_parse_query(const uint8_t *data, int len, char *name,
                          size_t name_len)
{
    if (len < 12 || !name || name_len == 0) return 0;

    uint16_t flags = (data[2] << 8) | data[3];
    uint16_t qdcount = (data[4] << 8) | data[5];

    /* Only process queries (QR=0) with at least one question */
    if ((flags & 0x8000) || qdcount == 0) return 0;

    int pos = 12;
    char domain_buf[256];
    domain_buf[0] = '\0';

    for (int i = 0; i < qdcount && pos < len; i++) {
        pos = dns_read_name(data, len, pos, domain_buf, sizeof(domain_buf));
        if (pos < 0 || pos + 4 > len) break;
        pos += 4; /* QTYPE(2) + QCLASS(2) */
    }

    if (!domain_buf[0]) return 0;

    strncpy(name, domain_buf, name_len - 1);
    name[name_len - 1] = '\0';
    return 1;
}

int proto_dns_parse_response(const uint8_t *data, int len, proto_dns_rec *recs,
                             int max)
{
    if (len < 12 || !recs || max <= 0) return 0;

    uint16_t flags = (data[2] << 8) | data[3];
    uint16_t qdcount = (data[4] << 8) | data[5];
    uint16_t ancount = (data[6] << 8) | data[7];

    /* Only process responses (QR=1) with answers */
    if (!(flags & 0x8000) || ancount == 0) return 0;

    int pos = 12;
    char qname_buf[256] = "";

    for (int i = 0; i < qdcount && pos < len; i++) {
        int new_pos = dns_read_name(data, len, pos, qname_buf,
                                    sizeof(qname_buf));
        if (new_pos < 0) break;
        pos = new_pos;
        if (pos + 4 > len) break;
        pos += 4; /* QTYPE(2) + QCLASS(2) */
    }

    char domain_buf[256];
    int written = 0;

    for (int i = 0; i < ancount && pos < len && written < max; i++) {
        if (pos + 10 > len) break;

        int new_pos = dns_read_name(data, len, pos, domain_buf,
                                    sizeof(domain_buf));
        if (new_pos < 0) break;
        pos = new_pos;

        if (!domain_buf[0] && qname_buf[0])
            strncpy(domain_buf, qname_buf, sizeof(domain_buf) - 1);

        uint16_t atype = (data[pos] << 8) | data[pos + 1];
        uint16_t rdlength = (data[pos + 8] << 8) | data[pos + 9];
        pos += 10;

        if (pos + rdlength > len) break;

        proto_dns_rec *r = &recs[written];
        strncpy(r->name, domain_buf[0] ? domain_buf : qname_buf,
                sizeof(r->name) - 1);
        r->name[sizeof(r->name) - 1] = '\0';
        r->is_v6 = 0;
        r->ip4 = 0;
        memset(r->ip6, 0, sizeof(r->ip6));

        if (atype == DNS_TYPE_A && rdlength == 4) {
            memcpy(&r->ip4, data + pos, 4);
            r->is_v6 = 0;
            written++;
        } else if (atype == DNS_TYPE_AAAA && rdlength == 16) {
            memcpy(r->ip6, data + pos, 16);
            r->is_v6 = 1;
            written++;
        }

        pos += rdlength;
    }

    return written;
}

int proto_tls_parse_sni(const uint8_t *data, int len, char *host,
                        size_t host_len)
{
    if (len < 5 || !host || host_len == 0) return 0;

    uint8_t content_type = data[0];
    uint16_t version = (data[1] << 8) | data[2];
    uint16_t record_len = (data[3] << 8) | data[4];

    if (content_type != 22) return 0;       /* Handshake */
    if (version < 0x0301) return 0;         /* TLS 1.0+ */

    int pos = 5;
    if (pos + record_len > len) return 0;   /* incomplete record */

    if (pos + 4 > len) return 0;
    uint8_t hs_type = data[pos];
    uint32_t hs_len = (data[pos + 1] << 16) | (data[pos + 2] << 8) |
                      data[pos + 3];
    pos += 4;

    if (hs_type != 1) return 0;             /* ClientHello */
    if (pos + hs_len > len) return 0;

    if (pos + 38 > len) return 0;
    pos += 2;  /* legacy_version */
    pos += 32; /* random */

    uint8_t sid_len = data[pos];
    pos += 1 + sid_len;
    if (pos + 2 > len) return 0;

    uint16_t cs_len = (data[pos] << 8) | data[pos + 1];
    pos += 2 + cs_len;
    if (pos + 1 > len) return 0;

    uint8_t cm_len = data[pos];
    pos += 1 + cm_len;
    if (pos + 2 > len) return 0;

    uint16_t ext_len = (data[pos] << 8) | data[pos + 1];
    pos += 2;
    int ext_end = pos + ext_len;
    if (ext_end > len) ext_end = len;

    while (pos + 4 <= ext_end) {
        uint16_t ext_type = (data[pos] << 8) | data[pos + 1];
        uint16_t ext_data_len = (data[pos + 2] << 8) | data[pos + 3];
        pos += 4;

        if (ext_type == 0x0000 && pos + ext_data_len <= ext_end) {
            if (pos + 5 > ext_end) break;
            pos += 2; /* server_name_list length */
            if (pos + 3 > ext_end) break;
            uint8_t name_type = data[pos];
            uint16_t name_len = (data[pos + 1] << 8) | data[pos + 2];
            pos += 3;
            if (name_type == 0 && name_len > 0 &&
                pos + name_len <= ext_end) {
                int copy_len = name_len < (int)host_len - 1
                                   ? name_len : (int)host_len - 1;
                memcpy(host, data + pos, copy_len);
                host[copy_len] = '\0';
                if (strchr(host, '.'))
                    return 1;
            }
            break;
        }
        pos += ext_data_len;
    }

    return 0;
}

int proto_http_parse_host(const uint8_t *data, int len, char *host,
                          size_t host_len)
{
    if (len < 10 || !host || host_len == 0) return 0;

    if (!matches_method(data, len, "GET ") &&
        !matches_method(data, len, "POST ") &&
        !matches_method(data, len, "HEAD ") &&
        !matches_method(data, len, "PUT ") &&
        !matches_method(data, len, "OPTIONS ")) {
        return 0;
    }

    const uint8_t *host_start = mem_find(data, (size_t)len, "\r\nHost: ");
    if (!host_start) return 0;
    host_start += 8;

    const uint8_t *host_end =
        mem_find(host_start, (size_t)(len - (host_start - data)), "\r\n");
    if (!host_end) return 0;

    int host_len_i = (int)(host_end - host_start);
    if (host_len_i >= (int)host_len)
        host_len_i = host_len - 1;
    if (host_len_i <= 0) return 0;

    memcpy(host, host_start, host_len_i);
    host[host_len_i] = '\0';

    char *port_colon = strrchr(host, ':');
    if (port_colon) *port_colon = '\0';

    return strchr(host, '.') ? 1 : 0;
}
