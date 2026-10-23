# -*- coding: utf-8 -*-
"""生成派生密钥的参考值"""
import hmac, hashlib

skey = "YOUR_SECRET_KEY_HERE"   # 填自己的 SecretKey
date = "2026-10-22"
service = "cvm"

k = ("TC3" + skey).encode()
kd = hmac.new(k, date.encode(), hashlib.sha256).digest()
ks = hmac.new(kd, service.encode(), hashlib.sha256).digest()
ksg = hmac.new(ks, b"tc3_request", hashlib.sha256).digest()

print("k_date    =", kd.hex())
print("k_service =", ks.hex())
print("k_signing =", ksg.hex())

# 最终签名
sts = "TC3-HMAC-SHA256\n1792670012\n2026-10-22/cvm/tc3_request\nb7511c2a57e10458e52fe57af7b28c25796fc538db71eef3b256d94528d8834d"
sig = hmac.new(ksg, sts.encode(), hashlib.sha256).hexdigest()
print("signature =", sig)
