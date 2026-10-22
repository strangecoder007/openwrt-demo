/*
 * shcmd - a tiny, allowlisted HTTP command executor for OpenWrt.
 *
 * Design notes (read before changing anything):
 *
 *   1. There is NO shell. Every whitelisted entry is an argv vector. The
 *      request may append positional arguments; those arguments are handed to
 *      execvp as separate argv slots, never concatenated into a string. So
 *      "a=; rm -rf /" is just one literal argument, not a second command.
 *      Anything that genuinely needs a pipeline must be baked into the config
 *      by the operator as a fixed "sh -c '...'" entry.
 *
 *   2. The token is compared in constant time. Query parameter and
 *      Authorization: Bearer are both accepted.
 *
 *   3. Source addresses are checked against a CIDR list (v4 and v6).
 *
 *   4. Output is capped at SHCMD_CAP bytes. The child is put into its own
 *      process group and killed on timeout, so a wedged command can never
 *      outlive the request.
 *
 *   5. Single-threaded, blocking, one request at a time. This is a remote
 *      control, not a web server; serializing requests keeps the code small
 *      and the failure modes obvious.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SHCMD_VERSION "0.1.0"

#define MAX_REQ        8192            /* request head limit */
#define MAX_OUT        (64 * 1024)     /* captured output per command */
#define MAX_ARGS       16              /* extra argv slots from the request */
#define MAX_ARG_LEN    512
#define MAX_ENTRIES    32
#define MAX_CMD_ARGV   8               /* argv slots inside one entry */
#define MAX_ADDRS      16
#define MAX_TOKENS     4

/* ------------------------------------------------------------------ */
/* small utilities                                                     */
/* ------------------------------------------------------------------ */

