/*
 * llm.c —— 大模型对话实现
 *
 * MOCK   : 本地规则应答（离线可用，用于验证链路）
 * OPENAI : 任意 OpenAI 兼容接口
 *
 * 环境变量：
 *   LLM_BASE_URL    默认 https://api.hunyuan.cloud.tencent.com/v1
 *   LLM_API_KEY     Bearer token
 *   LLM_MODEL       默认 hunyuan-lite
 *   LLM_MAX_HISTORY 保留的对话轮数，默认 3
 */
#include "llm.h"
#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HIST_MAX 8
#define HIST_TEXT_MAX 512

static llm_backend_t g_backend = LLM_BACKEND_MOCK;
static int g_init = 0;

static char g_base_url[256] = "https://api.hunyuan.cloud.tencent.com/v1";
static char g_api_key[256]  = "";
static char g_model[128]    = "hunyuan-lite";
static int  g_max_history   = 3;

/* 多轮历史（环形缓冲） */
static char g_hist_user[HIST_MAX][HIST_TEXT_MAX];
static char g_hist_asst[HIST_MAX][HIST_TEXT_MAX];
static int  g_hist_count = 0;
static int  g_hist_head  = 0;

/* 长期记忆摘要（拼进 system prompt，约 50 token） */
static char g_profile[512] = "";

/* ================= MOCK ================= */
struct rule { const char *kw; const char *reply; };

static const struct rule g_rules[] = {
    { "\xe5\xa4\xa9\xe6\xb0\x94",   "今天的天气我暂时还看不到实时数据，不过可以帮你说点别的。" },
    { "\xe6\x97\xb6\xe9\x97\xb4",   "现在是板子本地时间，你可以用 date 命令查看。" },
    { "\xe4\xbd\xa0\xe6\x98\xaf\xe8\xb0\x81", "我是跑在这块 i.MX6ULL 开发板上的语音助手。" },
    { "\xe6\xad\x8c",   "我还没有接入音乐服务，先给你念首诗吧：床前明月光，疑是地上霜。" },
    { "\xe8\xaf\x97",   "床前明月光，疑是地上霜。举头望明月，低头思故乡。" },
    { "\xe4\xbd\xa0\xe5\xa5\xbd",   "你好！我在听。" },
    { "\xe8\xb0\xa2\xe8\xb0\xa2",   "不客气。" },
    { "\xe5\x86\x8d\xe8\xa7\x81",   "再见，随时叫我。" },
};

static int mock_chat(const char *user, char *out, size_t outlen)
{
    size_t i;

    if (!user || !user[0]) {
        snprintf(out, outlen, "我没听清，再说一遍好吗？");
        return 0;
    }

    for (i = 0; i < sizeof(g_rules)/sizeof(g_rules[0]); i++) {
        if (strstr(user, g_rules[i].kw)) {
            snprintf(out, outlen, "%s", g_rules[i].reply);
            fprintf(stderr, "[llm-mock] 命中规则，回复 %zu 字节\n", strlen(out));
            return 0;
        }
    }

    snprintf(out, outlen,
             "你说的是「%s」。我现在是本地假实现，还接不了真正的大模型。", user);
    fprintf(stderr, "[llm-mock] 无匹配\n");
    return 0;
}

/* ================= JSON 工具 ================= */
static void json_escape(const char *src, char *dst, size_t dstlen)
{
    size_t i = 0;
    for (; *src && i + 8 < dstlen; src++) {
        unsigned char c = (unsigned char)*src;
        switch (c) {
        case '"':  dst[i++]='\\'; dst[i++]='"';  break;
        case '\\': dst[i++]='\\'; dst[i++]='\\'; break;
        case '\n': dst[i++]='\\'; dst[i++]='n';  break;
        case '\r': dst[i++]='\\'; dst[i++]='r';  break;
        case '\t': dst[i++]='\\'; dst[i++]='t';  break;
        default:
            if (c < 0x20) i += (size_t)snprintf(dst + i, dstlen - i, "\\u%04x", c);
            else          dst[i++] = (char)c;
        }
    }
    dst[i] = 0;
}

