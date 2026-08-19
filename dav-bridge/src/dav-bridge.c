/*
 * dav-bridge - minimal JSON CGI bridge for the WeChat Mini Program
 *
 * WeChat's wx.request only accepts a fixed set of HTTP methods
 * (OPTIONS/GET/HEAD/POST/PUT/DELETE/TRACE/CONNECT); PROPFIND and MKCOL
 * are not among them, and real devices fail with "network argv error"
 * while the developer tool happens to let them through. This CGI
 * exposes exactly the two WebDAV operations the mini program needs as
 * plain GET requests:
 *
 *   GET /cgi-bin/dav-bridge.cgi?op=ls&path=<url-encoded>&depth=0|1
 *        -> 200 {"ok":true,"items":[{href,contentLength,contentType,
 *                                   lastModified,isDir}, ...]}
 *           depth 1 lists the children of the directory; depth 0
 *           returns just the resource itself (404 if it does not exist).
 *   GET /cgi-bin/dav-bridge.cgi?op=mkdir&path=<url-encoded>
 *        -> 201 created; 405 already exists (mirrors MKCOL semantics).
 *
 * Authentication is handled by lighttpd mod_auth before this program
 * runs, so the CGI only checks that REMOTE_USER is set. All paths are
 * validated to stay below /mnt/sd (the WebDAV root) and ".." segments
 * are rejected, so the bridge cannot escape the SD card.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define DAV_PREFIX   "/dav"
#define SD_ROOT      "/mnt/sd"
#define MAX_QS_LEN   2048
#define MAX_PATH_LEN 1024

static void out_header(int status, const char *status_text)
{
	printf("Status: %d %s\r\n", status, status_text);
	printf("Content-Type: application/json; charset=utf-8\r\n");
	printf("Cache-Control: no-store\r\n");
	printf("\r\n");
}

static int out_error(int status, const char *status_text, const char *msg)
{
	out_header(status, status_text);
	printf("{\"ok\":false,\"error\":\"");
	for (const char *p = msg; *p; p++) {
		if (*p == '"' || *p == '\\')
			putchar('\\');
		putchar(*p);
	}
	printf("\"}\n");
	return 0;
}

static int hex_val(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* In-place percent-decoder. Returns false on malformed escapes or %00. */
static bool percent_decode(char *s)
{
	char *dst = s;

	while (*s) {
		if (s[0] == '%') {
			int hi = s[1] ? hex_val((unsigned char)s[1]) : -1;
			int lo = s[2] ? hex_val((unsigned char)s[2]) : -1;

			if (hi < 0 || lo < 0 || (hi == 0 && lo == 0))
				return false;
			*dst++ = (char)((hi << 4) | lo);
			s += 3;
		} else {
			*dst++ = *s++;
		}
	}
	*dst = '\0';
	return true;
}

/* Reject any path with a ".." path segment (after URL decoding). */
static bool has_dotdot(const char *p)
{
	while (*p) {
		while (*p == '/')
			p++;
		if (!*p)
			break;
		const char *seg = p;
		while (*p && *p != '/')
			p++;
		if ((size_t)(p - seg) == 2 && seg[0] == '.' && seg[1] == '.')
			return true;
	}
	return false;
}

/*
 * Convert a client path ("/dav/backup/...") into a filesystem path under
 * /mnt/sd. The client path must start with /dav or /dav/ and must not
 * contain ".." segments.
 */
static bool make_fs_path(const char *req, char *out, size_t outsz)
{
	size_t plen = sizeof(DAV_PREFIX) - 1; /* strlen("/dav") */

	if (strncmp(req, DAV_PREFIX, plen) != 0)
		return false;
	if (req[plen] != '\0' && req[plen] != '/')
		return false;
	if (has_dotdot(req + plen))
		return false;

	if (req[plen] == '\0')
		snprintf(out, outsz, "%s", SD_ROOT);
	else
		snprintf(out, outsz, "%s%s", SD_ROOT, req + plen);
	return true;
}

/*
 * Resolve a path that must already exist and verify it stays below
 * SD_ROOT (follows symlinks, so a symlink pointing outside is rejected).
 */
