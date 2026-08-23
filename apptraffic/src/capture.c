/*
 * capture.c - packet capture thread and L3 demultiplexer.
 *
 * Parses Ethernet + IPv4/IPv6 (including extension headers and fragment
 * reassembly via ipreasm), decides the transport/ports, and routes DNS to the
 * DNS callback and TLS ClientHello / HTTP Host through a small TCP first-
 * record reassembler to the SNI callback.
 *
 * Copyright (C) 2024
 * Licensed under GPL-2.0
 */

#define _GNU_SOURCE

#include "apptraffic.h"
#include "capture.h"
#include "proto.h"
#include "tcp_reasm.h"
#include "ipreasm.h"
#include "dnsmap.h"
#include "l7.h"

#include <pcap.h>
#include <pthread.h>
#include <errno.h>

enum {
    STREAM_TLS = 0,
    STREAM_HTTP = 1,
    STREAM_L7 = 2
};

#define STREAM_HASH 4096
#define STREAM_MAX  4096
#define STREAM_IDLE 120      /* reclaim done/idle streams after this many sec */

struct stream {
    int          in_use;
    int          is_v6;
    uint8_t      src[16];
    uint8_t      dst[16];
    uint16_t     sport;
    uint16_t     dport;
    int          type;       /* STREAM_TLS / STREAM_HTTP */
    tcp_reasm    reasm;
    int          l7_done;    /* L7 first-payload already attempted */
    time_t       last_seen;
    struct stream *next;
};

static pcap_t *g_pcap = NULL;
static apptraffic_dns_cb g_dns_callback = NULL;
static apptraffic_sni_cb g_sni_callback = NULL;
static struct stream *stream_hash[STREAM_HASH];
static unsigned int stream_count;

void capture_set_dns_callback(apptraffic_dns_cb cb)
{
    g_dns_callback = cb;
}

void capture_set_sni_callback(apptraffic_sni_cb cb)
{
    g_sni_callback = cb;
}

static unsigned int stream_key(unsigned int idx, int is_v6,
                               const uint8_t *src, const uint8_t *dst,
                               uint16_t sport, uint16_t dport)
{
    uint32_t h = (is_v6 ? 0x9e3779b9u : 0u) ^ (uint32_t)sport ^
                 ((uint32_t)dport << 16);
    int n = is_v6 ? 16 : 4;
    for (int i = 0; i < n; i++) {
        h = (h * 31u) + src[i];
        h = (h * 31u) + dst[i];
    }
    return h % STREAM_HASH;
}

static int stream_match(const struct stream *s, int is_v6,
                        const uint8_t *src, const uint8_t *dst, uint16_t sport,
                        uint16_t dport)
{
    if (s->is_v6 != is_v6 || s->sport != sport || s->dport != dport)
        return 0;
    int n = is_v6 ? 16 : 4;
    return memcmp(s->src, src, n) == 0 && memcmp(s->dst, dst, n) == 0;
}

static void stream_free(struct stream *s)
{
    if (!s) return;
    tcp_reasm_free(&s->reasm);
    free(s);
}

static void stream_age(void)
{
    time_t now = time(NULL);
    for (int i = 0; i < STREAM_HASH; i++) {
        struct stream **pp = &stream_hash[i];
        while (*pp) {
            struct stream *s = *pp;
            if (s->reasm.done || (now - s->last_seen > STREAM_IDLE)) {
                *pp = s->next;
                stream_count--;
                stream_free(s);
            } else {
                pp = &s->next;
            }
        }
    }
}

