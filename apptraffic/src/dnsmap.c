/*
 * dnsmap.c - IP->domain cache and domain->application mapping.
 *
 * The IP->domain cache is populated by the capture thread via
 * mapping_add_dns() when a DNS A/AAAA response or a TLS SNI is observed.
 * The domain->application table is loaded once from app-mapping.txt.
 *
 * Copyright (C) 2024
 * Licensed under GPL-2.0
 */

#define _GNU_SOURCE

#include "dnsmap.h"
#include <ifaddrs.h>
#include <net/if.h>

/* DNS cache hash table */
static struct dns_entry *dns_hash[MAX_DNS_CACHE];
static pthread_mutex_t dns_mutex = PTHREAD_MUTEX_INITIALIZER;

/* App mapping list */
static struct app_mapping *app_mappings = NULL;
static pthread_mutex_t app_mutex = PTHREAD_MUTEX_INITIALIZER;

/* This host's own IPv4 addresses (from getifaddrs) */
#define MAX_LOCAL_ADDRS 64
static uint32_t g_local_addrs[MAX_LOCAL_ADDRS];
static int g_local_count;
static pthread_mutex_t local_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Subnet broadcast addresses (from getifaddrs ifa_broadaddr) */
static uint32_t g_local_bcast[MAX_LOCAL_ADDRS];
static int g_local_bcast_count;

/* Per-flow SNI host cache (5-tuple key -> hostname). This is the exact
 * hostname from a connection's own ClientHello, so it resolves the CDN /
 * shared-IP problem that a single IP->domain cache cannot. */
#define FLOW_HOST_BUCKETS 8192
#define FLOW_HOST_MAX     32768
#define FLOW_HOST_TIMEOUT 600

struct flow_host {
    uint32_t         src_ip;
    uint32_t         dst_ip;
    uint16_t         src_port;
    uint16_t         dst_port;
    uint8_t          proto;
    char             host[256];
    time_t           expires;
    struct flow_host *next;
};

static struct flow_host *flow_host_hash[FLOW_HOST_BUCKETS];
static pthread_mutex_t flow_host_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned int flow_host_count;

/* Per-flow L7 signature result cache */
struct flow_app {
    uint32_t         src_ip;
    uint32_t         dst_ip;
    uint16_t         src_port;
    uint16_t         dst_port;
    uint8_t          proto;
    char             app[64];
    char             cat[32];
    time_t           expires;
    struct flow_app  *next;
};

static struct flow_app *flow_app_hash[FLOW_HOST_BUCKETS];
static pthread_mutex_t flow_app_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned int flow_app_count;

static inline unsigned int hash_ip(uint32_t ip)
{
    return (ip ^ (ip >> 16) ^ (ip >> 8)) % MAX_DNS_CACHE;
}

/* True if the string is a dotted-quad IPv4 literal. */
static int is_ipv4_literal(const char *s)
{
    struct in_addr a;
    return s && inet_pton(AF_INET, s, &a) == 1;
}

void mapping_add_dns(const char *domain, uint32_t ip, int is_v6,
                     const uint8_t *ip6, int confidence)
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

    /* If an entry for (same IP, same domain) already exists, bump its
     * confidence (SNI > DNS) and refresh its TTL instead of duplicating. */
    for (struct dns_entry *e = dns_hash[idx]; e; e = e->next) {
        int same = is_v6 ? (e->is_v6 && memcmp(e->ip6, ip6, 16) == 0)
                         : (!e->is_v6 && e->ip4 == ip);
        if (same && strcmp(e->domain, domain) == 0) {
            e->expires = time(NULL) + g_config.dns_timeout;
            if (confidence > e->confidence)
                e->confidence = (uint8_t)confidence;
            pthread_mutex_unlock(&dns_mutex);
            return;
        }
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
    entry->confidence = (uint8_t)confidence;
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
    int best_conf = -1;
    time_t best_exp = 0;

    while (entry) {
        if (!entry->is_v6 && entry->ip4 == ip && entry->expires > now) {
            /* Prefer the highest-confidence mapping (SNI > DNS); break ties by
             * the most recently refreshed. */
            if (entry->confidence > best_conf ||
                (entry->confidence == best_conf && entry->expires > best_exp)) {
                best_conf = entry->confidence;
                best_exp = entry->expires;
                result = entry->domain;
            }
        }
        entry = entry->next;
    }

    pthread_mutex_unlock(&dns_mutex);
    return result;
}

