/*
 * apptraffic - Application-aware traffic analysis tool for OpenWrt
 *
 * Main entry point: configuration, daemon loop, flow statistics (from
 * /proc/net/nf_conntrack), SQLite storage/output, and wiring of the packet
 * capture callbacks into the DNS/SNI mapping. Packet parsing lives in
 * capture.c / proto.c / tcp_reasm.c / ipreasm.c, DNS+app mapping in dnsmap.c.
 *
 * Copyright (C) 2024
 * Licensed under GPL-2.0
 */

#define _GNU_SOURCE

#include "apptraffic.h"
#include "dnsmap.h"
#include "capture.h"
#include "ipreasm.h"
#include "l7.h"
#include <getopt.h>

/* Global state */
struct config g_config;
int g_running = 1;
static struct db_handle *g_db = NULL;

/* Flow table */
static struct flow_entry *flow_hash[MAX_FLOW_ENTRIES];
static pthread_mutex_t flow_mutex = PTHREAD_MUTEX_INITIALIZER;

static inline unsigned int hash_flow(uint32_t src_ip, uint32_t dst_ip,
                                     uint16_t src_port, uint16_t dst_port,
                                     uint8_t proto)
{
    uint32_t h = src_ip ^ dst_ip ^ (src_port << 16 | dst_port) ^ proto;
    return (h ^ (h >> 16)) % MAX_FLOW_ENTRIES;
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

        if (strncmp(line, "ipv4", 4) != 0) continue;

        int src_cnt = 0, dst_cnt = 0, sport_cnt = 0, dport_cnt = 0;
        int pkt_cnt = 0, byte_cnt = 0;

        char *tok = strtok(line, " \t");
        int field = 0;

        while (tok) {
            /* token layout: ipv4 2 <proto-name> <proto-num> <timeleft> ...
             * token[3] is the numeric protocol (6=tcp,17=udp,1=icmp), which is
             * stable across reads. Reading token[4] (timeleft) instead made the
             * protocol change every 5s and split one connection into many flows,
             * massively double-counting bytes. */
            if (field == 3) {
                protocol = atoi(tok);
            }

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
                *eq = '=';
            }

            tok = strtok(NULL, " \t\n");
            field++;
        }

        if (src_ip == 0 || dst_ip == 0) continue;
        if (rx_bytes == 0 && tx_bytes == 0) continue;

        unsigned int idx = hash_flow(src_ip, dst_ip, src_port, dst_port,
                                     protocol);
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

        if (found) {
            int64_t drx, dtx, drxp, dtxp;
            drx  = (int64_t)rx_bytes  - (int64_t)flow->prev_rx_bytes;
            dtx  = (int64_t)tx_bytes  - (int64_t)flow->prev_tx_bytes;
            drxp = (int64_t)rx_packets - (int64_t)flow->prev_rx_packets;
            dtxp = (int64_t)tx_packets - (int64_t)flow->prev_tx_packets;

            if (drx > 0)  { flow->rx_bytes  += (uint64_t)drx;  flow->dirty = 1; }
            if (dtx > 0)  { flow->tx_bytes  += (uint64_t)dtx;  flow->dirty = 1; }
            if (drxp > 0) { flow->rx_packets += (uint64_t)drxp; flow->dirty = 1; }
            if (dtxp > 0) { flow->tx_packets += (uint64_t)dtxp; flow->dirty = 1; }
        } else {
            flow->rx_bytes  += rx_bytes;
            flow->tx_bytes  += tx_bytes;
            flow->rx_packets += rx_packets;
            flow->tx_packets += tx_packets;
            if (rx_bytes || tx_bytes) flow->dirty = 1;
        }

        flow->prev_rx_bytes  = rx_bytes;
        flow->prev_tx_bytes  = tx_bytes;
        flow->prev_rx_packets = rx_packets;
        flow->prev_tx_packets = tx_packets;
        flow->last_seen = now;
    }

    fclose(fp);

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

