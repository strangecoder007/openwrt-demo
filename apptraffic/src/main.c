/*
 * apptraffic - Application-aware traffic analysis tool for OpenWrt
 *
 * Identifies which apps and websites users are accessing by:
 * 1. Capturing DNS queries to build IP→domain mapping
 * 2. Extracting TLS SNI from HTTPS connections
 * 3. Reading netfilter conntrack for flow statistics
 * 4. Matching domains against known app patterns
 * 5. Storing results in SQLite with JSON/CSV export
 *
 * Copyright (C) 2024
 * Licensed under GPL-2.0
 */

#include "apptraffic.h"
#include <getopt.h>

/* Global state */
static struct config g_config;
static volatile int g_running = 1;
static struct db_handle *g_db = NULL;

/* DNS cache hash table */
static struct dns_entry *dns_hash[MAX_DNS_CACHE];
static pthread_mutex_t dns_mutex = PTHREAD_MUTEX_INITIALIZER;

/* App mapping list */
static struct app_mapping *app_mappings = NULL;
static pthread_mutex_t app_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Flow table */
static struct flow_entry *flow_hash[MAX_FLOW_ENTRIES];
static pthread_mutex_t flow_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Simple hash functions */
static inline unsigned int hash_ip(uint32_t ip) {
    return (ip ^ (ip >> 16) ^ (ip >> 8)) % MAX_DNS_CACHE;
}

static inline unsigned int hash_flow(uint32_t src_ip, uint32_t dst_ip,
                                      uint16_t src_port, uint16_t dst_port,
                                      uint8_t proto) {
    uint32_t h = src_ip ^ dst_ip ^ (src_port << 16 | dst_port) ^ proto;
    return (h ^ (h >> 16)) % MAX_FLOW_ENTRIES;
}

/* ================================================================
 * DNS Cache Management
 * ================================================================ */

void mapping_add_dns(const char *domain, uint32_t ip, int is_v6, const uint8_t *ip6)
{
    if (!domain || !*domain) return;

    pthread_mutex_lock(&dns_mutex);

    unsigned int idx = is_v6 ? 0 : hash_ip(ip);
    /* For IPv6, use first 4 bytes for hash */
    if (is_v6 && ip6) {
        uint32_t h;
        memcpy(&h, ip6, 4);
        idx = hash_ip(h);
    }

    struct dns_entry *entry = calloc(1, sizeof(*entry));
    if (!entry) {
        pthread_mutex_unlock(&dns_mutex);
        return;
    }

    if (is_v6 && ip6) {
        memcpy(entry->ip6, ip6, 16);
        entry->is_v6 = 1;
    } else {
        entry->ip4 = ip;
        entry->is_v6 = 0;
    }
    strncpy(entry->domain, domain, sizeof(entry->domain) - 1);
    entry->expires = time(NULL) + g_config.dns_timeout;
    entry->next = dns_hash[idx];
    dns_hash[idx] = entry;

    pthread_mutex_unlock(&dns_mutex);
}

const char *mapping_lookup_domain(uint32_t ip)
{
    pthread_mutex_lock(&dns_mutex);

    unsigned int idx = hash_ip(ip);
    struct dns_entry *entry = dns_hash[idx];
    time_t now = time(NULL);
    const char *result = NULL;

    while (entry) {
        if (!entry->is_v6 && entry->ip4 == ip && entry->expires > now) {
            result = entry->domain;
            break;
        }
        entry = entry->next;
    }

    pthread_mutex_unlock(&dns_mutex);
    return result;
}

void mapping_expire_dns(void)
{
    pthread_mutex_lock(&dns_mutex);

    time_t now = time(NULL);
    for (int i = 0; i < MAX_DNS_CACHE; i++) {
        struct dns_entry **prev = &dns_hash[i];
        struct dns_entry *entry = dns_hash[i];
        while (entry) {
            if (entry->expires < now) {
                *prev = entry->next;
                free(entry);
                entry = *prev;
            } else {
                prev = &entry->next;
                entry = entry->next;
            }
        }
    }

    pthread_mutex_unlock(&dns_mutex);
}

/* ================================================================
 * App Mapping Management
 * ================================================================ */

int mapping_load(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "Warning: Cannot open app mapping file: %s\n", path);
        return -1;
    }

    pthread_mutex_lock(&app_mutex);

    /* Free existing mappings */
    struct app_mapping *am = app_mappings;
    while (am) {
        struct app_mapping *next = am->next;
        free(am);
        am = next;
    }
    app_mappings = NULL;

    char line[1024];
    int count = 0;

    while (fgets(line, sizeof(line), fp)) {
        /* Trim newline */
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        /* Trim carriage return */
        nl = strchr(line, '\r');
        if (nl) *nl = '\0';

        /* Skip empty lines and comments */
        if (!line[0] || line[0] == '#') continue;

        /* Parse CSV-like: pattern,app_name[,category[,priority]] */
        char *pattern = strtok(line, ",");
        char *app_name = strtok(NULL, ",");
        char *category = strtok(NULL, ",");
        char *priority_str = strtok(NULL, ",");

        if (!pattern || !app_name) continue;

        /* Trim whitespace */
        while (*pattern == ' ' || *pattern == '\t') pattern++;
        while (*app_name == ' ' || *app_name == '\t') app_name++;

        struct app_mapping *entry = calloc(1, sizeof(*entry));
        if (!entry) continue;

        strncpy(entry->pattern, pattern, sizeof(entry->pattern) - 1);
        strncpy(entry->app_name, app_name, sizeof(entry->app_name) - 1);

        if (category) {
            while (*category == ' ' || *category == '\t') category++;
            strncpy(entry->category, category, sizeof(entry->category) - 1);
        } else {
            strcpy(entry->category, "General");
        }

        entry->priority = priority_str ? atoi(priority_str) : 0;

        /* Insert sorted by priority (higher first) */
        if (!app_mappings || entry->priority > app_mappings->priority) {
            entry->next = app_mappings;
            app_mappings = entry;
        } else {
            struct app_mapping *prev = app_mappings;
            while (prev->next && prev->next->priority >= entry->priority)
                prev = prev->next;
            entry->next = prev->next;
            prev->next = entry;
        }
        count++;
    }

    fclose(fp);
    pthread_mutex_unlock(&app_mutex);
    return count;
}