/* ================================================================
 * Per-flow SNI host cache (exact hostname per TCP connection)
 * ================================================================ */

static unsigned int flow_host_key(uint32_t src_ip, uint32_t dst_ip,
                                  uint16_t src_port, uint16_t dst_port,
                                  uint8_t proto)
{
    uint32_t h = src_ip ^ dst_ip ^ ((uint32_t)src_port << 16 | dst_port) ^
                 (uint32_t)proto;
    return (h ^ (h >> 16)) % FLOW_HOST_BUCKETS;
}

static void flow_host_expire(time_t now)
{
    for (int i = 0; i < FLOW_HOST_BUCKETS; i++) {
        struct flow_host **pp = &flow_host_hash[i];
        while (*pp) {
            struct flow_host *h = *pp;
            if (h->expires < now) {
                *pp = h->next;
                flow_host_count--;
                free(h);
            } else {
                pp = &h->next;
            }
        }
    }
}

void mapping_add_flow_host(uint32_t src_ip, uint32_t dst_ip, uint16_t src_port,
                           uint16_t dst_port, uint8_t proto, const char *host)
{
    if (!host || !*host) return;
    time_t now = time(NULL);

    pthread_mutex_lock(&flow_host_mutex);

    unsigned int idx = flow_host_key(src_ip, dst_ip, src_port, dst_port, proto);
    for (struct flow_host *h = flow_host_hash[idx]; h; h = h->next) {
        if (h->src_ip == src_ip && h->dst_ip == dst_ip &&
            h->src_port == src_port && h->dst_port == dst_port &&
            h->proto == proto) {
            strncpy(h->host, host, sizeof(h->host) - 1);
            h->host[sizeof(h->host) - 1] = '\0';
            h->expires = now + FLOW_HOST_TIMEOUT;
            pthread_mutex_unlock(&flow_host_mutex);
            return;
        }
    }

    if (flow_host_count >= FLOW_HOST_MAX) {
        flow_host_expire(now);
        if (flow_host_count >= FLOW_HOST_MAX) {
            pthread_mutex_unlock(&flow_host_mutex);
            return; /* give up rather than grow unbounded */
        }
    }

    struct flow_host *h = calloc(1, sizeof(*h));
    if (!h) {
        pthread_mutex_unlock(&flow_host_mutex);
        return;
    }
    h->src_ip = src_ip;
    h->dst_ip = dst_ip;
    h->src_port = src_port;
    h->dst_port = dst_port;
    h->proto = proto;
    strncpy(h->host, host, sizeof(h->host) - 1);
    h->host[sizeof(h->host) - 1] = '\0';
    h->expires = now + FLOW_HOST_TIMEOUT;
    h->next = flow_host_hash[idx];
    flow_host_hash[idx] = h;
    flow_host_count++;

    pthread_mutex_unlock(&flow_host_mutex);
}

const char *mapping_lookup_flow_host(uint32_t src_ip, uint32_t dst_ip,
                                     uint16_t src_port, uint16_t dst_port,
                                     uint8_t proto)
{
    time_t now = time(NULL);
    pthread_mutex_lock(&flow_host_mutex);

    unsigned int idx = flow_host_key(src_ip, dst_ip, src_port, dst_port, proto);
    struct flow_host *h = flow_host_hash[idx];
    while (h) {
        if (h->src_ip == src_ip && h->dst_ip == dst_ip &&
            h->src_port == src_port && h->dst_port == dst_port &&
            h->proto == proto && h->expires > now) {
            pthread_mutex_unlock(&flow_host_mutex);
            return h->host;
        }
        h = h->next;
    }

    pthread_mutex_unlock(&flow_host_mutex);
    return NULL;
}

