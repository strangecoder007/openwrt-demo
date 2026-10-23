/*
 * sha1_hmac.c —— 纯 C 实现的 SHA1 / HMAC-SHA1 / Base64 / MD5
 *
 * 不依赖 OpenSSL，避免 glibc/musl ABI 冲突。
 * 代码量小、无外部依赖，适合嵌入式。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ======================== SHA1 ======================== */
typedef struct {
    uint32_t state[5];
    uint64_t count;         /* 位数 */
    unsigned char buffer[64];
} sha1_ctx;

#define ROL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_init(sha1_ctx *c)
{
    c->state[0] = 0x67452301;
    c->state[1] = 0xEFCDAB89;
    c->state[2] = 0x98BADCFE;
    c->state[3] = 0x10325476;
    c->state[4] = 0xC3D2E1F0;
    c->count = 0;
}

static void sha1_transform(uint32_t state[5], const unsigned char block[64])
{
    uint32_t w[80], a, b, c, d, e, t;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i*4] << 24) |
               ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8) |
               ((uint32_t)block[i*4+3]);
    }
    for (i = 16; i < 80; i++)
        w[i] = ROL32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    a = state[0]; b = state[1]; c = state[2]; d = state[3]; e = state[4];

    for (i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | ((~b) & d);           k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                      k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);    k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                      k = 0xCA62C1D6; }

        t = ROL32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL32(b, 30); b = a; a = t;
    }

    state[0] += a; state[1] += b; state[2] += c;
    state[3] += d; state[4] += e;
}

static void sha1_update(sha1_ctx *c, const void *data, size_t len)
{
    const unsigned char *p = data;
    size_t idx = (size_t)((c->count >> 3) & 63);
    size_t i;

    c->count += (uint64_t)len << 3;

    if (idx) {
        size_t fill = 64 - idx;
        if (len < fill) {
            memcpy(c->buffer + idx, p, len);
            return;
        }
        memcpy(c->buffer + idx, p, fill);
        sha1_transform(c->state, c->buffer);
        p += fill;
        len -= fill;
    }

    for (i = 0; i + 64 <= len; i += 64)
        sha1_transform(c->state, p + i);

    if (i < len)
        memcpy(c->buffer, p + i, len - i);
}

static void sha1_final(sha1_ctx *c, unsigned char digest[20])
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

    sha1_update(c, pad, padlen + 8);

    for (i = 0; i < 5; i++) {
        digest[i*4]   = (unsigned char)(c->state[i] >> 24);
        digest[i*4+1] = (unsigned char)(c->state[i] >> 16);
        digest[i*4+2] = (unsigned char)(c->state[i] >> 8);
        digest[i*4+3] = (unsigned char)(c->state[i]);
    }
}

void sha1(const void *data, size_t len, unsigned char digest[20])
{
    sha1_ctx c;
    sha1_init(&c);
    sha1_update(&c, data, len);
    sha1_final(&c, digest);
}

void sha1_hex(const void *data, size_t len, char *out)
{
    unsigned char d[20];
    int i;
    sha1(data, len, d);
    for (i = 0; i < 20; i++)
        sprintf(out + i * 2, "%02x", d[i]);
    out[40] = 0;
}

/* ======================== HMAC-SHA1 ======================== */
void hmac_sha1(const void *key, size_t key_len,
               const void *data, size_t data_len,
               unsigned char out[20])
{
    unsigned char k[64];
    unsigned char ipad[64], opad[64];
    unsigned char inner[20];
    sha1_ctx c;
    size_t i;

    memset(k, 0, sizeof(k));
    if (key_len > 64) {
        sha1(key, key_len, k);
    } else {
        memcpy(k, key, key_len);
    }

    for (i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5C;
    }

    sha1_init(&c);
    sha1_update(&c, ipad, 64);
    sha1_update(&c, data, data_len);
    sha1_final(&c, inner);

    sha1_init(&c);
    sha1_update(&c, opad, 64);
    sha1_update(&c, inner, 20);
    sha1_final(&c, out);
}