static void logmsg(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("shcmd: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static void *xmalloc(size_t n)
{
	void *p = malloc(n);
	if (!p) {
		logmsg("out of memory");
		exit(1);
	}
	return p;
}

static char *xstrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = xmalloc(n);
	memcpy(p, s, n);
	return p;
}

/* constant-time compare; length差异也计入，避免早退 */
static int ct_equal(const char *a, const char *b)
{
	size_t la = strlen(a), lb = strlen(b), n = la > lb ? la : lb, i;
	unsigned char diff = (unsigned char)(la ^ lb);

	for (i = 0; i < n; i++) {
		unsigned char ca = i < la ? (unsigned char)a[i] : 0;
		unsigned char cb = i < lb ? (unsigned char)b[i] : 0;
		diff |= (unsigned char)(ca ^ cb);
	}
	return diff == 0;
}

/* ------------------------------------------------------------------ */
/* configuration                                                       */
/* ------------------------------------------------------------------ */

struct arg_spec {
	char *label;
	char *example;
	char *expect;
	/*
	 * Machine-readable constraint, filled from `enum=` / `range=` options on
	 * the `arg` directive. Kept separate from the human prose in `expect` so
	 * /schema can hand a caller something it can actually validate against
	 * instead of a sentence it has to guess from.
	 */
	char *enums;         /* comma separated, or NULL */
	int   min, max;      /* range, valid only when has_range */
	int   has_range;
	int   required;
};

struct entry {
	char *name;
	char *argv[MAX_CMD_ARGV + 1];
	int   argc;
	int   timeout;
	struct arg_spec args[MAX_ARGS];
	int   n_argdesc;
	char *desc;              /* one-line human description */
	int   danger;            /* nonzero = UI should warn before running */
};

struct addr_rule {
	int  family;          /* AF_INET / AF_INET6 */
	int  bits;
	unsigned char raw[16];
};

struct config {
	char *listen;             /* "0.0.0.0" or "::" style */
	int   port;
	char *tokens[MAX_TOKENS];
	int   n_tokens;
	struct addr_rule addrs[MAX_ADDRS];
	int   n_addrs;
	struct entry entries[MAX_ENTRIES];
	int   n_entries;
	int   max_args;
	char *realm;
};

static struct config cfg;

/*
 * Descriptors the forked child must not inherit. Tracked globally because
 * run_argv() is the only place that forks, and it needs to close exactly these
 * two rather than sweeping a whole fd range (see the comment there about
 * procd's flock fd breaking when low fds are torn down).
 */
static int g_listen_fd = -1;
static int g_client_fd = -1;

static void cfg_defaults(void)
{
	memset(&cfg, 0, sizeof(cfg));
	cfg.listen = xstrdup("0.0.0.0");
	cfg.port = 8099;
	cfg.max_args = 4;
	cfg.realm = xstrdup("shcmd");
}

/*
 * Parse a trimmed config line:
 *   token <string>
 *   listen <addr>
 *   port <n>
 *   allow <cidr>
 *   max_args <n>
 *   command <name> <timeout> <program> [args...]
 */
static void cfg_line(char *line, int lineno)
{
	/*
	 * Room for generous command lines: `command <n> <t> <prog> <8 argv>` plus
	 * `arg <slot> <label> <example> <expect>` needs more than the argv limit
	 * alone, and silently truncating would drop a trailing script.
	 */
	char *tok[MAX_CMD_ARGV + 12];
	int ntok = 0;
	char *p = line;

	while (*p && ntok < (int)(sizeof(tok) / sizeof(tok[0]))) {
		while (*p && isspace((unsigned char)*p))
			p++;
		if (!*p || *p == '#')
			break;
		if (*p == '"' || *p == '\'') {
			/*
			 * Quoted token: the opening quote is dropped and the
			 * matching closing quote becomes the terminator. This is an
			 * in-place shrink, so the destination must never run ahead
			 * of the source. (The earlier version started the copy one
			 * byte ahead of the source pointer and silently ate the
			 * first character, e.g. "test-..." became "est-...".)
			 *
			 * BOTH quote characters must be honoured: `command foo 10
			 * sh -c 'logread | grep x'` is the normal way to whitelist a
			 * pipeline, and treating the single quote as an ordinary
			 * byte splits that script on its spaces.
			 */
			/*
			 * Quoted token: drop the opening quote, keep the contents,
			 * and stop at the matching closing quote.
			 *
			 * This used to copy in place with a single advancing pointer,
			 * which caused two separate bugs:
			 *   1. starting the destination one byte ahead ate the first
			 *      character ("test-..." became "est-...").
			 *   2. terminating at the source pointer clobbered the closing
			 *      quote when the copy was in place (no spaces), so the
			 *      scan position never advanced and every later token on
			 *      the line was lost.
			 *
			 * Using two independent cursors (read / write) removes both
			 * failure modes at once and makes the delimiter handling
			 * explicit.
			 */
			char quote = *p;
			char *read = p + 1;   /* first content byte */
			char *write = read;   /* destination: may lag if len shrinks */

			tok[ntok++] = write;
			while (*read && *read != quote)
				*write++ = *read++;
			/*
			 * Decide whether a closing quote was reached BEFORE writing the
			 * terminator. When the quoted text contains no spaces the copy
			 * is in place, so write == read and `*write = '\0'` overwrites
			 * the very quote we need to step over. Testing *read after the
			 * write sees '\0' instead of '"', the position never advances,
			 * and the outer loop terminates at once -- silently swallowing
			 * every remaining token on the line. That is exactly why
			 * `arg 1 "Lbl" "ex"` produced ntok=3 instead of 4.
			 */
			int closed = (*read == quote);
			*write = '\0';

			if (closed)
				read++;        /* step past the closing quote */
			p = read;
		} else {
			tok[ntok++] = p;
			while (*p && !isspace((unsigned char)*p))
				p++;
			if (*p)
				*p++ = '\0';
		}
	}
	if (ntok == 0)
		return;

	if (getenv("SHCMD_DEBUG_PARSE")) {
		fprintf(stderr, "line %d: ntok=%d", lineno, ntok);
		for (int d = 0; d < ntok; d++)
			fprintf(stderr, " [%s]", tok[d]);
		fputc('\n', stderr);
	}

	if (!strcmp(tok[0], "token") && ntok >= 2) {
		if (cfg.n_tokens < MAX_TOKENS)
			cfg.tokens[cfg.n_tokens++] = xstrdup(tok[1]);
		else
			logmsg("line %d: too many tokens, ignored", lineno);
	} else if (!strcmp(tok[0], "listen") && ntok >= 2) {
		free(cfg.listen);
		cfg.listen = xstrdup(tok[1]);
	} else if (!strcmp(tok[0], "port") && ntok >= 2) {
		cfg.port = atoi(tok[1]);
	} else if (!strcmp(tok[0], "max_args") && ntok >= 2) {
		cfg.max_args = atoi(tok[1]);
		if (cfg.max_args < 0)
			cfg.max_args = 0;
		if (cfg.max_args > MAX_ARGS)
			cfg.max_args = MAX_ARGS;
	} else if (!strcmp(tok[0], "allow") && ntok >= 2) {
		char buf[128];
		char *slash;
		int bits = -1;

		snprintf(buf, sizeof(buf), "%s", tok[1]);
		slash = strchr(buf, '/');
		if (slash) {
			*slash++ = '\0';
			bits = atoi(slash);
		}
		if (cfg.n_addrs >= MAX_ADDRS) {
			logmsg("line %d: too many allow rules", lineno);
			return;
		}
		struct addr_rule *r = &cfg.addrs[cfg.n_addrs];
		if (inet_pton(AF_INET, buf, r->raw) == 1) {
			r->family = AF_INET;
			r->bits = bits < 0 ? 32 : bits;
		} else if (inet_pton(AF_INET6, buf, r->raw) == 1) {
			r->family = AF_INET6;
			r->bits = bits < 0 ? 128 : bits;
		} else {
			logmsg("line %d: bad address '%s'", lineno, buf);
			return;
		}
		if (r->bits < 0 || r->bits > (r->family == AF_INET ? 32 : 128)) {
			logmsg("line %d: bad prefix length in '%s'", lineno, tok[1]);
			return;
		}
		cfg.n_addrs++;
	} else if (!strcmp(tok[0], "command") && ntok >= 4) {
		if (cfg.n_entries >= MAX_ENTRIES) {
			logmsg("line %d: too many commands", lineno);
			return;
		}
		struct entry *e = &cfg.entries[cfg.n_entries];
		memset(e, 0, sizeof(*e));
		e->name = xstrdup(tok[1]);
		e->timeout = atoi(tok[2]);
		if (e->timeout <= 0)
			e->timeout = 10;
		e->argc = 0;
		for (int i = 3; i < ntok && e->argc < MAX_CMD_ARGV; i++)
			e->argv[e->argc++] = xstrdup(tok[i]);
		e->argv[e->argc] = NULL;
		cfg.n_entries++;
	} else if (!strcmp(tok[0], "desc") && ntok >= 2) {
		/* Attaches a one-line description to the most recent command. */
		if (cfg.n_entries == 0) {
			logmsg("line %d: desc before any command", lineno);
			return;
		}
		struct entry *e = &cfg.entries[cfg.n_entries - 1];
		free(e->desc);
		e->desc = xstrdup(tok[1]);
	} else if (!strcmp(tok[0], "danger") && ntok >= 1) {
		if (cfg.n_entries == 0) {
			logmsg("line %d: danger before any command", lineno);
			return;
		}
		cfg.entries[cfg.n_entries - 1].danger = 1;
	} else if (!strcmp(tok[0], "arg") && ntok >= 4) {
		/*
		 * arg <slot> <label> <example> [expected]
		 * Purely documentation for the trailing argv slots: the daemon
		 * does not validate values, it only passes argv through. The UI
		 * uses these to render labelled inputs instead of blank boxes.
		 */
		if (cfg.n_entries == 0) {
			logmsg("line %d: arg before any command", lineno);
			return;
		}
		struct entry *e = &cfg.entries[cfg.n_entries - 1];
		int slot = atoi(tok[1]);
		if (slot < 1 || slot > MAX_ARGS) {
			logmsg("line %d: arg slot %d out of range", lineno, slot);
			return;
		}

		struct arg_spec *a = &e->args[slot - 1];
		free(a->label);
		free(a->example);
		free(a->expect);
		free(a->enums);
		memset(a, 0, sizeof(*a));

		a->label = xstrdup(tok[2]);
		a->example = xstrdup(tok[3]);
		a->expect = ntok >= 5 ? xstrdup(tok[4]) : NULL;

		/*
		 * Optional machine-readable options come after the prose, as
		 * key=value tokens:
		 *     enum=a,b,c     the value must be one of these
		 *     range=N-M      the value must be an integer in [N,M]
		 *     required       the caller must supply this slot
		 * These are what /schema exports and what the daemon enforces
		 * before the command runs, so a caller learns the boundary from
		 * the contract instead of from a failed attempt.
		 *
		 * Options are recognised by their prefix, not by position: the
		 * prose `expect` field is optional, so a fixed offset would
		 * mistake an option for prose or vice versa.
		 */
		for (int i = 4; i < ntok; i++) {
			const char *t = tok[i];

			if (!strncmp(t, "enum=", 5)) {
				free(a->enums);
				a->enums = xstrdup(t + 5);
			} else if (!strncmp(t, "range=", 6)) {
				const char *r = t + 6;
				char *dash = strchr(r, '-');
				if (!dash) {
					logmsg("line %d: range needs N-M, got '%s'",
					       lineno, r);
					continue;
				}
				*dash = '\0';
				a->min = atoi(r);
				a->max = atoi(dash + 1);
				a->has_range = 1;
				if (a->min > a->max) {
					int tmp = a->min;
					a->min = a->max;
					a->max = tmp;
				}
			} else if (!strcmp(t, "required")) {
				a->required = 1;
			} else if (i > 4) {
				/* i == 4 is the optional prose field; later
				 * unrecognised tokens are genuine typos. */
				logmsg("line %d: unknown arg option '%s'",
				       lineno, t);
			}
		}

		if (slot > e->n_argdesc)
			e->n_argdesc = slot;
	} else {
		logmsg("line %d: unknown directive '%s'", lineno, tok[0]);
	}
}

static int cfg_load(const char *path)
{
	FILE *f = fopen(path, "r");
	/*
	 * Generous enough for a long `sh -c '...'` script on one line. Keep the
	 * overlong check below, though: fgets() silently truncates at the buffer
	 * size, and a truncated script could parse into a DIFFERENT command
	 * rather than failing loudly.
	 */
	char line[4096];
	int lineno = 0;

	if (!f) {
		logmsg("cannot open config %s: %s", path, strerror(errno));
		return -1;
	}
	while (fgets(line, sizeof(line), f)) {
		size_t n;
		lineno++;
		n = strlen(line);

		if (n == sizeof(line) - 1 && line[n - 1] != '\n') {
			/* Likely truncated: consume the rest and refuse the line. */
			int c;
			while ((c = fgetc(f)) != EOF && c != '\n')
				;
			logmsg("line %d: longer than %zu bytes, ignored",
			       lineno, sizeof(line) - 1);
			continue;
		}

		while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			line[--n] = '\0';
		cfg_line(line, lineno);
	}
	fclose(f);
	return 0;
}

/* ------------------------------------------------------------------ */
/* authorization                                                       */
/* ------------------------------------------------------------------ */

static int addr_allowed(const struct sockaddr_storage *ss)
{
	int i;

	if (cfg.n_addrs == 0)
		return 1;                /* no allow rules => any source */

	for (i = 0; i < cfg.n_addrs; i++) {
		const struct addr_rule *r = &cfg.addrs[i];
		const unsigned char *cand;

		if (r->family != ss->ss_family)
			continue;
		if (ss->ss_family == AF_INET)
			cand = (const unsigned char *)&((const struct sockaddr_in *)ss)->sin_addr;
		else
			cand = (const unsigned char *)&((const struct sockaddr_in6 *)ss)->sin6_addr;

		int whole = r->bits / 8, rem = r->bits % 8, ok = 1;
		if (memcmp(cand, r->raw, whole))
			ok = 0;
		else if (rem) {
			unsigned char mask = (unsigned char)(0xff << (8 - rem));
			if ((cand[whole] & mask) != (r->raw[whole] & mask))
				ok = 0;
		}
		if (ok)
			return 1;
	}
	return 0;
}

static int token_ok(const char *cand)
{
	int i;

	if (cfg.n_tokens == 0)
		return 0;                /* no token configured => refuse all */
	if (!cand)
		return 0;
	for (i = 0; i < cfg.n_tokens; i++)
		if (ct_equal(cand, cfg.tokens[i]))
			return 1;

	/* still burn a comparison when nothing matched */
	ct_equal(cand, cfg.tokens[0]);
	return 0;
}

/* ------------------------------------------------------------------ */
/* HTTP helpers                                                        */
/* ------------------------------------------------------------------ */

static const char *status_text(int code)
{
	switch (code) {
	case 200: return "OK";
	case 400: return "Bad Request";
	case 401: return "Unauthorized";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 408: return "Request Timeout";
	case 413: return "Payload Too Large";
	case 500: return "Internal Server Error";
	case 503: return "Service Unavailable";
	default:  return "Unknown";
	}
}

/*
 * Minimal percent-decoding, in place-ish: dst must hold strlen(src)+1.
 * '+' becomes space (form encoding). Bad escapes are copied literally so a
 * hostile request degrades into a mismatch rather than an error.
 */
static void url_decode(const char *src, char *dst, size_t dstlen)
{
	size_t o = 0;

	while (*src && o + 1 < dstlen) {
		if (*src == '+') {
			dst[o++] = ' ';
			src++;
		} else if (*src == '%' && isxdigit((unsigned char)src[1]) &&
			   isxdigit((unsigned char)src[2])) {
			char hex[3] = { src[1], src[2], 0 };
			dst[o++] = (char)strtol(hex, NULL, 16);
			src += 3;
		} else {
			dst[o++] = *src++;
		}
	}
	dst[o] = '\0';
}

/*
 * Find a query/form parameter. Returns 0 on hit. Only the first occurrence is
 * used; a repeated parameter is ignored rather than merged.
 */
static int param_get(const char *query, const char *key, char *out, size_t outlen)
{
	size_t klen = strlen(key);
	const char *p = query;

	if (!query)
		return -1;
	while (*p) {
		const char *amp = strchr(p, '&');
		size_t len = amp ? (size_t)(amp - p) : strlen(p);

		if (len > klen && !strncmp(p, key, klen) && p[klen] == '=') {
			char tmp[MAX_ARG_LEN * 2];
			size_t vlen = len - klen - 1;
			if (vlen >= sizeof(tmp))
				return -1;
			memcpy(tmp, p + klen + 1, vlen);
			tmp[vlen] = '\0';
			url_decode(tmp, out, outlen);
			return 0;
		}
		if (!amp)
			break;
		p = amp + 1;
	}
	return -1;
}

static void send_response(int fd, int code, const char *ctype,
			  const char *body, size_t blen, int extra_hdr)
{
	char head[512];
	int n;

	n = snprintf(head, sizeof(head),
		     "HTTP/1.1 %d %s\r\n"
		     "Content-Type: %s\r\n"
		     "Content-Length: %zu\r\n"
		     "Cache-Control: no-store\r\n"
		     "Connection: close\r\n",
		     code, status_text(code), ctype, blen);
	if (n < 0)
		return;
	if (code == 401 && extra_hdr) {
		char auth[256];
		int m = snprintf(auth, sizeof(auth),
				 "WWW-Authenticate: Bearer realm=\"%s\"\r\n", cfg.realm);
		if (m > 0) {
			size_t copy = (size_t)m < sizeof(head) - (size_t)n - 2
				      ? (size_t)m : sizeof(head) - (size_t)n - 2;
			memcpy(head + n, auth, copy);
			n += (int)copy;
		}
	}
	if ((size_t)n + 2 < sizeof(head)) {
		head[n++] = '\r';
		head[n++] = '\n';
	}
	send(fd, head, (size_t)n, MSG_NOSIGNAL);
	if (body && blen)
		send(fd, body, blen, MSG_NOSIGNAL);
}

static void reply_text(int fd, int code, const char *msg)
{
	char buf[512];
	int n = snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}\n", msg);

	send_response(fd, code, "application/json", buf, n > 0 ? (size_t)n : 0,
		      code == 401);
}

