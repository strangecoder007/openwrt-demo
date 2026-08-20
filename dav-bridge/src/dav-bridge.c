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
 *   POST /cgi-bin/dav-bridge.cgi?op=upload&path=<url-encoded>
 *        (multipart/form-data, part name "file")
 *        -> 201 {"ok":true,"items":[],"path":"/dav/.../<final name>"}
 *        The target name is assigned server-side (O_EXCL + -N suffix) and
 *        the body is streamed to disk, so concurrent uploads of the same
 *        file name neither corrupt each other nor exhaust board RAM.
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
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DAV_PREFIX   "/dav"
#define SD_ROOT      "/mnt/sd"
#define PASSWD_FILE  "/etc/lighttpd/webdav.passwd"
#define ADMIN_USER   "backup"
#define MAX_QS_LEN   2048
#define MAX_PATH_LEN 1024
#define MAX_FORM_BYTES 4096
#define MAX_UPLOAD_BYTES (64U * 1024 * 1024)

#define CHUNK_SIZE       65536
#define MAX_HEADER_BYTES 4096
#define MAX_PART_LINE    256
#define MAX_NAME_TRIES   1000

/* RENAME_NOREPLACE (Linux >= 3.15; the board runs 4.1.15) */
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1
#endif

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

/* 派生图（.thumb.jpg / .preview.jpg 结尾）不参与目录列举，
 * 避免被当作普通文件/再生成链条 */
