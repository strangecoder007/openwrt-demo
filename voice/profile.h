/*
 * profile.h —— 长期记忆（用户档案）
 *
 * 存的是「提炼出的事实」，不是对话原文。
 * 存放：/var/lib/voice/profile.conf（key=value 平面文件，人类可读）
 *
 * 为什么用 key=value 而不是 JSON：
 *   板子上没有 JSON 库，profile 结构固定且不含嵌套，平面文件解析 30 行搞定，
 *   人也能直接改。
 */
#ifndef PROFILE_H
#define PROFILE_H

#include <stddef.h>

#define PROFILE_PATH      "/var/lib/voice/profile.conf"
#define PROFILE_FACT_MAX  8
#define PROFILE_FACT_LEN  160

typedef struct {
    /* 结构化字段 */
    char name[32];
    char city[32];
    char job[64];
    char answer_style[16];     /* short / detailed */

    /* 自由事实（最多 8 条） */
    char facts[PROFILE_FACT_MAX][PROFILE_FACT_LEN];
    int  fact_count;

    /* 统计 */
    int  total_rounds;
    char last_seen[24];        /* ISO8601 本地时间 */
} profile_t;

/* 加载（不存在则返回 0 并留空骨架） */
int  profile_load(profile_t *p);

/* 原子保存：先写 .tmp 再 rename，避免断电写坏 */
int  profile_save(const profile_t *p);

/* 生成给 system prompt 用的中文摘要（~50 token） */
void profile_to_prompt(const profile_t *p, char *out, size_t outlen);

/*
 * 从一段对话原文里提炼事实（调 LLM）。
 *   dialog   对话文本（Role: 内容 的格式）
 *   p        输入输出（就地更新）
 * 返回 0 成功。
 */
int  profile_extract(const char *dialog, profile_t *p);

/* 清空（voicectl forget） */
void profile_clear(profile_t *p);

/* 打印（调试） */
void profile_dump(const profile_t *p);

#endif /* PROFILE_H */