static void json_escape(const char *in, size_t inlen, char *out, size_t outlen)
{
	size_t i, o = 0;

	for (i = 0; i < inlen && o + 8 < outlen; i++) {
		unsigned char c = (unsigned char)in[i];
		switch (c) {
		case '"':  out[o++] = '\\'; out[o++] = '"';  break;
		case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
		case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
		case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
		case '\t': out[o++] = '\\'; out[o++] = 't';  break;
		default:
			if (c < 0x20)
				o += (size_t)snprintf(out + o, outlen - o, "\\u%04x", c);
			else
				out[o++] = (char)c;
		}
	}
	out[o] = '\0';
}

/* ------------------------------------------------------------------ */
/* command execution                                                   */
/* ------------------------------------------------------------------ */

struct run_result {
	char  *out;
	size_t len;
	int    status;      /* child exit status, or -1 */
	int    timed_out;
	int    truncated;
};

/*
 * Run one argv vector, capture up to MAX_OUT bytes of combined stdout/stderr.
 *
 * The child gets its own process group so a timeout kills the whole tree.
 * Note the ordering: we close the write end in the parent before reading, so
 * read() returns 0 (EOF) once every descendant holding the pipe has exited.
 */
static void run_argv(char *const argv[], int timeout, struct run_result *res)
{
	int pipefd[2];
	pid_t pid;

	memset(res, 0, sizeof(*res));
	res->status = -1;
	res->out = xmalloc(MAX_OUT + 1);

	if (pipe(pipefd) < 0) {
		snprintf(res->out, MAX_OUT, "shcmd: pipe: %s\n", strerror(errno));
		res->len = strlen(res->out);
		return;
	}

	pid = fork();
	if (pid < 0) {
		snprintf(res->out, MAX_OUT, "shcmd: fork: %s\n", strerror(errno));
		res->len = strlen(res->out);
		close(pipefd[0]);
		close(pipefd[1]);
		return;
	}

	if (pid == 0) {
		/* child */
		setpgid(0, 0);
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		if (pipefd[1] > STDERR_FILENO)
			close(pipefd[1]);

		/*
		 * Close inherited descriptors we know about, but do NOT sweep every
		 * fd >= 3: an init script needs its own low descriptors, and
		 * /lib/functions/procd.sh specifically uses fd 1000 for its lock.
		 *
		 * Note the real cause of the "restart via shcmd never works"
		 * symptom was NOT here -- it was `limits nofile="64 64"` in
		 * files/shcmd.init capping the daemon at 64 descriptors, so fd 1000
		 * could not be opened by any child. See that file for the details.
		 */
		if (g_listen_fd >= 0 && g_listen_fd > STDERR_FILENO)
			close(g_listen_fd);
		if (g_client_fd >= 0 && g_client_fd > STDERR_FILENO)
			close(g_client_fd);

		execvp(argv[0], argv);
		fprintf(stderr, "shcmd: exec %s: %s\n", argv[0], strerror(errno));
		_exit(127);
	}

	/* parent */
	setpgid(pid, pid);
	close(pipefd[1]);

	struct timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);

	size_t used = 0;
	int eof = 0;

	while (!eof) {
		struct pollfd pfd;
		struct timespec now;
		long elapsed_ms, remain_ms;

		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed_ms = (now.tv_sec - start.tv_sec) * 1000L +
			     (now.tv_nsec - start.tv_nsec) / 1000000L;
		remain_ms = (long)timeout * 1000L - elapsed_ms;
		if (remain_ms <= 0) {
			res->timed_out = 1;
			break;
		}

		pfd.fd = pipefd[0];
		pfd.events = POLLIN;
		pfd.revents = 0;

		int pr = poll(&pfd, 1, (int)(remain_ms > 1000 ? 1000 : remain_ms));
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (pr == 0)
			continue;

		if (used >= MAX_OUT) {
			/* keep draining to let the child finish, discard extra */
			char sink[4096];
			ssize_t n = read(pipefd[0], sink, sizeof(sink));
			if (n > 0) {
				res->truncated = 1;
				continue;
			}
			if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN))
				break;
			continue;
		}

		ssize_t n = read(pipefd[0], res->out + used, MAX_OUT - used);
		if (n > 0) {
			used += (size_t)n;
			if (used >= MAX_OUT)
				res->truncated = 1;
		} else if (n == 0) {
			eof = 1;
		} else if (errno != EINTR && errno != EAGAIN) {
			break;
		}
	}

	if (!eof && res->timed_out)
		kill(-pid, SIGKILL);

	close(pipefd[0]);

	int wstatus = 0;
	for (;;) {
		pid_t w = waitpid(pid, &wstatus, 0);
		if (w == pid) {
			if (WIFEXITED(wstatus))
				res->status = WEXITSTATUS(wstatus);
			else if (WIFSIGNALED(wstatus))
				res->status = 128 + WTERMSIG(wstatus);
			break;
		}
		if (w < 0 && errno != EINTR)
			break;
	}

	res->out[used] = '\0';
	res->len = used;
}

