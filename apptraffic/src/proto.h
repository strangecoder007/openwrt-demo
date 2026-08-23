#ifndef PROTO_H
#define PROTO_H

#include <stdint.h>
#include <stddef.h>

/* DNS record types (standard values from arpa/nameser.h) */
#ifndef DNS_TYPE_A
#define DNS_TYPE_A    1
#endif
#ifndef DNS_TYPE_AAAA
#define DNS_TYPE_AAAA 28
#endif

/* A single A/AAAA answer extracted from a DNS response */
typedef struct proto_dns_rec {
    char     name[256];   /* owner name (or question name fallback) */
    uint32_t ip4;         /* valid when !is_v6 */
    uint8_t  ip6[16];     /* valid when is_v6 */
    int      is_v6;
} proto_dns_rec;

/*
 * DNS response parser. Extracts up to max A/AAAA answers into recs.
 * Returns the number of records filled (0 if none / malformed).
 */
int proto_dns_parse_response(const uint8_t *data, int len, proto_dns_rec *recs,
                             int max);

/*
 * DNS query parser. Copies the first question name into name.
 * Returns 1 if a qname was found, 0 otherwise.
 */
int proto_dns_parse_query(const uint8_t *data, int len, char *name,
                          size_t name_len);

/*
 * TLS ClientHello SNI extractor. Expects a complete TLS record (5-byte
 * header + record body) in data/len. Copies the SNI hostname into host.
 * Returns 1 if a valid (containing a dot) hostname was found.
 */
int proto_tls_parse_sni(const uint8_t *data, int len, char *host,
                        size_t host_len);

/*
 * HTTP request Host extractor. Expects the beginning of an HTTP request.
 * Copies the Host header value into host (port stripped).
 * Returns 1 if a Host header with a dot was found.
 */
int proto_http_parse_host(const uint8_t *data, int len, char *host,
                          size_t host_len);

#endif /* PROTO_H */