const char *mapping_lookup_app(const char *domain)
{
    if (!domain) return NULL;

    pthread_mutex_lock(&app_mutex);

    struct app_mapping *am = app_mappings;
    while (am) {
        if (fnmatch(am->pattern, domain, 0) == 0) {
            pthread_mutex_unlock(&app_mutex);
            return am->app_name;
        }
        am = am->next;
    }

    pthread_mutex_unlock(&app_mutex);

    /* Fallback: extract top-level domain as app name */
    const char *dot = strrchr(domain, '.');
    if (dot) {
        /* Find second-level domain */
        const char *prev = domain;
        const char *p = domain;
        while (p < dot) {
            if (*p == '.') prev = p + 1;
            p++;
        }
        /* prev now points to the SLD */
        static char fallback[128];
        strncpy(fallback, prev, sizeof(fallback) - 1);
        fallback[sizeof(fallback) - 1] = '\0';
        /* Capitalize first letter */
        if (fallback[0] >= 'a' && fallback[0] <= 'z')
            fallback[0] -= 32;
        return fallback;
    }

    return "Unknown";
}

const char *mapping_lookup_category(const char *domain)
{
    if (!domain) return "General";

    pthread_mutex_lock(&app_mutex);

    struct app_mapping *am = app_mappings;
    while (am) {
        if (fnmatch(am->pattern, domain, 0) == 0) {
            pthread_mutex_unlock(&app_mutex);
            return am->category;
        }
        am = am->next;
    }

    pthread_mutex_unlock(&app_mutex);
    return "General";
}

/* ================================================================
 * Packet Capture (DNS + TLS SNI)
 * ================================================================ */

#include <pcap.h>

static pcap_t *g_pcap = NULL;
static void (*g_dns_callback)(const char *domain, uint32_t ip, int is_v6, const uint8_t *ip6) = NULL;
static void (*g_sni_callback)(const char *domain, uint32_t ip) = NULL;

void capture_set_dns_callback(void (*cb)(const char *domain, uint32_t ip, int is_v6, const uint8_t *ip6)) {
    g_dns_callback = cb;
}

void capture_set_sni_callback(void (*cb)(const char *domain, uint32_t ip)) {
    g_sni_callback = cb;
}

/* Read a DNS domain name at offset, handling compression pointers.
 * Returns the byte position after the name (after 0x00 or compression pointer).
 * On error returns -1. */
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
            /* End of domain name */
            if (!jumped) return_pos = pos + 1;
            break;
        }

        if ((label_len & 0xC0) == 0xC0) {
            /* Compression pointer: top 2 bits = 11, rest 14 bits = offset */
            if (pos + 2 > data_len) return -1;
            uint16_t ptr = ((label_len & 0x3F) << 8) | data[pos + 1];
            if (ptr >= (uint16_t)data_len) return -1;
            if (!jumped) {
                return_pos = pos + 2; /* return position after the 2-byte pointer */
                jumped = 1;
            }
            pos = ptr;
            continue;
        }

        /* Regular label: length byte + label characters */
        if (label_len > 63 || pos + 1 + label_len > data_len) return -1;

        /* Add dot separator between labels */
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

/* Parse DNS query packet to extract the domain name being queried.
 * DNS queries come from clients TO port 53. */
static void parse_dns_query(const uint8_t *data, int len)
{
    if (len < 12) return;

    uint16_t flags = (data[2] << 8) | data[3];
    uint16_t qdcount = (data[4] << 8) | data[5];

    /* Only process queries (QR=0) with at least one question */
    if ((flags & 0x8000) || qdcount == 0) return;

    int pos = 12;
    char domain_buf[256];

    /* Parse first question to get domain name */
    for (int i = 0; i < qdcount && pos < len; i++) {
        pos = dns_read_name(data, len, pos, domain_buf, sizeof(domain_buf));
        if (pos < 0 || pos + 4 > len) break;
        pos += 4; /* QTYPE(2) + QCLASS(2) */
    }

    if (domain_buf[0]) {
        /* DNS queries contain domain names but no IP addresses.
         * We don't store them directly (need transaction ID matching
         * with the response to get the IP), but the domain extraction
         * logic is available for future enhancements. */
    }
}

/* Parse DNS response packet to extract A/AAAA records */
static void parse_dns_response(const uint8_t *data, int len)
{
    if (len < 12) return;

    /* DNS header: ID(2) Flags(2) QDCOUNT(2) ANCOUNT(2) NSCOUNT(2) ARCOUNT(2) */
    uint16_t flags = (data[2] << 8) | data[3];
    uint16_t qdcount = (data[4] << 8) | data[5];
    uint16_t ancount = (data[6] << 8) | data[7];

    /* Only process responses (QR=1) with answers */
    if (!(flags & 0x8000) || ancount == 0) return;

    int pos = 12;
    char qname_buf[256] = "";

    /* Extract domain name from question section before skipping it */
    for (int i = 0; i < qdcount && pos < len; i++) {
        int new_pos = dns_read_name(data, len, pos, qname_buf, sizeof(qname_buf));
        if (new_pos < 0) break;
        pos = new_pos;
        if (pos + 4 > len) break;
        pos += 4; /* QTYPE(2) + QCLASS(2) */
    }

    /* Parse answer section */
    char domain_buf[256];

    for (int i = 0; i < ancount && pos < len; i++) {
        if (pos + 10 > len) break;

        /* Read answer NAME (may be compressed) */
        int new_pos = dns_read_name(data, len, pos, domain_buf, sizeof(domain_buf));
        if (new_pos < 0) break;
        pos = new_pos;

        /* If compressed name resolved to empty, use question domain as fallback */
        if (!domain_buf[0] && qname_buf[0]) {
            strncpy(domain_buf, qname_buf, sizeof(domain_buf) - 1);
        }

        uint16_t atype = (data[pos] << 8) | data[pos+1];
        /* uint16_t aclass = (data[pos+2] << 8) | data[pos+3]; */
        /* uint32_t ttl = (data[pos+4] << 24) | ...; */
        uint16_t rdlength = (data[pos+8] << 8) | data[pos+9];
        pos += 10;

        if (pos + rdlength > len) break;

        /* A record */
        if (atype == DNS_TYPE_A && rdlength == 4 && g_dns_callback) {
            uint32_t ip;
            memcpy(&ip, data + pos, 4);
            g_dns_callback(domain_buf[0] ? domain_buf : qname_buf, ip, 0, NULL);
        }
        /* AAAA record */
        else if (atype == DNS_TYPE_AAAA && rdlength == 16 && g_dns_callback) {
            g_dns_callback(domain_buf[0] ? domain_buf : qname_buf, 0, 1, data + pos);
        }

        pos += rdlength;
    }
}