static bool under_root(const char *path)
{
	char resolved[PATH_MAX];
	size_t root_len = strlen(SD_ROOT);

	if (!realpath(path, resolved))
		return false;
	if (strncmp(resolved, SD_ROOT, root_len) != 0)
		return false;
	if (resolved[root_len] != '\0' && resolved[root_len] != '/')
		return false;
	return true;
}

/*
 * Find the longest existing ancestor of path (path itself may not exist
 * yet, e.g. the target of mkdir). Used to prove the existing part of a
 * new path stays below SD_ROOT.
 */
static bool existing_prefix(const char *path, char *out, size_t outsz)
{
	char buf[PATH_MAX];
	size_t len = strlen(path);

	if (len >= sizeof(buf))
		return false;

	while (len > 1) {
		while (len > 1 && path[len - 1] != '/')
			len--;
		if (len <= 1)
			break;
		memcpy(buf, path, len);
		buf[len] = '\0';
		if (access(buf, F_OK) == 0) {
			snprintf(out, outsz, "%s", buf);
			return true;
		}
		len--;
	}
	snprintf(out, outsz, "%s", SD_ROOT);
	return access(out, F_OK) == 0;
}

/* mkdir -p with 0755. Returns false on a real error (EEXIST is fine). */
static bool mkdir_p(const char *path)
{
	char buf[PATH_MAX];
	size_t len = strlen(path);

	if (len == 0 || len + 2 > sizeof(buf))
		return false;
	memcpy(buf, path, len);
	if (buf[len - 1] != '/')
		buf[len++] = '/';
	buf[len] = '\0';

	for (char *p = buf + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
			*p = '/';
			return false;
		}
		*p = '/';
	}
	return true;
}

static const char *content_type_for(const char *name)
{
	static const struct {
		const char *ext;
		const char *type;
	} map[] = {
		{ "jpg",  "image/jpeg" },       { "jpeg", "image/jpeg" },
		{ "png",  "image/png" },        { "gif",  "image/gif" },
		{ "heic", "image/heic" },       { "webp", "image/webp" },
		{ "bmp",  "image/bmp" },
		{ "mp4",  "video/mp4" },        { "m4v",  "video/mp4" },
		{ "mov",  "video/quicktime" },
		{ "mkv",  "video/x-matroska" },
		{ "avi",  "video/x-msvideo" },
		{ "3gp",  "video/3gpp" },
	};
	const char *dot = strrchr(name, '.');
	size_t i;

	if (!dot)
		return "application/octet-stream";
	for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		if (strcasecmp(dot + 1, map[i].ext) == 0)
			return map[i].type;
	}
	return "application/octet-stream";
}

static void print_last_modified(time_t t)
{
	struct tm tm;
	char buf[64];

	gmtime_r(&t, &tm);
	strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
	printf("%s", buf);
}

static bool uri_safe(unsigned char c)
{
	if (isalnum(c))
		return true;
	switch (c) {
	case '-': case '_': case '.': case '~': case '/':
		return true;
	default:
		return false;
	}
}

/*
 * Print a JSON href in the same style lighttpd mod_webdav uses: the
 * /dav/ URL prefix plus a percent-encoded UTF-8 path, with a trailing
 * slash for directories.
 */
static void print_href(const char *fs_path, bool is_dir)
{
	const char *rel = fs_path + strlen(SD_ROOT);
	const unsigned char *p = (const unsigned char *)rel;
	size_t rel_len = strlen(rel);

	printf("\"/dav");
	for (; *p; p++) {
		if (uri_safe(*p))
			putchar(*p);
		else
			printf("%%%02X", *p);
	}
	if (is_dir && rel_len > 0 && rel[rel_len - 1] != '/')
		putchar('/');
	printf("\"");
}

static void print_item(const char *fs_path, const char *name,
		       const struct stat *st)
{
	bool is_dir = S_ISDIR(st->st_mode);

	printf("{\"href\":");
	print_href(fs_path, is_dir);
	printf(",\"contentLength\":");
	if (is_dir)
		printf("null");
	else
		printf("%lld", (long long)st->st_size);
	printf(",\"contentType\":\"");
	if (!is_dir)
		printf("%s", content_type_for(name));
	printf("\",\"lastModified\":\"");
	print_last_modified(st->st_mtime);
	printf("\",\"isDir\":%s}", is_dir ? "true" : "false");
}