static int parse_openai_content(const char *json, char *out, size_t outlen)
{
    const char *p, *q;
    size_t n = 0;

    p = strstr(json, "\"choices\"");
    if (!p) return -1;

    /* 注意：推理模型（如 hy3）会先输出 reasoning_content，
     * 正式回答在 content。这里先找 "content" 但必须避开 reasoning_content。
     * 做法：找 "\"content\"" 时排除前面紧邻 "reasoning_" 的情况。 */
    q = p;
    while ((q = strstr(q, "\"content\"")) != NULL) {
        /* 检查是否属于 reasoning_content */
        if (q >= p + 12 && strncmp(q - 10, "reasoning_", 10) == 0) {
            q += 9;
            continue;
        }
        break;
    }
    if (!q) return -1;
    p = q;
    p += strlen("\"content\"");
    while (*p && *p != ':') p++;
    if (*p != ':') return -1;
    p++;
    while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') p++;
    if (*p != '"') return -1;
    p++;

    q = p;
    while (*q && n + 4 < outlen) {
        if (*q == '\\' && q[1]) {
            q++;
            switch (*q) {
            case 'n': out[n++] = '\n'; break;
            case 'r': out[n++] = '\r'; break;
            case 't': out[n++] = '\t'; break;
            case '"': out[n++] = '"';  break;
            case '\\': out[n++] = '\\'; break;
            case '/': out[n++] = '/';  break;
            case 'u': {
                unsigned int cp = 0;
                int k;
                if (!q[1] || !q[2] || !q[3] || !q[4]) { q++; continue; }
                for (k = 1; k <= 4; k++) {
                    char c = q[k];
                    cp <<= 4;
                    if (c >= '0' && c <= '9') cp |= (unsigned)(c - '0');
                    else if (c >= 'a' && c <= 'f') cp |= (unsigned)(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') cp |= (unsigned)(c - 'A' + 10);
                }
                q += 4;
                if (cp < 0x80) {
                    out[n++] = (char)cp;
                } else if (cp < 0x800) {
                    out[n++] = (char)(0xC0 | (cp >> 6));
                    out[n++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    out[n++] = (char)(0xE0 | (cp >> 12));
                    out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    out[n++] = (char)(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: out[n++] = *q; break;
            }
            q++;
            continue;
        }
        if (*q == '"') break;
        out[n++] = *q++;
    }
    out[n] = 0;
    return (n > 0) ? 0 : -1;
}

static void parse_error(const char *json, char *out, size_t outlen)
{
    const char *p = strstr(json, "\"message\"");
    size_t n = 0;
    if (!p) { snprintf(out, outlen, "未知错误"); return; }
    p += 9;
    while (*p && *p != ':') p++;
    p++;
    while (*p == ' ' || *p == '"') p++;
    while (*p && *p != '"' && n + 1 < outlen) out[n++] = *p++;
    out[n] = 0;
}

/* ================= 历史 ================= */
static void hist_add(const char *user, const char *assistant)
{
    int idx;

    if (g_max_history <= 0 || g_max_history > HIST_MAX) return;

    if (g_hist_count < g_max_history) {
        idx = (g_hist_head + g_hist_count) % HIST_MAX;
        g_hist_count++;
    } else {
        idx = g_hist_head;
        g_hist_head = (g_hist_head + 1) % HIST_MAX;
    }

    snprintf(g_hist_user[idx], HIST_TEXT_MAX, "%s", user);
    snprintf(g_hist_asst[idx], HIST_TEXT_MAX, "%s", assistant);
}

void llm_reset_history(void)
{
    g_hist_count = 0;
    g_hist_head = 0;
}

/* ================= OpenAI 兼容 ================= */
static int openai_chat(const char *user, char *out, size_t outlen)
{
    char esc_user[2048];
    char msgs[8192];
    char body[10240];
    static char resp[65536];      /* 推理模型的响应可能很大 */
    char hdr[768];
    char url[512];
    int n, i, rc;
    char err[256];

    if (!g_api_key[0]) {
        snprintf(out, outlen, "(未配置 LLM_API_KEY)");
        fprintf(stderr, "[llm] 缺少 LLM_API_KEY\n");
        return -1;
    }

    /* system prompt + 长期记忆摘要 */
    if (g_profile[0]) {
        char esc_prof[1024];
        json_escape(g_profile, esc_prof, sizeof(esc_prof));
        n = snprintf(msgs, sizeof(msgs),
                     "{\"role\":\"system\",\"content\":"
                     "\"你是一个中文语音助手。直接给出最终回答，不要展示思考过程。"
                     "回答必须控制在40字以内，不要用markdown、列表或表情符号，"
                     "因为答案会被直接朗读出来。%s\"}", esc_prof);
    } else {
        n = snprintf(msgs, sizeof(msgs),
                     "{\"role\":\"system\",\"content\":"
                     "\"你是一个中文语音助手。直接给出最终回答，不要展示思考过程。"
                     "回答必须控制在40字以内，不要用markdown、列表或表情符号，"
                     "因为答案会被直接朗读出来。\"}");
    }

    for (i = 0; i < g_hist_count; i++) {
        int idx = (g_hist_head + i) % HIST_MAX;
        char eu[1024], ea[1024];
        json_escape(g_hist_user[idx], eu, sizeof(eu));
        json_escape(g_hist_asst[idx], ea, sizeof(ea));
        n += snprintf(msgs + n, sizeof(msgs) - (size_t)n,
                      ",{\"role\":\"user\",\"content\":\"%s\"}"
                      ",{\"role\":\"assistant\",\"content\":\"%s\"}", eu, ea);
        if (n >= (int)sizeof(msgs) - 200) break;
    }

    json_escape(user, esc_user, sizeof(esc_user));
    n += snprintf(msgs + n, sizeof(msgs) - (size_t)n,
                  ",{\"role\":\"user\",\"content\":\"%s\"}", esc_user);

    snprintf(body, sizeof(body),
             "{\"model\":\"%s\",\"messages\":[%s],\"stream\":false,"
             "\"temperature\":0.7,\"max_tokens\":2048}", g_model, msgs);

    snprintf(url, sizeof(url), "%s/chat/completions", g_base_url);

    snprintf(hdr, sizeof(hdr),
             "Authorization: Bearer %s\nContent-Type: application/json",
             g_api_key);

    fprintf(stderr, "[llm] POST %s  model=%s  hist=%d  body=%zu\n",
            url, g_model, g_hist_count, strlen(body));

    rc = http_post(url, hdr, body, strlen(body), resp, sizeof(resp));
    if (rc < 0) {
        snprintf(out, outlen, "(网络请求失败)");
        fprintf(stderr, "[llm] HTTP 请求失败 rc=%d\n", rc);
        return -2;
    }

    if (parse_openai_content(resp, out, outlen) < 0) {
        parse_error(resp, err, sizeof(err));
        fprintf(stderr, "[llm] 解析失败: %s\n原始: %.400s\n", err, resp);
        snprintf(out, outlen, "(大模型返回异常)");
        return -3;
    }

    hist_add(user, out);
    return 0;
}

/* ================= 公共 ================= */
int llm_init(llm_backend_t backend)
{
    const char *v;

    g_backend = backend;

    if ((v = getenv("LLM_BASE_URL")) && v[0])
        snprintf(g_base_url, sizeof(g_base_url), "%s", v);
    if ((v = getenv("LLM_API_KEY")))
        snprintf(g_api_key, sizeof(g_api_key), "%s", v);
    if ((v = getenv("LLM_MODEL")) && v[0])
        snprintf(g_model, sizeof(g_model), "%s", v);
    if ((v = getenv("LLM_MAX_HISTORY")))
        g_max_history = atoi(v);
    if (g_max_history > HIST_MAX) g_max_history = HIST_MAX;

    g_init = 1;
    fprintf(stderr, "[llm] 初始化: %s\n", llm_backend_name());
    if (g_backend == LLM_BACKEND_OPENAI)
        llm_print_config();
    return 0;
}

int llm_chat(const char *user_text, char *out, size_t outlen)
{
    if (!g_init) return -1;
    if (!out || outlen == 0) return -1;

    switch (g_backend) {
    case LLM_BACKEND_MOCK:   return mock_chat(user_text, out, outlen);
    case LLM_BACKEND_OPENAI: return openai_chat(user_text, out, outlen);
    default:                 return -1;
    }
}

void llm_deinit(void) { g_init = 0; }

/* ---- 长期记忆注入 ---- */
void llm_set_profile(const char *summary)
{
    if (summary && summary[0])
        snprintf(g_profile, sizeof(g_profile), "%s", summary);
    else
        g_profile[0] = 0;

    if (g_profile[0])
        fprintf(stderr, "[llm] 已注入用户档案(%zu 字节)\n", strlen(g_profile));
}

/* ---- 导出最近 N 轮对话（给提炼器用）---- */
void llm_dump_dialog(char *out, size_t outlen, int max_rounds)
{
    size_t used = 0;
    int i, start;

    if (!out || outlen == 0) return;
    out[0] = 0;
    if (g_hist_count == 0) return;

    if (max_rounds <= 0 || max_rounds > g_hist_count)
        max_rounds = g_hist_count;

    start = g_hist_count - max_rounds;

    for (i = start; i < g_hist_count; i++) {
        int idx = (g_hist_head + i) % HIST_MAX;
        int n = snprintf(out + used, outlen - used,
                         "用户: %s\n助手: %s\n",
                         g_hist_user[idx], g_hist_asst[idx]);
        if (n <= 0 || used + (size_t)n >= outlen) break;
        used += (size_t)n;
    }
}

int llm_history_count(void)
{
    return g_hist_count;
}

const char *llm_backend_name(void)
{
    switch (g_backend) {
    case LLM_BACKEND_MOCK:   return "mock";
    case LLM_BACKEND_OPENAI: return "openai";
    default:                 return "unknown";
    }
}

void llm_print_config(void)
{
    char masked[64];
    size_t len = strlen(g_api_key);

    if (len > 12)
        snprintf(masked, sizeof(masked), "%.8s...%s",
                 g_api_key, g_api_key + len - 4);
    else if (len > 0)
        snprintf(masked, sizeof(masked), "(too short)");
    else
        snprintf(masked, sizeof(masked), "(未设置)");

    fprintf(stderr, "[llm] base_url = %s\n", g_base_url);
    fprintf(stderr, "[llm] model    = %s\n", g_model);
    fprintf(stderr, "[llm] api_key  = %s\n", masked);
    fprintf(stderr, "[llm] history  = %d 轮\n", g_max_history);
}