/* ------------------------------------------------------------------ */
/* request handling                                                    */
/* ------------------------------------------------------------------ */

static struct entry *entry_find(const char *name)
{
	int i;

	if (!name)
		return NULL;
	for (i = 0; i < cfg.n_entries; i++)
		if (!strcmp(cfg.entries[i].name, name))
			return &cfg.entries[i];
	return NULL;
}

/*
 * Serialise one command entry. `verbose` adds the machine-readable
 * constraint fields (enum / range / required) that /schema exposes; the plain
 * index keeps them too, since they cost little and are useful to the UI.
 */
static size_t emit_entry(char *body, size_t cap, size_t o,
			 const struct entry *e, int first)
{
	char esc[1024];
	int j;

	if (o >= cap - 2048)
		return o;

	json_escape(e->desc ? e->desc : "", strlen(e->desc ? e->desc : ""),
		    esc, sizeof(esc));
	o += (size_t)snprintf(body + o, cap - o,
			      "%s{\"name\":\"%s\",\"timeout\":%d,"
			      "\"desc\":\"%s\",\"danger\":%s,\"args\":[",
			      first ? "" : ",", e->name, e->timeout, esc,
			      e->danger ? "true" : "false");

	for (j = 0; j < e->n_argdesc; j++) {
		const struct arg_spec *a = &e->args[j];
		char ld[512] = "", le[512] = "", lx[512] = "";
		char len[512] = "";

		/*
		 * The request parameter that carries this slot: slot 1 is "a",
		 * slot 2 is "a1", slot 3 is "a2", ... Getting this wrong in a
		 * client silently drops every argument after the first.
		 */
		char param[16];
		if (j == 0)
			snprintf(param, sizeof(param), "a");
		else
			snprintf(param, sizeof(param), "a%d", j);

		if (a->label)
			json_escape(a->label, strlen(a->label), ld, sizeof(ld));
		if (a->example)
			json_escape(a->example, strlen(a->example),
				    le, sizeof(le));
		if (a->expect)
			json_escape(a->expect, strlen(a->expect),
				    lx, sizeof(lx));
		if (a->enums)
			json_escape(a->enums, strlen(a->enums), len, sizeof(len));

		o += (size_t)snprintf(body + o, cap - o,
				      "%s{\"slot\":%d,\"param\":\"%s\","
				      "\"label\":\"%s\",\"example\":\"%s\","
				      "\"expect\":\"%s\",\"required\":%s",
				      j ? "," : "", j + 1, param,
				      ld, le, lx,
				      a->required ? "true" : "false");

		if (a->enums)
			o += (size_t)snprintf(body + o, cap - o,
					      ",\"enum\":\"%s\"", len);
		if (a->has_range)
			o += (size_t)snprintf(body + o, cap - o,
					      ",\"range\":[%d,%d]",
					      a->min, a->max);

		o += (size_t)snprintf(body + o, cap - o, "}");

		if (o >= cap - 2048)
			return o;
	}
	o += (size_t)snprintf(body + o, cap - o, "]}");
	return o;
}