void conntrack_foreach_flow(void (*cb)(struct flow_entry *flow, void *user),
                            void *user)
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

    char db_file[512];
    snprintf(db_file, sizeof(db_file), "%s/traffic.db", path);

    int rc = sqlite3_open(db_file, &db->conn);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n",
                sqlite3_errmsg(db->conn));
        sqlite3_close(db->conn);
        free(db);
        return NULL;
    }

    sqlite3_exec(db->conn, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(db->conn, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);

    const char *create_sql =
        "CREATE TABLE IF NOT EXISTS traffic ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  timestamp INTEGER NOT NULL,"
        "  day INTEGER NOT NULL,"
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

    int table_exists = 0;
    int has_day = 0;
    sqlite3_stmt *info_stmt = NULL;
    if (sqlite3_prepare_v2(db->conn,
            "SELECT name FROM sqlite_master WHERE type='table' AND name='traffic'",
            -1, &info_stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(info_stmt) == SQLITE_ROW)
            table_exists = 1;
        sqlite3_finalize(info_stmt);
    }
    if (table_exists &&
        sqlite3_prepare_v2(db->conn, "PRAGMA table_info(traffic)",
                           -1, &info_stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(info_stmt) == SQLITE_ROW) {
            const char *name = (const char *)sqlite3_column_text(info_stmt, 1);
            if (name && strcmp(name, "day") == 0) {
                has_day = 1;
                break;
            }
        }
        sqlite3_finalize(info_stmt);
    }

    if (table_exists && !has_day) {
        sqlite3_exec(db->conn, "ALTER TABLE traffic RENAME TO traffic_old",
                     NULL, NULL, NULL);
        sqlite3_exec(db->conn, "DROP INDEX IF EXISTS idx_traffic_timestamp",
                     NULL, NULL, NULL);
        sqlite3_exec(db->conn, "DROP INDEX IF EXISTS idx_traffic_app", NULL,
                     NULL, NULL);
        sqlite3_exec(db->conn, "DROP INDEX IF EXISTS idx_traffic_domain", NULL,
                     NULL, NULL);

        sqlite3_exec(db->conn, create_sql, NULL, NULL, NULL);

        int rc = sqlite3_exec(db->conn,
            "INSERT INTO traffic (timestamp, day, src_ip, dst_ip, src_port,"
            "  dst_port, protocol, domain, app_name, app_category,"
            "  rx_bytes, tx_bytes, rx_packets, tx_packets)"
            "SELECT MAX(timestamp), timestamp/86400,"
            "  src_ip, dst_ip, src_port, dst_port, protocol,"
            "  MAX(domain), MAX(app_name), MAX(app_category),"
            "  SUM(rx_bytes), SUM(tx_bytes), SUM(rx_packets), SUM(tx_packets)"
            " FROM traffic_old"
            " GROUP BY src_ip, dst_ip, src_port, dst_port, protocol, timestamp/86400",
            NULL, NULL, NULL);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "Note: could not migrate old traffic rows (%s), "
                    "dropping them\n", sqlite3_errmsg(db->conn));
        }
        sqlite3_exec(db->conn, "DROP TABLE IF EXISTS traffic_old", NULL, NULL,
                     NULL);
    } else if (!table_exists) {
        sqlite3_exec(db->conn, create_sql, NULL, NULL, NULL);
    }

    sqlite3_exec(db->conn,
        "CREATE INDEX IF NOT EXISTS idx_traffic_timestamp ON traffic(timestamp)",
        NULL, NULL, NULL);
    sqlite3_exec(db->conn,
        "CREATE INDEX IF NOT EXISTS idx_traffic_app ON traffic(app_name)",
        NULL, NULL, NULL);
    sqlite3_exec(db->conn,
        "CREATE INDEX IF NOT EXISTS idx_traffic_domain ON traffic(domain)",
        NULL, NULL, NULL);
    sqlite3_exec(db->conn,
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_traffic_flow_day ON traffic("
        "src_ip, dst_ip, src_port, dst_port, protocol, day)",
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
                         const char *app, const char *domain,
                         const char *category)
{
    if (!db || !db->conn || !flow) return -1;

    char src_ip[64], dst_ip[64];
    inet_ntop(AF_INET, &flow->src_ip, src_ip, sizeof(src_ip));
    inet_ntop(AF_INET, &flow->dst_ip, dst_ip, sizeof(dst_ip));

    const char *proto_str = "tcp";
    if (flow->protocol == IPPROTO_UDP) proto_str = "udp";
    else if (flow->protocol == IPPROTO_ICMP) proto_str = "icmp";

    const char *sql_insert =
        "INSERT OR IGNORE INTO traffic (timestamp, day, src_ip, dst_ip,"
        "  src_port, dst_port, protocol, domain, app_name, app_category,"
        "  rx_bytes, tx_bytes, rx_packets, tx_packets)"
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, 0, 0, 0)";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->conn, sql_insert, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL prepare error: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }

    /* "day" column actually stores a TS_BUCKET (5-minute) bucket so each flow's
     * bytes land in the bucket when they were last seen -> accurate time series. */
    sqlite3_int64 day = (sqlite3_int64)(flow->last_seen / TS_BUCKET);

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)flow->last_seen);
    sqlite3_bind_int64(stmt, 2, day);
    sqlite3_bind_text(stmt, 3, src_ip, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, dst_ip, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 5, flow->src_port);
    sqlite3_bind_int(stmt, 6, flow->dst_port);
    sqlite3_bind_text(stmt, 7, proto_str, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 8, domain ? domain : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 9, app ? app : "Unknown", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 10,
                      category ? category
                               : (domain ? mapping_lookup_category(domain)
                                         : "General"),
                      -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        fprintf(stderr, "SQL insert error: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }

    const char *sql_update =
        "UPDATE traffic SET"
        "  timestamp = ?1,"
        "  domain = ?2,"
        "  app_name = ?3,"
        "  app_category = ?4,"
        "  rx_bytes = rx_bytes + ?5,"
        "  tx_bytes = tx_bytes + ?6,"
        "  rx_packets = rx_packets + ?7,"
        "  tx_packets = tx_packets + ?8"
        " WHERE src_ip = ?9 AND dst_ip = ?10 AND src_port = ?11"
        "   AND dst_port = ?12 AND protocol = ?13 AND day = ?14";

    stmt = NULL;
    rc = sqlite3_prepare_v2(db->conn, sql_update, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL prepare error: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)flow->last_seen);
    sqlite3_bind_text(stmt, 2, domain ? domain : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, app ? app : "Unknown", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4,
                      category ? category
                               : (domain ? mapping_lookup_category(domain)
                                         : "General"),
                      -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)flow->rx_bytes);
    sqlite3_bind_int64(stmt, 6, (sqlite3_int64)flow->tx_bytes);
    sqlite3_bind_int64(stmt, 7, (sqlite3_int64)flow->rx_packets);
    sqlite3_bind_int64(stmt, 8, (sqlite3_int64)flow->tx_packets);
    sqlite3_bind_text(stmt, 9, src_ip, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 10, dst_ip, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 11, flow->src_port);
    sqlite3_bind_int(stmt, 12, flow->dst_port);
    sqlite3_bind_text(stmt, 13, proto_str, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 14, day);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        fprintf(stderr, "SQL update error: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }

    return 0;
}