static bool is_derived_name(const char *name)
{
	static const char *const suffixes[] = { ".thumb.jpg", ".preview.jpg" };
	size_t len = strlen(name);
	size_t n = sizeof(suffixes) / sizeof(suffixes[0]);
	size_t i;
	size_t slen;

	for (i = 0; i < n; i++) {
		slen = strlen(suffixes[i]);
		if (len >= slen && strcasecmp(name + len - slen, suffixes[i]) == 0)
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
	char prefix[PATH_MAX];
	struct stat st;
	DIR *d = NULL;
	size_t fs_len;

	if (!make_fs_path(path, fs, sizeof(fs)))
		return out_error(400, "Bad Request", "invalid path");

	/*
	 * The target may not exist yet (depth-0 existence probe); realpath
	 * would fail on a missing path, so prove the deepest existing
	 * ancestor stays below SD_ROOT instead, then stat -> 404 if absent.
	 */
	if (!existing_prefix(fs, prefix, sizeof(prefix)) ||
	    !under_root(prefix))
		return out_error(400, "Bad Request", "path escapes root");

	if (stat(fs, &st) != 0) {
		if (errno == ENOENT)
			return out_error(404, "Not Found", "not_found");
		return out_error(500, "Internal Server Error", strerror(errno));
	}

	/* Trim a trailing slash so child hrefs do not contain "//". */
	fs_len = strlen(fs);
	if (fs_len > 1 && fs[fs_len - 1] == '/')
		fs[fs_len - 1] = '\0';

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
			if (is_derived_name(e->d_name))
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

static size_t find_bytes(const char *hay, size_t hlen, size_t from,
			 const char *needle, size_t nlen)
{
	size_t i;

	if (from > hlen || nlen == 0 || nlen > hlen - from)
		return (size_t)-1;
	for (i = from; i + nlen <= hlen; i++) {
		if (memcmp(hay + i, needle, nlen) == 0)
			return i;
	}
	return (size_t)-1;
}

/*
 * Extract the boundary token from the multipart Content-Type. The env
 * string is owned by the CGI runtime, so we copy the token into buf.
 */
static bool get_boundary(char *buf, size_t bufsz)
{
	const char *ct = getenv("CONTENT_TYPE");
	const char *p;
	size_t len;

	if (!ct)
		return false;
	p = strstr(ct, "boundary=");
	if (!p)
		return false;
	p += strlen("boundary=");
	if (*p == '"')
		p++;
	len = strcspn(p, " \t\r\n\"");
	if (len == 0 || len >= bufsz)
		return false;
	memcpy(buf, p, len);
	buf[len] = '\0';
	return true;
}

/*
 * Read the whole request body (max MAX_UPLOAD_BYTES) into memory. The
 * board has enough RAM for a 50MB video, and parsing multipart from a
 * single buffer is far less error-prone than a streaming state machine.
 */
static int read_body(char **out, size_t *out_len)
{
	const char *cl = getenv("CONTENT_LENGTH");
	char buf[16384];
	size_t total = 0;
	size_t chunk;

	*out = NULL;
	*out_len = 0;

	if (cl) {
		unsigned long long want = strtoull(cl, NULL, 10);

		if (want > MAX_UPLOAD_BYTES)
			return 413;
		if (want == 0)
			return 0;
		*out = malloc(want);
		if (!*out)
			return 500;
		while (total < want) {
			chunk = fread(*out + total, 1, want - total, stdin);
			if (chunk == 0)
				break;
			total += chunk;
		}
		*out_len = total;
		return 0;
	}

	for (;;) {
		chunk = fread(buf, 1, sizeof(buf), stdin);
		if (chunk == 0)
			break;
		if (total + chunk > MAX_UPLOAD_BYTES) {
			free(*out);
			*out = NULL;
			return 413;
		}
		{
			char *nb = realloc(*out, total + chunk);
			if (!nb) {
				free(*out);
				*out = NULL;
				return 500;
			}
			*out = nb;
		}
		memcpy(*out + total, buf, chunk);
		total += chunk;
	}
	*out_len = total;
	return 0;
}

/* ---------- streaming multipart upload ---------- */

typedef struct {
	unsigned char buf[CHUNK_SIZE];
	size_t len;
	size_t pos;
} stream_reader;

static int sr_fill(stream_reader *r)
{
	r->pos = 0;
	r->len = fread(r->buf, 1, sizeof(r->buf), stdin);
	return r->len ? 0 : -1;
}

static int sr_byte(stream_reader *r, unsigned char *out)
{
	if (r->pos >= r->len && sr_fill(r) != 0)
		return -1;
	*out = r->buf[r->pos++];
	return 0;
}

/*
 * Make sure the next n bytes are buffered (refills and shifts as needed).
 * Returns 0 when available, -1 on EOF before n bytes could be read.
 */
static int sr_peek(stream_reader *r, size_t n)
{
	while (r->len - r->pos < n) {
		if (r->pos > 0) {
			memmove(r->buf, r->buf + r->pos, r->len - r->pos);
			r->len -= r->pos;
			r->pos = 0;
		}
		if (r->len == sizeof(r->buf))
			return -1; /* pattern larger than buffer; never happens */
		{
			size_t got = fread(r->buf + r->len, 1,
					   sizeof(r->buf) - r->len, stdin);

			if (got == 0)
				return -1;
			r->len += got;
		}
	}
	return 0;
}

/* Read a line (LF or CRLF terminated); terminator is not stored. */
static int sr_read_line(stream_reader *r, char *out, size_t cap, size_t *olen)
{
	size_t n = 0;
	unsigned char b;

	for (;;) {
		if (sr_byte(r, &b) != 0)
			return -1;
		if (b == '\n')
			break;
		if (n + 1 >= cap)
			return -1;
		if (b != '\r')
			out[n++] = (char)b;
	}
	out[n] = '\0';
	*olen = n;
	return 0;
}

/* Read part headers up to the blank line ("\r\n\r\n"), including it. */
static int sr_read_headers(stream_reader *r, char *out, size_t cap,
			   size_t *olen)
{
	size_t n = 0;
	unsigned char b;

	for (;;) {
		if (sr_byte(r, &b) != 0)
			return -1;
		if (n + 1 >= cap)
			return -1;
		out[n++] = (char)b;
		if (n >= 4 && memcmp(out + n - 4, "\r\n\r\n", 4) == 0) {
			out[n] = '\0';
			*olen = n;
			return 0;
		}
	}
}

/*
 * Stream file data into outf until the terminator pattern ("\r\n--boundary")
 * is seen. Detection works across chunk boundaries by withholding the last
 * (plen - 1) bytes of each chunk; memory stays bounded by CHUNK_SIZE.
 * Returns 0 on success, -1 malformed (EOF), -2 write error, -3 too large.
 */
static int sr_copy_until_boundary(stream_reader *r, FILE *outf,
				  const char *pat, size_t plen,
				  size_t *total, size_t max)
{
	size_t keep = plen - 1;

	for (;;) {
		size_t found;

		if (sr_peek(r, plen) != 0)
			return -1;
		/*
		 * The terminator may start anywhere inside the uncommitted
		 * window (it often begins before the most recent fill), so
		 * search the whole window rather than only its first byte.
		 */
		found = find_bytes((const char *)r->buf, r->len, r->pos,
				   pat, plen);
		if (found != (size_t)-1) {
			size_t writable = found - r->pos;

			if (writable > 0) {
				if (fwrite(r->buf + r->pos, 1, writable,
					   outf) != writable)
					return -2;
				r->pos += writable;
				*total += writable;
				if (*total > max)
					return -3;
			}
			return 0;
		}
		{
			size_t avail = r->len - r->pos;
			size_t writable = avail > keep ? avail - keep : 0;

			if (writable > 0) {
				if (fwrite(r->buf + r->pos, 1, writable,
					   outf) != writable)
					return -2;
				r->pos += writable;
				*total += writable;
				if (*total > max)
					return -3;
			}
		}
	}
}

/*
 * Build the n-th candidate target for an original path: name, name-1,
 * name-2 ... inserting the suffix before the final extension (same scheme
 * the mini program used client-side, now enforced atomically server-side).
 */
static int build_candidate(const char *fs, int n, char *out, size_t outsz)
{
	const char *slash;
	const char *dot;
	size_t base_len;

	if (n <= 0) {
		if (strlen(fs) >= outsz)
			return -1;
		strcpy(out, fs);
		return 0;
	}
	slash = strrchr(fs, '/');
	dot = strrchr(fs, '.');
	base_len = (dot && (!slash || dot > slash)) ? (size_t)(dot - fs)
						    : strlen(fs);
	if (snprintf(out, outsz, "%.*s-%d%s", (int)base_len, fs, n,
		     fs + base_len) >= (int)outsz)
		return -1;
	return 0;
}

/*
 * Atomically publish tmp as final. Uses renameat2(RENAME_NOREPLACE) so an
 * already-published file is never overwritten; on kernels/filesystems that
 * do not support it, falls back to plain rename() (same-name overwrite, the
 * old behaviour, mitigated by the client's existence probe).
 * Returns 0 on success, -1 on error (errno set), -2 when the target exists.
 */
static int publish_part(const char *tmp, const char *final)
{
#if defined(SYS_renameat2)
	if (syscall(SYS_renameat2, AT_FDCWD, tmp, AT_FDCWD, final,
		     RENAME_NOREPLACE) == 0)
		return 0;
	if (errno == EEXIST)
		return -2;
	if (errno != ENOSYS && errno != EINVAL)
		return -1;
#endif
	return rename(tmp, final) == 0 ? 0 : -1;
}

/*
 * op=upload&path=<url-encoded>  (POST, multipart/form-data, part name
 * "file"). WeChat wx.uploadFile has onProgressUpdate, unlike wx.request,
 * so uploads go through this CGI instead of a WebDAV PUT.
 *
 * The body is parsed and written to disk in a streaming fashion (bounded
 * CHUNK_SIZE buffer) instead of buffering up to 64MB per request, so
 * several concurrent uploads do not exhaust board RAM. The target is
 * created with O_EXCL and auto-suffixed (-1, -2, ...) server-side when the
 * name is taken, closing the check-then-act race the mini program's
 * client-side probe used to have. The final path is returned as JSON so
 * the client can place derived thumbnails next to the real file.
 */
static int op_upload(const char *path)
{
	char fs[PATH_MAX];
	char prefix[PATH_MAX];
	char boundary[128];
	char partline[MAX_PART_LINE];
	char hdrs[MAX_HEADER_BYTES];
	char tmp[PATH_MAX];
	char final_path[PATH_MAX];
	char pat[2 + 2 + sizeof(boundary)];
	size_t b_len, plen, hlen, pat_len, total = 0;
	stream_reader r;
	FILE *outf = NULL;
	int fd = -1;
	int n;
	int rc;

	if (!make_fs_path(path, fs, sizeof(fs)))
		return out_error(400, "Bad Request", "invalid path");
	if (!existing_prefix(fs, prefix, sizeof(prefix)) ||
	    !under_root(prefix))
		return out_error(400, "Bad Request", "path escapes root");

	if (!get_boundary(boundary, sizeof(boundary)))
		return out_error(400, "Bad Request", "missing boundary");
	b_len = strlen(boundary);

	/* Early Content-Length guard (still enforced while streaming). */
	{
		const char *cl = getenv("CONTENT_LENGTH");

		if (cl) {
			unsigned long long want = strtoull(cl, NULL, 10);

			if (want > MAX_UPLOAD_BYTES)
				return out_error(413, "Payload Too Large",
						 "too large");
		}
	}

	r.len = 0;
	r.pos = 0;

	/* First line must be "--boundary". */
	if (sr_read_line(&r, partline, sizeof(partline), &plen) != 0 ||
	    plen != b_len + 2 || memcmp(partline, "--", 2) != 0 ||
	    memcmp(partline + 2, boundary, b_len) != 0)
		return out_error(400, "Bad Request", "malformed multipart");

	/* Part headers; the bridge contract is exactly one file part. */
	if (sr_read_headers(&r, hdrs, sizeof(hdrs), &hlen) != 0)
		return out_error(400, "Bad Request", "malformed multipart");
	if (find_bytes(hdrs, hlen, 0, "name=\"file\"", 10) == (size_t)-1)
		return out_error(400, "Bad Request", "no file part");

	/* Reserve a unique <name>.part with O_EXCL, auto-suffixing on clash. */
	for (n = 0; n < MAX_NAME_TRIES; n++) {
		if (build_candidate(fs, n, final_path,
				    sizeof(final_path)) != 0)
			return out_error(500, "Internal Server Error",
					 "path too long");
		snprintf(tmp, sizeof(tmp), "%s.part", final_path);
		fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0644);
		if (fd >= 0)
			break;
		if (errno != EEXIST)
			return out_error(500, "Internal Server Error",
					 strerror(errno));
	}
	if (fd < 0)
		return out_error(409, "Conflict", "name space exhausted");

	outf = fdopen(fd, "wb");
	if (!outf) {
		close(fd);
		unlink(tmp);
		return out_error(500, "Internal Server Error",
				 strerror(errno));
	}

	/* "\r\n--" + boundary terminates the file part. */
	pat[0] = '\r';
	pat[1] = '\n';
	pat[2] = '-';
	pat[3] = '-';
	memcpy(pat + 4, boundary, b_len);
	pat_len = b_len + 4;

	rc = sr_copy_until_boundary(&r, outf, pat, pat_len, &total,
				    MAX_UPLOAD_BYTES);
	if (rc == -3) {
		fclose(outf);
		unlink(tmp);
		return out_error(413, "Payload Too Large", "too large");
	}
	if (rc == -2) {
		fclose(outf);
		unlink(tmp);
		return out_error(500, "Internal Server Error", "write failed");
	}
	if (rc != 0) {
		fclose(outf);
		unlink(tmp);
		return out_error(400, "Bad Request", "malformed multipart");
	}
	if (fclose(outf) != 0) {
		unlink(tmp);
		return out_error(500, "Internal Server Error", "write failed");
	}

	/*
	 * Publish; if the target appeared while we were writing (the
	 * client-side probe raced), keep the data and retry the next free
	 * suffix instead of overwriting someone else's upload.
	 */
	for (; n < MAX_NAME_TRIES; n++) {
		if (build_candidate(fs, n, final_path,
				    sizeof(final_path)) != 0) {
			unlink(tmp);
			return out_error(500, "Internal Server Error",
					 "path too long");
		}
		rc = publish_part(tmp, final_path);
		if (rc == 0) {
			out_header(201, "Created");
			printf("{\"ok\":true,\"items\":[],\"path\":");
			print_href(final_path, false);
			printf("}\n");
			return 0;
		}
		if (rc == -2)
			continue; /* target exists -> try next -N suffix */
		unlink(tmp);
		return out_error(500, "Internal Server Error",
				 strerror(errno));
	}
	unlink(tmp);
	return out_error(409, "Conflict", "name space exhausted");
}

/*
 * Extract "name=<url-encoded value>" from an application/x-www-form-urlencoded
 * body. The value is percent-decoded into out. Returns false when the field
 * is missing or too long.
 */
static bool form_field(const char *body, size_t len, const char *name,
		       char *out, size_t outsz)
{
	size_t nlen = strlen(name);
	size_t i;
	size_t vstart;
	size_t vlen;

	for (i = 0; i + nlen < len; i++) {
		if (i > 0 && body[i - 1] != '&')
			continue;
		if (strncmp(body + i, name, nlen) != 0)
			continue;
		if (body[i + nlen] != '=')
			continue;
		vstart = i + nlen + 1;
		vlen = 0;
		while (vstart + vlen < len && body[vstart + vlen] != '&')
			vlen++;
		if (vlen >= outsz)
			return false;
		memcpy(out, body + vstart, vlen);
		out[vlen] = '\0';
		return percent_decode(out);
	}
	return false;
}

/* 用户名：3-32 个 [A-Za-z0-9._-]，首字符必须是字母或数字 */
static bool valid_username(const char *name)
{
	size_t len = strlen(name);
	size_t i;

	if (len < 3 || len > 32)
		return false;
	if (!isalnum((unsigned char)name[0]))
		return false;
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)name[i];
		if (!isalnum(c) && c != '.' && c != '_' && c != '-')
			return false;
	}
	return true;
}

