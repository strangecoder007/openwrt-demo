#include <stdio.h>
#include <string.h>
#include "wifi-probe.h"

int main(void)
{
    uint8_t body[] = {0,0,0,0,
                      0x00,0x05,'H','e','l','l','o',
                      0x01,0x01,0x82};
    struct wp_probe p;
    memset(&p, 0, sizeof(p));
    if (wp_parse_mgmt_body(body, sizeof(body), 4, &p) != 0) { printf("rc\n"); return 1; }
    if (p.ssid_len != 5 || memcmp(p.ssid, "Hello", 5) != 0) { printf("ssid err len=%d\n", p.ssid_len); return 2; }
    if (p.ssid_broadcast != 0) { printf("broadcast err\n"); return 3; }
    if (p.n_rates != 1 || p.rates[0] != 0x82) { printf("rate err n=%d\n", p.n_rates); return 4; }
    printf("probe OK ssid=%s rate=0x%02x\n", p.ssid, p.rates[0]);
    return 0;
}