static void list_commands(int fd)
{
	char body[MAX_OUT];
	size_t o = 0;
	int i;

	o += (size_t)snprintf(body + o, sizeof(body) - o,
			      "{\"ok\":true,\"version\":\"%s\",\"commands\":[",
			      SHCMD_VERSION);
	for (i = 0; i < cfg.n_entries; i++) {
		if (o >= sizeof(body) - 2048)
			break;
		o = emit_entry(body, sizeof(body), o, &cfg.entries[i], i == 0);
	}
	o += (size_t)snprintf(body + o, sizeof(body) - o, "]}\n");
	send_response(fd, 200, "application/json", body, o, 0);
}

/*
 * /schema - the full machine-readable contract.
 *
 * This is what a program (or an assistant driving one) should read before
 * calling /r. It states, per command: the request URL shape, every argument's
 * parameter name, its constraint, whether it is required, the timeout, and the
 * exact shape of the response. That way a caller does not have to discover
 * boundaries by trial and error -- the daemon rejects out-of-enum values
 * anyway, so guessing just burns round trips.
 */
static void send_schema(int fd)
{
	char body[MAX_OUT];
	size_t o = 0;
	int i;

	o += (size_t)snprintf(body + o, sizeof(body) - o,
		"{\"ok\":true,\"version\":\"%s\","
		"\"request\":{"
		    "\"url\":\"/r?n=<command>&k=<token>[&a=<v1>][&a1=<v2>]...\","
		    "\"params\":{"
			"\"n\":\"command name, required\","
			"\"k\":\"token, required (or Authorization: Bearer)\","
			"\"a\":\"slot 1 value\","
			"\"a1\":\"slot 2 value\","
			"\"a2\":\"slot 3 value\"},"
		    "\"note\":\"argument parameter names are a, a1, a2 ... (slot N maps to a(N-1)); values are passed as individual argv entries and are never shell-interpreted unless the command wraps them in sh -c\","
		    "\"methods\":[\"GET\",\"POST\"]"
		"},"
		"\"response\":{"
		    "\"ok\":\"bool\",\"command\":\"string\",\"status\":\"int exit code (128+N if signalled)\","
		    "\"timed_out\":\"bool\",\"truncated\":\"bool (output capped at 65536 bytes)\","
		    "\"output\":\"string, combined stdout+stderr\","
		    "\"error\":\"string, present only when ok=false\""
		"},"
		"\"errors\":{"
		    "\"400\":\"bad request, missing required argument, or argument outside its enum/range\","
		    "\"401\":\"missing or wrong token\",\"403\":\"source address not allowed\","
		    "\"404\":\"unknown command\",\"405\":\"method not GET/POST\","
		    "\"413\":\"request too large\",\"503\":\"daemon unreachable via LuCI proxy\""
		"},"
		"\"commands\":[", SHCMD_VERSION);

	for (i = 0; i < cfg.n_entries; i++) {
		if (o >= sizeof(body) - 4096)
			break;
		o = emit_entry(body, sizeof(body), o, &cfg.entries[i], i == 0);
	}

	o += (size_t)snprintf(body + o, sizeof(body) - o, "]}\n");
	send_response(fd, 200, "application/json", body, o, 0);
}

