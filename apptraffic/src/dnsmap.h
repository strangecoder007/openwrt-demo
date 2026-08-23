#ifndef DNSMAP_H
#define DNSMAP_H

#include "apptraffic.h"

/* IP -> domain cache */
void mapping_add_dns(const char *domain, uint32_t ip, int is_v6,
                     const uint8_t *ip6, int confidence);
const char *mapping_lookup_domain(uint32_t ip);
void mapping_expire_dns(void);

/* Per-flow (5-tuple) SNI host cache: the authoritative hostname for a given
 * connection, from its own TLS ClientHello. Takes priority over the IP cache. */
void mapping_add_flow_host(uint32_t src_ip, uint32_t dst_ip, uint16_t src_port,
                           uint16_t dst_port, uint8_t proto,
                           const char *host);
const char *mapping_lookup_flow_host(uint32_t src_ip, uint32_t dst_ip,
                                     uint16_t src_port, uint16_t dst_port,
                                     uint8_t proto);

/* Per-flow L7 signature result cache (5-tuple -> app/category). */
void mapping_add_flow_app(uint32_t src_ip, uint32_t dst_ip, uint16_t src_port,
                          uint16_t dst_port, uint8_t proto,
                          const char *app, const char *cat);
int mapping_lookup_flow_app(uint32_t src_ip, uint32_t dst_ip, uint16_t src_port,
                            uint16_t dst_port, uint8_t proto,
                            const char **app, const char **cat);

/* Domain -> app mapping (loaded from app-mapping.txt) */
int mapping_load(const char *path);
const char *mapping_lookup_app(const char *domain);
const char *mapping_lookup_category(const char *domain);

/* Local (this host's own addresses) service recognition:
 * enumerate the host's IPv4 addresses, then label flows whose destination is
 * one of them by the well-known service on the destination port. */
void mapping_init_local(void);
void mapping_free_local(void);
int mapping_local_service(uint32_t dst_ip, uint16_t dst_port,
                          const char **app, const char **cat);

/* True if ip is protocol noise we should not store: IPv4 multicast
 * (224.0.0.0/4), loopback (127.0.0.0/8), limited broadcast and any of this
 * host's subnet broadcast addresses. */
int mapping_is_noise_ip(uint32_t ip);

#endif /* DNSMAP_H */
