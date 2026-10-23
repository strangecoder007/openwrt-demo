/*
 * tencent_sign.h —— 腾讯云 TC3-HMAC-SHA256 签名
 *
 * 签名流程：
 *   1. CanonicalRequest =
 *        HTTPMethod \n CanonicalURI \n CanonicalQueryString \n
 *        CanonicalHeaders \n SignedHeaders \n HashedRequestPayload
 *   2. StringToSign =
 *        "TC3-HMAC-SHA256" \n Timestamp \n Date \n
 *        HexEncode(SHA256(CanonicalRequest))
 *   3. SecretDate    = HMAC_SHA256("TC3" + SecretKey, Date)
 *      SecretService = HMAC_SHA256(SecretDate, Service)
 *      SecretSigning = HMAC_SHA256(SecretService, "tc3_request")
 *   4. Signature = HexEncode(HMAC_SHA256(SecretSigning, StringToSign))
 *   5. Authorization =
 *        "TC3-HMAC-SHA256 Credential=<SecretId>/<Date>/<Service>/tc3_request, "
 *        "SignedHeaders=<...>, Signature=<Signature>"
 */
#ifndef TENCENT_SIGN_H
#define TENCENT_SIGN_H

#include <stddef.h>

#define TC_HOST_MAX 128

/*
 * 计算腾讯云签名并生成完整 Authorization 头。
 *
 *   secret_id    SecretId
 *   secret_key   SecretKey
 *   service      服务名（asr / tts）
 *   host         API 域名（asr.tencentcloudapi.com）
 *   action       X-TC-Action（SentenceRecognition / TextToVoice）
 *   version      X-TC-Version（2019-06-14 / 2019-08-23）
 *   region       X-TC-Region，可为空串
 *   payload      JSON 请求体
 *   payload_len  长度
 *   timestamp    当前 Unix 时间戳（秒）
 *   auth_out     输出 Authorization 头值
 *   auth_len     缓冲大小
 *
 * 返回 0 成功。
 */
int tc3_sign(const char *secret_id, const char *secret_key,
             const char *service, const char *host,
             const char *action, const char *version, const char *region,
             const char *payload, size_t payload_len,
             long timestamp,
             char *auth_out, size_t auth_len);

#endif /* TENCENT_SIGN_H */