int database_commit(struct db_handle *db)
{
    if (db && db->conn) {
        sqlite3_wal_checkpoint(db->conn, NULL);
    }
    return 0;
}

int database_prune(struct db_handle *db, int retention_days)
{
    if (!db || !db->conn || retention_days <= 0) return -1;

    char sql[256];
    snprintf(sql, sizeof(sql),
        "DELETE FROM traffic WHERE timestamp < strftime('%%s','now') - %d * 86400",
        retention_days);

    int rc = sqlite3_exec(db->conn, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Prune error: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }

    sqlite3_wal_checkpoint_v2(db->conn, NULL, SQLITE_CHECKPOINT_TRUNCATE,
                              NULL, NULL);
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
        group_col = "src_ip";
    } else if (strcmp(group_by, "category") == 0) {
        group_col = "app_category";
    } else {
        group_col = "app_name";
    }

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
            time_t secs = atol(period);
            if (secs > 0) {
                snprintf(time_filter, sizeof(time_filter),
                    "WHERE timestamp >= %ld", (long)(time(NULL) - secs));
            }
        }
    }

    /* Local traffic is stored with an empty domain; the domain view should not
     * show the board's own IP. Other views (app/host/category) keep it. */
    char where[256] = "";
    if (strcmp(group_by, "domain") == 0) {
        strcpy(where, " WHERE domain != ''");
        if (time_filter[0])
            snprintf(where + strlen(where), sizeof(where) - strlen(where),
                     " AND %s", time_filter + 6); /* skip leading "WHERE " */
    } else {
        strncpy(where, time_filter, sizeof(where) - 1);
        where[sizeof(where) - 1] = '\0';
    }

    char extra_cols[256] = "";
    int has_extra = 0;
    int col_app_name = -1, col_app_cat = -1;

    if (strcmp(group_by, "app") == 0) {
        snprintf(extra_cols, sizeof(extra_cols),
            ", MAX(app_category) as app_category");
        col_app_cat = 1;
        has_extra = 1;
    } else if (strcmp(group_by, "domain") == 0) {
        snprintf(extra_cols, sizeof(extra_cols),
            ", MAX(app_name) as app_name, MAX(app_category) as app_category");
        col_app_name = 1;
        col_app_cat = 2;
        has_extra = 2;
    } else if (strcmp(group_by, "host") == 0 || strcmp(group_by, "mac") == 0) {
        snprintf(extra_cols, sizeof(extra_cols),
            ", MAX(app_name) as app_name, MAX(app_category) as app_category");
        col_app_name = 1;
        col_app_cat = 2;
        has_extra = 2;
    } else if (strcmp(group_by, "host_app") == 0) {
        snprintf(extra_cols, sizeof(extra_cols),
            ", app_name as app_name, MAX(app_category) as app_category");
        col_app_name = 1;
        col_app_cat = 2;
        has_extra = 2;
    } else if (strcmp(group_by, "category") == 0) {
        extra_cols[0] = '\0';
        has_extra = 0;
    } else {
        snprintf(extra_cols, sizeof(extra_cols),
            ", MAX(app_category) as app_category");
        col_app_cat = 1;
        has_extra = 1;
    }

    if (strcmp(group_by, "host_app") == 0) {
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
            group_col, extra_cols, where, group_col, group_col);
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
            group_col, extra_cols, where);
    }

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Query error: %s\n", sqlite3_errmsg(db->conn));
        return NULL;
    }

    int col_offset = 1 + has_extra;
    struct traffic_stat *head = NULL, *tail = NULL;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        struct traffic_stat *stat = calloc(1, sizeof(*stat));
        if (!stat) continue;

        const char *key = (const char *)sqlite3_column_text(stmt, 0);
        if (key) strncpy(stat->key, key, sizeof(stat->key) - 1);

        if (strcmp(group_by, "app") == 0) {
            if (key) strncpy(stat->app_name, key, sizeof(stat->app_name) - 1);
        } else if (strcmp(group_by, "host_app") == 0) {
            const char *an = (const char *)sqlite3_column_text(stmt,
                                                               col_app_name);
            if (an) strncpy(stat->app_name, an, sizeof(stat->app_name) - 1);
        } else if (col_app_name >= 1) {
            const char *an = (const char *)sqlite3_column_text(stmt,
                                                               col_app_name);
            if (an) strncpy(stat->app_name, an, sizeof(stat->app_name) - 1);
        }

        if (strcmp(group_by, "category") == 0) {
            if (key) strncpy(stat->app_category, key,
                             sizeof(stat->app_category) - 1);
        } else if (col_app_cat >= 1) {
            const char *ac = (const char *)sqlite3_column_text(stmt, col_app_cat);
            if (ac) strncpy(stat->app_category, ac,
                            sizeof(stat->app_category) - 1);
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
 * Time series + real-time alert queries (for the LuCI live view)
 * ================================================================ */

/* Convert a period string to a number of seconds back from now. Numeric
 * strings are seconds; keywords map to the usual windows; default 1 hour. */
static time_t window_secs(const char *period)
{
    if (period && period[0]) {
        if (strcmp(period, "today") == 0 || strcmp(period, "yesterday") == 0)
            return (time_t)(time(NULL) % 86400);
        if (strcmp(period, "week") == 0) return (time_t)(7 * 86400);
        if (strcmp(period, "month") == 0) return (time_t)(30 * 86400);
        long s = atol(period);
        if (s > 0) return (time_t)s;
    }
    return (time_t)3600;
}

/* Per-device x app time series, binned into TS_BUCKET (5-minute) intervals. */
void database_timeseries_json(struct db_handle *db, time_t since)
{
    if (!db || !db->conn) return;
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT (timestamp/?)*? AS ts, src_ip, app_name, app_category,"
        "       SUM(rx_bytes) rx, SUM(tx_bytes) tx, COUNT(*) c "
        "FROM traffic WHERE timestamp>=? GROUP BY ts, src_ip, app_name "
        "ORDER BY ts ASC LIMIT 20000";
    int rc = sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "ts query error: %s\n", sqlite3_errmsg(db->conn));
        return;
    }
    sqlite3_bind_int(stmt, 1, TS_BUCKET);
    sqlite3_bind_int(stmt, 2, TS_BUCKET);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)since);

    printf("{\"bucket_secs\": %d, \"entries\": [", TS_BUCKET);
    int first = 1;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (!first) printf(",");
        first = 0;
        const char *ip = (const char *)sqlite3_column_text(stmt, 1);
        const char *app = (const char *)sqlite3_column_text(stmt, 2);
        const char *cat = (const char *)sqlite3_column_text(stmt, 3);
        printf("\n  {\"ts\": %lld, \"src_ip\": \"%s\", \"app\": \"%s\", "
               "\"category\": \"%s\", \"rx\": %lld, \"tx\": %lld, "
               "\"conn\": %lld}",
               (long long)sqlite3_column_int64(stmt, 0),
               ip ? ip : "", app ? app : "", cat ? cat : "",
               (long long)sqlite3_column_int64(stmt, 4),
               (long long)sqlite3_column_int64(stmt, 5),
               (long long)sqlite3_column_int64(stmt, 6));
    }
    sqlite3_finalize(stmt);
    printf("\n]}\n");
}

