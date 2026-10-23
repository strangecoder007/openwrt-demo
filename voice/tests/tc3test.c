/*
 * tc3test.c —— 验证 TC3 签名（v3：date/service 用斜杠拼接）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "crypto_sha256.h"
#include "tencent_sign.h"

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
    char buf[1024];

    printf("=== SHA256 ===\n");
    sha256_hex("abc", 3, buf);
    check("sha256('abc')", buf,
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    printf("\n=== HMAC-SHA256 (RFC4231) ===\n");
    {
        unsigned char key[20];
        memset(key, 0x0b, 20);
        hmac_sha256_hex(key, 20, "Hi There", 8, buf);
        check("hmac case1", buf,
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    }

    printf("\n=== TC3 asr ===\n");
    {
        const char *p = "{\"EngSerViceType\":\"16k_zh\",\"SourceType\":1,\"VoiceFormat\":\"pcm\",\"Data\":\"AAAA\",\"DataLen\":3}";
        if (tc3_sign("testSecretId", "testSecretKey", "asr",
                     "asr.tencentcloudapi.com", "SentenceRecognition",
                     "2019-06-14", "ap-shanghai", p, strlen(p),
                     1729500000, buf, sizeof(buf)) == 0)
            check("tc3 asr", buf,
                  "TC3-HMAC-SHA256 Credential=testSecretId/2024-10-21/asr/tc3_request, "
                  "SignedHeaders=content-type;host, "
                  "Signature=037ba558113c5649ee059d3acfc9d1658a8e58c8204f2c50508b14e16c0e4452");
        else { printf("  [FAIL] tc3_sign err\n"); fails++; }
    }

    printf("\n=== TC3 tts ===\n");
    {
        const char *p = "{\"Text\":\"你好\",\"SessionId\":\"abc123\",\"Volume\":5,\"Speed\":0,\"ProjectId\":0,\"ModelType\":1,\"VoiceType\":1001,\"PrimaryLanguage\":1,\"SampleRate\":16000,\"Codec\":\"pcm\"}";
        if (tc3_sign("testSecretId", "testSecretKey", "tts",
                     "tts.tencentcloudapi.com", "TextToVoice",
                     "2019-08-23", "ap-shanghai", p, strlen(p),
                     1729500000, buf, sizeof(buf)) == 0)
            check("tc3 tts", buf,
                  "TC3-HMAC-SHA256 Credential=testSecretId/2024-10-21/tts/tc3_request, "
                  "SignedHeaders=content-type;host, "
                  "Signature=cd920d94e688de81f2f6694a6b86552bf3badd2b5d809c1923f40ccddbd9674e");
        else { printf("  [FAIL] tc3_sign err\n"); fails++; }
    }

    /* 真实凭据验证用例：需要用你自己的 SecretId/SecretKey 跑，
     * 期望值依赖具体密钥，所以这里不写死。
     * 用法：
     *   export TENCENT_SECRET_ID=...  TENCENT_SECRET_KEY=...
     *   ./tc3test --real
     * 验证方式：拿这个签名去调 cvm DescribeRegions，返回 200 即通过。 */
    if (getenv("TENCENT_SECRET_ID") && getenv("TENCENT_SECRET_KEY")) {
        printf("\n=== TC3 cvm（真实凭据）===\n");
        const char *p = "{}";
        char sid[128], skey[128];
        snprintf(sid, sizeof(sid), "%s", getenv("TENCENT_SECRET_ID"));
        snprintf(skey, sizeof(skey), "%s", getenv("TENCENT_SECRET_KEY"));
        if (tc3_sign(sid, skey, "cvm", "cvm.tencentcloudapi.com",
                     "DescribeRegions", "2017-03-12", "", p, strlen(p),
                     1792670012, buf, sizeof(buf)) == 0) {
            printf("  [INFO] 生成签名成功（请用该签名实际调用接口验证）\n");
            printf("         %s\n", buf);
        } else {
            printf("  [FAIL] tc3_sign err\n");
            fails++;
        }
    } else {
        printf("\n=== TC3 cvm（真实凭据）已跳过 ===\n");
        printf("  设 TENCENT_SECRET_ID / TENCENT_SECRET_KEY 后可验证\n");
    }

    printf("\n结果: %s (%d 失败)\n", fails == 0 ? "全部通过" : "有失败", fails);
    return fails == 0 ? 0 : 1;
}
