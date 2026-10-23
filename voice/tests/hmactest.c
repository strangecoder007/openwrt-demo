#include <stdio.h>
#include <string.h>
#include "crypto_sha256.h"

int main(void)
{
    char hex[65];
    unsigned char out[32];

    printf("=== SHA256 基础 ===\n");
    sha256_hex("abc", 3, hex);
    printf("sha256('abc') = %s\n", hex);
    printf("  期望         ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n");
    printf("  匹配: %s\n\n",
           !strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") ? "YES" : "NO");

    printf("=== HMAC-SHA256 RFC4231 测试用例 1 ===\n");
    /* key = 0x0b x 20, data = "Hi There" */
    {
        unsigned char key[20];
        memset(key, 0x0b, 20);
        hmac_sha256(key, 20, "Hi There", 8, out);
        for (int i = 0; i < 32; i++) sprintf(hex + i*2, "%02x", out[i]);
        printf("got  %s\n", hex);
        printf("want b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7\n");
        printf("匹配: %s\n\n",
               !strcmp(hex, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7") ? "YES" : "NO");
    }

    printf("=== HMAC-SHA256 RFC4231 测试用例 2 ===\n");
    /* key = "Jefe", data = "what do ya want for nothing?" */
    {
        hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, out);
        for (int i = 0; i < 32; i++) sprintf(hex + i*2, "%02x", out[i]);
        printf("got  %s\n", hex);
        printf("want 5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843\n");
        printf("匹配: %s\n\n",
               !strcmp(hex, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843") ? "YES" : "NO");
    }

    printf("=== 派生密钥链（TC3）===\n");
    {
        const char *skey = "YOUR_SECRET_KEY_HERE";
        const char *date = "2026-10-22";
        const char *svc  = "cvm";
        unsigned char kd[32], ks[32], ksg[32];
        char key[256];

        snprintf(key, sizeof(key), "TC3%s", skey);
        printf("SecretDateKey = %s\n", key);

        hmac_sha256(key, strlen(key), date, strlen(date), kd);
        for (int i = 0; i < 32; i++) sprintf(hex + i*2, "%02x", kd[i]);
        printf("k_date    = %s\n", hex);

        hmac_sha256(kd, 32, svc, strlen(svc), ks);
        for (int i = 0; i < 32; i++) sprintf(hex + i*2, "%02x", ks[i]);
        printf("k_service = %s\n", hex);

        hmac_sha256(ks, 32, "tc3_request", 11, ksg);
        for (int i = 0; i < 32; i++) sprintf(hex + i*2, "%02x", ksg[i]);
        printf("k_signing = %s\n", hex);
        printf("\n(用 tests/keyderive.py 生成对应参考值)\n");
        printf("k_date    = ?\n");
        printf("k_service = ?\n");
        printf("k_signing = ?\n");
    }
    return 0;
}
