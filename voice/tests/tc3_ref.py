# -*- coding: utf-8 -*-
"""
腾讯云 TC3-HMAC-SHA256 签名参考实现

用途：生成测试向量，验证 C 实现（tests/tc3test.c）的正确性。

⚠️ 重要：参考实现必须是独立的。
如果用被测实现自己生成向量，对规范的同 一个误解会在两边抵消，
测试照样通过而实现实际是坏的。这份 Python 就是那个「独立的第三方」。
"""
import hashlib, hmac, time


def tc3_sign(secret_id, secret_key, service, host, payload, timestamp):
    """按腾讯云 TC3 规范计算签名。

    关键点（踩过坑）：CredentialScope 是 date/service/tc3_request，
    日期和 service 之间是**斜杠**，不是换行。
    """
    date = time.strftime("%Y-%m-%d", time.gmtime(timestamp))

    # 1. CanonicalRequest —— 只签 content-type 和 host
    signed_headers = "content-type;host"
    canonical_headers = (
        "content-type:application/json\n"
        f"host:{host}\n"
    )
    hashed_payload = hashlib.sha256(payload.encode()).hexdigest()

    canonical_request = (
        "POST\n"
        "/\n"
        "\n"
        f"{canonical_headers}\n"
        f"{signed_headers}\n"
        f"{hashed_payload}"
    )

    # 2. StringToSign —— date 和 service 用斜杠连接
    hashed_canonical = hashlib.sha256(canonical_request.encode()).hexdigest()
    string_to_sign = (
        "TC3-HMAC-SHA256\n"
        f"{timestamp}\n"
        f"{date}/{service}/tc3_request\n"
        f"{hashed_canonical}"
    )

    # 3. 三级派生密钥
    def _hmac(key, msg):
        return hmac.new(key, msg.encode(), hashlib.sha256).digest()

    k_date = _hmac(("TC3" + secret_key).encode(), date)
    k_service = _hmac(k_date, service)
    k_signing = _hmac(k_service, "tc3_request")

    # 4. 最终签名
    signature = hmac.new(k_signing, string_to_sign.encode(),
                         hashlib.sha256).hexdigest()

    auth = (
        f"TC3-HMAC-SHA256 Credential={secret_id}/{date}/{service}/tc3_request, "
        f"SignedHeaders={signed_headers}, Signature={signature}"
    )
    return auth, canonical_request, string_to_sign, signature, hashed_payload


if __name__ == "__main__":
    # ---- 官方文档示例的中间值核对（不涉及真实凭据）----
    # 目的：验证 CanonicalRequest 和 HashedPayload 的实现正确性
    official_payload = (
        '{"Limit": 1, "Filters": [{"Values": ["\\u672a\\u547d\\u540d"], '
        '"Name": "instance-name"}]}'
    )
    auth, cr, sts, sig, hp = tc3_sign(
        "YOUR_SECRET_ID", "YOUR_SECRET_KEY",
        "cvm", "cvm.tencentcloudapi.com", official_payload, 1551113065)

    print("=== 官方文档示例中间值核对 ===")
    print("HashedRequestPayload:")
    print("  got :", hp)
    print("  want: 35e9c5b0e3ae67532d3c9f17ead6c90222632e5b1ff7f6e89887f1398934f064")
    print("  match:", hp == "35e9c5b0e3ae67532d3c9f17ead6c90222632e5b1ff7f6e89887f1398934f064")
    print()
    print("HashedCanonicalRequest:")
    hcr = hashlib.sha256(cr.encode()).hexdigest()
    print("  got :", hcr)
    print("  want: 5ffe6a04c0664d6b969fab9a13bdab201d63ee709638e2749d62a09ca18d7031")
    print("  match:", hcr == "5ffe6a04c0664d6b969fab9a13bdab201d63ee709638e2749d62a09ca18d7031")
    print()

    # ---- 自己的测试用例（用假凭据，供 C 实现比对）----
    tests = [
        ("testSecretId", "testSecretKey", "asr", "asr.tencentcloudapi.com",
         '{"EngSerViceType":"16k_zh","SourceType":1,"VoiceFormat":"pcm",'
         '"Data":"AAAA","DataLen":3}',
         1729500000),
        ("testSecretId", "testSecretKey", "tts", "tts.tencentcloudapi.com",
         '{"Text":"你好","SessionId":"abc123","Volume":5,"Speed":0,'
         '"ProjectId":0,"ModelType":1,"VoiceType":1001,'
         '"PrimaryLanguage":1,"SampleRate":16000,"Codec":"pcm"}',
         1729500000),
    ]

    print("=== 测试向量（供 tc3test.c 比对）===")
    for i, (sid, skey, svc, host, payload, ts) in enumerate(tests, 1):
        auth, cr, sts, sig, hp = tc3_sign(sid, skey, svc, host, payload, ts)
        print(f"[{i}] service={svc}  model-args host={host} ts={ts}")
        print(f"    signature = {sig}")
        print(f"    auth      = {auth}")
        print()