/* ================================================================
 * Per-flow L7 signature result cache
 * ================================================================ */

static void flow_app_expire(time_t now)
{
    for (int i = 0; i < FLOW_HOST_BUCKETS; i++) {
        struct flow_app **pp = &flow_app_hash[i];
        while (*pp) {
            struct flow_app *h = *pp;
            if (h->expires < now) {
                *pp = h->next;
                flow_app_count--;
                free(h);
            } else {
                pp = &h->next;
            }
        }
    }
}

void mapping_add_flow_app(uint32_t src_ip, uint32_t dst_ip, uint16_t src_port,
                          uint16_t dst_port, uint8_t proto,
                          const char *app, const char *cat)
{
    if (!app || !*app) return;
    time_t now = time(NULL);
    pthread_mutex_lock(&flow_app_mutex);

    unsigned int idx = flow_host_key(src_ip, dst_ip, src_port, dst_port, proto);
    for (struct flow_app *h = flow_app_hash[idx]; h; h = h->next) {
        if (h->src_ip == src_ip && h->dst_ip == dst_ip &&
            h->src_port == src_port && h->dst_port == dst_port &&
            h->proto == proto) {
            strncpy(h->app, app, sizeof(h->app) - 1);
            h->app[sizeof(h->app) - 1] = '\0';
            strncpy(h->cat, cat ? cat : "General", sizeof(h->cat) - 1);
            h->cat[sizeof(h->cat) - 1] = '\0';
            h->expires = now + FLOW_HOST_TIMEOUT;
            pthread_mutex_unlock(&flow_app_mutex);
            return;
        }
    }

    if (flow_app_count >= FLOW_HOST_MAX) {
        flow_app_expire(now);
        if (flow_app_count >= FLOW_HOST_MAX) {
            pthread_mutex_unlock(&flow_app_mutex);
            return;
        }
    }

    struct flow_app *h = calloc(1, sizeof(*h));
    if (!h) {
        pthread_mutex_unlock(&flow_app_mutex);
        return;
    }
    h->src_ip = src_ip;
    h->dst_ip = dst_ip;
    h->src_port = src_port;
    h->dst_port = dst_port;
    h->proto = proto;
    strncpy(h->app, app, sizeof(h->app) - 1);
    h->app[sizeof(h->app) - 1] = '\0';
    strncpy(h->cat, cat ? cat : "General", sizeof(h->cat) - 1);
    h->cat[sizeof(h->cat) - 1] = '\0';
    h->expires = now + FLOW_HOST_TIMEOUT;
    h->next = flow_app_hash[idx];
    flow_app_hash[idx] = h;
    flow_app_count++;
    pthread_mutex_unlock(&flow_app_mutex);
}

