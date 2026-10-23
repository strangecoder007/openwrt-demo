/*
 * crypto_sha256.h —— SHA256 / HMAC-SHA256
 */
#ifndef CRYPTO_SHA256_H
#define CRYPTO_SHA256_H

#include <stddef.h>

void sha256(const void *data, size_t len, unsigned char digest[32]);
void sha256_hex(const void *data, size_t len, char *out);

void hmac_sha256(const void *key, size_t key_len,
                 const void *data, size_t data_len,
                 unsigned char out[32]);
void hmac_sha256_hex(const void *key, size_t key_len,
                     const void *data, size_t data_len,
                     char *out);

#endif /* CRYPTO_SHA256_H */
