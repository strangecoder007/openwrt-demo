/*
 * http.h —— 极简 HTTP/HTTPS 客户端
 *
 * 基于 wget 命令行（板子上有 GNU Wget 1.20.3 且支持 https）。
 * 优点：不用在程序里处理 TLS，省事且稳。
 * 缺点：依赖 wget 存在；大文件要落盘。
 */
#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

#define HTTP_URL_MAX    512
#define HTTP_HDR_MAX    512

/*
 * GET 请求，结果写入 out 缓冲（文本）。
 *   url     完整 URL
 *   header  额外请求头，可为 NULL；多条用 \n 分隔
 *   out     输出缓冲
 *   outlen  缓冲大小
 * 返回 0 成功，负值失败。
 */
int http_get(const char *url, const char *header, char *out, size_t outlen);

/*
 * POST 请求。
 *   url       完整 URL
 *   header    额外请求头（\n 分隔），可为 NULL
 *   body      请求体
 *   body_len  请求体长度
 *   out       输出缓冲（可为 NULL，表示不要响应）
 *   outlen    缓冲大小
 * 返回 0 成功，负值失败。
 */
int http_post(const char *url, const char *header,
              const void *body, size_t body_len,
              char *out, size_t outlen);

/*
 * POST 二进制请求体，响应保存到文件。
 *   url, header 同上
 *   body, body_len 请求体（二进制安全）
 *   out_file    响应保存路径
 * 返回 0 成功。
 */
int http_post_to_file(const char *url, const char *header,
                      const void *body, size_t body_len,
                      const char *out_file);

/*
 * POST 二进制，响应也当二进制收到内存。
 */
int http_post_binary(const char *url, const char *header,
                     const void *body, size_t body_len,
                     void *out, size_t cap, size_t *out_len);

/* 检查 wget 是否可用 */
int http_available(void);

#endif /* HTTP_H */
