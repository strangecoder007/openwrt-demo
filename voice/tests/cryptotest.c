/*
 * cryptotest.c —— 验证自实现的 crypto 与 aliyun_sign
 */
#include <stdio.h>
#include <string.h>
#include "crypto.h"
#include "aliyun_sign.h"

static int fails = 0;

static void check(const char *name, const char *got, const char *want)
{
    if (!strcmp(got, want)) {
        printf("  [PASS] %s\n", name);
    } else {
        printf("  [FAIL] %s\n    got : %s\n    want: %s\n", name, got, want);
        fails++;
    }
}

int main(void)
{
    char buf[4096];

    printf("=== SHA1 ===\n");
    sha1_hex("abc", 3, buf);
    check("sha1('abc')", buf, "a9993e364706816aba3e25717850c26c9cd0d89d");

    printf("\n=== HMAC-SHA1 + Base64 ===\n");
    aliyun_hmac_sha1_b64("key", 3, "data", 4, buf, sizeof(buf));
    check("hmac_sha1_b64('key','data')", buf, "EEFSxb/coHvGM+69RhmfAlXJ9J0=");

    printf("\n=== Base64 ===\n");
    base64_encode((const unsigned char *)"hello", 5, buf, sizeof(buf));
    check("b64('hello')", buf, "aGVsbG8=");
    base64_encode((const unsigned char *)"a", 1, buf, sizeof(buf));
    check("b64('a')", buf, "YQ==");
    base64_encode((const unsigned char *)"ab", 2, buf, sizeof(buf));
    check("b64('ab')", buf, "YWI=");

    printf("\n=== percent-encode ===\n");
    aliyun_percent_encode("2026-10-21T01:00:00Z", buf, sizeof(buf));
    check("encode(timestamp)", buf, "2026-10-21T01%3A00%3A00Z");
    aliyun_percent_encode("a/b c~d_e.f-g", buf, sizeof(buf));
    check("encode(mixed)", buf, "a%2Fb%20c~d_e.f-g");

    printf("\n=== RPC 签名 用例1 ===\n");
    {
        const char *canon =
            "AccessKeyId%3DtestAkId123%26Action%3DCreateToken";
        /* 注意：aliyun_sign_rpc 期望的是已 encode 好的规范串 */
        /* 这里直接喂 Python 生成的完整规范串 */
        const char *full_canon =
            "AccessKeyId%3DtestAkId123%26Action%3DCreateToken%26Format%3DJSON"
            "%26RegionId%3Dcn-shanghai%26SignatureMethod%3DHMAC-SHA1"
            "%26SignatureNonce%3D1234567890-12345%26SignatureVersion%3D1.0"
            "%26Timestamp%3D2026-10-21T01%253A00%253A00Z%26Version%3D2019-02-28";
        (void)canon;
        aliyun_sign_rpc("POST", full_canon, "testSecretABC", buf, sizeof(buf));
        check("sign case1", buf, "4iJTJdFvwKoHeBZnpr01vAH0zYo=");
    }

    printf("\n=== RPC 签名 用例2 ===\n");
    {
        const char *full_canon =
            "AccessKeyId%3Dabc%26Action%3DTest"
            "%26Timestamp%3D2026-01-01T00%253A00%253A00Z";
        aliyun_sign_rpc("GET", full_canon, "sec", buf, sizeof(buf));
        check("sign case2", buf, "+gjar2ZFpHXdvHcs1AMApcayx9s=");
    }

    printf("\n=== MD5 ===\n");
    md5_hex("abc", 3, buf);
    check("md5('abc')", buf, "900150983cd24fb0d6963f7d28e17f72");

    printf("\n结果: %s (%d 失败)\n", fails == 0 ? "全部通过" : "有失败", fails);
    return fails == 0 ? 0 : 1;
}