/* Extract TLS SNI from ClientHello packet */
static void parse_tls_sni(const uint8_t *data, int len, uint32_t dst_ip)
{
    /* TLS record: ContentType(1) Version(2) Length(2) */
    if (len < 5) return;

    uint8_t content_type = data[0];
    uint16_t version = (data[1] << 8) | data[2];
    uint16_t record_len = (data[3] << 8) | data[4];

    if (content_type != 22) return; /* Handshake */
    if (version < 0x0301) return;   /* SSL 3.0+ */

    int pos = 5;
    if (pos + record_len > len) return;

    /* Handshake: Type(1) Length(3) */
    if (pos + 4 > len) return;
    uint8_t hs_type = data[pos];
    uint32_t hs_len = (data[pos+1] << 16) | (data[pos+2] << 8) | data[pos+3];
    pos += 4;

    if (hs_type != 1) return; /* ClientHello */
    if (pos + hs_len > len) return;

    /* ClientHello: Version(2) Random(32) SessionID... */
    if (pos + 38 > len) return;
    pos += 2; /* Skip legacy_version */
    pos += 32; /* Skip random */

    /* Session ID */
    uint8_t sid_len = data[pos];
    pos += 1 + sid_len;
    if (pos + 2 > len) return;

    /* Cipher Suites */
    uint16_t cs_len = (data[pos] << 8) | data[pos+1];
    pos += 2 + cs_len;
    if (pos + 1 > len) return;

    /* Compression Methods */
    uint8_t cm_len = data[pos];
    pos += 1 + cm_len;
    if (pos + 2 > len) return;

    /* Extensions */
    uint16_t ext_len = (data[pos] << 8) | data[pos+1];
    pos += 2;
    int ext_end = pos + ext_len;
    if (ext_end > len) ext_end = len;

    while (pos + 4 <= ext_end) {
        uint16_t ext_type = (data[pos] << 8) | data[pos+1];
        uint16_t ext_data_len = (data[pos+2] << 8) | data[pos+3];
        pos += 4;

        if (ext_type == 0x0000 && pos + ext_data_len <= ext_end) {
            /* SNI extension - server_name */
            if (pos + 5 > ext_end) break;
            /* Skip server_name_list length (2 bytes) */
            /* uint16_t sn_len = (data[pos] << 8) | data[pos+1]; */
            pos += 2;
            if (pos + 3 > ext_end) break;
            /* Name type (1 byte) */
            uint8_t name_type = data[pos];
            uint16_t name_len = (data[pos+1] << 8) | data[pos+2];
            pos += 3;
            if (name_type == 0 && pos + name_len <= ext_end && name_len > 0) {
                char sni_host[256];
                int copy_len = name_len < (int)sizeof(sni_host) - 1 ? name_len : (int)sizeof(sni_host) - 1;
                memcpy(sni_host, data + pos, copy_len);
                sni_host[copy_len] = '\0';

                /* Only accept valid-looking hostnames */
                if (strchr(sni_host, '.') && g_sni_callback) {
                    g_sni_callback(sni_host, dst_ip);
                }
                break;
            }
            break;
        }
        pos += ext_data_len;
    }
}

static void pcap_callback(u_char *user, const struct pcap_pkthdr *hdr, const u_char *bytes)
{
    (void)user;
    if (hdr->len < 14) return;

    /* Ethernet header: dst(6) src(6) type(2) */
    uint16_t ether_type = (bytes[12] << 8) | bytes[13];

    const uint8_t *ip_data;
    int ip_len = hdr->len - 14;

    if (ether_type == 0x0800) {
        /* IPv4 */
        ip_data = bytes + 14;
    } else if (ether_type == 0x8100) {
        /* VLAN tag - skip 4 more bytes */
        ether_type = (bytes[16] << 8) | bytes[17];
        if (ether_type == 0x0800) {
            ip_data = bytes + 18;
            ip_len -= 4;
        } else {
            return;
        }
    } else {
        return;
    }

    if (ip_len < 20) return;

    uint8_t ihl = ip_data[0] & 0x0F;
    uint8_t protocol = ip_data[9];
    int ip_hdr_len = ihl * 4;

    if (ip_hdr_len < 20 || ip_hdr_len > ip_len) return;

    uint32_t src_ip, dst_ip;
    memcpy(&src_ip, ip_data + 12, 4);
    memcpy(&dst_ip, ip_data + 16, 4);

    const uint8_t *transport = ip_data + ip_hdr_len;
    int transport_len = ip_len - ip_hdr_len;

    if (protocol == IPPROTO_UDP && transport_len >= 8) {
        uint16_t src_port = (transport[0] << 8) | transport[1];
        uint16_t dst_port = (transport[2] << 8) | transport[3];
        int udp_len = (transport[4] << 8) | transport[5];

        /* DNS traffic - capture queries (client → port 53) */
        if (dst_port == 53 && udp_len >= 8) {
            parse_dns_query(transport + 8, transport_len - 8);
        }
        /* DNS traffic - capture responses (port 53 → client) */
        if (src_port == 53 && udp_len >= 8) {
            parse_dns_response(transport + 8, transport_len - 8);
        }
    } else if (protocol == IPPROTO_TCP && transport_len >= 20) {
        uint16_t dst_port = (transport[2] << 8) | transport[3];
        uint8_t data_offset = (transport[12] >> 4) & 0x0F;
        int tcp_hdr_len = data_offset * 4;

        if (tcp_hdr_len > transport_len) return;

        const uint8_t *tcp_data = transport + tcp_hdr_len;
        int tcp_data_len = transport_len - tcp_hdr_len;

        /* Capture TLS SNI from ClientHello (port 443) */
        if ((dst_port == 443 || dst_port == 8443) && tcp_data_len > 0) {
            /* Only the first packet (SYN has no data, ClientHello is first data packet) */
            uint8_t tcp_flags = transport[13];
            if (!(tcp_flags & 0x02)) { /* Not SYN */
                parse_tls_sni(tcp_data, tcp_data_len, dst_ip);
            }
        }

        /* HTTP Host header capture (port 80) */
        if ((dst_port == 80 || dst_port == 8080) && tcp_data_len > 10) {
            /* Check for HTTP request with Host header */
            if (strncasecmp((const char *)tcp_data, "GET ", 4) == 0 ||
                strncasecmp((const char *)tcp_data, "POST ", 5) == 0 ||
                strncasecmp((const char *)tcp_data, "HEAD ", 5) == 0) {

                /* Search for Host: header in the data */
                const char *host_start = strcasestr((const char *)tcp_data, "\r\nHost: ");
                if (host_start) {
                    host_start += 8; /* Skip "\r\nHost: " */
                    const char *host_end = strstr(host_start, "\r\n");
                    if (host_end) {
                        int host_len = host_end - host_start;
                        char host[256];
                        if (host_len >= (int)sizeof(host)) host_len = (int)sizeof(host) - 1;
                        memcpy(host, host_start, host_len);
                        host[host_len] = '\0';

                        /* Strip port from host if present */
                        char *port_colon = strrchr(host, ':');
                        if (port_colon) *port_colon = '\0';

                        if (strchr(host, '.') && g_sni_callback) {
                            g_sni_callback(host, dst_ip);
                        }
                    }
                }
            }
        }
    }
}