/* Devices x apps whose total bytes in the window exceed threshold (bytes). */
void database_alerts_json(struct db_handle *db, time_t since,
                          long long threshold)
{
    if (!db || !db->conn) return;
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT src_ip, app_name, app_category,"
        "       SUM(rx_bytes+tx_bytes) total, COUNT(*) c "
        "FROM traffic WHERE timestamp>=? "
        "GROUP BY src_ip, app_name HAVING total > ? "
        "ORDER BY total DESC LIMIT 100";
    int rc = sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "alert query error: %s\n", sqlite3_errmsg(db->conn));
        return;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)since);
    sqlite3_bind_int64(stmt, 2, threshold);
    printf("{\"threshold_bytes\": %lld, \"entries\": [", threshold);
    int first = 1;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (!first) printf(",");
        first = 0;
        const char *ip = (const char *)sqlite3_column_text(stmt, 0);
        const char *app = (const char *)sqlite3_column_text(stmt, 1);
        const char *cat = (const char *)sqlite3_column_text(stmt, 2);
        printf("\n  {\"src_ip\": \"%s\", \"app\": \"%s\", "
               "\"category\": \"%s\", \"total_bytes\": %lld, \"conn\": %lld}",
               ip ? ip : "", app ? app : "", cat ? cat : "",
               (long long)sqlite3_column_int64(stmt, 3),
               (long long)sqlite3_column_int64(stmt, 4));
    }
    sqlite3_finalize(stmt);
    printf("\n]}\n");
}

