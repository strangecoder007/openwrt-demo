#include "wifi-probe.h"
#include <string.h>

int wp_ieee80211_parse(const uint8_t *data, int len, struct wp_ieee80211 *h)
{
    if (!data || !h || len < 24) return -1;
    uint16_t fc = (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
    h->type = (fc >> 2) & 0x3;
    h->subtype = (fc >> 4) & 0xf;
    memcpy(h->da, data + 4, 6);
    memcpy(h->sa, data + 10, 6);
    memcpy(h->bssid, data + 16, 6);
    h->have_sa = 1; h->have_da = 1; h->have_bssid = 1;
    h->body = data + 24;
    h->body_len = len - 24;
    return 0;
}