/* 密码：6-128 个非控制字符 */
static bool valid_password(const char *pass)
{
	size_t len = strlen(pass);
	size_t i;

	if (len < 6 || len > 128)
		return false;
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)pass[i];
		if (c < 0x20 || c == 0x7f)
			return false;
	}
	return true;
}

/* 检查用户名是否已存在于 htpasswd 文件（"name:" 行首前缀） */
static bool user_exists(const char *name)
{
	FILE *f = fopen(PASSWD_FILE, "r");
	char line[256];
	size_t nlen = strlen(name);
	bool found = false;

	if (!f)
		return false;
	while (fgets(line, sizeof(line), f)) {
		size_t llen = strcspn(line, "\r\n");
		if (llen > nlen && strncmp(line, name, nlen) == 0 &&
		    line[nlen] == ':') {
			found = true;
			break;
		}
	}
	fclose(f);
	return found;
}

/*
 * Append "name:hash" to PASSWD_FILE. The hash is produced by
 * "openssl passwd -apr1" (Apache MD5-crypt, the format lighttpd mod_auth
 * expects). The password travels as an argv element, so it is briefly
 * visible in the process list; on this single-admin board that is
 * acceptable. A future hardening step is implementing apr1 with libcrypto
 * to avoid the exec entirely.
 */
