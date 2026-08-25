#ifndef WIFI_PROBE_H
#define WIFI_PROBE_H

#include <stdint.h>
#include <time.h>

#define WP_VERSION "0.1.0"
#define WP_DEFAULT_IFACE    "mon0"
#define WP_DEFAULT_DB_PATH  "/var/lib/wifi-probe"
#define WP_DEFAULT_GAP      300
#define WP_DEFAULT_RSSI_NEAR -55
#define WP_DEFAULT_RSSI_MID  -65
#define WP_DEFAULT_RETENTION 30
#define WP_MAX_DEVICES      4096
#define WP_MAC_KEY_LEN      17   /* 16 hex + NUL */
#define WP_SSID_MAX         256
#define WP_SALT_DEFAULT     "wifi-probe-static-salt-v1"

enum wp_rssi_bin { WP_RSSI_UNKNOWN = 0, WP_RSSI_NEAR, WP_RSSI_MID, WP_RSSI_FAR };

struct wp_config {
    char iface[64];
    int  channel;
    int  anonymize_mac;
    char mac_salt[64];
    int  session_gap;
    int  rssi_near;
    int  rssi_mid;
    int  retention_days;
    int  commit_interval;
    char db_path[128];
    char capture_filter[16];  /* "all" | "probe" */
    int  verbose;
};

extern struct wp_config g_config;
extern int g_running;

/* ---- radiotap ---- */
struct wp_radiotap {
    int have_rssi; int rssi;   /* dBm */
    int have_freq; int freq;   /* MHz */
    int have_rate; int rate;   /* kbps (500kbps units * 500) */
};
int wp_radiotap_parse(const uint8_t *data, int len, struct wp_radiotap *rt);

/* ---- ieee80211 MAC header ---- */
struct wp_ieee80211 {
    int type, subtype;
    uint8_t sa[6], da[6], bssid[6];
    int have_sa, have_da, have_bssid;
    const uint8_t *body;
    int body_len;
};
int wp_ieee80211_parse(const uint8_t *data, int len, struct wp_ieee80211 *h);

/* ---- probe / beacon body ---- */
struct wp_probe {
    char ssid[WP_SSID_MAX]; int ssid_len;
    int ssid_broadcast;
    int is_beacon;
    int rates[32]; int n_rates;
};
int wp_parse_mgmt_body(const uint8_t *body, int len, int subtype, struct wp_probe *p);

/* ---- device aggregate ---- */
struct wp_device {
    char mac_key[WP_MAC_KEY_LEN];
    time_t first_seen, last_seen, visit_start;
    int visit_count, in_visit;
    int best_rssi, worst_rssi, rssi_bin, is_ap;
    char ssids[1024];
    struct wp_device *next;
};
struct wp_visit_event {
    char mac_key[WP_MAC_KEY_LEN];
    time_t start, end;
    char ssid[WP_SSID_MAX];
    int rssi_bin, is_ap;
};
void wp_dev_init(void);
struct wp_device *wp_dev_observe(const uint8_t *sa, int rssi, const char *ssid, int is_ap, time_t now);
int wp_dev_collect_ended_visits(time_t now, struct wp_visit_event *evs, int max);
void wp_dev_foreach(void (*cb)(struct wp_device*, void*), void *user);
int wp_rssi_to_bin(int rssi, int near, int mid);
const char *wp_rssi_bin_name(int bin);
void wp_anon_mac(const uint8_t *sa, const char *salt, char *out);

/* ---- capture ---- */
int wp_capture_init(const char *iface, const char *capture_filter);
void *wp_capture_run(void *arg);
void wp_capture_stop(void);
int wp_capture_active(void);
int wp_capture_process_frame(const uint8_t *bytes, int len, time_t now);

/* ---- db ---- */
struct wp_db;
struct wp_db *wp_db_open(const char *path);
void wp_db_close(struct wp_db *db);
int wp_db_store_device(struct wp_db *db, const char *mac_key, time_t first, time_t last,
                       int best, int worst, int bin, const char *ssids, int is_ap);
int wp_db_store_visit(struct wp_db *db, const char *mac_key, time_t start, time_t end,
                      const char *ssid, int bin, int is_ap);
void wp_db_prune(struct wp_db *db, int days);
int wp_db_query_json(struct wp_db *db, const char *group, time_t since);
int wp_db_query_csv(struct wp_db *db, const char *group, time_t since);

/* ---- cli ---- */
int wp_parse_args(int argc, char **argv, struct wp_config *cfg, int *daemon,
                  char *fmt, char *group, char *period);

#endif
