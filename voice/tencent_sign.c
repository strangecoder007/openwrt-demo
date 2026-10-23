/*
 * tencent_sign.c 修正：不再由调用方传 payload_len，
 * 而是要求调用方保证传入的 payload 与实际发送的 body 完全一致。
 *
 * 关键教训：wget --post-file 会在文件末尾补一个换行（如果文件本身没有换行）。
 * 因此这里必须用 --post-data 或确保文件和内存一致。
 *
 * 更稳妥的方案：改用 --post-data（字符串），避免文件换行问题。
 * 但 --post-data 对二进制不安全，JSON 是文本所以没问题。
 */
#include "tencent_sign.h"
#include "crypto_sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void ts_to_date(long ts, char *out, size_t outlen)
{
    time_t t = (time_t)ts;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(out, outlen, "%Y-%m-%d", &tmv);
}

int tc3_sign(const char *secret_id, const char *secret_key,
             const char *service, const char *host,
             const char *action, const char *version, const char *region,
             const char *payload, size_t payload_len,
             long timestamp,
             char *auth_out, size_t auth_len)
{
    char date[16];
    char hashed_payload[65];
    char canonical_headers[512];
    char signed_headers[64];
    char canonical_request[2048];
    char hashed_canonical[65];
    char string_to_sign[1024];

    unsigned char k_date[32], k_service[32], k_signing[32];
    char secret_date_key[256];
    unsigned char signature[32];
    char signature_hex[65];
    int n;

    (void)version;
    (void)region;

    ts_to_date(timestamp, date, sizeof(date));
    sha256_hex(payload, payload_len, hashed_payload);

    /* 只签 content-type 和 host。
     * 注意：Content-Type 必须与实际发送的完全一致，包括 charset 部分。
     * 实测用 "application/json"（不带 charset）可以成功，
     * 带 "; charset=utf-8" 会导致签名校验失败（wget 发送时可能被改写）。 */
    snprintf(canonical_headers, sizeof(canonical_headers),
             "content-type:application/json\n"
             "host:%s\n", host);
    snprintf(signed_headers, sizeof(signed_headers), "content-type;host");

    n = snprintf(canonical_request, sizeof(canonical_request),
                 "POST\n"
                 "/\n"
                 "\n"
                 "%s"
                 "\n"
                 "%s\n"
                 "%s",
                 canonical_headers, signed_headers, hashed_payload);
    if (n <= 0 || n >= (int)sizeof(canonical_request))
        return -1;

    sha256_hex(canonical_request, (size_t)n, hashed_canonical);

    /* 关键：CredentialScope = date + "/" + service + "/tc3_request"，
     * 日期和 service 之间是斜杠，不是换行。
     * 之前写成 "%s\n%s/tc3_request" 导致签名与官方不符。 */
    n = snprintf(string_to_sign, sizeof(string_to_sign),
                 "TC3-HMAC-SHA256\n%ld\n%s/%s/tc3_request\n%s",
                 timestamp, date, service, hashed_canonical);
    if (n <= 0 || n >= (int)sizeof(string_to_sign))
        return -1;

    snprintf(secret_date_key, sizeof(secret_date_key), "TC3%s", secret_key);
    hmac_sha256(secret_date_key, strlen(secret_date_key), date, strlen(date), k_date);
    hmac_sha256(k_date, 32, service, strlen(service), k_service);
    hmac_sha256(k_service, 32, "tc3_request", 11, k_signing);
    hmac_sha256(k_signing, 32, string_to_sign, strlen(string_to_sign), signature);

    {
        int i;
        for (i = 0; i < 32; i++)
            sprintf(signature_hex + i * 2, "%02x", signature[i]);
        signature_hex[64] = 0;
    }

    n = snprintf(auth_out, auth_len,
                 "TC3-HMAC-SHA256 Credential=%s/%s/%s/tc3_request, "
                 "SignedHeaders=%s, Signature=%s",
                 secret_id, date, service, signed_headers, signature_hex);
    if (n <= 0 || n >= (int)auth_len)
        return -1;

    (void)action;
    return 0;
}