int capture_init(const char *iface)
{
    char errbuf[PCAP_ERRBUF_SIZE];
    int snaplen = 2048;
    int promisc = 1;
    int timeout_ms = 500;

    /* Build filter for DNS + HTTP + TLS */
    const char *filter =
        "port 53 or "
        "port 80 or port 8080 or "
        "port 443 or port 8443";

    const char *dev = iface && iface[0] ? iface : "any";

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

/* ================================================================
 * Conntrack Flow Reading
 * ================================================================ */

int conntrack_init(void)
{
    /* We'll read from /proc/net/nf_conntrack for simplicity */
    return 0;
}

void conntrack_update(void)
{
    FILE *fp = fopen("/proc/net/nf_conntrack", "r");
    if (!fp) {
        /* Try alternate path */
        fp = fopen("/proc/net/ip_conntrack", "r");
        if (!fp) return;
    }

    pthread_mutex_lock(&flow_mutex);

    char line[1024];
    time_t now = time(NULL);

    while (fgets(line, sizeof(line), fp)) {
        uint32_t src_ip = 0, dst_ip = 0;
        uint16_t src_port = 0, dst_port = 0;
        uint8_t protocol = IPPROTO_TCP;
        uint64_t rx_bytes = 0, tx_bytes = 0;
        uint64_t rx_packets = 0, tx_packets = 0;

        /* Parse line like:
         * ipv4 2 tcp 6 300 ESTABLISHED src=192.168.1.100 dst=8.8.8.8
         * sport=12345 dport=443 packets=100 bytes=5000
         * src=8.8.8.8 dst=192.168.1.100 sport=443 dport=12345
         * packets=80 bytes=32000 [ASSURED] mark=0 zone=0 use=2
         *
         * The line has TWO directions: original (LAN→WAN) then reply (WAN→LAN).
         * We use occurrence counters to distinguish them:
         *   1st src/dst/sport/dport → original direction
         *   1st packets/bytes → rx (download)
         *   2nd src/dst/sport/dport → reply direction (ignored for IP/port)
         *   2nd packets/bytes → tx (upload)
         */

        if (strncmp(line, "ipv4", 4) != 0) continue;

        /* Occurrence counters for direction tracking */
        int src_cnt = 0, dst_cnt = 0, sport_cnt = 0, dport_cnt = 0;
        int pkt_cnt = 0, byte_cnt = 0;

        char *tok = strtok(line, " \t");
        int field = 0;

        while (tok) {
            if (field == 4) {
                /* Transport protocol */
                if (strcmp(tok, "tcp") == 0) protocol = IPPROTO_TCP;
                else if (strcmp(tok, "udp") == 0) protocol = IPPROTO_UDP;
                else protocol = atoi(tok);
            }

            /* Parse key=value pairs */
            char *eq = strchr(tok, '=');
            if (eq) {
                *eq = '\0';
                char *key = tok;
                char *val = eq + 1;

                if (strcmp(key, "src") == 0) {
                    src_cnt++;
                    if (src_cnt == 1)
                        inet_pton(AF_INET, val, &src_ip);
                } else if (strcmp(key, "dst") == 0) {
                    dst_cnt++;
                    if (dst_cnt == 1)
                        inet_pton(AF_INET, val, &dst_ip);
                } else if (strcmp(key, "sport") == 0) {
                    sport_cnt++;
                    if (sport_cnt == 1)
                        src_port = atoi(val);
                } else if (strcmp(key, "dport") == 0) {
                    dport_cnt++;
                    if (dport_cnt == 1)
                        dst_port = atoi(val);
                } else if (strcmp(key, "packets") == 0) {
                    pkt_cnt++;
                    if (pkt_cnt == 1)
                        tx_packets = strtoull(val, NULL, 10);
                    else
                        rx_packets = strtoull(val, NULL, 10);
                } else if (strcmp(key, "bytes") == 0) {
                    byte_cnt++;
                    if (byte_cnt == 1)
                        tx_bytes = strtoull(val, NULL, 10);
                    else
                        rx_bytes = strtoull(val, NULL, 10);
                }

                *eq = '='; /* restore */
            }

            tok = strtok(NULL, " \t\n");
            field++;
        }

        if (src_ip == 0 || dst_ip == 0) continue;
        if (rx_bytes == 0 && tx_bytes == 0) continue;

        /* Find or create flow entry */
        unsigned int idx = hash_flow(src_ip, dst_ip, src_port, dst_port, protocol);
        struct flow_entry *flow = flow_hash[idx];
        int found = 0;

        while (flow) {
            if (flow->src_ip == src_ip && flow->dst_ip == dst_ip &&
                flow->src_port == src_port && flow->dst_port == dst_port &&
                flow->protocol == protocol) {
                found = 1;
                break;
            }
            flow = flow->next;
        }

        if (!found) {
            flow = calloc(1, sizeof(*flow));
            if (!flow) continue;
            flow->src_ip = src_ip;
            flow->dst_ip = dst_ip;
            flow->src_port = src_port;
            flow->dst_port = dst_port;
            flow->protocol = protocol;
            flow->first_seen = now;
            flow->next = flow_hash[idx];
            flow_hash[idx] = flow;
        }

        /* Compute delta since last conntrack read.
         * conntrack counters are CUMULATIVE (total bytes since connection start).
         * We compute the increment and accumulate it into the flow entry.
         * On database commit, the accumulated delta is stored and reset to 0.
         * This prevents double-counting in SUM queries. */
        if (found && flow->prev_rx_bytes > 0) {
            int64_t drx, dtx, drxp, dtxp;
            drx  = (int64_t)rx_bytes  - (int64_t)flow->prev_rx_bytes;
            dtx  = (int64_t)tx_bytes  - (int64_t)flow->prev_tx_bytes;
            drxp = (int64_t)rx_packets - (int64_t)flow->prev_rx_packets;
            dtxp = (int64_t)tx_packets - (int64_t)flow->prev_tx_packets;

            /* Handle conntrack counter reset (connection reused) */
            if (drx > 0)  { flow->rx_bytes  += (uint64_t)drx;  flow->dirty = 1; }
            if (dtx > 0)  { flow->tx_bytes  += (uint64_t)dtx;  flow->dirty = 1; }
            if (drxp > 0) { flow->rx_packets += (uint64_t)drxp; flow->dirty = 1; }
            if (dtxp > 0) { flow->tx_packets += (uint64_t)dtxp; flow->dirty = 1; }
        } else {
            /* First time seeing this flow: store current cumulative as initial delta */
            flow->rx_bytes  += rx_bytes;
            flow->tx_bytes  += tx_bytes;
            flow->rx_packets += rx_packets;
            flow->tx_packets += tx_packets;
            if (rx_bytes || tx_bytes) flow->dirty = 1;
        }

        /* Save current cumulative values for next delta calculation */
        flow->prev_rx_bytes  = rx_bytes;
        flow->prev_tx_bytes  = tx_bytes;
        flow->prev_rx_packets = rx_packets;
        flow->prev_tx_packets = tx_packets;
        flow->last_seen = now;
    }

    fclose(fp);

    /* Expire old flows */
    time_t cutoff = now - g_config.flow_timeout;
    for (int i = 0; i < MAX_FLOW_ENTRIES; i++) {
        struct flow_entry **prev = &flow_hash[i];
        struct flow_entry *flow = flow_hash[i];
        while (flow) {
            if (flow->last_seen < cutoff) {
                *prev = flow->next;
                free(flow);
                flow = *prev;
            } else {
                prev = &flow->next;
                flow = flow->next;
            }
        }
    }

    pthread_mutex_unlock(&flow_mutex);
}

void conntrack_foreach_flow(void (*cb)(struct flow_entry *flow, void *user), void *user)
{
    pthread_mutex_lock(&flow_mutex);
    for (int i = 0; i < MAX_FLOW_ENTRIES; i++) {
        struct flow_entry *flow = flow_hash[i];
        while (flow) {
            cb(flow, user);
            flow = flow->next;
        }
    }
    pthread_mutex_unlock(&flow_mutex);
}

/* ================================================================
 * SQLite Database
 * ================================================================ */

#include <sqlite3.h>

struct db_handle {
    sqlite3 *conn;
};

struct db_handle *database_open(const char *path)
{
    struct db_handle *db = calloc(1, sizeof(*db));
    if (!db) return NULL;

    /* Open database file (init script ensures directory exists) */
    char db_file[512];
    snprintf(db_file, sizeof(db_file), "%s/traffic.db", path);

    int rc = sqlite3_open(db_file, &db->conn);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db->conn));
        sqlite3_close(db->conn);
        free(db);
        return NULL;
    }

    /* Enable WAL mode for better concurrent access */
    sqlite3_exec(db->conn, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(db->conn, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);

    /* Create tables */
    const char *create_sql =
        "CREATE TABLE IF NOT EXISTS traffic ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  timestamp INTEGER NOT NULL,"
        "  src_ip TEXT NOT NULL,"
        "  dst_ip TEXT NOT NULL,"
        "  src_port INTEGER,"
        "  dst_port INTEGER,"
        "  protocol TEXT,"
        "  domain TEXT,"
        "  app_name TEXT,"
        "  app_category TEXT,"
        "  rx_bytes INTEGER DEFAULT 0,"
        "  tx_bytes INTEGER DEFAULT 0,"
        "  rx_packets INTEGER DEFAULT 0,"
        "  tx_packets INTEGER DEFAULT 0"
        ")"
    ;
    sqlite3_exec(db->conn, create_sql, NULL, NULL, NULL);

    /* Create indices */
    sqlite3_exec(db->conn,
        "CREATE INDEX IF NOT EXISTS idx_traffic_timestamp ON traffic(timestamp)",
        NULL, NULL, NULL);
    sqlite3_exec(db->conn,
        "CREATE INDEX IF NOT EXISTS idx_traffic_app ON traffic(app_name)",
        NULL, NULL, NULL);
    sqlite3_exec(db->conn,
        "CREATE INDEX IF NOT EXISTS idx_traffic_domain ON traffic(domain)",
        NULL, NULL, NULL);

    return db;
}

