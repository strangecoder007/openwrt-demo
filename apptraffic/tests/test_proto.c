/*
 * test_proto.c - standalone unit tests for the pure protocol parsers and the
 * TCP first-record reassembler. Compiles with system gcc, no libpcap needed:
 *
 *   gcc -std=gnu99 -Wall -I src -o /tmp/test_proto \
 *       src/proto.c src/tcp_reasm.c tests/test_proto.c
 *
 * Feed crafted byte streams to proto_*_parse_* and tcp_reasm_* to validate
 * the "first record" reconstruction, retransmission handling and DNS parsing.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include "proto.h"
#include "tcp_reasm.h"
#include "dnsmap.h"
#include "l7.h"

/* Provide the definition for the extern g_config used inside dnsmap.c */
struct config g_config;

static int g_fail;
static int g_pass;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; printf("  ok: %s\n", msg); } \
    else { g_fail++; printf("  FAIL: %s\n", msg); } \
} while (0)

/* Build a TLS 1.2 ClientHello carrying server_name "example.com".
 * Fills buf, returns the total TLS record length. */
static size_t build_clienthello(uint8_t *buf, size_t cap)
{
    const char *name = "example.com";
    size_t name_len = strlen(name);

    /* extensions block */
    size_t ext_sni_data_len = 5 + name_len;    /* list_len(2)+type(1)+len(2)+name */
    size_t ext_block_len = 4 + ext_sni_data_len;
    size_t ch_fields_len = 43 + ext_block_len; /* 2+32+1+4+2+2+ext_len(2)+block */
    size_t hs_len = ch_fields_len;
    size_t record_len = 4 + hs_len;

    if (record_len + 5 > cap) return 0;

    uint8_t *p = buf;
    /* TLS record header */
    p[0] = 0x16;               /* handshake */
    p[1] = 0x03; p[2] = 0x01;  /* TLS 1.0 record version */
    p[3] = (uint8_t)(record_len >> 8);
    p[4] = (uint8_t)(record_len & 0xff);
    p += 5;

    /* Handshake header: type=ClientHello */
    p[0] = 0x01;
    p[1] = (uint8_t)(hs_len >> 16);
    p[2] = (uint8_t)(hs_len >> 8);
    p[3] = (uint8_t)(hs_len & 0xff);
    p += 4;

    /* legacy_version */
    p[0] = 0x03; p[1] = 0x03; p += 2;
    /* random */
    memset(p, 0xAA, 32); p += 32;
    /* session id */
    p[0] = 0x00; p += 1;
    /* cipher suites */
    p[0] = 0x00; p[1] = 0x02; p[2] = 0x13; p[3] = 0x01; p += 4;
    /* compression */
    p[0] = 0x01; p[1] = 0x00; p += 2;
    /* extensions length */
    p[0] = (uint8_t)(ext_block_len >> 8);
    p[1] = (uint8_t)(ext_block_len & 0xff); p += 2;
    /* SNI extension */
    p[0] = 0x00; p[1] = 0x00;                      /* type=server_name */
    p[2] = (uint8_t)(ext_sni_data_len >> 8);
    p[3] = (uint8_t)(ext_sni_data_len & 0xff); p += 4;
    /* server_name_list length */
    size_t list_len = 3 + name_len;
    p[0] = (uint8_t)(list_len >> 8);
    p[1] = (uint8_t)(list_len & 0xff); p += 2;
    /* name type host_name */
    p[0] = 0x00; p += 1;
    /* name length */
    p[0] = (uint8_t)(name_len >> 8);
    p[1] = (uint8_t)(name_len & 0xff); p += 2;
    memcpy(p, name, name_len); p += name_len;

    return record_len + 5;
}

static void test_tls_single(void)
{
    printf("[TLS] single complete ClientHello\n");
    uint8_t buf[4096];
    size_t len = build_clienthello(buf, sizeof(buf));
    char host[256];
    int hit = proto_tls_parse_sni(buf, (int)len, host, sizeof(host));
    CHECK(hit == 1 && strcmp(host, "example.com") == 0, "SNI extracted");
}

