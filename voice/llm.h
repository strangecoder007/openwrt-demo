/*
 * llm.h —— 大模型对话接口（通用 OpenAI 兼容）
 *
 * 设计：只保留两个后端
 *   MOCK   本地规则（离线可用）
 *   OPENAI 任意 OpenAI 兼容接口（腾讯混元 / 阿里百炼 / DeepSeek / Kimi / Ollama ...）
 *
 * 通过环境变量配置，无需改代码：
 *   LLM_BACKEND   = mock | openai
 *   LLM_BASE_URL  = https://api.xxx.com/v1
 *   LLM_API_KEY   = sk-xxx
 *   LLM_MODEL     = 模型名
 */
#ifndef LLM_H
#define LLM_H

#include <stddef.h>

#define LLM_REPLY_MAX 2048

typedef enum {
    LLM_BACKEND_MOCK = 0,
    LLM_BACKEND_OPENAI,
} llm_backend_t;

int  llm_init(llm_backend_t backend);

/*
 * 输入用户文本，输出回复文本。
 * 返回 0 成功，负值失败。
 */
int  llm_chat(const char *user_text, char *out, size_t outlen);

void llm_deinit(void);
const char *llm_backend_name(void);

/* 清空对话历史（多轮上下文） */
void llm_reset_history(void);

/* 打印当前配置（不显示 key 全文） */
void llm_print_config(void);

/*
 * 注入长期记忆（用户档案摘要）。
 * 这段文字会拼在 system prompt 里，约 50 token。
 * 传 NULL 或空串表示清除。
 */
void llm_set_profile(const char *summary);

/*
 * 取出最近 N 轮对话原文，格式：
 *   用户: xxx
 *   助手: yyy
 * 用于提炼长期记忆。out 为空串表示没有历史。
 */
void llm_dump_dialog(char *out, size_t outlen, int max_rounds);

/* 当前历史轮数 */
int llm_history_count(void);

#endif /* LLM_H */