static void handle_client(int fd, const struct sockaddr_storage *peer)
{
	char req[MAX_REQ + 1];
	size_t used = 0;
	char *body_start = NULL;
	ssize_t n;

	if (!addr_allowed(peer)) {
		reply_text(fd, 403, "source address not allowed");
		return;
	}

	/* read until end of headers */
	for (;;) {
		if (used >= MAX_REQ) {
			reply_text(fd, 413, "request too large");
			return;
		}
		n = recv(fd, req + used, MAX_REQ - used, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			reply_text(fd, 400, "read error");
			return;
		}
		if (n == 0)
			break;
		used += (size_t)n;
		req[used] = '\0';
		body_start = strstr(req, "\r\n\r\n");
		if (body_start) {
			body_start += 4;
			break;
		}
	}
	req[used] = '\0';

	if (!body_start) {
		reply_text(fd, 400, "malformed request");
		return;
	}

	/* request line */
	char *line_end = strstr(req, "\r\n");
	if (!line_end) {
		reply_text(fd, 400, "malformed request line");
		return;
	}
	*line_end = '\0';

	char method[16], target[MAX_REQ];
	if (sscanf(req, "%15s %4095s", method, target) != 2) {
		reply_text(fd, 400, "malformed request line");
		return;
	}
	if (strcmp(method, "GET") && strcmp(method, "POST")) {
		reply_text(fd, 405, "only GET and POST");
		return;
	}

	/* headers we care about */
	char auth_hdr[MAX_ARG_LEN * 2] = "";
	char *hdr = line_end + 2;
	char *hdr_end = body_start - 2;
	long content_length = -1;
	while (hdr < hdr_end) {
		char *eol = strstr(hdr, "\r\n");
		if (!eol || eol > hdr_end)
			break;
		*eol = '\0';
		char *colon = strchr(hdr, ':');
		if (colon) {
			*colon = '\0';
			char *val = colon + 1;
			while (*val == ' ' || *val == '\t')
				val++;
			if (!strcasecmp(hdr, "Authorization"))
				snprintf(auth_hdr, sizeof(auth_hdr), "%s", val);
			else if (!strcasecmp(hdr, "Content-Length"))
				content_length = strtol(val, NULL, 10);
		}
		hdr = eol + 2;
	}

	/*
	 * For POST, make sure the whole form body has actually arrived. A single
	 * recv() often returns the head only; acting on a partial body would
	 * randomly drop parameters.
	 */
	if (!strcmp(method, "POST")) {
		if (content_length > (long)MAX_REQ) {
			reply_text(fd, 413, "body too large");
			return;
		}
		if (content_length > 0) {
			size_t want = (size_t)content_length;
			size_t have = used - (size_t)(body_start - req);
			while (have < want) {
				if (used >= MAX_REQ) {
					reply_text(fd, 413, "body too large");
					return;
				}
				n = recv(fd, req + used, MAX_REQ - used, 0);
				if (n <= 0) {
					if (n < 0 && errno == EINTR)
						continue;
					break;      /* short body: use what we have */
				}
				used += (size_t)n;
				have += (size_t)n;
			}
			req[used] = '\0';
		}
	}

	/* path + own query string */
	char *query = strchr(target, '?');
	if (query)
		*query++ = '\0';

	char *form_body = NULL;
	if (!strcmp(method, "POST"))
		form_body = body_start;

	/* ---- authentication ---- */
	char token[256] = "";
	if (param_get(query, "k", token, sizeof(token)) != 0 ||
	    !token[0]) {
		const char *bp = "Bearer ";
		size_t bl = strlen(bp);
		if (!strncmp(auth_hdr, bp, bl))
			snprintf(token, sizeof(token), "%s", auth_hdr + bl);
		else if (form_body && param_get(form_body, "k", token,
						sizeof(token)) == 0) {
			/* picked up from the POST body */
		} else {
			token[0] = '\0';
		}
	}
	if (!token_ok(token)) {
		logmsg("auth failure from client");
		reply_text(fd, 401, "bad or missing token");
		return;
	}

	/* ---- routing ---- */
	if (!strcmp(target, "/") || !strcmp(target, "/c")) {
		list_commands(fd);
		return;
	}
	if (!strcmp(target, "/schema")) {
		send_schema(fd);
		return;
	}
	if (strcmp(target, "/r")) {
		reply_text(fd, 404, "no such endpoint");
		return;
	}

	char name[128] = "";
	if (param_get(query, "n", name, sizeof(name)) != 0 || !name[0]) {
		if (!form_body ||
		    param_get(form_body, "n", name, sizeof(name)) != 0 ||
		    !name[0]) {
			reply_text(fd, 400, "missing parameter n");
			return;
		}
	}

	struct entry *e = entry_find(name);
	if (!e) {
		reply_text(fd, 404, "unknown command");
		return;
	}

	/* ---- extra arguments ---- */
	char *argv[MAX_CMD_ARGV + MAX_ARGS + 1];
	int argc = 0;
	int i;

	/*
	 * Collect caller arguments first, then decide how they enter argv.
	 *
	 * Two mechanisms, and the difference matters:
	 *
	 *   1. Placeholder substitution. If the config wrote "{a1}" (or "{a2}"
	 *      etc.) inside an argv slot, that occurrence is replaced by the
	 *      corresponding caller value *inside that same slot*. This is the
	 *      only correct way to feed `sh -c 'script'`: the script text is one
	 *      argv slot, and appending the value after it would land in $0
	 *      rather than $1.
	 *
	 *   2. Plain append, when the config has no placeholders. Convenient for
	 *      `ping -c 3 -W 2` style commands where the value goes last.
	 */
	char *extra[MAX_ARGS];
	int n_extra = 0;
	char val[MAX_ARG_LEN];

	if (cfg.max_args > 0 && param_get(query, "a", val, sizeof(val)) == 0 &&
	    val[0])
		extra[n_extra++] = xstrdup(val);

	for (i = 1; i < cfg.max_args && n_extra < MAX_ARGS; i++) {
		char key[16];

		snprintf(key, sizeof(key), "a%d", i);
		if (param_get(query, key, val, sizeof(val)) == 0 && val[0])
			extra[n_extra++] = xstrdup(val);
	}

	/*
	 * Enforce the declared constraints BEFORE running anything.
	 *
	 * The shell scripts do validate too, but relying on them means a caller
	 * learns the boundary from a failed execution (and, for a command with
	 * side effects, possibly after doing half the work). Checking here keeps
	 * the contract in one place, makes the error uniform, and lets /schema
	 * tell the truth about what is acceptable.
	 */
	for (i = 0; i < e->n_argdesc; i++) {
		const struct arg_spec *a = &e->args[i];
		const char *v = (i < n_extra) ? extra[i] : NULL;

		if (!v || !v[0]) {
			if (a->required) {
				for (int k = 0; k < n_extra; k++)
					free(extra[k]);
				reply_text(fd, 400, "missing required argument");
				return;
			}
			continue;
		}

		if (a->enums) {
			int ok = 0;
			const char *p = a->enums;
			size_t vlen = strlen(v);

			while (*p) {
				const char *comma = strchr(p, ',');
				size_t l = comma ? (size_t)(comma - p) : strlen(p);
				if (l == vlen && !strncmp(p, v, l)) {
					ok = 1;
					break;
				}
				if (!comma)
					break;
				p = comma + 1;
			}
			if (!ok) {
				char msg[512];
				snprintf(msg, sizeof(msg),
					 "argument %d: '%s' is not one of: %s",
					 i + 1, v, a->enums);
				for (int k = 0; k < n_extra; k++)
					free(extra[k]);
				reply_text(fd, 400, msg);
				return;
			}
		}

		if (a->has_range) {
			char *end = NULL;
			long n = strtol(v, &end, 10);
			if (!end || *end != '\0' || n < a->min || n > a->max) {
				char msg[256];
				snprintf(msg, sizeof(msg),
					 "argument %d must be an integer in %d-%d",
					 i + 1, a->min, a->max);
				for (int k = 0; k < n_extra; k++)
					free(extra[k]);
				reply_text(fd, 400, msg);
				return;
			}
		}
	}

	/*
	 * Everything below argv[] may own heap memory: substitution copies for
	 * slots that contained {aN}/{saN}, plus any extra[] entries that were
	 * appended. Track ownership explicitly rather than guessing from the
	 * index, because a substituted slot sits where the config slot was.
	 */
	int owned[MAX_CMD_ARGV + MAX_ARGS + 1];
	memset(owned, 0, sizeof(owned));

	int used_placeholder = 0;

	for (i = 0; i < e->argc; i++) {
		const char *src = e->argv[i];
		if (!strstr(src, "{a") && !strstr(src, "{s")) {
			argv[argc++] = e->argv[i];
			continue;
		}
		/* Build a substituted copy of this single argv slot. */
		size_t cap = strlen(src) + 1;
		for (int k = 0; k < n_extra; k++)
			cap += strlen(extra[k]) * 4 + 8;   /* room for quoting */
		char *out = xmalloc(cap);
		size_t o = 0;
		const char *p = src;

		while (*p) {
			/*
			 * Two forms share one code path:
			 *   {aN}  -> { a N }          at p,        length 4
			 *   {saN} -> { s a N }        at p,        length 5
			 * In both cases the digit sits at p[2] ({aN}) or p[3] ({saN}).
			 * Parse the digit and the total length explicitly instead of
			 * rebasing a pointer: an earlier version set q = p + 1 for the
			 * {saN} case and then tested q[0] == '{', which can never hold
			 * (q[0] is 's'), so {saN} was silently left unsubstituted.
			 */
			int is_quoted = 0;
			int idx = -1;
			int skip = 0;

			if (p[0] == '{' && p[1] == 's' && p[2] == 'a' &&
			    isdigit((unsigned char)p[3])) {
				is_quoted = 1;
				idx = p[3] - '1';
				skip = 5;
			} else if (p[0] == '{' && p[1] == 'a' &&
				   isdigit((unsigned char)p[2])) {
				idx = p[2] - '1';
				skip = 4;
			}

			if (idx >= 0 && idx < n_extra) {
				const char *v = extra[idx];

				if (is_quoted) {
					out[o++] = '\'';
					for (const char *s = v; *s; s++) {
						if (*s == '\'') {
							/* ' -> '\'' */
							memcpy(out + o, "'\\''", 4);
							o += 4;
						} else {
							out[o++] = *s;
						}
					}
					out[o++] = '\'';
				} else {
					size_t l = strlen(v);
					memcpy(out + o, v, l);
					o += l;
				}
				p += skip;
				used_placeholder = 1;
				continue;
			}

			/*
			 * A placeholder naming a slot the caller did not supply. Leaving
			 * the literal "{sa1}" in argv would be worse than useless: a
			 * shell command would see a meaningless string and the error
			 * would blame the wrong thing. Refuse, naming the slot.
			 */
			if (idx >= 0) {
				for (int k = 0; k < n_extra; k++)
					free(extra[k]);
				for (int k = 0; k < argc; k++)
					if (owned[k])
						free(argv[k]);
				free(out);
				reply_text(fd, 400, "missing required argument");
				return;
			}
			out[o++] = *p++;
		}
		out[o] = '\0';
		owned[argc] = 1;
		argv[argc++] = out;
	}

	if (!used_placeholder) {
		for (i = 0; i < n_extra; i++) {
			owned[argc] = 1;
			argv[argc++] = extra[i];
		}
	} else {
		/*
		 * Placeholders consumed the values it referenced; anything the
		 * caller sent that no placeholder used is simply dropped, so its
		 * buffer has to be released here.
		 */
		for (i = 0; i < n_extra; i++) {
			int used = 0;
			for (int k = 0; k < argc; k++)
				if (argv[k] == extra[i])
					used = 1;
			if (!used)
				free(extra[i]);
		}
	}

	argv[argc] = NULL;

	/* ---- run ---- */
	struct run_result res;
	run_argv(argv, e->timeout, &res);

	char *escaped = xmalloc(res.len * 6 + 1);
	json_escape(res.out, res.len, escaped, res.len * 6 + 1);

	char body[MAX_OUT + 1024];
	int blen = snprintf(body, sizeof(body),
			    "{\"ok\":%s,\"command\":\"%s\",\"status\":%d,"
			    "\"timed_out\":%s,\"truncated\":%s,\"output\":\"%s\"}\n",
			    (res.status == 0 && !res.timed_out) ? "true" : "false",
			    e->name, res.status,
			    res.timed_out ? "true" : "false",
			    res.truncated ? "true" : "false",
			    escaped);
	if (blen < 0)
		blen = 0;
	if ((size_t)blen >= sizeof(body))
		blen = sizeof(body) - 1;

	send_response(fd, 200, "application/json", body, (size_t)blen, 0);

	for (i = 0; i < argc; i++)
		if (owned[i])
			free(argv[i]);
	free(escaped);
	free(res.out);
}