static void test_tls_reassembled(void)
{
    printf("[TLS] ClientHello split across two TCP segments\n");
    uint8_t buf[4096];
    size_t len = build_clienthello(buf, sizeof(buf));

    size_t split = 30; /* cut within the ClientHello */
    tcp_reasm r;
    tcp_reasm_init(&r);
    uint32_t base = 1000;
    tcp_reasm_set_isn(&r, base - 1); /* ISN -> first data seq = base */

    /* Feed segment 2 first (out of order) */
    int g1 = tcp_reasm_feed(&r, base + split, buf + split, (int)(len - split));
    /* Then segment 1 */
    int g2 = tcp_reasm_feed(&r, base, buf, (int)split);

    size_t dlen;
    const uint8_t *data = tcp_reasm_data(&r, &dlen);
    CHECK(g1 == 0 && g2 == 1, "out-of-order first, gap then growth");
    CHECK(dlen >= len, "reassembled full record");

    char host[256];
    int hit = proto_tls_parse_sni(data, (int)dlen, host, sizeof(host));
    CHECK(hit == 1 && strcmp(host, "example.com") == 0, "SNI after reassembly");
    tcp_reasm_free(&r);
}

static void test_tls_retransmit(void)
{
    printf("[TLS] duplicate retransmission does not corrupt state\n");
    uint8_t buf[4096];
    size_t len = build_clienthello(buf, sizeof(buf));
    tcp_reasm r;
    tcp_reasm_init(&r);
    uint32_t base = 500;
    tcp_reasm_set_isn(&r, base - 1);

    tcp_reasm_feed(&r, base, buf, (int)len);
    size_t dlen_before;
    (void)tcp_reasm_data(&r, &dlen_before);
    int g = tcp_reasm_feed(&r, base, buf, (int)len);
    size_t dlen_after;
    (void)tcp_reasm_data(&r, &dlen_after);
    CHECK(g == 0 && dlen_before == dlen_after, "retransmit ignored, no dup bytes");
    tcp_reasm_free(&r);
}

static void test_dns_response(void)
{
    printf("[DNS] response with A + AAAA answers\n");
    uint8_t b[512];
    size_t n = 0;
    /* header */
    b[n++] = 0x12; b[n++] = 0x34;          /* id */
    b[n++] = 0x81; b[n++] = 0x80;          /* flags QR=1, RD=1, RA=1 */
    b[n++] = 0x00; b[n++] = 0x01;          /* qdcount */
    b[n++] = 0x00; b[n++] = 0x02;          /* ancount */
    b[n++] = 0x00; b[n++] = 0x00;          /* nscount */
    b[n++] = 0x00; b[n++] = 0x00;          /* arcount */
    /* question: example.com A IN */
    b[n++] = 7; memcpy(b + n, "example", 7); n += 7;
    b[n++] = 3; memcpy(b + n, "com", 3); n += 3;
    b[n++] = 0x00;
    b[n++] = 0x00; b[n++] = 0x01;          /* qtype A */
    b[n++] = 0x00; b[n++] = 0x01;          /* qclass IN */
    /* answer1: pointer to QNAME, A 1.2.3.4 */
    b[n++] = 0xC0; b[n++] = 0x0C;
    b[n++] = 0x00; b[n++] = 0x01;
    b[n++] = 0x00; b[n++] = 0x01;
    b[n++] = 0x00; b[n++] = 0x00; b[n++] = 0x00; b[n++] = 0x3C;
    b[n++] = 0x00; b[n++] = 0x04;
    b[n++] = 1; b[n++] = 2; b[n++] = 3; b[n++] = 4;
    /* answer2: pointer to QNAME, AAAA 2001:db8::1 */
    b[n++] = 0xC0; b[n++] = 0x0C;
    b[n++] = 0x00; b[n++] = 0x1C;
    b[n++] = 0x00; b[n++] = 0x01;
    b[n++] = 0x00; b[n++] = 0x00; b[n++] = 0x00; b[n++] = 0x3C;
    b[n++] = 0x00; b[n++] = 0x10;
    static const uint8_t v6[16] = {0x20,0x01,0x0d,0xb8,0,0,0,0,0,0,0,0,0,0,0,1};
    memcpy(b + n, v6, 16); n += 16;

    proto_dns_rec recs[8];
    int cnt = proto_dns_parse_response(b, (int)n, recs, 8);
    CHECK(cnt == 2, "two answers parsed");
    if (cnt == 2) {
        CHECK(recs[0].is_v6 == 0 && ntohl(recs[0].ip4) == 0x01020304,
              "first answer is A 1.2.3.4");
        CHECK(strcmp(recs[0].name, "example.com") == 0, "answer name resolved");
        CHECK(recs[1].is_v6 == 1 && recs[1].ip6[0] == 0x20,
              "second answer is AAAA");
    }
}

