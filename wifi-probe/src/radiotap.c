#include "wifi-probe.h"
#include <string.h>

/* radiotap 字段的对齐/大小表（覆盖低索引常见字段） */
static const uint8_t ralign[] = {8,1,1,2,2,1,1,2,2,2,1,1,1,1,2,1,2,1,2,1,2,2,1,1,1,1,2,2,2,2,1,1};
static const uint8_t rsize[]  = {8,1,1,4,2,1,1,2,2,2,1,1,1,1,2,1,2,1,2,1,2,2,1,1,1,1,2,2,2,2,1,1};

static uint16_t rd_le16(const uint8_t *p){ return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd_le32(const uint8_t *p){
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int wp_radiotap_parse(const uint8_t *data, int len, struct wp_radiotap *rt)
{
    if (!data || !rt || len < 8) return -1;
    uint16_t rlen = rd_le16(data + 2);
    if (rlen > (uint16_t)len || rlen < 8) return -1;

    memset(rt, 0, sizeof(*rt));
    int off = 4;

    /* 先沿扩展位收集全部 present word */
    uint32_t pw[8]; int nw = 0;
    uint32_t cur = rd_le32(data + off); off += 4;
    pw[nw++] = cur;
    while (cur & 0x80000000u) {
        if ((off + 4) > rlen || nw >= 8) return -1;
        cur = rd_le32(data + off); off += 4;
        pw[nw++] = cur;
    }

    /* 再按全局字段序号枚举 */
    for (int w = 0; w < nw; w++) {
        for (int b = 0; b < 32; b++) {
            if (b == 31) continue;                 /* 扩展位，不是字段 */
            if (!(pw[w] & (1u << b))) continue;
            int idx = w * 32 + b;
            if (idx < (int)sizeof(ralign)) {
                int a = ralign[idx], s = rsize[idx];
                int pad = ((a - (off % a)) % a);
                off += pad;
                if ((off + s) > rlen) return -1;
                if (idx == 2)      { rt->rate = (int)data[off] * 500; rt->have_rate = 1; }
                else if (idx == 3) { rt->freq = (int)rd_le16(data + off); rt->have_freq = 1; }
                else if (idx == 5) { rt->rssi = (int)(int8_t)data[off]; rt->have_rssi = 1; }
                off += s;
            }
        }
    }
    return 0;
}
