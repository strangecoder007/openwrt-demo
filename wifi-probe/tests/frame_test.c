#include <stdio.h>
#include <string.h>
#include "wifi-probe.h"

struct wp_config g_config;
int g_running = 1;

static struct wp_device *s_first;
static void count_cb(struct wp_device *d, void *u){ (void)u; if (!s_first) s_first = d; }

int main(void)
{
    g_config.anonymize_mac = 1;
    strcpy(g_config.mac_salt, "salt");
    g_config.rssi_near = -55;
    g_config.rssi_mid = -65;
    g_config.session_gap = 300;

    /* radiotap(15) + 802.11 probe-req(24 头 + 14 body) = 53 */
    uint8_t rt[] = {0x00,0x00,0x0f,0x00, 0x2e,0x00,0x00,0x00, 0x01,0x02,
                    0x9e,0x09,0x00,0x00, 0xa0};
    uint8_t body[] = {0,0,0,0, 0x00,0x05,'H','e','l','l','o', 0x01,0x01,0x82};
    uint8_t f[sizeof(rt) + 24 + sizeof(body)];
    memcpy(f, rt, sizeof(rt));
    uint8_t *w = f + sizeof(rt);
    memset(w, 0, 24);
    w[0] = 0x40;                            /* probe req */
    memset(w + 4, 0xff, 6);
    uint8_t sa[6] = {0xaa,0xbb,0xcc,0xdd,0xee,0xff};
    memcpy(w + 10, sa, 6);
    memset(w + 16, 0xff, 6);
    memcpy(w + 24, body, sizeof(body));

    wp_dev_init();
    s_first = NULL;
    int rc = wp_capture_process_frame(f, sizeof(f), 100);
    if (rc != 0) { printf("proc rc=%d\n", rc); return 1; }
    wp_dev_foreach(count_cb, NULL);
    if (!s_first) { printf("no dev\n"); return 2; }
    if (s_first->best_rssi != -96 || s_first->rssi_bin != WP_RSSI_FAR) {
        printf("rssi %d bin %d\n", s_first->best_rssi, s_first->rssi_bin); return 3;
    }
    printf("capture OK key=%s rssi=%d bin=%s\n",
           s_first->mac_key, s_first->best_rssi, wp_rssi_bin_name(s_first->rssi_bin));
    return 0;
}
