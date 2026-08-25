#include <stdio.h>
#include <string.h>
#include "wifi-probe.h"

struct wp_config g_config;
int g_running = 1;

int main(void)
{
    g_config.anonymize_mac = 1;
    strcpy(g_config.mac_salt, "salt");
    g_config.rssi_near = -55;
    g_config.rssi_mid = -65;
    g_config.session_gap = 300;

    uint8_t sa[6] = {0x11,0x22,0x33,0x44,0x55,0x66};
    wp_dev_init();

    struct wp_device *d1 = wp_dev_observe(sa, -60, "Home", 0, 100);
    struct wp_device *d2 = wp_dev_observe(sa, -58, "Home", 0, 200);
    if (!d1 || d1 != d2) { printf("same dev fail\n"); return 1; }
    if (d1->visit_count != 1 || d2->rssi_bin != WP_RSSI_MID) {
        printf("bin/visit fail %d bin=%d\n", d1->visit_count, d2->rssi_bin); return 2;
    }

    char key[WP_MAC_KEY_LEN];
    wp_anon_mac(sa, "salt", key);
    if (strcmp(d2->mac_key, key) != 0) { printf("anon key fail %s %s\n", d2->mac_key, key); return 3; }

    struct wp_visit_event ev[4];
    int n = wp_dev_collect_ended_visits(1000, ev, 4);
    if (n != 1) { printf("collect fail n=%d\n", n); return 4; }
    if (ev[0].start != 100 || ev[0].end != 1000) {
        printf("visit ts fail %ld %ld\n", (long)ev[0].start, (long)ev[0].end); return 5;
    }

    printf("device OK key=%s bin=%s visits=%d\n",
           d2->mac_key, wp_rssi_bin_name(d2->rssi_bin), d2->visit_count);
    return 0;
}
