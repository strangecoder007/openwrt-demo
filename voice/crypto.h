/*
 * crypto.h —— 纯 C 实现的摘要/编码
 */
#ifndef CRYPTO_H
#define CRYPTO_H

#include <stddef.h>

void sha1(const void *data, size_t len, unsigned char digest[20]);
void sha1_hex(const void *data, size_t len, char *out);

void hmac_sha1(const void *key, size_t key_len,
               const void *data, size_t data_len,
               unsigned char out[20]);

int  base64_encode(const unsigned char *in, size_t inlen,
                   char *out, size_t outlen);

void md5_hex(const void *data, size_t len, char *out);

#endif /* CRYPTO_H */
