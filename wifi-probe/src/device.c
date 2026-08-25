#include "wifi-probe.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define DEV_BUCKETS 256
static struct wp_device *g_dev[DEV_BUCKETS];

void wp_dev_init(void){ memset(g_dev, 0, sizeof(g_dev)); }

int wp_rssi_to_bin(int rssi, int near, int mid)
{
    if (rssi == 0) return WP_RSSI_UNKNOWN;
    if (rssi >= near) return WP_RSSI_NEAR;
    if (rssi >= mid)  return WP_RSSI_MID;
    return WP_RSSI_FAR;
}

const char *wp_rssi_bin_name(int bin)
{
    switch (bin) {
    case WP_RSSI_NEAR: return "near";
    case WP_RSSI_MID:  return "mid";
    case WP_RSSI_FAR:  return "far";
    default:           return "unknown";
    }
}

void wp_anon_mac(const uint8_t *sa, const char *salt, char *out)
{
    uint64_t h = 1469598103934665603ULL;
    const uint8_t *p = (const uint8_t *)salt;
    while (*p) { h ^= *p++; h *= 1099511628211ULL; }
    for (int i = 0; i < 6; i++) { h ^= sa[i]; h *= 1099511628211ULL; }
    snprintf(out, WP_MAC_KEY_LEN, "%016llx", (unsigned long long)h);
}

static int bucket(const char *k)
{
    unsigned h = 0; while (*k) h = h * 31 + *k++;
    return h % DEV_BUCKETS;
}
static struct wp_device *find_by_key(const char *k)
{
    int b = bucket(k);
    for (struct wp_device *d = g_dev[b]; d; d = d->next)
        if (strcmp(d->mac_key, k) == 0) return d;
    return NULL;
}
static struct wp_device *dev_get_or_create(const char *k, time_t now)
{
    struct wp_device *d = find_by_key(k);
    if (d) return d;
    d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    strncpy(d->mac_key, k, sizeof(d->mac_key) - 1);
    d->first_seen = d->last_seen = d->visit_start = now;
    d->in_visit = 1; d->visit_count = 1;
    d->best_rssi = -127; d->worst_rssi = 0; d->rssi_bin = WP_RSSI_UNKNOWN;
    int b = bucket(k); d->next = g_dev[b]; g_dev[b] = d;
    return d;
}

struct wp_device *wp_dev_observe(const uint8_t *sa, int rssi, const char *ssid,
                                 int is_ap, time_t now)
{
    char k[WP_MAC_KEY_LEN];
    if (g_config.anonymize_mac)
        wp_anon_mac(sa, g_config.mac_salt, k);
    else
        snprintf(k, sizeof(k), "%02x%02x%02x%02x%02x%02x",
                 sa[0], sa[1], sa[2], sa[3], sa[4], sa[5]);
    struct wp_device *d = dev_get_or_create(k, now);
    if (!d) return NULL;
    if (rssi != 0) {
        if (rssi > d->best_rssi) d->best_rssi = rssi;
        if (d->worst_rssi == 0 || rssi < d->worst_rssi) d->worst_rssi = rssi;
        d->rssi_bin = wp_rssi_to_bin(rssi, g_config.rssi_near, g_config.rssi_mid);
    }
    if (is_ap) d->is_ap = 1;
    if (ssid && ssid[0]) {
        char tmp[1200];
        snprintf(tmp, sizeof(tmp), "%s%c%s", d->ssids[0] ? d->ssids : "",
                 d->ssids[0] ? ',' : ' ', ssid);
        if (strlen(tmp) < sizeof(d->ssids)) strcpy(d->ssids, tmp);
    }
    d->last_seen = now;
    if (!d->in_visit) { d->in_visit = 1; d->visit_start = now; d->visit_count++; }
    return d;
}

int wp_dev_collect_ended_visits(time_t now, struct wp_visit_event *evs, int max)
{
    int n = 0;
    for (int b = 0; b < DEV_BUCKETS; b++)
        for (struct wp_device *d = g_dev[b]; d; d = d->next) {
            if (!d->in_visit || d->is_ap) continue;
            if (now - d->last_seen > g_config.session_gap && n < max) {
                snprintf(evs[n].mac_key, sizeof(evs[n].mac_key), "%s", d->mac_key);
                evs[n].start = d->visit_start; evs[n].end = now;
                evs[n].rssi_bin = d->rssi_bin; evs[n].is_ap = d->is_ap;
                char *sp = d->ssids; while (*sp == ' ') sp++;
                char *sc = strchr(sp, ',');
                int l = sc ? (int)(sc - sp) : (int)strlen(sp);
                if (l > (int)sizeof(evs[n].ssid) - 1) l = sizeof(evs[n].ssid) - 1;
                memcpy(evs[n].ssid, sp, l); evs[n].ssid[l] = 0;
                d->in_visit = 0; n++;
            }
        }
    return n;
}

void wp_dev_foreach(void (*cb)(struct wp_device *, void *), void *user)
{
    for (int b = 0; b < DEV_BUCKETS; b++)
        for (struct wp_device *d = g_dev[b]; d; d = d->next) cb(d, user);
}
