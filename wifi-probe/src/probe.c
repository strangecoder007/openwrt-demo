#include "wifi-probe.h"
#include <string.h>

int wp_parse_mgmt_body(const uint8_t *body, int len, int subtype, struct wp_probe *p)
{
    if (!body || !p) return -1;
    memset(p, 0, sizeof(*p));
    p->is_beacon = (subtype == 8);
    if (len < 4) return -1;                 /* capability(2) + listen interval(2) */

    int off = 4;
    while (off + 2 <= len) {
        uint8_t id = body[off];
        uint8_t ilen = body[off + 1];
        if (off + 2 + ilen > len) break;
        if (id == 0) {                      /* SSID */
            int cl = ilen;
            if (cl > WP_SSID_MAX - 1) cl = WP_SSID_MAX - 1;
            memcpy(p->ssid, body + off + 2, cl);
            p->ssid[cl] = 0;
            p->ssid_len = cl;
            if (ilen == 0) p->ssid_broadcast = 1;
        } else if (id == 1 || id == 50) {   /* rates / extended rates */
            for (int i = 0; i < ilen && p->n_rates < 32; i++)
                p->rates[p->n_rates++] = body[off + 2 + i];
        }
        off += 2 + ilen;
    }
    return 0;
}
