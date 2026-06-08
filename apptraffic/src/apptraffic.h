/*
 * apptraffic - Application-aware traffic analysis tool for OpenWrt
 * Copyright (C) 2024
 * Licensed under GPL-2.0
 */

#ifndef APPTRAFFIC_H
#define APPTRAFFIC_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fnmatch.h>
#include <unistd.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>

/* Maximum number of IP→domain mappings in memory */
#define MAX_DNS_CACHE      65536
#define MAX_FLOW_ENTRIES   32768

/* Timeouts */
#define DNS_CACHE_TIMEOUT  3600    /* 1 hour */
#define FLOW_TIMEOUT       300     /* 5 minutes idle */
#define COMMIT_INTERVAL    60      /* commit every 60 seconds */

/* Default paths */
#define DEFAULT_DB_PATH    "/var/lib/apptraffic"
#define DEFAULT_APP_MAP    "/usr/share/apptraffic/app-mapping.txt"

/* DNS record types (standard values from arpa/nameser.h) */
#ifndef DNS_TYPE_A
#define DNS_TYPE_A    1
#endif
#ifndef DNS_TYPE_AAAA
#define DNS_TYPE_AAAA 28
#endif

/* Hash table entry for IP -> domain mapping */
struct dns_entry {
    uint32_t        ip4;           /* IPv4 address (network order) */
    uint8_t         ip6[16];       /* IPv6 address */
    int             is_v6;
    char            domain[256];   /* resolved domain name */
    time_t          expires;       /* when this entry expires */
    struct dns_entry *next;
};

/* Flow statistics entry */
struct flow_entry {
    uint32_t        src_ip;
    uint32_t        dst_ip;
    uint16_t        src_port;
    uint16_t        dst_port;
    uint8_t         protocol;      /* IPPROTO_TCP or IPPROTO_UDP */
    uint64_t        rx_bytes;      /* accumulated delta since last commit */
    uint64_t        tx_bytes;      /* accumulated delta since last commit */
    uint64_t        rx_packets;    /* accumulated delta since last commit */
    uint64_t        tx_packets;    /* accumulated delta since last commit */
    uint64_t        prev_rx_bytes; /* last-seen conntrack cumulative value */
    uint64_t        prev_tx_bytes;
    uint64_t        prev_rx_packets;
    uint64_t        prev_tx_packets;
    int             dirty;         /* has new data since last commit */
    time_t          first_seen;
    time_t          last_seen;
    struct flow_entry *next;
};

/* Application mapping entry */
struct app_mapping {
    char            pattern[256];  /* domain pattern (glob/wildcard) */
    char            app_name[128]; /* application/website name */
    char            category[64];  /* category: social, video, etc. */
    int             priority;      /* higher = more specific match */
    struct app_mapping *next;
};

/* Traffic statistics for output */
struct traffic_stat {
    char            key[256];      /* group key (app name, domain, IP) */
    char            app_name[128]; /* mapped application name */
    char            app_category[64]; /* app category */
    uint64_t        rx_bytes;
    uint64_t        tx_bytes;
    uint64_t        rx_packets;
    uint64_t        tx_packets;
    uint32_t        connections;
    struct traffic_stat *next;
};

/* Configuration */
struct config {
    char            db_path[256];
    char            app_map_path[256];
    char            iface[32];
    int             commit_interval;
    int             dns_timeout;
    int             flow_timeout;
    int             daemon_mode;
    char            output_format[16];
    char            group_by[32];
    char            period[64];
    char            csv_delim[4];
};

/* Database handle */
struct db_handle;

/* Function declarations */

/* Packet capture */
int  capture_init(const char *iface);
void *capture_run(void *arg);
void capture_stop(void);
void capture_set_dns_callback(void (*cb)(const char *domain, uint32_t ip, int is_v6, const uint8_t *ip6));
void capture_set_sni_callback(void (*cb)(const char *domain, uint32_t ip));

/* Conntrack flow reading */
int  conntrack_init(void);
void conntrack_update(void);
void conntrack_foreach_flow(void (*cb)(struct flow_entry *flow, void *user), void *user);

/* Domain→app mapping */
int  mapping_load(const char *path);
const char *mapping_lookup_app(const char *domain);
const char *mapping_lookup_category(const char *domain);
const char *mapping_lookup_domain(uint32_t ip);
void mapping_add_dns(const char *domain, uint32_t ip, int is_v6, const uint8_t *ip6);
void mapping_expire_dns(void);

/* SQLite database */
struct db_handle *database_open(const char *path);
void database_close(struct db_handle *db);
int  database_store_flow(struct db_handle *db, struct flow_entry *flow, const char *app, const char *domain);
int  database_commit(struct db_handle *db);
struct traffic_stat *database_query(struct db_handle *db, const char *group_by, const char *period);
void database_free_stats(struct traffic_stat *stats);

/* Output formatters */
void output_json(struct traffic_stat *stats, const char *group_by);
void output_csv(struct traffic_stat *stats, const char *group_by, const char *delim);

#endif /* APPTRAFFIC_H */