void database_close(struct db_handle *db)
{
    if (!db) return;
    if (db->conn) {
        sqlite3_exec(db->conn, "PRAGMA optimize", NULL, NULL, NULL);
        sqlite3_close(db->conn);
    }
    free(db);
}

int database_store_flow(struct db_handle *db, struct flow_entry *flow,
                         const char *app, const char *domain)
{
    if (!db || !db->conn || !flow) return -1;

    char src_ip[64], dst_ip[64];
    inet_ntop(AF_INET, &flow->src_ip, src_ip, sizeof(src_ip));
    inet_ntop(AF_INET, &flow->dst_ip, dst_ip, sizeof(dst_ip));

    const char *proto_str = "tcp";
    if (flow->protocol == IPPROTO_UDP) proto_str = "udp";
    else if (flow->protocol == IPPROTO_ICMP) proto_str = "icmp";

    const char *sql =
        "INSERT INTO traffic (timestamp, src_ip, dst_ip, src_port, dst_port,"
        "  protocol, domain, app_name, app_category, rx_bytes, tx_bytes,"
        "  rx_packets, tx_packets)"
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL prepare error: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)flow->last_seen);
    sqlite3_bind_text(stmt, 2, src_ip, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, dst_ip, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, flow->src_port);
    sqlite3_bind_int(stmt, 5, flow->dst_port);
    sqlite3_bind_text(stmt, 6, proto_str, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 7, domain ? domain : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 8, app ? app : "Unknown", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 9, domain ? mapping_lookup_category(domain) : "General", -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 10, (sqlite3_int64)flow->rx_bytes);
    sqlite3_bind_int64(stmt, 11, (sqlite3_int64)flow->tx_bytes);
    sqlite3_bind_int64(stmt, 12, (sqlite3_int64)flow->rx_packets);
    sqlite3_bind_int64(stmt, 13, (sqlite3_int64)flow->tx_packets);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        fprintf(stderr, "SQL insert error: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }

    return 0;
}

int database_commit(struct db_handle *db)
{
    /* WAL mode handles this automatically, but we can checkpoint */
    if (db && db->conn) {
        sqlite3_wal_checkpoint(db->conn, NULL);
    }
    return 0;
}

