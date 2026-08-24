#ifndef IPREASM_H
#define IPREASM_H

#include <stdint.h>
#include <stddef.h>

/* 单个重组数据报最多保留的 IP 负载字节数。
 * 我们只处理 DNS 这类 UDP 分片（A/AAAA 大响应），几个 KB 足够。超过就放弃。 */
#define IPFRAG_MAX_LEN 8192

/*
 * 喂一个 IP 分片。当整条数据报被拼完整时，把重组后的 payload 拷进 outbuf
 * 并返回 1；否则返回 0（调用方继续等后续分片）。
 *
 * 参数：
 *   is_v6        : 1=IPv6, 0=IPv4（影响 src/dst 长度 16/4）
 *   src/dst       : 源/目的地址（网络序，长度随 is_v6）
 *   id            : IP 标识（v4 的 id 或 v6 分片头的 identification）
 *   proto         : 最终传输层协议（TCP/UDP），用于把不同协议分开重组
 *   payload       : 本分片去掉 IP 头（及 IPv6 分片头）后的负载字节
 *   payload_len   : 负载长度
 *   offset_bytes  : 本分片的字节偏移（由调用方从分片头算好，v4 是 MF+偏移域*8）
 *   mf            : More-Fragments 标志（1=后面还有分片，0=最后一片）
 *   outbuf/cap    : 输出缓冲区及其容量
 *   outlen        : 输出重组后的长度
 */
int ipfrag_feed(int is_v6,
                const uint8_t *src, const uint8_t *dst, uint32_t id,
                uint8_t proto,
                const uint8_t *payload, int payload_len,
                int offset_bytes, int mf,
                uint8_t *outbuf, size_t outbuf_cap, size_t *outlen);

/* 清理闲置超过 IPFRAG_TIMEOUT 的重组条目（由 daemon 主循环周期调用）。 */
void ipfrag_age(void);

#endif /* IPREASM_H */
