#include <stdio.h>
#include <string.h>
#include "wifi-probe.h"

static int chk(const char *name, const uint8_t *rt, int n,
               int want_rssi, int want_freq, int want_rate)
{
    struct wp_radiotap o;
    memset(&o, 0, sizeof(o));
    if (wp_radiotap_parse(rt, n, &o) != 0) { printf("%s rc\n", name); return 1; }
    if (!o.have_rssi || o.rssi != want_rssi) { printf("%s rssi %d\n", name, o.rssi); return 2; }
    if (!o.have_freq || o.freq != want_freq) { printf("%s freq %d\n", name, o.freq); return 3; }
    if (!o.have_rate || o.rate != want_rate) { printf("%s rate %d\n", name, o.rate); return 4; }
    printf("%s OK: rssi=%d freq=%d rate=%d\n", name, o.rssi, o.freq, o.rate);
    return 0;
}

int main(void)
{
    /* 无扩展 present word */
    uint8_t a[] = {
        0x00,0x00, 0x0f,0x00, 0x2e,0x00,0x00,0x00,
        0x01, 0x02, 0x9e,0x09,0x00,0x00, 0xa0
    };
    int r = chk("noext", a, sizeof(a), -96, 2462, 1000);
    if (r) return r;

    /* 带扩展 present word（word0 设 bit31，word1 无位） */
    uint8_t b[] = {
        0x00,0x00, 0x13,0x00,          /* len=19 */
        0x2c,0x00,0x00,0x80,            /* present word0: bits 2,3,5,31 */
        0x00,0x00,0x00,0x00,            /* present word1 */
        0x02,                           /* RATE */
        0x00,                           /* pad to align CHANNEL */
        0x9e,0x09,                      /* CHANNEL freq=2462 */
        0x00,0x00,                      /* channel flags */
        0xa0                            /* DBM_ANTSIGNAL=-96 */
    };
    r = chk("ext", b, sizeof(b), -96, 2462, 1000);
    if (r) return r;

    /* 板子 AR9271 实测一帧 probe 的 radiotap（36B，含扩展 present word） */
    uint8_t rt_real[] = {
        0x00,0x00,0x24,0x00, 0x2f,0x40,0x00,0xa0,
        0x20,0x08,0x00,0x00, 0x00,0x00,0x00,0x00,
        0x59,0xa4,0x2d,0x09, 0x04,0x00,0x00,0x00,
        0x10,0x02,0x9e,0x09, 0xa0,0x00,0xb1,0x00,
        0x00,0x00,0xaa,0x00
    };
    r = chk("real", rt_real, sizeof(rt_real), -79, 2462, 1000);
    if (r) return r;

    return 0;
}