struct traffic_stat *database_query(struct db_handle *db, const char *group_by,
                                     const char *period)
{
    if (!db || !db->conn) return NULL;

    char sql[1024];
    const char *group_col;

    if (strcmp(group_by, "app") == 0) {
        group_col = "app_name";
    } else if (strcmp(group_by, "domain") == 0) {
        group_col = "domain";
    } else if (strcmp(group_by, "host") == 0 || strcmp(group_by, "mac") == 0) {
        group_col = "src_ip";
    } else if (strcmp(group_by, "host_app") == 0) {
        /* Per-device app breakdown: group by (src_ip, app_name) */
        group_col = "src_ip";
    } else if (strcmp(group_by, "category") == 0) {
        group_col = "app_category";
    } else {
        group_col = "app_name";
    }

    /* Build time filter based on period */
    char time_filter[128] = "";
    if (period && period[0]) {
        if (strcmp(period, "today") == 0) {
            snprintf(time_filter, sizeof(time_filter),
                "WHERE timestamp >= strftime('%%s', 'now', 'start of day')");
        } else if (strcmp(period, "yesterday") == 0) {
            snprintf(time_filter, sizeof(time_filter),
                "WHERE timestamp >= strftime('%%s', 'now', 'start of day', '-1 day') "
                "AND timestamp < strftime('%%s', 'now', 'start of day')");
        } else if (strcmp(period, "week") == 0) {
            snprintf(time_filter, sizeof(time_filter),
                "WHERE timestamp >= strftime('%%s', 'now', 'start of day', '-7 days')");
        } else if (strcmp(period, "month") == 0) {
            snprintf(time_filter, sizeof(time_filter),
                "WHERE timestamp >= strftime('%%s', 'now', 'start of day', '-30 days')");
        } else {
            /* Try to parse as number of seconds */
            time_t secs = atol(period);
            if (secs > 0) {
                snprintf(time_filter, sizeof(time_filter),
                    "WHERE timestamp >= %ld", (long)(time(NULL) - secs));
            }
        }
    }

    /* Build SELECT with extra app_name/app_category columns */
    char extra_cols[256] = "";
    int has_extra = 0;
    int col_app_name = -1, col_app_cat = -1;

    if (strcmp(group_by, "app") == 0) {
        /* group_key is app_name, extra: app_category */
        snprintf(extra_cols, sizeof(extra_cols),
            ", MAX(app_category) as app_category");
        col_app_cat = 1;
        has_extra = 1;
    } else if (strcmp(group_by, "domain") == 0) {
        /* group_key is domain, extra: app_name, app_category */
        snprintf(extra_cols, sizeof(extra_cols),
            ", MAX(app_name) as app_name, MAX(app_category) as app_category");
        col_app_name = 1;
        col_app_cat = 2;
        has_extra = 2;
    } else if (strcmp(group_by, "host") == 0 || strcmp(group_by, "mac") == 0) {
        /* group_key is src_ip, extra: app_name, app_category */
        snprintf(extra_cols, sizeof(extra_cols),
            ", MAX(app_name) as app_name, MAX(app_category) as app_category");
        col_app_name = 1;
        col_app_cat = 2;
        has_extra = 2;
    } else if (strcmp(group_by, "host_app") == 0) {
        /* group_key is src_ip, extra: app_name and app_category */
        snprintf(extra_cols, sizeof(extra_cols),
            ", app_name as app_name, MAX(app_category) as app_category");
        col_app_name = 1;
        col_app_cat = 2;
        has_extra = 2;
    } else if (strcmp(group_by, "category") == 0) {
        /* group_key is app_category, no extra needed */
        extra_cols[0] = '\0';
        has_extra = 0;
    } else {
        /* default: treat as app */
        snprintf(extra_cols, sizeof(extra_cols),
            ", MAX(app_category) as app_category");
        col_app_cat = 1;
        has_extra = 1;
    }

    if (strcmp(group_by, "host_app") == 0) {
        /* For host_app: GROUP BY both src_ip and app_name.
         * ORDER BY src_ip first so frontend can group consecutive rows. */
        snprintf(sql, sizeof(sql),
            "SELECT %s as group_key%s,"
            "  SUM(rx_bytes) as total_rx,"
            "  SUM(tx_bytes) as total_tx,"
            "  SUM(rx_packets) as total_rx_pkts,"
            "  SUM(tx_packets) as total_tx_pkts,"
            "  COUNT(*) as conn_count "
            "FROM traffic %s "
            "GROUP BY %s, app_name "
            "ORDER BY %s, total_rx + total_tx DESC "
            "LIMIT 250",
            group_col, extra_cols, time_filter, group_col, group_col);
    } else {
        snprintf(sql, sizeof(sql),
            "SELECT %s as group_key%s,"
            "  SUM(rx_bytes) as total_rx,"
            "  SUM(tx_bytes) as total_tx,"
            "  SUM(rx_packets) as total_rx_pkts,"
            "  SUM(tx_packets) as total_tx_pkts,"
            "  COUNT(*) as conn_count "
            "FROM traffic %s "
            "GROUP BY group_key "
            "ORDER BY total_rx + total_tx DESC "
            "LIMIT 200",
            group_col, extra_cols, time_filter);
    }

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Query error: %s\n", sqlite3_errmsg(db->conn));
        return NULL;
    }

    int col_offset = 1 + has_extra; /* columns after group_key and extra */

    struct traffic_stat *head = NULL, *tail = NULL;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        struct traffic_stat *stat = calloc(1, sizeof(*stat));
        if (!stat) continue;

        const char *key = (const char *)sqlite3_column_text(stmt, 0);
        if (key) strncpy(stat->key, key, sizeof(stat->key) - 1);

        /* Populate app_name from group_key or extra column */
        if (strcmp(group_by, "app") == 0) {
            /* For app grouping, key IS the app_name */
            if (key) strncpy(stat->app_name, key, sizeof(stat->app_name) - 1);
        } else if (strcmp(group_by, "host_app") == 0) {
            /* For host_app, app_name comes from the grouping column directly */
            const char *an = (const char *)sqlite3_column_text(stmt, col_app_name);
            if (an) strncpy(stat->app_name, an, sizeof(stat->app_name) - 1);
        } else if (col_app_name >= 1) {
            const char *an = (const char *)sqlite3_column_text(stmt, col_app_name);
            if (an) strncpy(stat->app_name, an, sizeof(stat->app_name) - 1);
        }

        /* Populate app_category from extra column or group_key */
        if (strcmp(group_by, "category") == 0) {
            /* For category grouping, key IS the category */
            if (key) strncpy(stat->app_category, key, sizeof(stat->app_category) - 1);
        } else if (col_app_cat >= 1) {
            const char *ac = (const char *)sqlite3_column_text(stmt, col_app_cat);
            if (ac) strncpy(stat->app_category, ac, sizeof(stat->app_category) - 1);
        }

        stat->rx_bytes = sqlite3_column_int64(stmt, col_offset);
        stat->tx_bytes = sqlite3_column_int64(stmt, col_offset + 1);
        stat->rx_packets = sqlite3_column_int64(stmt, col_offset + 2);
        stat->tx_packets = sqlite3_column_int64(stmt, col_offset + 3);
        stat->connections = sqlite3_column_int(stmt, col_offset + 4);

        if (!head) {
            head = tail = stat;
        } else {
            tail->next = stat;
            tail = stat;
        }
    }

    sqlite3_finalize(stmt);
    return head;
}