static struct stream *stream_find_or_create(int is_v6, const uint8_t *src,
                                            const uint8_t *dst, uint16_t sport,
                                            uint16_t dport, int type)
{
    unsigned int idx = stream_key(0, is_v6, src, dst, sport, dport);
    struct stream *s;
    for (s = stream_hash[idx]; s; s = s->next) {
        if (stream_match(s, is_v6, src, dst, sport, dport)) {
            s->last_seen = time(NULL);
            return s;
        }
    }

    if (stream_count >= STREAM_MAX) {
        stream_age();
        if (stream_count >= STREAM_MAX) {
            /* still full: free the least recently used stream anywhere */
            struct stream *oldest = NULL;
            for (int i = 0; i < STREAM_HASH; i++) {
                for (struct stream *t = stream_hash[i]; t; t = t->next) {
                    if (!oldest || t->last_seen < oldest->last_seen)
                        oldest = t;
                }
            }
            if (oldest) {
                /* find parent to unlink */
                for (int i = 0; i < STREAM_HASH; i++) {
                    struct stream **pp = &stream_hash[i];
                    while (*pp) {
                        if (*pp == oldest) {
                            *pp = oldest->next;
                            stream_count--;
                            stream_free(oldest);
                            break;
                        }
                        pp = &(*pp)->next;
                    }
                }
            }
        }
    }

    s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->in_use = 1;
    s->is_v6 = is_v6;
    s->sport = sport;
    s->dport = dport;
    s->type = type;
    s->last_seen = time(NULL);
    int n = is_v6 ? 16 : 4;
    memcpy(s->src, src, n);
    memcpy(s->dst, dst, n);
    tcp_reasm_init(&s->reasm);
    s->next = stream_hash[idx];
    stream_hash[idx] = s;
    stream_count++;
    return s;
}

/* Is dst one of the "server" ports we care about? (client -> server) */
static int is_server_port_tls(uint16_t p)
{
    return p == 443 || p == 8443;
}

static int is_server_port_http(uint16_t p)
{
    return p == 80 || p == 8080;
}

/* Ports we sniff the first payload of for L7 signature matching */
static int is_l7_port(uint16_t p)
{
    return p == 22 || p == 3389 || p == 8000 || p == 4000;
}

/* Record the exact hostname for this connection so the flow's own SNI wins
 * over a shared/CDN IP that resolves to many domains. Only meaningful for
 * IPv4 flows (conntrack accounting in apptraffic is IPv4 only). */
static void store_sni_flow_host(const struct stream *s, const char *host)
{
    if (!s || !host || s->is_v6) return;
    uint32_t sip = 0, dip = 0;
    memcpy(&sip, s->src, 4);
    memcpy(&dip, s->dst, 4);
    mapping_add_flow_host(sip, dip, s->sport, s->dport, IPPROTO_TCP, host);
}

static void store_sni_flow_app(const struct stream *s, const char *app,
                               const char *cat)
{
    if (!s || !app || s->is_v6) return;
    uint32_t sip = 0, dip = 0;
    memcpy(&sip, s->src, 4);
    memcpy(&dip, s->dst, 4);
    mapping_add_flow_app(sip, dip, s->sport, s->dport, IPPROTO_TCP, app, cat);
}

static void dispatch_dns(int is_query, const uint8_t *data, int len,
                         const uint8_t *dst_ip6, int is_v6)
{
    if (is_query) {
        char name[256];
        /* For now queries are not used to build the mapping (a response is
         * needed for the IP), but the parse keeps us ready for DoH matching. */
        proto_dns_parse_query(data, len, name, sizeof(name));
        return;
    }
    if (!g_dns_callback) return;

    proto_dns_rec recs[8];
    int n = proto_dns_parse_response(data, len, recs, 8);
    for (int i = 0; i < n; i++) {
        if (recs[i].is_v6)
            g_dns_callback(recs[i].name, 0, 1, recs[i].ip6);
        else
            g_dns_callback(recs[i].name, recs[i].ip4, 0, NULL);
    }
    (void)dst_ip6;
    (void)is_v6;
}

