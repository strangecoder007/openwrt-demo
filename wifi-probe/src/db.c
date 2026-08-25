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

int wp_db_query_json(struct wp_db *db, const char *group, time_t since)
{
    if (!db || !db->conn) return -1;
    printf("{\"group\":\"%s\",\"entries\":[", group);
    const char *sql =
        "SELECT mac_key,first_seen,last_seen,best_rssi,worst_rssi,rssi_bin,ssids,is_ap,visit_count"
        " FROM devices WHERE last_seen >= ? ORDER BY last_seen DESC LIMIT 500";
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, (sqlite3_int64)since);
        int first = 1;
        while (sqlite3_step(s) == SQLITE_ROW) {
            if (!first) printf(",");
            first = 0;
            const char *mk = (const char *)sqlite3_column_text(s, 0);
            const char *bin = (const char *)sqlite3_column_text(s, 5);
            const char *ss = (const char *)sqlite3_column_text(s, 6);
            printf("\n {\"mac_key\":\"%s\",\"first\":%lld,\"last\":%lld,"
                   "\"best_rssi\":%d,\"worst_rssi\":%d,\"bin\":\"%s\","
                   "\"ssids\":\"%s\",\"is_ap\":%d,\"visits\":%d}",
                   mk ? mk : "",
                   (long long)sqlite3_column_int64(s, 1),
                   (long long)sqlite3_column_int64(s, 2),
                   sqlite3_column_int(s, 3), sqlite3_column_int(s, 4),
                   bin ? bin : "", ss ? ss : "", sqlite3_column_int(s, 7),
                   sqlite3_column_int(s, 8));
        }
        sqlite3_finalize(s);
    }
    printf("]}\n");
    return 0;
}

int wp_db_query_csv(struct wp_db *db, const char *group, time_t since)
{
    if (!db || !db->conn) return -1;
    printf("mac_key,first,last,best_rssi,worst_rssi,rssi_bin,ssids,is_ap\n");
    const char *sql =
        "SELECT mac_key,first_seen,last_seen,best_rssi,worst_rssi,rssi_bin,ssids,is_ap,visit_count"
        " FROM devices WHERE last_seen >= ? ORDER BY last_seen DESC LIMIT 500";
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, (sqlite3_int64)since);
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
