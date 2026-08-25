#include <stdio.h>
#include <string.h>
#include "wifi-probe.h"

int main(void)
{
    uint8_t f[26]; /* 24字节 MAC 头 + 2字节 body */
    memset(f, 0, sizeof(f));
    f[0] = 0x40; f[1] = 0x00;               /* type=0(mgt), subtype=4(probe req) */
    memset(f + 4, 0xff, 6);                  /* DA = broadcast */
    uint8_t sa[6] = {0xaa,0xbb,0xcc,0xdd,0xee,0xff};
    memcpy(f + 10, sa, 6);
    memset(f + 16, 0xff, 6);                 /* BSSID */
    struct wp_ieee80211 h;
    memset(&h, 0, sizeof(h));
    if (wp_ieee80211_parse(f, sizeof(f), &h) != 0) { printf("parse rc\n"); return 1; }
    if (h.type != 0 || h.subtype != 4) { printf("type/sub err %d/%d\n", h.type, h.subtype); return 2; }
    if (!h.have_sa || memcmp(h.sa, sa, 6) != 0) { printf("sa err\n"); return 3; }
    if (h.body_len != 2) { printf("body len %d\n", h.body_len); return 4; }
    printf("ieee80211 OK type=%d sub=%d body=%d\n", h.type, h.subtype, h.body_len);
    return 0;
}