void database_free_stats(struct traffic_stat *stats)
{
    while (stats) {
        struct traffic_stat *next = stats->next;
        free(stats);
        stats = next;
    }
}

/* ================================================================
 * Output Functions
 * ================================================================ */

void output_json(struct traffic_stat *stats, const char *group_by)
{
    /* Map group_by to a consistent JSON key field name */
    const char *json_key;
    if (strcmp(group_by, "app") == 0) {
        json_key = "app_name";
    } else if (strcmp(group_by, "host") == 0 || strcmp(group_by, "mac") == 0) {
        json_key = "src_ip";
    } else if (strcmp(group_by, "host_app") == 0) {
        json_key = "src_ip";
    } else if (strcmp(group_by, "category") == 0) {
        json_key = "app_category";
    } else {
        json_key = group_by; /* "domain", etc. */
    }

    printf("{\n");
    printf("  \"group_by\": \"%s\",\n", group_by);
    printf("  \"timestamp\": %ld,\n", (long)time(NULL));
    printf("  \"entries\": [\n");

    int first = 1;
    struct traffic_stat *s = stats;
    while (s) {
        if (!first) printf(",\n");
        first = 0;

        printf("    {\n");
        /* Primary key field with consistent name */
        printf("      \"%s\": \"%s\",\n", json_key, s->key);
        /* Output app_name if it's not already the group key */
        if (s->app_name[0] && strcmp(group_by, "app") != 0)
            printf("      \"app_name\": \"%s\",\n", s->app_name);
        /* Output app_category if it's not already the group key */
        if (s->app_category[0] && strcmp(group_by, "category") != 0)
            printf("      \"app_category\": \"%s\",\n", s->app_category);
        printf("      \"rx_bytes\": %llu,\n", (unsigned long long)s->rx_bytes);
        printf("      \"tx_bytes\": %llu,\n", (unsigned long long)s->tx_bytes);
        printf("      \"rx_packets\": %llu,\n", (unsigned long long)s->rx_packets);
        printf("      \"tx_packets\": %llu,\n", (unsigned long long)s->tx_packets);
        printf("      \"connections\": %u,\n", s->connections);
        printf("      \"total_bytes\": %llu\n", (unsigned long long)(s->rx_bytes + s->tx_bytes));
        printf("    }");

        s = s->next;
    }

    printf("\n  ]\n}\n");
}

void output_csv(struct traffic_stat *stats, const char *group_by, const char *delim)
{
    if (!delim) delim = ",";

    /* Map group_by to consistent key name (same as JSON) */
    const char *json_key;
    if (strcmp(group_by, "app") == 0) {
        json_key = "app_name";
    } else if (strcmp(group_by, "host") == 0 || strcmp(group_by, "mac") == 0) {
        json_key = "src_ip";
    } else if (strcmp(group_by, "category") == 0) {
        json_key = "app_category";
    } else {
        json_key = group_by;
    }

    /* Header: key, app_name, app_category, rx_bytes, tx_bytes, rx_packets, tx_packets, connections */
    printf("%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s\n",
        json_key, delim,
        "app_name", delim,
        "app_category", delim,
        "rx_bytes", delim,
        "tx_bytes", delim,
        "rx_packets", delim,
        "tx_packets", delim,
        "connections");

    /* Data rows */
    struct traffic_stat *s = stats;
    while (s) {
        printf("%s%s%s%s%s%s%llu%s%llu%s%llu%s%llu%s%u\n",
            s->key, delim,
            s->app_name[0] ? s->app_name : "", delim,
            s->app_category[0] ? s->app_category : "", delim,
            (unsigned long long)s->rx_bytes, delim,
            (unsigned long long)s->tx_bytes, delim,
            (unsigned long long)s->rx_packets, delim,
            (unsigned long long)s->tx_packets, delim,
            s->connections);
        s = s->next;
    }
}

/* ================================================================
 * DNS / SNI Callbacks for Capture
 * ================================================================ */

static void on_dns_response(const char *domain, uint32_t ip, int is_v6, const uint8_t *ip6)
{
    if (domain && ip != 0) {
        mapping_add_dns(domain, ip, is_v6, ip6);
    }
}

static void on_sni_hostname(const char *domain, uint32_t ip)
{
    if (domain && ip != 0) {
        mapping_add_dns(domain, ip, 0, NULL);
    }
}

/* ================================================================
 * Flow processing callback for database storage
 * ================================================================ */

struct store_ctx {
    struct db_handle *db;
};

static void flow_to_db_cb(struct flow_entry *flow, void *user)
{
    struct store_ctx *ctx = (struct store_ctx *)user;

    /* Skip flows with no new data since last commit */
    if (!flow->dirty || (flow->rx_bytes == 0 && flow->tx_bytes == 0))
        return;

    /* Try to identify the domain for this flow */
    const char *domain = mapping_lookup_domain(flow->dst_ip);
    if (!domain) {
        domain = mapping_lookup_domain(flow->src_ip);
    }

    /* Fallback: use destination IP as string when domain can't be resolved */
    char ip_fallback[64];
    if (!domain) {
        inet_ntop(AF_INET, &flow->dst_ip, ip_fallback, sizeof(ip_fallback));
        domain = ip_fallback;
    }

    /* Map domain to app name */
    const char *app = "Unknown";
    if (domain == ip_fallback) {
        /* Domain is an IP address — can't match against app patterns */
        app = "Unknown";
    } else {
        app = mapping_lookup_app(domain);
    }

    database_store_flow(ctx->db, flow, app, domain);

    /* Reset accumulated deltas after storing */
    flow->rx_bytes = 0;
    flow->tx_bytes = 0;
    flow->rx_packets = 0;
    flow->tx_packets = 0;
    flow->dirty = 0;
}

/* ================================================================
 * Daemon Mode
 * ================================================================ */

static void daemon_loop(void)
{
    time_t last_commit = 0;
    time_t last_expire = 0;
    time_t last_conntrack = 0;

    while (g_running) {
        time_t now = time(NULL);

        /* Update conntrack every 5 seconds */
        if (now - last_conntrack >= 5) {
            conntrack_update();
            last_conntrack = now;
        }

        /* Store flows to database and commit */
        if (now - last_commit >= g_config.commit_interval) {
            struct store_ctx ctx = { g_db, 0 };
            conntrack_foreach_flow(flow_to_db_cb, &ctx);
            database_commit(g_db);
            last_commit = now;
        }

        /* Expire old DNS entries periodically */
        if (now - last_expire >= 300) {
            mapping_expire_dns();
            last_expire = now;
        }

        sleep(5);
    }
}