/* ======================== Base64 ======================== */
static const char b64tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64_encode(const unsigned char *in, size_t inlen, char *out, size_t outlen)
{
    size_t i, o = 0;

    if (outlen < (inlen + 2) / 3 * 4 + 1)
        return -1;

    for (i = 0; i + 2 < inlen; i += 3) {
        out[o++] = b64tab[in[i] >> 2];
        out[o++] = b64tab[((in[i] & 0x03) << 4) | (in[i+1] >> 4)];
        out[o++] = b64tab[((in[i+1] & 0x0F) << 2) | (in[i+2] >> 6)];
        out[o++] = b64tab[in[i+2] & 0x3F];
    }

    if (i < inlen) {
        out[o++] = b64tab[in[i] >> 2];
        if (i + 1 < inlen) {
            out[o++] = b64tab[((in[i] & 0x03) << 4) | (in[i+1] >> 4)];
            out[o++] = b64tab[(in[i+1] & 0x0F) << 2];
        } else {
            out[o++] = b64tab[(in[i] & 0x03) << 4];
            out[o++] = '=';
        }
        out[o++] = '=';
    }

    out[o] = 0;
    return (int)o;
}

/* ======================== MD5（NLS 部分接口可能用到）======================== */
typedef struct {
    uint32_t state[4];
    uint64_t count;
    unsigned char buffer[64];
} md5_ctx;

#define F(x,y,z) (((x) & (y)) | ((~x) & (z)))
#define G(x,y,z) (((x) & (z)) | ((y) & (~z)))
#define H(x,y,z) ((x) ^ (y) ^ (z))
#define I(x,y,z) ((y) ^ ((x) | (~z)))
#define ROTL(x,n) (((x) << (n)) | ((x) >> (32-(n))))

static void md5_transform(uint32_t state[4], const unsigned char block[64])
{
    static const uint32_t K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };
    static const int S[64] = {
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
    };
    uint32_t M[16], A, B, C, D, f, g, tmp;
    int i;

    for (i = 0; i < 16; i++)
        M[i] = (uint32_t)block[i*4] | ((uint32_t)block[i*4+1] << 8) |
               ((uint32_t)block[i*4+2] << 16) | ((uint32_t)block[i*4+3] << 24);

    A = state[0]; B = state[1]; C = state[2]; D = state[3];

    for (i = 0; i < 64; i++) {
        uint32_t fval;
        int gidx;
        if (i < 16)      { fval = F(B,C,D); gidx = i; }
        else if (i < 32) { fval = G(B,C,D); gidx = (5*i + 1) % 16; }
        else if (i < 48) { fval = H(B,C,D); gidx = (3*i + 5) % 16; }
        else             { fval = I(B,C,D); gidx = (7*i) % 16; }

        tmp = D; D = C; C = B;
        B = B + ROTL(A + fval + K[i] + M[gidx], S[i]);
        A = tmp;
    }

    state[0] += A; state[1] += B; state[2] += C; state[3] += D;
}

static void md5_init(md5_ctx *c)
{
    c->state[0] = 0x67452301;
    c->state[1] = 0xefcdab89;
    c->state[2] = 0x98badcfe;
    c->state[3] = 0x10325476;
    c->count = 0;
}

static void md5_update(md5_ctx *c, const void *data, size_t len)
{
    const unsigned char *p = data;
    size_t idx = (size_t)((c->count >> 3) & 63);
    size_t i;

    c->count += (uint64_t)len << 3;

    if (idx) {
        size_t fill = 64 - idx;
        if (len < fill) { memcpy(c->buffer + idx, p, len); return; }
        memcpy(c->buffer + idx, p, fill);
        md5_transform(c->state, c->buffer);
        p += fill; len -= fill;
    }

    for (i = 0; i + 64 <= len; i += 64)
        md5_transform(c->state, p + i);

    if (i < len) memcpy(c->buffer, p + i, len - i);
}

static void md5_final(md5_ctx *c, unsigned char digest[16])
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
        pad[padlen + i] = (unsigned char)(bits >> (8 * i));

    md5_update(c, pad, padlen + 8);

    for (i = 0; i < 4; i++) {
        digest[i*4]   = (unsigned char)(c->state[i]);
        digest[i*4+1] = (unsigned char)(c->state[i] >> 8);
        digest[i*4+2] = (unsigned char)(c->state[i] >> 16);
        digest[i*4+3] = (unsigned char)(c->state[i] >> 24);
    }
}

void md5_hex(const void *data, size_t len, char *out)
{
    md5_ctx c;
    unsigned char d[16];
    int i;
    md5_init(&c);
    md5_update(&c, data, len);
    md5_final(&c, d);
    for (i = 0; i < 16; i++)
        sprintf(out + i * 2, "%02x", d[i]);
    out[32] = 0;
}