/* Called from pcap_callback with the transport header (tp) and its length. */
static void process_transport(int is_v6, const uint8_t *src, const uint8_t *dst,
                              uint8_t proto, const uint8_t *tp, int tlen)
{
    if (proto == IPPROTO_UDP && tlen >= 8) {
        uint16_t sport = (tp[0] << 8) | tp[1];
        uint16_t dport = (tp[2] << 8) | tp[3];
        int udp_len = (tp[4] << 8) | tp[5];
        if (udp_len > tlen) udp_len = tlen;
        if (dport == 53 && udp_len >= 8)
            dispatch_dns(1, tp + 8, udp_len - 8, dst, is_v6);
        else if (sport == 53 && udp_len >= 8)
            dispatch_dns(0, tp + 8, udp_len - 8, dst, is_v6);
        else if (!is_v6 && is_l7_port(dport) && udp_len >= 8) {
            const char *app = NULL, *cat = NULL;
            if (l7_match(IPPROTO_UDP, dport, tp + 8, udp_len - 8,
                         &app, &cat)) {
                uint32_t sip = 0, dip = 0;
                memcpy(&sip, src, 4);
                memcpy(&dip, dst, 4);
                mapping_add_flow_app(sip, dip, sport, dport, IPPROTO_UDP,
                                     app, cat);
            }
        }
        return;
    }

    if (proto == IPPROTO_TCP && tlen >= 20) {
        uint16_t sport = (tp[0] << 8) | tp[1];
        uint16_t dport = (tp[2] << 8) | tp[3];
        uint8_t doff = (tp[12] >> 4) & 0x0F;
        int tcp_hdr_len = doff * 4;
        if (tcp_hdr_len > tlen) return;
        if (tcp_hdr_len < 20) return;

        uint8_t flags = tp[13];
        uint32_t seq;
        memcpy(&seq, tp + 4, 4);
        seq = ntohl(seq);

        int type = -1;
        if (is_server_port_tls(dport))
            type = STREAM_TLS;
        else if (is_server_port_http(dport))
            type = STREAM_HTTP;
        else if (is_l7_port(dport))
            type = STREAM_L7;
        if (type < 0)
            return; /* server -> client direction, ignore */

        struct stream *s = stream_find_or_create(is_v6, src, dst, sport, dport,
                                                 type);
        if (!s)
            return;

        /* Prefer the client's SYN to learn the initial sequence number */
        if (flags & 0x02) { /* SYN */
            tcp_reasm_set_isn(&s->reasm, seq);
            return;
        }

        const uint8_t *payload = tp + tcp_hdr_len;
        int payload_len = tlen - tcp_hdr_len;
        if (payload_len <= 0)
            return; /* ACK / pure control, no data */

        if (type == STREAM_L7) {
            /* Signatures (SSH-/RDP/QQ) live in the very first data segment;
             * no reassembly needed. Store the result per 5-tuple once. */
            if (!s->l7_done && !s->is_v6) {
                const char *app = NULL, *cat = NULL;
                if (l7_match(IPPROTO_TCP, dport, payload, payload_len,
                             &app, &cat))
                    store_sni_flow_app(s, app, cat);
                s->l7_done = 1;
            }
            return;
        }

        if (s->reasm.done)
            return;

        tcp_reasm_feed(&s->reasm, seq, payload, payload_len);

        const uint8_t *data;
        size_t dlen;
        data = tcp_reasm_data(&s->reasm, &dlen);
        if (!data || dlen == 0)
            return;

        if (type == STREAM_TLS) {
            if (dlen >= 5 && data[0] == 0x16) {
                uint16_t rl = (data[3] << 8) | data[4];
                size_t need = 5 + (size_t)rl;
                if (dlen >= need) {
                    char host[256];
                    if (g_sni_callback &&
                        proto_tls_parse_sni(data, dlen, host, sizeof(host))) {
                        if (is_v6) {
                            g_sni_callback(host, 0, 1, dst);
                        } else {
                            uint32_t ip4 = 0;
                            memcpy(&ip4, dst, 4);
                            g_sni_callback(host, ip4, 0, NULL);
                        }
                        store_sni_flow_host(s, host);
                    }
                    tcp_reasm_mark_consumed(&s->reasm);
                }
            } else if (dlen >= REASM_MAX_LEN) {
                tcp_reasm_mark_consumed(&s->reasm);
            }
        } else if (type == STREAM_HTTP) {
            if (dlen >= 10) {
                char host[256];
                if (proto_http_parse_host(data, dlen, host, sizeof(host))) {
                    if (is_v6) {
                        g_sni_callback(host, 0, 1, dst);
                    } else {
                        uint32_t ip4 = 0;
                        memcpy(&ip4, dst, 4);
                        g_sni_callback(host, ip4, 0, NULL);
                    }
                    store_sni_flow_host(s, host);
                    tcp_reasm_mark_consumed(&s->reasm);
                }
            }
            if (dlen >= REASM_MAX_LEN)
                tcp_reasm_mark_consumed(&s->reasm);
        }
    }
}

