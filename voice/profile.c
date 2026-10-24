/*
 * profile.c —— 长期记忆（用户档案）
 *
 * 设计要点：
 *
 * 1. 存提炼后的事实，不存对话原文
 *    "用户住在成都" 而不是 "用户昨天问过成都天气，我说晴"
 *
 * 2. key=value 平面文件
 *    板子没有 JSON 库；profile 结构固定且无嵌套，平面文件 30 行解析搞定。
 *
 * 3. 原子写
 *    先写 profile.conf.tmp，fsync，再 rename。断电不会写坏原文件。
 *
 * 4. 提炼要额外调一次 LLM，所以不能每轮都做
 *    - 每 8 轮做一次（= HIST_MAX，不超过短期记忆容量）
 *    - 正常退出时补做一次
 */
#include "profile.h"
#include "llm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#define PROFILE_DIR "/var/lib/voice"

/* ---------------- 工具 ---------------- */

/* 去掉行尾的 \r\n 和首尾空格 */
static void trim(char *s)
{
    char *p = s;
    size_t len;

    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);

    len = strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r' ||
                       s[len-1] == ' ' || s[len-1] == '\t')) {
        s[--len] = 0;
    }
}

static void now_iso(char *out, size_t outlen)
{
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(out, outlen, "%Y-%m-%dT%H:%M:%S", &tmv);
}

/* ---------------- 加载 ---------------- */

int profile_load(profile_t *p)
{
    FILE *f;
    char line[512];

    if (!p) return -1;
    memset(p, 0, sizeof(*p));
    snprintf(p->answer_style, sizeof(p->answer_style), "short");

    f = fopen(PROFILE_PATH, "r");
    if (!f) {
        /* 不存在是正常的（第一次运行） */
        return 0;
    }

    while (fgets(line, sizeof(line), f)) {
        char *eq;
        char *k, *v;

        trim(line);
        if (line[0] == 0 || line[0] == '#') continue;

        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        k = line;
        v = eq + 1;
        trim(k);
        trim(v);

        if (!strcmp(k, "name"))
            snprintf(p->name, sizeof(p->name), "%s", v);
        else if (!strcmp(k, "city"))
            snprintf(p->city, sizeof(p->city), "%s", v);
        else if (!strcmp(k, "job"))
            snprintf(p->job, sizeof(p->job), "%s", v);
        else if (!strcmp(k, "answer_style"))
            snprintf(p->answer_style, sizeof(p->answer_style), "%s", v);
        else if (!strcmp(k, "total_rounds"))
            p->total_rounds = atoi(v);
        else if (!strcmp(k, "last_seen"))
            snprintf(p->last_seen, sizeof(p->last_seen), "%s", v);
        else if (!strncmp(k, "fact", 4)) {
            if (p->fact_count < PROFILE_FACT_MAX && v[0]) {
                snprintf(p->facts[p->fact_count], PROFILE_FACT_LEN, "%s", v);
                p->fact_count++;
            }
        }
    }

    fclose(f);
    return 0;
}

/* ---------------- 保存 ---------------- */