static void test_http_host(void)
{
    printf("[HTTP] Host header extraction\n");
    const char *req = "GET /index.html HTTP/1.1\r\nHost: www.example.com:8080\r\n"
                      "User-Agent: t\r\n\r\n";
    char host[256];
    int hit = proto_http_parse_host((const uint8_t *)req, (int)strlen(req),
                                    host, sizeof(host));
    CHECK(hit == 1 && strcmp(host, "www.example.com") == 0,
          "Host extracted with port stripped");
}

static void test_confidence_and_flow_host(void)
{
    memset(&g_config, 0, sizeof(g_config));
    g_config.dns_timeout = 3600;

    printf("[DNS/SNI] confidence selection + per-flow host\n");
    uint32_t ip = htonl(0x0a000001); /* 10.0.0.1 */

    /* DNS (conf 1) first, then SNI (conf 2) for the same IP */
    mapping_add_dns("dns.example.com", ip, 0, NULL, 1);
    mapping_add_dns("sni.example.com", ip, 0, NULL, 2);
    const char *d = mapping_lookup_domain(ip);
    CHECK(d && strcmp(d, "sni.example.com") == 0,
          "SNI (conf=2) wins over DNS (conf=1) for the same IP");

    /* Exact per-connection SNI host cache */
    uint32_t src = htonl(0xaabbcc01);
    mapping_add_flow_host(src, ip, 5555, 443, IPPROTO_TCP,
                          "exact.example.com");
    const char *fh = mapping_lookup_flow_host(src, ip, 5555, 443, IPPROTO_TCP);
    CHECK(fh && strcmp(fh, "exact.example.com") == 0,
          "per-flow SNI host returned for the same 5-tuple");
    CHECK(mapping_lookup_flow_host(src, ip, 5556, 443, IPPROTO_TCP) == NULL,
          "per-flow SNI host miss on a different source port");
}

static void test_noise_ip(void)
{
    printf("[NOISE] ip classification\n");
    CHECK(mapping_is_noise_ip(htonl(0xE00000FB)), "224.0.0.251 (mDNS) is noise");
    CHECK(mapping_is_noise_ip(htonl(0xEFFFFFFA)), "239.255.255.250 (SSDP) is noise");
    CHECK(mapping_is_noise_ip(htonl(0xFFFFFFFF)), "255.255.255.255 (broadcast) is noise");
    CHECK(mapping_is_noise_ip(htonl(0x7F000001)), "127.0.0.1 (loopback) is noise");
    CHECK(!mapping_is_noise_ip(htonl(0xC0A86401)), "192.168.100.1 is not noise");
    CHECK(!mapping_is_noise_ip(htonl(0x08080808)), "8.8.8.8 is not noise");
}

static void test_l7(void)
{
    printf("[L7] signature matching\n");
    int n = l7_load("src/l7-rules.txt");
    CHECK(n > 0, "L7 rules loaded");

    const char *app, *cat;
    const char *ssh = "SSH-2.0-OpenSSH_7.6";
    int m = l7_match(IPPROTO_TCP, 22, (const uint8_t *)ssh, (int)strlen(ssh),
                     &app, &cat);
    CHECK(m && strcmp(app, "SSH") == 0, "SSH matched on port 22");

    const uint8_t rdp[] = {0x03, 0, 0, 0x13, 0x0e, 0x00, 0x00};
    int m2 = l7_match(IPPROTO_TCP, 3389, rdp, (int)sizeof(rdp), &app, &cat);
    CHECK(m2 && strcmp(app, "RDP") == 0, "RDP matched (TPKT magic)");

    const char *qq = "\x02x";
    int m3 = l7_match(IPPROTO_UDP, 8000, (const uint8_t *)qq, (int)strlen(qq),
                      &app, &cat);
    CHECK(m3 && strcmp(app, "QQ") == 0, "QQ matched on UDP 8000 (port heuristic)");

    const char *http = "GET / HTTP/1.1\r\nHost: a.com\r\n\r\n";
    int m4 = l7_match(IPPROTO_TCP, 80, (const uint8_t *)http, (int)strlen(http),
                      &app, &cat);
    CHECK(m4 == 0, "no match for HTTP on port 80 (not an L7 rule)");

    const char *ssh2 = "SSH-2.0-OpenSSH";
    int m5 = l7_match(IPPROTO_TCP, 9999, (const uint8_t *)ssh2,
                      (int)strlen(ssh2), &app, &cat);
    CHECK(m5 == 0, "SSH payload on unknown port does not match");
}

int main(void)
{
    test_tls_single();
    test_tls_reassembled();
    test_tls_retransmit();
    test_dns_response();
    test_http_host();
    test_confidence_and_flow_host();
    test_noise_ip();
    test_l7();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