static void handle_ipv4(const uint8_t *ip, int ip_len)
{
    if (ip_len < 20) return;
    uint8_t ihl_raw = ip[0] & 0x0F;
    int ihl = ihl_raw * 4;
    if (ihl < 20 || ihl > ip_len) return;
    uint8_t proto = ip[9];
    uint16_t frag = (ip[6] << 8) | ip[7];
    int offset_bytes = (frag & 0x1FFF) * 8;
    int mf = (frag & 0x2000) != 0;
    uint32_t id = (ip[4] << 8) | ip[5];
    const uint8_t *src = ip + 12;
    const uint8_t *dst = ip + 16;
    int payload_len = ip_len - ihl;
    if (payload_len <= 0) return;

    if (offset_bytes == 0 && !mf) {
        process_transport(0, src, dst, proto, ip + ihl, payload_len);
    } else {
        uint8_t rbuf[IPFRAG_MAX_LEN];
        size_t rlen = 0;
        if (ipfrag_feed(0, src, dst, id, proto, ip + ihl, payload_len,
                        offset_bytes, mf, rbuf, sizeof(rbuf), &rlen)) {
            process_transport(0, src, dst, proto, rbuf, (int)rlen);
        }
    }
}

/* Walk IPv6 extension headers. Returns the final next-header (transport
 * protocol) and sets *tpoff to the offset of the transport header within
 * payload (or -1 and fills frag info when a Fragment header is met). */
static int ipv6_walk(const uint8_t *p, int plen, uint8_t *proto, int *tpoff,
                     int *frag_seen, int *frag_off, int *frag_mf,
                     uint32_t *frag_id)
{
    int off = 0;
    uint8_t nh = p[0]; /* initial next header */
    /* Note: caller passes payload = bytes after the fixed 40-byte header, so
     * p[0] is the first next-header value. */
    int count = 0;
    while (count++ < 16 && off >= 0 && off < plen) {
        if (nh == 44) { /* Fragment header */
            if (off + 8 > plen) return -1;
            uint8_t frag_nh = p[off];
            uint16_t foffm = (p[off + 2] << 8) | p[off + 3];
            *frag_seen = 1;
            *frag_off = (foffm >> 3) * 8;
            *frag_mf = foffm & 0x0001;
            *frag_id = ((uint32_t)p[off + 4] << 24) |
                       ((uint32_t)p[off + 5] << 16) |
                       ((uint32_t)p[off + 6] << 8) | p[off + 7];
            /* transport header starts right after the 8-byte fragment header
             * for the first fragment; the offset domain is that region. */
            *tpoff = off + 8;
            if (frag_nh == IPPROTO_TCP || frag_nh == IPPROTO_UDP) {
                *proto = frag_nh;
                return 0;
            }
            return -1; /* fragment into another extension chain: unsupported */
        }

        if (nh == 0 || nh == 43 || nh == 60 || nh == 135 || nh == 51) {
            if (nh == 51) { /* AH */
                if (off + 2 > plen) return -1;
                int ahlen = (p[off + 1] + 2) * 4;
                nh = p[off];
                off += ahlen;
                if (off > plen) return -1;
                continue;
            }
            if (off + 2 > plen) return -1;
            int hlen = (p[off + 1] + 1) * 8;
            nh = p[off];
            off += hlen;
            if (off > plen) return -1;
            continue;
        }

        *proto = nh;
        *tpoff = off;
        return 0;
    }
    return -1;
}

