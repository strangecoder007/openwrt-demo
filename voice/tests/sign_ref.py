# -*- coding: utf-8 -*-
"""用 Python 独立实现阿里云签名，生成测试向量用于验证 C 实现"""
import hmac, hashlib, base64, urllib.parse

def percent_encode(s):
    # 阿里云规范：只保留 A-Za-z0-9-_.~
    return urllib.parse.quote(s, safe='-_.~')

def sign_rpc(method, params: dict, ak_secret: str):
    # 1. 按 key 排序
    items = sorted(params.items())
    # 2. 拼规范串（key=value 都已 encode）
    canon = "&".join(f"{percent_encode(k)}={percent_encode(v)}" for k, v in items)
    # 3. StringToSign
    sts = f"{method}&{percent_encode('/')}&{percent_encode(canon)}"
    # 4. HMAC-SHA1
    key = (ak_secret + "&").encode()
    sig = hmac.new(key, sts.encode(), hashlib.sha1).digest()
    # 5. Base64
    return base64.b64encode(sig).decode(), canon, sts

# 测试用例（假凭据）
tests = [
    ("POST",
     {"AccessKeyId": "testAkId123",
      "Action": "CreateToken",
      "Format": "JSON",
      "RegionId": "cn-shanghai",
      "SignatureMethod": "HMAC-SHA1",
      "SignatureNonce": "1234567890-12345",
      "SignatureVersion": "1.0",
      "Timestamp": "2026-10-21T01:00:00Z",
      "Version": "2019-02-28"},
     "testSecretABC"),
    ("GET",
     {"AccessKeyId": "abc",
      "Action": "Test",
      "Timestamp": "2026-01-01T00:00:00Z"},
     "sec"),
]

for i, (m, p, sec) in enumerate(tests, 1):
    sig, canon, sts = sign_rpc(m, p, sec)
    print(f"=== 用例 {i} ===")
    print(f"method   : {m}")
    print(f"ak_secret: {sec}")
    print(f"canonical: {canon}")
    print(f"StringToSign: {sts}")
    print(f"Signature: {sig}")
    print()

# 额外：验证 base64 和 HMAC 的基本行为
print("=== 基础向量 ===")
print("HMAC-SHA1('key','data') =",
      base64.b64encode(hmac.new(b'key', b'data', hashlib.sha1).digest()).decode())
print("SHA1('abc') hex =", hashlib.sha1(b'abc').hexdigest())
