#ifndef TCP_REASM_H
#define TCP_REASM_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

/* 只保留流的“第一个数据记录”最多这么多字节。
 * 对 TLS 来说就是一个完整 record（5 字节头 + payload，通常 ClientHello < 4KB）；
 * 对 HTTP 就是请求行+头部这一小段。限制在几个 KB，板子上内存可控。 */
#define REASM_MAX_LEN 8192

/* 一条乱序到达、暂时接不上的 TCP 段（被暂存在 pending 链表里等缺口闭合）。 */
struct tcp_seg {
    uint32_t        off;    /* 该段在“流偏移域”里的起始偏移（相对 base_seq） */
    uint8_t        *data;   /* 该段的负载副本（malloc） */
    size_t          len;    /* 负载长度 */
    struct tcp_seg *next;   /* 链表，按 off 升序 */
};

/*
 * 每条 TCP 连接的“首记录”重装器。
 *
 * 核心思路：把一段 TCP 流抽象成一个“以 base_seq 为 0 的一段连续字节域”。
 *   offset = 段首seq - base_seq，就得到这段数据在流里从 0 开始的偏移。
 * 这样就能判断它是“接在已连续区域后面”（直接拼接）、还是“中间有缺口”
 * （先放 pending 等缺口闭合）、还是“重复/重传”（直接丢弃）。
 *
 * base_seq 的取值：
 *   - 抓到 SYN 时用 ISN+1（SYN 占一个序号，所以首个数据字节 seq = ISN+1），
 *     这是最准确的情况；
 *   - 如果抓包起点错过了 SYN（比如中途才接入），就退回用“看到的第一个数据段
 *     的 seq 当 base_seq”，假设抓包正好从记录头开始——和旧的“拿首个数据包当
 *     ClientHello”行为一致，只是一种启发式。
 *
 * 数据布局：
 *   - buf[0..used_len) 是当前已连续的“前缀”，一定从离线0开始、无空洞；
 *   - pending 是那些 off > used_len（有缺口）的乱序段，按 off 升序挂着；
 *   - 每来一段，先把连续前缀撑大，再尝试把 pending 里能接上的并进前缀
 *     （merge_pending），直到缺口重新闭合。
 * 一旦拿到完整的第一条记录（调用方判定），就 tcp_reasm_mark_consumed() 置
 * done 并释放所有缓冲，后续数据不再处理。这样内存只被“第一个记录”占住，
 * 不会因为一条长连接把整条流都缓存下来。
 */
typedef struct tcp_reasm {
    uint32_t        base_seq;   /* 映射到偏移0的序号 */
    int             have_base;  /* base_seq 是否已确定 */
    uint8_t        *buf;        /* 已连续的流前缀 [0, used_len) */
    size_t          used_len;   /* 前缀长度 */
    size_t          cap;        /* buf 分配容量 */
    struct tcp_seg *pending;    /* 乱序段，按 off 升序 */
    int             done;       /* 首记录已消费，停止缓存 */
    time_t          last_seen;  /* 最近一次喂数据时间，用于超时淘汰 */
} tcp_reasm;

void tcp_reasm_init(tcp_reasm *r);
void tcp_reasm_free(tcp_reasm *r);

/* Record the server-side initial sequence number learned from a SYN */
void tcp_reasm_set_isn(tcp_reasm *r, uint32_t isn);

/*
 * Feed one client->server data segment. Returns 1 if the contiguous prefix
 * grew (caller may re-check for a complete record), 0 otherwise.
 */
int tcp_reasm_feed(tcp_reasm *r, uint32_t seq, const uint8_t *payload, int len);

/* Return pointers to the current contiguous prefix */
const uint8_t *tcp_reasm_data(const tcp_reasm *r, size_t *len);

/* Stop buffering and release the buffers (first record handled) */
void tcp_reasm_mark_consumed(tcp_reasm *r);

#endif /* TCP_REASM_H */
