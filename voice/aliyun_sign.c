/*
 * aliyun_sign.c —— 阿里云 RPC 签名实现
 *
 * 用自实现的 HMAC-SHA1 / Base64（crypto.c），无 OpenSSL 依赖。
 */
#include "aliyun_sign.h"
#include "crypto.h"

#include <stdio.h>
#include <string.h>

/* ---------------- percent-encode ---------------- */
void aliyun_percent_encode(const char *src, char *dst, size_t dstlen)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t i = 0;

    if (dstlen == 0) return;

    for (; *src && i + 4 < dstlen; src++) {
        unsigned char c = (unsigned char)*src;
        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            dst[i++] = (char)c;
        } else {
            dst[i++] = '%';
            dst[i++] = hex[c >> 4];
            dst[i++] = hex[c & 0x0F];
        }
    }
    dst[i] = 0;
}

/* ---------------- HMAC-SHA1 + Base64 ---------------- */
int aliyun_hmac_sha1_b64(const void *key, size_t key_len,
                         const void *data, size_t data_len,
                         char *out, size_t outlen)
{
    unsigned char digest[20];
    hmac_sha1(key, key_len, data, data_len, digest);
    return (base64_encode(digest, 20, out, outlen) > 0) ? 0 : -1;
}

/* ---------------- SHA1 hex ---------------- */
void aliyun_sha1_hex(const void *data, size_t len, char *out)
{
    sha1_hex(data, len, out);
}

/* ---------------- RPC 签名 ---------------- */
int aliyun_sign_rpc(const char *method,
                    const char *canonical_query,
                    const char *ak_secret,
                    char *out, size_t outlen)
{
    char string_to_sign[4096];
    char key[256];
    int n;

    n = snprintf(string_to_sign, sizeof(string_to_sign),
                 "%s&%%2F&%s", method, canonical_query);
    if (n <= 0 || n >= (int)sizeof(string_to_sign))
        return -1;

    snprintf(key, sizeof(key), "%s&", ak_secret);

    return aliyun_hmac_sha1_b64(key, strlen(key),
                                string_to_sign, strlen(string_to_sign),
                                out, outlen);
}