static int append_htpasswd(const char *name, const char *pass)
{
	int pipefd[2];
	pid_t pid;
	char hash[128];
	size_t hlen = 0;
	ssize_t got;
	FILE *f;
	int status;

	if (pipe(pipefd) != 0)
		return -1;
	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	if (pid == 0) {
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		close(pipefd[1]);
		execl("/usr/bin/openssl", "openssl", "passwd", "-apr1",
		      pass, (char *)NULL);
		_exit(127);
	}
	close(pipefd[1]);
	while (hlen < sizeof(hash) - 1) {
		got = read(pipefd[0], hash + hlen, sizeof(hash) - 1 - hlen);
		if (got <= 0)
			break;
		hlen += (size_t)got;
	}
	close(pipefd[0]);
	waitpid(pid, &status, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || hlen == 0)
		return -1;
	while (hlen > 0 && (hash[hlen - 1] == '\n' || hash[hlen - 1] == '\r'))
		hlen--;
	hash[hlen] = '\0';
	if (hlen == 0)
		return -1;

	f = fopen(PASSWD_FILE, "a");
	if (!f)
		return -1;
	if (fprintf(f, "%s:%s\n", name, hash) < 0) {
		fclose(f);
		return -1;
	}
	if (fclose(f) != 0)
		return -1;
	return 0;
}

