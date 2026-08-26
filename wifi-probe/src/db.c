#include "wifi-probe.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

struct wp_db { sqlite3 *conn; };

static const char *bin_str(int bin)
{
    switch (bin) {
    case WP_RSSI_NEAR: return "near";
    case WP_RSSI_MID:  return "mid";
    case WP_RSSI_FAR:  return "far";
    default:           return "unknown";
    }
}

static void sql_exec(struct wp_db *db, const char *sql)
{
    char *err = NULL;
    if (sqlite3_exec(db->conn, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "sql error: %s\n", err ? err : "?");
        sqlite3_free(err);
    }
}

struct wp_db *wp_db_open(const char *path)
{
    struct wp_db *db = calloc(1, sizeof(*db));
    if (!db) return NULL;

    if (strcmp(path, ":memory:") == 0) {
        if (sqlite3_open(":memory:", &db->conn) != SQLITE_OK) { free(db); return NULL; }
    } else {
        char dbfile[512];
        snprintf(dbfile, sizeof(dbfile), "%s/traffic.db", path);
        mkdir(path, 0755);   /* 若已存在则 EEXIST，忽略 */
        if (sqlite3_open(dbfile, &db->conn) != SQLITE_OK) { free(db); return NULL; }
    }
    sql_exec(db, "PRAGMA journal_mode=WAL");
    sql_exec(db, "PRAGMA synchronous=NORMAL");
    sql_exec(db, "CREATE TABLE IF NOT EXISTS devices("
                 "mac_key TEXT PRIMARY KEY, first_seen INTEGER, last_seen INTEGER,"
                 "best_rssi INTEGER, worst_rssi INTEGER, rssi_bin TEXT,"
                 "ssids TEXT, is_ap INTEGER, visit_count INTEGER DEFAULT 0)");
    sql_exec(db, "CREATE TABLE IF NOT EXISTS visits("
                 "id INTEGER PRIMARY KEY AUTOINCREMENT, mac_key TEXT,"
                 "start_ts INTEGER, end_ts INTEGER, ssid TEXT,"
                 "rssi_bin TEXT, is_ap INTEGER)");
    sql_exec(db, "CREATE INDEX IF NOT EXISTS idx_visits_start ON visits(start_ts)");

    /* 迁移：老表缺 visit_count 列则补上 */
    sqlite3_stmt *s = NULL;
    int has_visit = 0;
    if (sqlite3_prepare_v2(db->conn, "PRAGMA table_info(devices)", -1, &s, NULL) == SQLITE_OK) {
        while (sqlite3_step(s) == SQLITE_ROW) {
            const char *nm = (const char *)sqlite3_column_text(s, 1);
            if (nm && strcmp(nm, "visit_count") == 0) { has_visit = 1; break; }
        }
        sqlite3_finalize(s);
    }
    if (!has_visit)
        sql_exec(db, "ALTER TABLE devices ADD COLUMN visit_count INTEGER DEFAULT 0");
    return db;
}

void wp_db_close(struct wp_db *db)
{
    if (db) { sqlite3_close(db->conn); free(db); }
}

int wp_db_store_device(struct wp_db *db, const char *mk, time_t first, time_t last,
                       int best, int worst, int bin, const char *ssids, int is_ap)
{
    if (!db || !db->conn || !mk) return -1;
    const char *sql =
        "INSERT INTO devices(mac_key,first_seen,last_seen,best_rssi,worst_rssi,rssi_bin,ssids,is_ap)"
        " VALUES(?,?,?,?,?,?,?,?)"
        " ON CONFLICT(mac_key) DO UPDATE SET"
        " last_seen=excluded.last_seen,"
        " best_rssi=MAX(devices.best_rssi,excluded.best_rssi),"
        " worst_rssi=CASE WHEN devices.worst_rssi=0 THEN excluded.worst_rssi"
        "   ELSE MIN(devices.worst_rssi,excluded.worst_rssi) END,"
        " ssids=excluded.ssids,"
        " is_ap=MAX(devices.is_ap,excluded.is_ap)";
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(s, 1, mk, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)first);
    sqlite3_bind_int64(s, 3, (sqlite3_int64)last);
    sqlite3_bind_int(s, 4, best);
    sqlite3_bind_int(s, 5, worst);
    sqlite3_bind_text(s, 6, bin_str(bin), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 7, ssids ? ssids : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 8, is_ap);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int wp_db_store_visit(struct wp_db *db, const char *mk, time_t start, time_t end,
                      const char *ssid, int bin, int is_ap)
{
    if (!db || !db->conn || !mk) return -1;
    const char *sql =
        "INSERT INTO visits(mac_key,start_ts,end_ts,ssid,rssi_bin,is_ap)"
        " VALUES(?,?,?,?,?,?)";
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(s, 1, mk, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)start);
    sqlite3_bind_int64(s, 3, (sqlite3_int64)end);
    sqlite3_bind_text(s, 4, ssid ? ssid : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 5, bin_str(bin), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 6, is_ap);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    if (rc == SQLITE_DONE) {
        sqlite3_stmt *u = NULL;
        if (sqlite3_prepare_v2(db->conn,
                "UPDATE devices SET visit_count=COALESCE(visit_count,0)+1 WHERE mac_key=?",
                -1, &u, NULL) == SQLITE_OK) {
            sqlite3_bind_text(u, 1, mk, -1, SQLITE_TRANSIENT);
            sqlite3_step(u);
            sqlite3_finalize(u);
        }
    }
    return (rc == SQLITE_DONE) ? 0 : -1;
}

