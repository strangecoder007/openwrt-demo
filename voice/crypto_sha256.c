/*
 * crypto_sha256.c —— SHA256 与 HMAC-SHA256（纯 C，无依赖）
 *
 * 腾讯云 TC3-HMAC-SHA256 签名需要。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct {
    uint32_t state[8];
    uint64_t count;         /* 位数 */
    unsigned char buffer[64];
} sha256_ctx;

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x,y,z)  (((x) & (y)) ^ ((~(x)) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x)   (ROR32(x, 2) ^ ROR32(x, 13) ^ ROR32(x, 22))
#define BSIG1(x)   (ROR32(x, 6) ^ ROR32(x, 11) ^ ROR32(x, 25))
#define SSIG0(x)   (ROR32(x, 7) ^ ROR32(x, 18) ^ ((x) >> 3))
#define SSIG1(x)   (ROR32(x, 17) ^ ROR32(x, 19) ^ ((x) >> 10))

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void sha256_init(sha256_ctx *c)
{
    c->state[0] = 0x6a09e667;
    c->state[1] = 0xbb67ae85;
    c->state[2] = 0x3c6ef372;
    c->state[3] = 0xa54ff53a;
    c->state[4] = 0x510e527f;
    c->state[5] = 0x9b05688c;
    c->state[6] = 0x1f83d9ab;
    c->state[7] = 0x5be0cd19;
    c->count = 0;
}

static void sha256_transform(uint32_t state[8], const unsigned char block[64])
{
    uint32_t w[64], a, b, c, d, e, f, g, h, t1, t2;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i*4] << 24) |
               ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8) |
               ((uint32_t)block[i*4+3]);
    }
    for (i = 16; i < 64; i++)
        w[i] = SSIG1(w[i-2]) + w[i-7] + SSIG0(w[i-15]) + w[i-16];

    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    for (i = 0; i < 64; i++) {
        t1 = h + BSIG1(e) + CH(e,f,g) + K256[i] + w[i];
        t2 = BSIG0(a) + MAJ(a,b,c);
        h = g; g = f; f = e;
        e = d + t1;
        d = c; c = b; b = a;
        a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void sha256_update(sha256_ctx *c, const void *data, size_t len)
{
    const unsigned char *p = data;
    size_t idx = (size_t)((c->count >> 3) & 63);
    size_t i;

    c->count += (uint64_t)len << 3;

    if (idx) {
        size_t fill = 64 - idx;
        if (len < fill) { memcpy(c->buffer + idx, p, len); return; }
        memcpy(c->buffer + idx, p, fill);
        sha256_transform(c->state, c->buffer);
        p += fill;
        len -= fill;
    }

    for (i = 0; i + 64 <= len; i += 64)
        sha256_transform(c->state, p + i);

    if (i < len)
        memcpy(c->buffer, p + i, len - i);
}

static void sha256_final(sha256_ctx *c, unsigned char digest[32])
{
    unsigned char pad[72];
    size_t idx = (size_t)((c->count >> 3) & 63);
    uint64_t bits = c->count;
    size_t padlen;
    int i;

    pad[0] = 0x80;
    padlen = (idx < 56) ? (56 - idx) : (120 - idx);
    memset(pad + 1, 0, padlen - 1);
    for (i = 0; i < 8; i++)
        pad[padlen + i] = (unsigned char)(bits >> (56 - 8 * i));

    sha256_update(c, pad, padlen + 8);

    for (i = 0; i < 8; i++) {
        digest[i*4]   = (unsigned char)(c->state[i] >> 24);
        digest[i*4+1] = (unsigned char)(c->state[i] >> 16);
        digest[i*4+2] = (unsigned char)(c->state[i] >> 8);
        digest[i*4+3] = (unsigned char)(c->state[i]);
    }
}

void sha256(const void *data, size_t len, unsigned char digest[32])
{
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, digest);
}

void sha256_hex(const void *data, size_t len, char *out)
{
    unsigned char d[32];
    int i;
    sha256(data, len, d);
    for (i = 0; i < 32; i++)
        sprintf(out + i * 2, "%02x", d[i]);
    out[64] = 0;
}

void hmac_sha256(const void *key, size_t key_len,
                 const void *data, size_t data_len,
                 unsigned char out[32])
{
    unsigned char k[64];
    unsigned char ipad[64], opad[64];
    unsigned char inner[32];
    sha256_ctx c;
    size_t i;

    memset(k, 0, sizeof(k));
    if (key_len > 64)
        sha256(key, key_len, k);
    else
        memcpy(k, key, key_len);

    for (i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5C;
    }

    sha256_init(&c);
    sha256_update(&c, ipad, 64);
    sha256_update(&c, data, data_len);
    sha256_final(&c, inner);

    sha256_init(&c);
    sha256_update(&c, opad, 64);
    sha256_update(&c, inner, 32);
    sha256_final(&c, out);
}

void hmac_sha256_hex(const void *key, size_t key_len,
                     const void *data, size_t data_len,
                     char *out)
{
    unsigned char d[32];
    int i;
    hmac_sha256(key, key_len, data, data_len, d);
    for (i = 0; i < 32; i++)
        sprintf(out + i * 2, "%02x", d[i]);
    out[64] = 0;
}
