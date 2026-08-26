#include "wifi-probe.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>

struct wp_config g_config;
int g_running = 1;

static void signal_handler(int sig){ (void)sig; g_running = 0; }

static void window_bounds(const char *period, time_t *since, time_t *end)
{
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    tmv.tm_hour = tmv.tm_min = tmv.tm_sec = 0;
    tmv.tm_isdst = -1;
    time_t today0 = mktime(&tmv);   /* 系统时区的今天 00:00 */
    *since = today0; *end = now;   /* 默认 today：今天0点 → 现在 */
    if (!period) return;
    if (strcmp(period, "yesterday") == 0) { *since = today0 - 86400; *end = today0; }
    else if (strcmp(period, "week") == 0)  { *since = now - 7 * 86400; *end = 0; }
    else if (strcmp(period, "month") == 0) { *since = now - 30 * 86400; *end = 0; }
    else {
        long v = atol(period);
        if (v > 0) { *since = now - (time_t)v; *end = 0; }
    }
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -d                daemon\n"
        "  -c FORMAT         output format json|csv (query)\n"
        "  -g GROUP          group by device|ssid|rssi|visits (query)\n"
        "  -t PERIOD         today|yesterday|week|month|secs (query)\n"
        "  -i IFACE          monitor interface (default mon0)\n"
        "  -D PATH           db path (default %s)\n"
        "  -G SECS           session gap (default %d)\n"
        "  -C SECS           commit interval (default 60)\n"
        "  -R DAYS           retention days (default %d)\n"
        "  -A 0|1            anonymize MAC (default 1)\n"
        "  -S SALT           mac salt\n"
        "  -f all|probe      capture filter (default all)\n"
        "  -V                version\n"
        "  -h                help\n",
        prog, WP_DEFAULT_DB_PATH, WP_DEFAULT_GAP, WP_DEFAULT_RETENTION);
}

static void daemon_loop(void)
{
    struct wp_db *db = wp_db_open(g_config.db_path);
    if (!db) { fprintf(stderr, "wifi-probe: cannot open db %s\n", g_config.db_path); return; }

    pthread_t tid;
    if (wp_capture_init(g_config.iface, g_config.capture_filter) == 0) {
        pthread_create(&tid, NULL, wp_capture_run, NULL);
        pthread_detach(tid);
        fprintf(stderr, "wifi-probe: capture on %s\n", g_config.iface);
    } else {
        fprintf(stderr, "wifi-probe: capture init failed\n");
    }

    time_t last_commit = 0, last_prune = 0;
    while (g_running) {
        time_t now = time(NULL);
        if (now - last_commit >= g_config.commit_interval) {
            struct wp_visit_event ev[64];
            int n = wp_dev_collect_ended_visits(now, ev, 64);
            for (int i = 0; i < n; i++) {
                wp_db_store_device(db, ev[i].mac_key, ev[i].start, ev[i].end,
                                   ev[i].best_rssi, ev[i].worst_rssi,
                                   ev[i].rssi_bin, ev[i].ssid, ev[i].is_ap);
                wp_db_store_visit(db, ev[i].mac_key, ev[i].start, ev[i].end,
                                  ev[i].ssid, ev[i].rssi_bin, ev[i].is_ap);
            }
            last_commit = now;
        }
        if (now - last_prune >= 3600) { wp_db_prune(db, g_config.retention_days); last_prune = now; }
        sleep(5);
    }
    wp_capture_stop();
    wp_db_close(db);
}

int main(int argc, char **argv)
{
    memset(&g_config, 0, sizeof(g_config));
    strcpy(g_config.iface, WP_DEFAULT_IFACE);
    strcpy(g_config.db_path, WP_DEFAULT_DB_PATH);
    strcpy(g_config.mac_salt, WP_SALT_DEFAULT);
    g_config.channel = 11;
    g_config.anonymize_mac = 1;
    g_config.session_gap = WP_DEFAULT_GAP;
    g_config.rssi_near = WP_DEFAULT_RSSI_NEAR;
    g_config.rssi_mid = WP_DEFAULT_RSSI_MID;
    g_config.retention_days = WP_DEFAULT_RETENTION;
    g_config.commit_interval = 60;
    strcpy(g_config.capture_filter, "all");

    int daemon = 0;
    char fmt[16] = "json", group[16] = "device", period[16] = "today";
    if (wp_parse_args(argc, argv, &g_config, &daemon, fmt, group, period) != 0) {
        usage(argv[0]); return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (daemon) {
        wp_dev_init();
        daemon_loop();
    } else {
        struct wp_db *db = wp_db_open(g_config.db_path);
        if (!db) { fprintf(stderr, "wifi-probe: no data at %s\n", g_config.db_path); return 1; }
        time_t since, end;
        window_bounds(period, &since, &end);
        if (strcmp(fmt, "csv") == 0) wp_db_query_csv(db, group, since, end);
        else                         wp_db_query_json(db, group, since, end);
        wp_db_close(db);
    }
    return 0;
}
