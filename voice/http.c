/*
 * http.c —— 基于 wget 的 HTTP 客户端
 *
 * 全部通过 popen 调 wget 完成，避免在程序里链接 TLS 库。
 * wget 的 --header 可以重复多次，用来带自定义头。
 */
#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define TMP_REQ   "/tmp/.voice_http_req.%d"
#define TMP_RESP  "/tmp/.voice_http_resp.%d"

static int g_wget_ok = -1;

int http_available(void)
{
    if (g_wget_ok < 0) {
        g_wget_ok = (system("command -v wget >/dev/null 2>&1") == 0) ? 1 : 0;
    }
    return g_wget_ok;
}

/* 把 \n 分隔的多条 header 拼成 wget 的 --header 参数 */
static void build_header_args(const char *header, char *out, size_t outlen)
{
    const char *p = header;
    size_t used = 0;

    out[0] = 0;
    if (!header || !*header)
        return;

    while (*p && used + 20 < outlen) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        int n = snprintf(out + used, outlen - used,
                         " --header='%.*s'", (int)len, p);
        if (n <= 0) break;
        used += (size_t)n;
        if (!nl) break;
        p = nl + 1;
    }
}

/* 转义文件路径给 shell 用（路径由我们自己生成，简单处理即可） */
static void shell_quote(const char *src, char *dst, size_t dstlen)
{
    size_t i = 0;
    dst[i++] = '\'';
    for (; *src && i + 3 < dstlen; src++) {
        if (*src == '\'') {
            dst[i++] = '\'';
            dst[i++] = '\\';
            dst[i++] = '\'';
            dst[i++] = '\'';
        } else {
            dst[i++] = *src;
        }
    }
    dst[i++] = '\'';
    dst[i] = 0;
}

static int read_file(const char *path, void *out, size_t cap, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(out, 1, cap, f);
    fclose(f);
    if (len) *len = n;
    return 0;
}

/* ---------------- GET ---------------- */
int http_get(const char *url, const char *header, char *out, size_t outlen)
{
    char hargs[HTTP_HDR_MAX];
    char cmd[2048];
    char tmp[64];
    FILE *p;
    size_t n = 0;
    int rc;

    if (!http_available())
        return -1;

    build_header_args(header, hargs, sizeof(hargs));

    snprintf(tmp, sizeof(tmp), TMP_RESP, (int)getpid());
    snprintf(cmd, sizeof(cmd),
             "wget -q -T 15 -O %s%s %s",
             tmp, hargs, url);

    rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "[http] GET 失败 (rc=%d): %s\n", rc, url);
        unlink(tmp);
        return -2;
    }

    p = fopen(tmp, "rb");
    if (!p) { unlink(tmp); return -3; }
    n = fread(out, 1, outlen - 1, p);
    fclose(p);
    unlink(tmp);

    out[n] = 0;
    return 0;
}

/* ---------------- POST（内存体 -> 文本响应）---------------- */
int http_post(const char *url, const char *header,
              const void *body, size_t body_len,
              char *out, size_t outlen)
{
    char hargs[HTTP_HDR_MAX];
    char cmd[2048];
    char req[64], resp[64];
    char qreq[128];
    FILE *f, *p;
    size_t n = 0;
    int rc;

    if (!http_available())
        return -1;

    build_header_args(header, hargs, sizeof(hargs));

    snprintf(req, sizeof(req), TMP_REQ, (int)getpid());
    snprintf(resp, sizeof(resp), TMP_RESP, (int)getpid());
    shell_quote(req, qreq, sizeof(qreq));

    f = fopen(req, "wb");
    if (!f) return -3;
    if (body_len && fwrite(body, 1, body_len, f) != body_len) {
        fclose(f); unlink(req); return -3;
    }
    fclose(f);

    snprintf(cmd, sizeof(cmd),
             "wget -q -T 20 -O %s %s --post-file=%s %s 2>&1",
             resp, hargs, qreq, url);

    rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "[http] POST 失败 (rc=%d): %s\n", rc, url);
        unlink(req);
        unlink(resp);
        return -2;
    }

    unlink(req);

    if (out && outlen) {
        p = fopen(resp, "rb");
        if (p) {
            n = fread(out, 1, outlen - 1, p);
            fclose(p);
            out[n] = 0;
        }
    }
    unlink(resp);
    return 0;
}

/* ---------------- POST（内存体 -> 文件响应）---------------- */
int http_post_to_file(const char *url, const char *header,
                      const void *body, size_t body_len,
                      const char *out_file)
{
    char hargs[HTTP_HDR_MAX];
    char cmd[2048];
    char req[64];
    char qreq[128], qout[256];
    FILE *f;
    int rc;

    if (!http_available())
        return -1;

    build_header_args(header, hargs, sizeof(hargs));

    snprintf(req, sizeof(req), TMP_REQ, (int)getpid());
    shell_quote(req, qreq, sizeof(qreq));
    shell_quote(out_file, qout, sizeof(qout));

    f = fopen(req, "wb");
    if (!f) return -3;
    if (body_len && fwrite(body, 1, body_len, f) != body_len) {
        fclose(f); unlink(req); return -3;
    }
    fclose(f);

    snprintf(cmd, sizeof(cmd),
             "wget -q -T 20 -O %s %s "
             "--post-file=%s %s 2>&1",
             qout, hargs, qreq, url);

    rc = system(cmd);
    unlink(req);

    if (rc != 0) {
        fprintf(stderr, "[http] POST->file 失败 (rc=%d): %s\n", rc, url);
        return -2;
    }
    return 0;
}

/* ---------------- POST（二进制响应）---------------- */
int http_post_binary(const char *url, const char *header,
                     const void *body, size_t body_len,
                     void *out, size_t cap, size_t *out_len)
{
    char resp[64];
    int r;

    snprintf(resp, sizeof(resp), TMP_RESP, (int)getpid());

    r = http_post_to_file(url, header, body, body_len, resp);
    if (r < 0) return r;

    if (read_file(resp, out, cap, out_len) < 0) {
        unlink(resp);
        return -4;
    }
    unlink(resp);
    return 0;
}