int profile_save(const profile_t *p)
{
    char tmp[128];
    FILE *f;
    int i;

    if (!p) return -1;

    /* 确保目录存在 */
    mkdir(PROFILE_DIR, 0755);

    snprintf(tmp, sizeof(tmp), PROFILE_PATH ".tmp");
    f = fopen(tmp, "w");
    if (!f) {
        fprintf(stderr, "[profile] 无法写 %s\n", tmp);
        return -1;
    }

    fprintf(f, "# voice 用户档案（长期记忆）\n");
    fprintf(f, "# 由 voicectl 自动维护，可手改\n");
    fprintf(f, "# 这些内容会拼进 system prompt 发给大模型\n\n");

    if (p->name[0]) fprintf(f, "name=%s\n", p->name);
    if (p->city[0]) fprintf(f, "city=%s\n", p->city);
    if (p->job[0])  fprintf(f, "job=%s\n", p->job);
    fprintf(f, "answer_style=%s\n", p->answer_style[0] ? p->answer_style : "short");
    fprintf(f, "\n");
    for (i = 0; i < p->fact_count; i++) {
        if (p->facts[i][0])
            fprintf(f, "fact%d=%s\n", i, p->facts[i]);
    }
    fprintf(f, "\n");
    fprintf(f, "total_rounds=%d\n", p->total_rounds);
    fprintf(f, "last_seen=%s\n", p->last_seen);

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    /* 原子替换 */
    if (rename(tmp, PROFILE_PATH) < 0) {
        perror("[profile] rename");
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* ---------------- 生成 prompt 摘要 ---------------- */

void profile_to_prompt(const profile_t *p, char *out, size_t outlen)
{
    size_t used = 0;
    int i;

    if (!out || outlen == 0) return;
    out[0] = 0;
    if (!p) return;

    /* 空档案不产生任何文本 */
    if (!p->name[0] && !p->city[0] && !p->job[0] && p->fact_count == 0)
        return;

    used += (size_t)snprintf(out + used, outlen - used, "用户信息：");
    if (p->name[0])
        used += (size_t)snprintf(out + used, outlen - used, "叫%s；", p->name);
    if (p->city[0])
        used += (size_t)snprintf(out + used, outlen - used, "住在%s；", p->city);
    if (p->job[0])
        used += (size_t)snprintf(out + used, outlen - used, "%s；", p->job);

    for (i = 0; i < p->fact_count && used + 40 < outlen; i++) {
        if (!p->facts[i][0]) continue;
        used += (size_t)snprintf(out + used, outlen - used,
                                 "%s；", p->facts[i]);
    }
}

/* ---------------- 提炼（调 LLM） ---------------- */

/*
 * 用一次额外 LLM 调用，从对话原文里抽出稳定事实。
 *
 * 提示词设计要点：
 *   - 明确「只要长期有效的信息」，排除一次性的（今天天气、刚才的问题）
 *   - 要求输出固定的 key=value 行，方便解析（不要 JSON，省得写解析器）
 *   - 限制条数，防止无限膨胀
 */
int profile_extract(const char *dialog, profile_t *p)
{
    char prompt[4096];
    char reply[LLM_REPLY_MAX] = {0};
    char *line, *saveptr;

    if (!dialog || !p) return -1;
    if (!dialog[0]) return 0;

    snprintf(prompt, sizeof(prompt),
        "从下面的对话中提取关于用户的**稳定长期信息**。\n"
        "\n"
        "只提取会长期有效的内容，例如：姓名、居住城市、职业、家庭成员、"
        "明显且反复出现的偏好。\n"
        "不要提取：一次性的问题、当天天气、临时的请求、"
        "你自己说过的话。\n"
        "如果对话里没有任何值得长期记住的信息，只输出 NO_FACTS。\n"
        "\n"
        "输出格式（每行一条，最多 8 条，不要任何解释文字）：\n"
        "name=姓名\n"
        "city=城市\n"
        "job=职业\n"
        "fact=其他事实\n"
        "\n"
        "对话：\n%s", dialog);

    if (llm_chat(prompt, reply, sizeof(reply)) < 0) {
        fprintf(stderr, "[profile] 提炼调用失败\n");
        return -1;
    }

    if (strstr(reply, "NO_FACTS")) {
        fprintf(stderr, "[profile] 本轮无可提炼信息\n");
        return 0;
    }

    /* 逐行解析 key=value */
    fprintf(stderr, "[profile] 提炼结果:\n%s\n", reply);

    for (line = strtok_r(reply, "\n", &saveptr);
         line;
         line = strtok_r(NULL, "\n", &saveptr)) {
        char *eq;
        char k[32], v[PROFILE_FACT_LEN];

        trim(line);
        if (line[0] == 0) continue;

        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        snprintf(k, sizeof(k), "%s", line);
        snprintf(v, sizeof(v), "%s", eq + 1);
        trim(k);
        trim(v);
        if (v[0] == 0) continue;

        if (!strcmp(k, "name") && v[0])
            snprintf(p->name, sizeof(p->name), "%s", v);
        else if (!strcmp(k, "city") && v[0]) {
            if (p->city[0] && strcmp(p->city, v))
                fprintf(stderr, "[profile] 城市变更: %s -> %s\n", p->city, v);
            snprintf(p->city, sizeof(p->city), "%s", v);
        }
        else if (!strcmp(k, "job") && v[0])
            snprintf(p->job, sizeof(p->job), "%s", v);
        else if (!strcmp(k, "fact") && v[0]) {
            int i, dup = 0;
            for (i = 0; i < p->fact_count; i++)
                if (!strcmp(p->facts[i], v)) { dup = 1; break; }
            if (!dup && p->fact_count < PROFILE_FACT_MAX)
                snprintf(p->facts[p->fact_count++], PROFILE_FACT_LEN, "%s", v);
        }
    }

    return 0;
}

/* ---------------- 其他 ---------------- */

void profile_clear(profile_t *p)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));
    snprintf(p->answer_style, sizeof(p->answer_style), "short");
}

void profile_dump(const profile_t *p)
{
    char buf[512];

    if (!p) return;
    profile_to_prompt(p, buf, sizeof(buf));

    fprintf(stderr, "[profile] 姓名: %s\n", p->name[0] ? p->name : "(未记录)");
    fprintf(stderr, "[profile] 城市: %s\n", p->city[0] ? p->city : "(未记录)");
    fprintf(stderr, "[profile] 职业: %s\n", p->job[0] ? p->job : "(未记录)");
    fprintf(stderr, "[profile] 风格: %s\n", p->answer_style);
    fprintf(stderr, "[profile] 事实: %d 条\n", p->fact_count);
    {
        int i;
        for (i = 0; i < p->fact_count; i++)
            fprintf(stderr, "[profile]   %d. %s\n", i + 1, p->facts[i]);
    }
    fprintf(stderr, "[profile] 累计轮数: %d\n", p->total_rounds);
    fprintf(stderr, "[profile] 上次: %s\n", p->last_seen[0] ? p->last_seen : "-");
    fprintf(stderr, "[profile] prompt 摘要: %s\n", buf[0] ? buf : "(空)");
}