/* ================================================================
 * Signal Handler
 * ================================================================ */

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ================================================================
 * Main Entry Point
 * ================================================================ */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "App Traffic Analyzer - Identify apps and websites from network traffic\n"
        "\n"
        "Modes:\n"
        "  -d, --daemon          Run as background daemon\n"
        "  -q, --query           Query stored traffic data (default)\n"
        "\n"
        "Query Options:\n"
        "  -c, --output FORMAT   Output format: json (default) or csv\n"
        "  -s, --separator CHAR  CSV field separator (default: comma)\n"
        "  -g, --group-by FIELD  Group by: app, domain, host, category\n"
        "  -t, --period PERIOD   Time period: today, week, month, or seconds\n"
        "\n"
        "Daemon Options:\n"
        "  -i, --interface IF    Interface to capture (default: br-lan)\n"
        "  -D, --database PATH   Database storage path (default: %s)\n"
        "  -m, --map-file PATH   App mapping file path (default: %s)\n"
        "  -C, --commit SECS     Commit interval in seconds (default: 60)\n"
        "  -I, --dns-timeout SEC DNS cache timeout in seconds (default: 3600)\n"
        "\n"
        "Other:\n"
        "  -h, --help            Show this help\n"
        "  -v, --version         Show version\n",
        prog, DEFAULT_DB_PATH, DEFAULT_APP_MAP);
}

int main(int argc, char *argv[])
{
    /* Set defaults */
    memset(&g_config, 0, sizeof(g_config));
    strcpy(g_config.db_path, DEFAULT_DB_PATH);
    strcpy(g_config.app_map_path, DEFAULT_APP_MAP);
    strcpy(g_config.iface, "br-lan");
    g_config.commit_interval = COMMIT_INTERVAL;
    g_config.dns_timeout = DNS_CACHE_TIMEOUT;
    g_config.flow_timeout = FLOW_TIMEOUT;
    g_config.daemon_mode = 0;
    strcpy(g_config.output_format, "json");
    strcpy(g_config.group_by, "app");

    /* Parse command line */
    static struct option long_opts[] = {
        { "daemon",      no_argument,       0, 'd' },
        { "query",       no_argument,       0, 'q' },
        { "output",      required_argument, 0, 'c' },
        { "separator",   required_argument, 0, 's' },
        { "group-by",    required_argument, 0, 'g' },
        { "period",      required_argument, 0, 't' },
        { "interface",   required_argument, 0, 'i' },
        { "database",    required_argument, 0, 'D' },
        { "map-file",    required_argument, 0, 'm' },
        { "commit",      required_argument, 0, 'C' },
        { "dns-timeout", required_argument, 0, 'I' },
        { "help",        no_argument,       0, 'h' },
        { "version",     no_argument,       0, 'v' },
        { 0, 0, 0, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "dqc:s:g:t:i:D:m:C:I:hv",
                              long_opts, NULL)) != -1) {
        switch (opt) {
        case 'd': g_config.daemon_mode = 1; break;
        case 'q': g_config.daemon_mode = 0; break;
        case 'c': strncpy(g_config.output_format, optarg, sizeof(g_config.output_format) - 1); break;
        case 's': strncpy(g_config.csv_delim, optarg, sizeof(g_config.csv_delim) - 1); break;
        case 'g': strncpy(g_config.group_by, optarg, sizeof(g_config.group_by) - 1); break;
        case 't': strncpy(g_config.period, optarg, sizeof(g_config.period) - 1); break;
        case 'i': strncpy(g_config.iface, optarg, sizeof(g_config.iface) - 1); break;
        case 'D': strncpy(g_config.db_path, optarg, sizeof(g_config.db_path) - 1); break;
        case 'm': strncpy(g_config.app_map_path, optarg, sizeof(g_config.app_map_path) - 1); break;
        case 'C': g_config.commit_interval = atoi(optarg); break;
        case 'I': g_config.dns_timeout = atoi(optarg); break;
        case 'v':
            printf("apptraffic v1.0.0 - Application Traffic Analyzer for OpenWrt\n");
            return 0;
        case 'h':
        default:
            print_usage(argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }

    /* Initialize app mapping */
    int map_count = mapping_load(g_config.app_map_path);
    if (map_count >= 0) {
        fprintf(stderr, "Loaded %d app mapping rules\n", map_count);
    }

    if (g_config.daemon_mode) {
        /* Daemon mode: capture traffic and store */
        fprintf(stderr, "Starting apptraffic daemon...\n");
        fprintf(stderr, "  Interface: %s\n", g_config.iface);
        fprintf(stderr, "  Database:  %s\n", g_config.db_path);
        fprintf(stderr, "  App Map:   %s\n", g_config.app_map_path);

        /* Open database */
        g_db = database_open(g_config.db_path);
        if (!g_db) {
            fprintf(stderr, "Failed to open database\n");
            return 1;
        }

        /* Set up signal handlers */
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);

        /* Set up packet capture callbacks */
        capture_set_dns_callback(on_dns_response);
        capture_set_sni_callback(on_sni_hostname);

        /* Initialize capture */
        if (capture_init(g_config.iface) != 0) {
            fprintf(stderr, "Warning: Packet capture init failed. "
                    "DNS/SNI detection disabled.\n");
        }

        /* Initialize conntrack */
        conntrack_init();

        /* Start packet capture in a background thread */
        pthread_t capture_thread;
        if (g_pcap) {
            pthread_create(&capture_thread, NULL, capture_run, NULL);
            pthread_detach(capture_thread);
            fprintf(stderr, "Packet capture started\n");
        }

        /* Run daemon loop */
        daemon_loop();

        /* Cleanup */
        capture_stop();
        database_close(g_db);
        fprintf(stderr, "apptraffic daemon stopped\n");

    } else {
        /* Query mode: read database and output */
        g_db = database_open(g_config.db_path);
        if (!g_db) {
            fprintf(stderr, "No traffic data available. "
                    "Start daemon first with: %s -d\n", argv[0]);
            return 1;
        }

        const char *period = g_config.period[0] ? g_config.period : "today";
        struct traffic_stat *stats = database_query(g_db, g_config.group_by, period);

        if (strcmp(g_config.output_format, "csv") == 0) {
            const char *delim = g_config.csv_delim[0] ? g_config.csv_delim : ",";
            output_csv(stats, g_config.group_by, delim);
        } else {
            output_json(stats, g_config.group_by);
        }

        database_free_stats(stats);
        database_close(g_db);
    }

    return 0;
}