/* ================================================================
 * Output Functions
 * ================================================================ */

void output_json(struct traffic_stat *stats, const char *group_by)
{
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
        json_key = group_by;
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
        printf("      \"%s\": \"%s\",\n", json_key, s->key);
        if (s->app_name[0] && strcmp(group_by, "app") != 0)
            printf("      \"app_name\": \"%s\",\n", s->app_name);
        if (s->app_category[0] && strcmp(group_by, "category") != 0)
            printf("      \"app_category\": \"%s\",\n", s->app_category);
        printf("      \"rx_bytes\": %llu,\n", (unsigned long long)s->rx_bytes);
        printf("      \"tx_bytes\": %llu,\n", (unsigned long long)s->tx_bytes);
        printf("      \"rx_packets\": %llu,\n", (unsigned long long)s->rx_packets);
        printf("      \"tx_packets\": %llu,\n", (unsigned long long)s->tx_packets);
        printf("      \"connections\": %u,\n", s->connections);
        printf("      \"total_bytes\": %llu\n",
               (unsigned long long)(s->rx_bytes + s->tx_bytes));
        printf("    }");

        s = s->next;
    }

    printf("\n  ]\n}\n");
}

void output_csv(struct traffic_stat *stats, const char *group_by,
                const char *delim)
{
    if (!delim) delim = ",";

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

    printf("%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s\n",
        json_key, delim,
        "app_name", delim,
        "app_category", delim,
        "rx_bytes", delim,
        "tx_bytes", delim,
        "rx_packets", delim,
        "tx_packets", delim,
        "connections");

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
 * DNS / SNI Callbacks (called from the capture thread)
 * ================================================================ */

static void on_dns_response(const char *domain, uint32_t ip, int is_v6,
                            const uint8_t *ip6)
{
    if (domain && (ip != 0 || (is_v6 && ip6))) {
        mapping_add_dns(domain, ip, is_v6, ip6, 1); /* DNS = low confidence */
    }
}

static void on_sni_hostname(const char *domain, uint32_t ip, int is_v6,
                            const uint8_t *ip6)
{
    if (domain && (ip != 0 || (is_v6 && ip6))) {
        mapping_add_dns(domain, ip, is_v6, ip6, 2); /* SNI = exact host */
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

    if (!flow->dirty || (flow->rx_bytes == 0 && flow->tx_bytes == 0))
        return;

    /* Drop protocol noise: IPv4 multicast, loopback, and broadcast traffic
     * (SSDP/mDNS/LLMNR/ARP-style chatter) so the views stay clean. */
    if (mapping_is_noise_ip(flow->src_ip) || mapping_is_noise_ip(flow->dst_ip)) {
        flow->rx_bytes = 0;
        flow->tx_bytes = 0;
        flow->rx_packets = 0;
        flow->tx_packets = 0;
        flow->dirty = 0;
        return;
    }

    /* Flows whose destination is this host itself (the gateway / board). Label
     * well-known local services instead of showing the bare IP as "General". */
    const char *lapp = NULL, *lcat = NULL;
    if (mapping_local_service(flow->dst_ip, flow->dst_port, &lapp, &lcat)) {
        /* Local (this-host) traffic is not an internet domain, so leave the
         * domain column empty; the "domain" view excludes it, while app/host/
         * category views still show it under the service name. */
        if (database_store_flow(ctx->db, flow, lapp, "", lcat) == 0) {
            flow->rx_bytes = 0;
            flow->tx_bytes = 0;
            flow->rx_packets = 0;
            flow->tx_packets = 0;
            flow->dirty = 0;
        }
        return;
    }

    /* 1) Exact per-connection SNI host (from this flow's own ClientHello).
     * 2) Best of the IP->domain cache (SNI > DNS, most recent).
     * 3) IP string as a last resort. */
    const char *domain = mapping_lookup_flow_host(flow->src_ip, flow->dst_ip,
                                                  flow->src_port, flow->dst_port,
                                                  flow->protocol);
    if (!domain)
        domain = mapping_lookup_domain(flow->dst_ip);
    if (!domain) {
        domain = mapping_lookup_domain(flow->src_ip);
    }

    char ip_fallback[64];
    if (!domain) {
        inet_ntop(AF_INET, &flow->dst_ip, ip_fallback, sizeof(ip_fallback));
        domain = ip_fallback;
    }

    const char *app = "Unknown", *cat = NULL;
    /* An L7 signature (SSH/RDP/QQ/...) is authoritative when present */
    if (mapping_lookup_flow_app(flow->src_ip, flow->dst_ip, flow->src_port,
                                flow->dst_port, flow->protocol,
                                &app, &cat)) {
        /* app/cat filled by the L7 matcher */
    } else if (domain != ip_fallback) {
        app = mapping_lookup_app(domain);
        cat = NULL;
    }

    if (database_store_flow(ctx->db, flow, app, domain, cat) == 0) {
        flow->rx_bytes = 0;
        flow->tx_bytes = 0;
        flow->rx_packets = 0;
        flow->tx_packets = 0;
        flow->dirty = 0;
    }
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

        if (now - last_conntrack >= 5) {
            conntrack_update();
            last_conntrack = now;
        }

        if (now - last_commit >= g_config.commit_interval) {
            struct store_ctx ctx = { g_db };
            conntrack_foreach_flow(flow_to_db_cb, &ctx);
            database_prune(g_db, g_config.retention_days);
            database_commit(g_db);
            last_commit = now;
        }

        if (now - last_expire >= 300) {
            mapping_expire_dns();
            ipfrag_age();
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
        "  -g, --group-by FIELD  Group by: app, domain, host, category, ts, alert\n"
        "  -t, --period PERIOD   Time period: today, week, month, or seconds\n"
        "  -A, --alert-mb MB     Alert threshold in MB (with -g alert)\n"
        "\n"
        "Daemon Options:\n"
        "  -i, --interface IF    Interface to capture (default: br-lan)\n"
        "  -D, --database PATH   Database storage path (default: %s)\n"
        "  -m, --map-file PATH   App mapping file path (default: %s)\n"
        "  -C, --commit SECS     Commit interval in seconds (default: 60)\n"
        "  -I, --dns-timeout SEC DNS cache timeout in seconds (default: 3600)\n"
        "  -R, --retention DAYS  Keep per-flow daily stats for N days (default: %d)\n"
        "\n"
        "Other:\n"
        "  -h, --help            Show this help\n"
        "  -v, --version         Show version\n",
        prog, DEFAULT_DB_PATH, DEFAULT_APP_MAP, RETENTION_DAYS);
}

int main(int argc, char *argv[])
{
    memset(&g_config, 0, sizeof(g_config));
    strcpy(g_config.db_path, DEFAULT_DB_PATH);
    strcpy(g_config.app_map_path, DEFAULT_APP_MAP);
    strcpy(g_config.iface, "br-lan");
    g_config.commit_interval = COMMIT_INTERVAL;
    g_config.dns_timeout = DNS_CACHE_TIMEOUT;
    g_config.flow_timeout = FLOW_TIMEOUT;
    g_config.retention_days = RETENTION_DAYS;
    g_config.daemon_mode = 0;
    strcpy(g_config.output_format, "json");
    strcpy(g_config.group_by, "app");

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
        { "retention",   required_argument, 0, 'R' },
        { "alert-mb",    required_argument, 0, 'A' },
        { "help",        no_argument,       0, 'h' },
        { "version",     no_argument,       0, 'v' },
        { 0, 0, 0, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "dqc:s:g:t:i:D:m:C:I:R:A:hv",
                              long_opts, NULL)) != -1) {
        switch (opt) {
        case 'd': g_config.daemon_mode = 1; break;
        case 'q': g_config.daemon_mode = 0; break;
        case 'c': strncpy(g_config.output_format, optarg,
                          sizeof(g_config.output_format) - 1); break;
        case 's': strncpy(g_config.csv_delim, optarg,
                          sizeof(g_config.csv_delim) - 1); break;
        case 'g': strncpy(g_config.group_by, optarg,
                          sizeof(g_config.group_by) - 1); break;
        case 't': strncpy(g_config.period, optarg,
                          sizeof(g_config.period) - 1); break;
        case 'i': strncpy(g_config.iface, optarg,
                          sizeof(g_config.iface) - 1); break;
        case 'D': strncpy(g_config.db_path, optarg,
                          sizeof(g_config.db_path) - 1); break;
        case 'm': strncpy(g_config.app_map_path, optarg,
                          sizeof(g_config.app_map_path) - 1); break;
        case 'C': g_config.commit_interval = atoi(optarg); break;
        case 'I': g_config.dns_timeout = atoi(optarg); break;
        case 'R': g_config.retention_days = atoi(optarg); break;
        case 'A': g_config.alert_mb = atoi(optarg); break;
        case 'v':
            printf("apptraffic v1.0.0 - Application Traffic Analyzer for OpenWrt\n");
            return 0;
        case 'h':
        default:
            print_usage(argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }

    int map_count = mapping_load(g_config.app_map_path);
    if (map_count >= 0) {
        fprintf(stderr, "Loaded %d app mapping rules\n", map_count);
    }

    if (g_config.daemon_mode) {
        fprintf(stderr, "Starting apptraffic daemon...\n");
        fprintf(stderr, "  Interface: %s\n", g_config.iface);
        fprintf(stderr, "  Database:  %s\n", g_config.db_path);
        fprintf(stderr, "  App Map:   %s\n", g_config.app_map_path);
        fprintf(stderr, "  Retention: %d days\n", g_config.retention_days);

        /* Collect this host's own IPv4 addresses for local service labelling */
        mapping_init_local();

        int l7_count = l7_load(DEFAULT_L7_RULES);
        if (l7_count >= 0) {
            fprintf(stderr, "Loaded %d L7 signature rules\n", l7_count);
        }

        g_db = database_open(g_config.db_path);
        if (!g_db) {
            fprintf(stderr, "Failed to open database\n");
            return 1;
        }

        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);

        capture_set_dns_callback(on_dns_response);
        capture_set_sni_callback(on_sni_hostname);

        if (capture_init(g_config.iface) != 0) {
            fprintf(stderr, "Warning: Packet capture init failed. "
                    "DNS/SNI detection disabled.\n");
        }

        conntrack_init();

        pthread_t capture_thread;
        if (capture_is_active()) {
            pthread_create(&capture_thread, NULL, capture_run, NULL);
            pthread_detach(capture_thread);
            fprintf(stderr, "Packet capture started\n");
        }

        daemon_loop();

        capture_stop();
        database_close(g_db);
        fprintf(stderr, "apptraffic daemon stopped\n");
    } else {
        g_db = database_open(g_config.db_path);
        if (!g_db) {
            fprintf(stderr, "No traffic data available. "
                    "Start daemon first with: %s -d\n", argv[0]);
            return 1;
        }

        const char *period = g_config.period[0] ? g_config.period : "today";
        time_t since = time(NULL) - (time_t)window_secs(period);

        if (strcmp(g_config.group_by, "ts") == 0) {
            database_timeseries_json(g_db, since);
        } else if (strcmp(g_config.group_by, "alert") == 0) {
            if (g_config.alert_mb <= 0) {
                printf("{\"threshold_bytes\": 0, \"entries\": []}\n");
            } else {
                database_alerts_json(g_db, since,
                                     (long long)g_config.alert_mb * 1024 * 1024);
            }
        } else {
            struct traffic_stat *stats = database_query(g_db, g_config.group_by,
                                                        period);

            if (strcmp(g_config.output_format, "csv") == 0) {
                const char *delim = g_config.csv_delim[0] ? g_config.csv_delim : ",";
                output_csv(stats, g_config.group_by, delim);
            } else {
                output_json(stats, g_config.group_by);
            }

            database_free_stats(stats);
        }
        database_close(g_db);
    }

    return 0;
}