static int op_ls(const char *path, int depth)
{
	char fs[PATH_MAX];
	struct stat st;
	DIR *d = NULL;

	if (!make_fs_path(path, fs, sizeof(fs)) || !under_root(fs))
		return out_error(400, "Bad Request", "invalid path");

	if (stat(fs, &st) != 0) {
		if (errno == ENOENT)
			return out_error(404, "Not Found", "not_found");
		return out_error(500, "Internal Server Error", strerror(errno));
	}

	if (depth != 0 && S_ISDIR(st.st_mode)) {
		d = opendir(fs);
		if (!d)
			return out_error(500, "Internal Server Error",
					 strerror(errno));
	}

	out_header(200, "OK");
	printf("{\"ok\":true,\"items\":[");

	if (!d) {
		const char *name = strrchr(fs, '/');
		print_item(fs, name ? name + 1 : fs, &st);
	} else {
		struct dirent *e;
		bool first = true;

		while ((e = readdir(d)) != NULL) {
			char child[PATH_MAX];
			struct stat cst;

			if (strcmp(e->d_name, ".") == 0 ||
			    strcmp(e->d_name, "..") == 0)
				continue;
			if (snprintf(child, sizeof(child), "%s/%s", fs,
				     e->d_name) >= (int)sizeof(child))
				continue;
			if (stat(child, &cst) != 0)
				continue;
			if (!first)
				printf(",");
			print_item(child, e->d_name, &cst);
			first = false;
		}
		closedir(d);
	}
	printf("]}\n");
	return 0;
}

static int op_mkdir(const char *path)
{
	char fs[PATH_MAX];
	char prefix[PATH_MAX];
	struct stat st;

	if (!make_fs_path(path, fs, sizeof(fs)))
		return out_error(400, "Bad Request", "invalid path");

	if (stat(fs, &st) == 0)
		return out_error(405, "Method Not Allowed", "already exists");

	if (!existing_prefix(fs, prefix, sizeof(prefix)) ||
	    !under_root(prefix))
		return out_error(400, "Bad Request", "path escapes root");

	if (!mkdir_p(fs))
		return out_error(500, "Internal Server Error",
				 strerror(errno));

	out_header(201, "Created");
	printf("{\"ok\":true,\"items\":[]}\n");
	return 0;
}

static bool parse_query(const char *qs, char *op, size_t op_sz,
			char *path, size_t path_sz, int *depth)
{
	char buf[MAX_QS_LEN + 1];
	char *tok;

	op[0] = '\0';
	path[0] = '\0';
	*depth = 1;

	if (!qs || strlen(qs) >= sizeof(buf))
		return false;
	strcpy(buf, qs);

	for (tok = strtok(buf, "&"); tok; tok = strtok(NULL, "&")) {
		if (strncmp(tok, "op=", 3) == 0) {
			if (strlen(tok + 3) >= op_sz)
				return false;
			strcpy(op, tok + 3);
			if (!percent_decode(op))
				return false;
		} else if (strncmp(tok, "path=", 5) == 0) {
			if (strlen(tok + 5) >= path_sz)
				return false;
			strcpy(path, tok + 5);
			if (!percent_decode(path))
				return false;
		} else if (strncmp(tok, "depth=", 6) == 0) {
			*depth = atoi(tok + 6);
		}
	}

	if (!op[0] || !path[0])
		return false;
	if (*depth != 0 && *depth != 1)
		return false;
	return true;
}

int main(void)
{
	const char *user = getenv("REMOTE_USER");
	const char *method = getenv("REQUEST_METHOD");
	const char *qs = getenv("QUERY_STRING");
	char op[16];
	char path[MAX_PATH_LEN];
	int depth;

	if (!user || !*user)
		return out_error(401, "Unauthorized", "unauthorized");
	if (method && strcmp(method, "GET") != 0)
		return out_error(405, "Method Not Allowed", "GET only");
	if (!parse_query(qs, op, sizeof(op), path, sizeof(path), &depth))
		return out_error(400, "Bad Request",
				 "missing or invalid query");

	if (strcmp(op, "ls") == 0)
		return op_ls(path, depth);
	if (strcmp(op, "mkdir") == 0)
		return op_mkdir(path);

	return out_error(400, "Bad Request", "unknown op");
}
