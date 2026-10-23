# -*- coding: utf-8 -*-
"""测 hy3 是否支持 OpenAI 标准 function calling"""
import json, urllib.request, urllib.error, ssl, sys, io, os

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

API_KEY = os.environ.get("LLM_API_KEY", "sk-xxxxxxxx")
BASE = os.environ.get("LLM_BASE_URL", "https://tokenhub.tencentmaas.com/v1")

TOOLS = [{
    "type": "function",
    "function": {
        "name": "get_weather",
        "description": "查询指定城市的当前天气",
        "parameters": {
            "type": "object",
            "properties": {
                "city": {"type": "string", "description": "城市名，如 成都"}
            },
            "required": ["city"]
        }
    }
}]

def chat(model, messages, tools=None, tool_choice=None):
    body = {
        "model": model,
        "messages": messages,
        "stream": False,
        "max_tokens": 2048,
    }
    if tools:
        body["tools"] = tools
    if tool_choice:
        body["tool_choice"] = tool_choice

    data = json.dumps(body, ensure_ascii=False).encode()
    req = urllib.request.Request(
        BASE + "/chat/completions", data=data, method="POST",
        headers={
            "Authorization": "Bearer " + API_KEY,
            "Content-Type": "application/json",
        })
    try:
        with urllib.request.urlopen(req, timeout=60,
                                    context=ssl.create_default_context()) as r:
            return 200, json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()[:600]
    except Exception as e:
        return None, str(e)

def show(tag, code, resp):
    print("--- %s  HTTP=%s" % (tag, code))
    if code != 200 or not isinstance(resp, dict):
        print("   ", str(resp)[:400])
        return None
    ch = resp.get("choices", [{}])[0]
    msg = ch.get("message", {})
    fr = ch.get("finish_reason")
    print("    finish_reason =", fr)
    print("    content       =", repr(msg.get("content", ""))[:120])
    if msg.get("reasoning_content"):
        print("    reasoning_len =", len(msg["reasoning_content"]))
    if "tool_calls" in msg:
        print("    ★ tool_calls  =", json.dumps(msg["tool_calls"], ensure_ascii=False)[:500])
    else:
        print("    tool_calls    = (无)")
    print("    usage         =", resp.get("usage", {}))
    return resp

# ---------- 测试 1：hy3 + tools ----------
print("=" * 70)
print("测试 1: hy3 + tools（问需要查天气的问题）")
print("=" * 70)
msgs = [
    {"role": "system", "content": "你是一个中文语音助手。需要实时数据时调用工具。"},
    {"role": "user", "content": "成都今天天气怎么样？"},
]
code, resp = chat("hy3", msgs, TOOLS)
r1 = show("hy3+tools", code, resp)

# ---------- 测试 2：hy3 + tool_choice=required ----------
print()
print("=" * 70)
print("测试 2: hy3 + tool_choice=required（强制调用）")
print("=" * 70)
code, resp = chat("hy3", msgs, TOOLS, "required")
r2 = show("hy3+required", code, resp)

# ---------- 测试 3：hunyuan-role-latest + tools ----------
print()
print("=" * 70)
print("测试 3: hunyuan-role-latest + tools")
print("=" * 70)
code, resp = chat("hunyuan-role-latest", msgs, TOOLS)
r3 = show("role-latest+tools", code, resp)

# ---------- 测试 4：如果有 tool_calls，回传结果看能否继续 ----------
print()
print("=" * 70)
print("测试 4: 回传工具结果，看模型能否继续生成")
print("=" * 70)

def find_tool_calls(r):
    if not isinstance(r, dict): return None
    ch = r.get("choices", [{}])[0]
    return ch.get("message", {}).get("tool_calls")

tc = find_tool_calls(r1) or find_tool_calls(r2) or find_tool_calls(r3)
if not tc:
    print("前面没有 tool_calls，跳过")
else:
    model_used = "hy3"
    msgs2 = list(msgs)
    msgs2.append({
        "role": "assistant",
        "content": None,
        "tool_calls": tc,
    })
    # 解析参数
    for call in tc:
        fn = call.get("function", {})
        print("    工具:", fn.get("name"), " 参数:", fn.get("arguments"))
        msgs2.append({
            "role": "tool",
            "tool_call_id": call.get("id"),
            "content": "成都：晴，18~25°C，东北风2级"
        })

    code, resp = chat(model_used, msgs2, TOOLS)
    show("回传后", code, resp)