int mapping_lookup_flow_app(uint32_t src_ip, uint32_t dst_ip, uint16_t src_port,
                            uint16_t dst_port, uint8_t proto,
                            const char **app, const char **cat)
{
    time_t now = time(NULL);
    pthread_mutex_lock(&flow_app_mutex);

    unsigned int idx = flow_host_key(src_ip, dst_ip, src_port, dst_port, proto);
    struct flow_app *h = flow_app_hash[idx];
    while (h) {
        if (h->src_ip == src_ip && h->dst_ip == dst_ip &&
            h->src_port == src_port && h->dst_port == dst_port &&
            h->proto == proto && h->expires > now) {
            *app = h->app;
            *cat = h->cat;
            pthread_mutex_unlock(&flow_app_mutex);
            return 1;
        }
        h = h->next;
    }

    pthread_mutex_unlock(&flow_app_mutex);
    return 0;
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

    /* Also drop stale per-flow SNI hosts in the same periodic sweep. */
    pthread_mutex_lock(&flow_host_mutex);
    flow_host_expire(time(NULL));
    pthread_mutex_unlock(&flow_host_mutex);

    pthread_mutex_lock(&flow_app_mutex);
    flow_app_expire(time(NULL));
    pthread_mutex_unlock(&flow_app_mutex);
}

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

    /* Don't treat an IP literal as a domain name for the SLD fallback
     * (e.g. "192.168.100.1" must not become a bogus app "100.1"). */
    if (is_ipv4_literal(domain))
        return "Unknown";

    /* Fallback: extract second-level domain as app name */
    const char *dot = strrchr(domain, '.');
    if (dot) {
        const char *prev = domain;
        const char *p = domain;
        while (p < dot) {
            if (*p == '.') prev = p + 1;
            p++;
        }
        static char fallback[128];
        strncpy(fallback, prev, sizeof(fallback) - 1);
        fallback[sizeof(fallback) - 1] = '\0';
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
 * Local (this host) service recognition
 * ================================================================ */

void mapping_init_local(void)
{
    pthread_mutex_lock(&local_mutex);
    g_local_count = 0;

    struct ifaddrs *ifa, *ifp;
    if (getifaddrs(&ifp) == 0) {
        for (ifa = ifp; ifa && g_local_count < MAX_LOCAL_ADDRS;
             ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr ||
                ifa->ifa_addr->sa_family != AF_INET)
                continue;
            uint32_t ip;
            memcpy(&ip, &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr, 4);
            if (ip == 0)
                continue;
            g_local_addrs[g_local_count++] = ip;

            if (ifa->ifa_broadaddr &&
                ifa->ifa_broadaddr->sa_family == AF_INET &&
                g_local_bcast_count < MAX_LOCAL_ADDRS) {
                uint32_t bcast;
                memcpy(&bcast,
                       &((struct sockaddr_in *)ifa->ifa_broadaddr)->sin_addr, 4);
                if (bcast != 0)
                    g_local_bcast[g_local_bcast_count++] = ntohl(bcast);
            }
        }
        freeifaddrs(ifp);
    }

    pthread_mutex_unlock(&local_mutex);
}

void mapping_free_local(void)
{
    pthread_mutex_lock(&local_mutex);
    g_local_count = 0;
    pthread_mutex_unlock(&local_mutex);
}

static int is_local_ip(uint32_t ip)
{
    for (int i = 0; i < g_local_count; i++) {
        if (g_local_addrs[i] == ip)
            return 1;
    }
    return 0;
}

int mapping_local_service(uint32_t dst_ip, uint16_t dst_port,
                          const char **app, const char **cat)
{
    pthread_mutex_lock(&local_mutex);
    int loc = is_local_ip(dst_ip);
    pthread_mutex_unlock(&local_mutex);
    if (!loc)
        return 0;

    switch (dst_port) {
    case 8080:
    case 8443:
    case 443:
        *app = "CloudDriveWebDAV";
        *cat = "Local";
        return 1;
    case 8081:
        /* Video surveillance (mjpg-streamer) */
        *app = "MJPGStreamer";
        *cat = "Video";
        return 1;
    case 80:
        *app = "LuCI";
        *cat = "Management";
        return 1;
    case 53:
        *app = "DNS";
        *cat = "System";
        return 1;
    case 22:
        *app = "SSH";
        *cat = "System";
        return 1;
    case 445:
    case 139:
    case 137:
        *app = "Samba";
        *cat = "System";
        return 1;
    case 51820:
        *app = "WireGuard";
        *cat = "VPN";
        return 1;
    default:
        *app = "Local";
        *cat = "System";
        return 1;
    }
}

int mapping_is_noise_ip(uint32_t ip)
{
    uint32_t h = ntohl(ip); /* ip arrives in network byte order */

    /* IPv4 multicast 224.0.0.0/4 */
    if ((h & 0xF0000000) == 0xE0000000)
        return 1;
    /* Loopback 127.0.0.0/8 */
    if ((h & 0xFF000000) == 0x7F000000)
        return 1;
    /* Limited broadcast 255.255.255.255 */
    if (h == 0xFFFFFFFF)
        return 1;

    pthread_mutex_lock(&local_mutex);
    for (int i = 0; i < g_local_bcast_count; i++) {
        if (g_local_bcast[i] == h) {
            pthread_mutex_unlock(&local_mutex);
            return 1;
        }
    }
    pthread_mutex_unlock(&local_mutex);
    return 0;
}
