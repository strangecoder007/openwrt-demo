#include "wifi-probe.h"
#include <string.h>

/* radiotap -> 802.11 头 -> probe body -> device. 纯函数，可 host 单测。 */
int wp_capture_process_frame(const uint8_t *bytes, int len, time_t now)
{
    if (!bytes || len < 8) return -1;
    int rtlen = bytes[2] | (bytes[3] << 8);
    if (rtlen < 8 || rtlen > len) return -1;

    struct wp_radiotap rt;
    memset(&rt, 0, sizeof(rt));
    if (wp_radiotap_parse(bytes, rtlen, &rt) != 0) return -1;
    if (len - rtlen < 24) return -1;

    struct wp_ieee80211 h;
    memset(&h, 0, sizeof(h));
    if (wp_ieee80211_parse(bytes + rtlen, len - rtlen, &h) != 0) return -1;
    if (h.type != 0) return 1;
    if (h.subtype != 4 && h.subtype != 8) return 1;

    struct wp_probe p;
    memset(&p, 0, sizeof(p));
    if (wp_parse_mgmt_body(h.body, h.body_len, h.subtype, &p) != 0) return -1;
    wp_dev_observe(h.sa, rt.have_rssi ? rt.rssi : 0, p.ssid, p.is_beacon, now);
    return 0;
}