static void handle_ipv6(const uint8_t *ip, int ip_len)
{
    if (ip_len < 40) return;
    uint16_t payload_len = ((ip[4] << 8) | ip[5]);
    const uint8_t *src = ip + 8;
    const uint8_t *dst = ip + 24;
    /* payload_len is the length after the fixed header; clamp to captured */
    int plen = payload_len;
    if (plen > ip_len - 40) plen = ip_len - 40;
    if (plen < 0) plen = 0;
    const uint8_t *payload = ip + 40;

    uint8_t proto;
    int tpoff = -1;
    int frag_seen = 0, frag_off = 0, frag_mf = 0;
    uint32_t frag_id = 0;
    int rc = ipv6_walk(payload, plen, &proto, &tpoff, &frag_seen, &frag_off,
                       &frag_mf, &frag_id);
    if (rc != 0)
        return;

    if (!frag_seen) {
        if (tpoff < 0 || tpoff > plen) return;
        process_transport(1, src, dst, proto, payload + tpoff,
                          plen - tpoff);
        return;
    }

    /* Fragmented: reassemble the fragmentable part (after the fragment hdr) */
    uint8_t rbuf[IPFRAG_MAX_LEN];
    size_t rlen = 0;
    const uint8_t *frag_payload = payload + tpoff; /* == payload+off+8 */
    int frag_payload_len = plen - tpoff;
    if (frag_payload_len < 0) return;
    if (ipfrag_feed(1, src, dst, frag_id, proto, frag_payload,
                    frag_payload_len, frag_off, frag_mf, rbuf, sizeof(rbuf),
                    &rlen)) {
        process_transport(1, src, dst, proto, rbuf, (int)rlen);
    }
}

static void pcap_callback(u_char *user, const struct pcap_pkthdr *hdr,
                          const u_char *bytes)
{
    (void)user;
    if (hdr->len < 14) return;

    uint16_t ether_type = (bytes[12] << 8) | bytes[13];
    const uint8_t *ip_data;
    int ip_len = hdr->len - 14;

    if (ether_type == 0x0800) {
        ip_data = bytes + 14;
    } else if (ether_type == 0x86DD) {
        ip_data = bytes + 14;
    } else if (ether_type == 0x8100) {
        /* VLAN: outer tag 4 bytes, read inner ethertype */
        if (hdr->len < 18) return;
        uint16_t inner = (bytes[16] << 8) | bytes[17];
        if (inner != 0x0800 && inner != 0x86DD)
            return;
        ip_data = bytes + 18;
        ip_len -= 4;
    } else {
        return;
    }

    if (ip_len < 20) return;

    if (ether_type == 0x0800 || (ether_type == 0x8100 &&
                                 ((bytes[16] << 8) | bytes[17]) == 0x0800)) {
        handle_ipv4((const uint8_t *)ip_data, ip_len);
    } else {
        handle_ipv6((const uint8_t *)ip_data, ip_len);
    }
}

int capture_init(const char *iface)
{
    char errbuf[PCAP_ERRBUF_SIZE];
    int snaplen = 4096;
    int promisc = 1;
    int timeout_ms = 500;

    /* Filter DNS + HTTP + TLS; 'port' matches both TCP and UDP so UDP 53
     * (DNS) and TCP 80/8080/443/8443 reach us. */
    const char *filter =
        "port 53 or "
        "port 80 or port 8080 or "
        "port 443 or port 8443 or "
        "port 22 or port 3389 or port 8000 or port 4000";

    const char *dev = (iface && iface[0]) ? iface : "any";

    g_pcap = pcap_open_live(dev, snaplen, promisc, timeout_ms, errbuf);
    if (!g_pcap) {
        fprintf(stderr, "pcap_open_live failed: %s\n", errbuf);
        return -1;
    }

    struct bpf_program fp;
    if (pcap_compile(g_pcap, &fp, filter, 1, PCAP_NETMASK_UNKNOWN) == -1) {
        fprintf(stderr, "pcap_compile failed: %s\n", pcap_geterr(g_pcap));
        pcap_close(g_pcap);
        g_pcap = NULL;
        return -1;
    }
    if (pcap_setfilter(g_pcap, &fp) == -1) {
        fprintf(stderr, "pcap_setfilter failed: %s\n", pcap_geterr(g_pcap));
        pcap_freecode(&fp);
        pcap_close(g_pcap);
        g_pcap = NULL;
        return -1;
    }
    pcap_freecode(&fp);
    return 0;
}

void *capture_run(void *arg)
{
    (void)arg;
    if (!g_pcap) return NULL;
    while (g_running) {
        pcap_dispatch(g_pcap, 10, pcap_callback, NULL);
    }
    return NULL;
}

void capture_stop(void)
{
    g_running = 0;
    if (g_pcap) {
        pcap_breakloop(g_pcap);
        pcap_close(g_pcap);
        g_pcap = NULL;
    }
}

int capture_is_active(void)
{
    return g_pcap != NULL;
}
