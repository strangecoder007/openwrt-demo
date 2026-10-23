/*
 * aliyun_sign.h —— 阿里云 API 签名工具（RPC 风格 v1.0 签名）
 *
 * NLS 的服务端 API 用的是阿里云 RPC 签名：
 *   StringToSign = HTTPMethod + "&" + percentEncode("/") + "&" + percentEncode(CanonicalizedQueryString)
 *   Signature    = Base64( HMAC-SHA1( AccessKeySecret + "&", StringToSign ) )
 *
 * 这里只实现我们需要的 GET/POST 表单风格。
 */
#ifndef ALIYUN_SIGN_H
#define ALIYUN_SIGN_H

#include <stddef.h>

/*
 * 对参数做 RFC3986 percent-encode（阿里云要求）。
 *   src    原始字符串
 *   dst    输出
 *   dstlen 缓冲大小
 */
void aliyun_percent_encode(const char *src, char *dst, size_t dstlen);

/*
 * 计算 RPC 签名。
 *   method          "GET" 或 "POST"
 *   canonical_query 已按 key 字典序排好且已 percent-encode 的查询串
 *                   （不含末尾的 &Signature=...）
 *   ak_secret       AccessKeySecret
 *   out             输出 Base64 签名
 *   outlen          缓冲大小
 */
int aliyun_sign_rpc(const char *method,
                    const char *canonical_query,
                    const char *ak_secret,
                    char *out, size_t outlen);

/*
 * HMAC-SHA1 + Base64 的通用封装。
 */
int aliyun_hmac_sha1_b64(const void *key, size_t key_len,
                         const void *data, size_t data_len,
                         char *out, size_t outlen);

/*
 * SHA1 摘要（hex），用于 NLS 的 MD5/SHA1 相关校验。
 */
void aliyun_sha1_hex(const void *data, size_t len, char *out);

#endif /* ALIYUN_SIGN_H */