/* ------------------------------------------------------------------ */
/* server                                                              */
/* ------------------------------------------------------------------ */

static int open_listener(const char *addr, int port)
{
	struct sockaddr_storage ss;
	socklen_t slen;
	int fd, on = 1;

	memset(&ss, 0, sizeof(ss));

	struct sockaddr_in *v4 = (struct sockaddr_in *)&ss;
	struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&ss;

	if (inet_pton(AF_INET, addr, &v4->sin_addr) == 1) {
		v4->sin_family = AF_INET;
		v4->sin_port = htons((uint16_t)port);
		slen = sizeof(*v4);
	} else if (inet_pton(AF_INET6, addr, &v6->sin6_addr) == 1) {
		v6->sin6_family = AF_INET6;
		v6->sin6_port = htons((uint16_t)port);
		slen = sizeof(*v6);
	} else {
		logmsg("bad listen address '%s'", addr);
		return -1;
	}

	fd = socket(ss.ss_family, SOCK_STREAM, 0);
	if (fd < 0) {
		logmsg("socket: %s", strerror(errno));
		return -1;
	}
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

	if (ss.ss_family == AF_INET6) {
		int on6 = 1;
		/*
		 * v6-only by default. "::" with V6ONLY off also accepts
		 * v4-mapped peers, which would silently bypass an IPv4-shaped
		 * allowlist. Set an explicit 0.0.0.0 instance instead if both
		 * families are wanted.
		 */
		setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on6, sizeof(on6));
	}

	if (bind(fd, (struct sockaddr *)&ss, slen) < 0) {
		logmsg("bind %s:%d: %s", addr, port, strerror(errno));
		close(fd);
		return -1;
	}
	if (listen(fd, 8) < 0) {
		logmsg("listen: %s", strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

int main(int argc, char **argv)
{
	const char *cfgpath = "/etc/shcmd.conf";
	int foreground = 0;
	int opt;

	cfg_defaults();

	while ((opt = getopt(argc, argv, "c:fhv")) != -1) {
		switch (opt) {
		case 'c':
			cfgpath = optarg;
			break;
		case 'f':
			foreground = 1;
			break;
		case 'v':
			printf("shcmd %s\n", SHCMD_VERSION);
			return 0;
		case 'h':
		default:
			printf("usage: shcmd [-f] [-c config]\n");
			return opt == 'h' ? 0 : 1;
		}
	}

	if (cfg_load(cfgpath) < 0)
		return 1;

	if (cfg.n_tokens == 0)
		logmsg("warning: no token configured, every request will be refused");
	if (cfg.n_entries == 0)
		logmsg("warning: no commands configured");

	if (!foreground) {
		pid_t pid = fork();
		if (pid < 0) {
			logmsg("fork: %s", strerror(errno));
			return 1;
		}
		if (pid > 0)
			return 0;
		setsid();
		int devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			if (devnull > STDERR_FILENO)
				close(devnull);
		}
	}

	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, SIG_DFL);

	/*
	 * Install the stop handlers WITHOUT SA_RESTART. With SA_RESTART (and on
	 * musl the accept() call can end up transparently restarted) the loop
	 * would never observe EINTR, so procd's SIGTERM would be ignored and it
	 * would escalate to SIGKILL -- which is exactly what the board logs
	 * showed before this change.
	 */
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	int lfd = open_listener(cfg.listen, cfg.port);
	if (lfd < 0)
		return 1;
	g_listen_fd = lfd;

	logmsg("listening on %s:%d, %d command(s), %d allow rule(s)",
	       cfg.listen, cfg.port, cfg.n_entries, cfg.n_addrs);

	while (!g_stop) {
		struct sockaddr_storage peer;
		socklen_t plen = sizeof(peer);

		int cfd = accept(lfd, (struct sockaddr *)&peer, &plen);
		if (cfd < 0) {
			if (errno == EINTR)
				continue;
			logmsg("accept: %s", strerror(errno));
			continue;
		}
		g_client_fd = cfd;

		struct timeval tv = { 5, 0 };
		setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

		handle_client(cfd, &peer);
		close(cfd);
	}

	close(lfd);
	logmsg("stopped");
	return 0;
}