void wp_db_prune(struct wp_db *db, int days)
{
    if (!db || !db->conn) return;
    time_t cutoff = time(NULL) - (time_t)days * 86400;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db->conn, "DELETE FROM visits WHERE start_ts < ?",
                           -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, (sqlite3_int64)cutoff);
        sqlite3_step(s); sqlite3_finalize(s);
    }
    if (sqlite3_prepare_v2(db->conn,
            "DELETE FROM devices WHERE mac_key NOT IN (SELECT DISTINCT mac_key FROM visits)",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_step(s); sqlite3_finalize(s);
    }
}

/* 将任意字节串转义为合法的 JSON 字符串内容（不含外层引号）。 */
static void json_escape(const char *in, char *out, size_t outsz)
{
    if (!outsz) return;
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p && (o + 5) < outsz) {
        unsigned char c = *p;
        if (c == '"')      { out[o++] = '\\'; out[o++] = '"'; p++; }
        else if (c == '\\') { out[o++] = '\\'; out[o++] = '\\'; p++; }
        else if (c < 0x20 || c == 0x7f) { out[o++] = '?'; p++; }   /* 含 \n \r \t 等所有控制字符 → ?，避免断行 */
        else if (c < 0x80) { out[o++] = (char)c; p++; }
        else {
            /* 高字节：尝试按 UTF-8 多字节处理，非法则替换 ? */
            int need = 0;
            if      ((c & 0xE0) == 0xC0) need = 1;
            else if ((c & 0xF0) == 0xE0) need = 2;
            else if ((c & 0xF8) == 0xF0) need = 3;
            int ok = (need > 0);
            for (int i = 1; ok && i <= need; i++)
                if ((p[i] & 0xC0) != 0x80) ok = 0;
            if (ok && (o + (size_t)need + 1) < outsz) {
                out[o++] = (char)c;
                for (int i = 1; i <= need; i++) out[o++] = (char)p[i];
                p += need + 1;
            } else {
                out[o++] = '?'; p++;
            }
        }
    }
    out[o] = 0;
}

int wp_db_query_json(struct wp_db *db, const char *group, time_t since, time_t end)
{
    if (!db || !db->conn) return -1;
    printf("{\"group\":\"%s\",\"entries\":[", group);
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT mac_key,first_seen,last_seen,best_rssi,worst_rssi,rssi_bin,ssids,is_ap,visit_count"
        " FROM devices WHERE last_seen >= ?%s ORDER BY last_seen DESC LIMIT 500",
        end > 0 ? " AND last_seen < ?" : "");
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, (sqlite3_int64)since);
        if (end > 0) sqlite3_bind_int64(s, 2, (sqlite3_int64)end);
        int first = 1;
        while (sqlite3_step(s) == SQLITE_ROW) {
            if (!first) printf(",");
            first = 0;
            const char *mk = (const char *)sqlite3_column_text(s, 0);
            const char *bin = (const char *)sqlite3_column_text(s, 5);
            const char *ss = (const char *)sqlite3_column_text(s, 6);
            char bmk[64], bbin[16], bss[2048];
            json_escape(mk ? mk : "", bmk, sizeof(bmk));
            json_escape(bin ? bin : "", bbin, sizeof(bbin));
            json_escape(ss ? ss : "", bss, sizeof(bss));
            printf("\n {\"mac_key\":\"%s\",\"first\":%lld,\"last\":%lld,"
                   "\"best_rssi\":%d,\"worst_rssi\":%d,\"bin\":\"%s\","
                   "\"ssids\":\"%s\",\"is_ap\":%d,\"visits\":%d}",
                   bmk,
                   (long long)sqlite3_column_int64(s, 1),
                   (long long)sqlite3_column_int64(s, 2),
                   sqlite3_column_int(s, 3), sqlite3_column_int(s, 4),
                   bbin, bss, sqlite3_column_int(s, 7),
                   sqlite3_column_int(s, 8));
        }
        sqlite3_finalize(s);
    }
    printf("]}\n");
    return 0;
}

int wp_db_query_csv(struct wp_db *db, const char *group, time_t since, time_t end)
{
    if (!db || !db->conn) return -1;
    printf("mac_key,first,last,best_rssi,worst_rssi,rssi_bin,ssids,is_ap\n");
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT mac_key,first_seen,last_seen,best_rssi,worst_rssi,rssi_bin,ssids,is_ap,visit_count"
        " FROM devices WHERE last_seen >= ?%s ORDER BY last_seen DESC LIMIT 500",
        end > 0 ? " AND last_seen < ?" : "");
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, (sqlite3_int64)since);
        if (end > 0) sqlite3_bind_int64(s, 2, (sqlite3_int64)end);
        while (sqlite3_step(s) == SQLITE_ROW) {
            printf("%s,%lld,%lld,%d,%d,%s,%s,%d,%d\n",
                   (const char *)sqlite3_column_text(s, 0),
                   (long long)sqlite3_column_int64(s, 1),
                   (long long)sqlite3_column_int64(s, 2),
                   sqlite3_column_int(s, 3), sqlite3_column_int(s, 4),
                   (const char *)sqlite3_column_text(s, 5),
                   (const char *)sqlite3_column_text(s, 6),
                   sqlite3_column_int(s, 7), sqlite3_column_int(s, 8));
        }
        sqlite3_finalize(s);
    }
    return 0;
}