/*
 * op=register (POST, application/x-www-form-urlencoded, fields user/pass).
 * Authentication already happened in lighttpd: this CGI is only reachable
 * with an administrator credential (auth.require lists backup for
 * /cgi-bin/dav-bridge.cgi), so a non-empty REMOTE_USER means the caller
 * may create accounts.
 */
static int op_register(void)
{
	const char *caller = getenv("REMOTE_USER");
	char *body = NULL;
	size_t body_len = 0;
	char user[64];
	char pass[160];
	int r;

	/* 列表/上传等操作所有有效账号可用；注册账号只有管理员能调 */
	if (!caller || strcmp(caller, ADMIN_USER) != 0)
		return out_error(403, "Forbidden", "admin only");

	r = read_body(&body, &body_len);
	if (r == 413)
		return out_error(413, "Payload Too Large", "too large");
	if (r == 500)
		return out_error(500, "Internal Server Error", "read failed");
	if (!body || body_len == 0 || body_len > MAX_FORM_BYTES) {
		free(body);
		return out_error(400, "Bad Request", "empty or too large");
	}
	if (!form_field(body, body_len, "user", user, sizeof(user)) ||
	    !form_field(body, body_len, "pass", pass, sizeof(pass))) {
		free(body);
		return out_error(400, "Bad Request", "missing user/pass");
	}
	free(body);

	if (!valid_username(user))
		return out_error(400, "Bad Request", "invalid username");
	if (!valid_password(pass))
		return out_error(400, "Bad Request", "invalid password");
	if (user_exists(user))
		return out_error(409, "Conflict", "user exists");
	if (append_htpasswd(user, pass) != 0)
		return out_error(500, "Internal Server Error",
				 "append failed");

	out_header(201, "Created");
	printf("{\"ok\":true,\"user\":\"%s\"}\n", user);
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

	if (!op[0])
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
	if (!parse_query(qs, op, sizeof(op), path, sizeof(path), &depth))
		return out_error(400, "Bad Request",
				 "missing or invalid query");

	if (strcmp(op, "upload") == 0) {
		if (!method || strcmp(method, "POST") != 0)
			return out_error(405, "Method Not Allowed",
					 "POST required");
		if (!path[0])
			return out_error(400, "Bad Request", "missing path");
		return op_upload(path);
	}
	if (strcmp(op, "register") == 0) {
		if (!method || strcmp(method, "POST") != 0)
			return out_error(405, "Method Not Allowed",
					 "POST required");
		return op_register();
	}
	if (method && strcmp(method, "GET") != 0)
		return out_error(405, "Method Not Allowed", "GET only");
	if (!path[0])
		return out_error(400, "Bad Request", "missing path");
	if (strcmp(op, "ls") == 0)
		return op_ls(path, depth);
	if (strcmp(op, "mkdir") == 0)
		return op_mkdir(path);

	return out_error(400, "Bad Request", "unknown op");
}
