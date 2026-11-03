/* Dreambox: the dcvmu.com network drive as a FileSwitch filing system.
 *
 * Every FileSwitch entry lands in dreambox_fs, which runs one operation as a
 * small state machine: it hands a request to the worker thread, says "call
 * again" until the answer is in, then carries on. Only this side touches guest
 * memory, and only the worker touches the network.
 *
 * Memory is the point of the design. File data moves through one 32KB window,
 * shared by every open file and by whole-file loads and saves, and only held
 * while something needs it. The server stages written blocks until a commit,
 * so a file of any size never has to fit here. A directory listing is kept as
 * the server's text, a few KB, so the Filer's paging and FileSwitch's many
 * "read catalogue info" calls don't each cost a round trip. The connection is
 * kept alive between requests and dropped when idle.
 *
 * Consoles link with a code shown in an error box and typed on dcvmu.com, so no
 * password crosses a TLS link that can't check certificates. */
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "dreambox.h"
#include "net.h"
#include "web.h"

#define BLOCK (32 * 1024)      /* the server's staging block, and our window */
#define LIST_MAX (16 * 1024)   /* a listing of 255 entries fits */
#define MAX_PATH 256
#define MAX_TARGET 1024
#define HANDLES 4
#define FS_BUFFER 1024         /* FileSwitch's buffer per open file */
#define LIST_FRESH_MS 5000
#define IDLE_CLOSE_MS 20000    /* well inside a proxy's keep-alive timeout */
#define TIMEOUT_MS 30000
#define RETRY_MS 5
#define TOKEN_MAX 64
#define ERR_BASE (0x10000u | BOX_FS_NUMBER << 8)
#define ERR_AGAIN 35
#define ERR_INPROGRESS 36
#define ERR_ALREADY 37
#define ERR_ISCONN 56
#define ERR_NOTCONN 57

enum { TYPE_NONE, TYPE_FILE, TYPE_DIR };
enum { MODE_READ, MODE_OUT, MODE_UP };
enum { ENTRY_OPEN, ENTRY_GET, ENTRY_PUT, ENTRY_ARGS, ENTRY_CLOSE, ENTRY_FILE, ENTRY_FUNC };

/* --- the worker: one HTTP request at a time --- */

enum { REQ_IDLE, REQ_PENDING, REQ_DONE };

static struct {
	volatile int state;
	int post;
	char target[MAX_TARGET];
	const uint8_t *body;
	int body_len;
	uint8_t *out;
	int out_cap;
	int status, out_len;   /* status 0: no answer, and message says why */
	char message[100];     /* the server's one line for a 4xx */
} req;

static struct { int fd; web_tls_t *tls; uint64_t used; } conn = {-1, NULL, 0};
static char server_host[64];
static int server_port, server_tls, started;
static char token[TOKEN_MAX];

static void conn_close(void)
{
	if (conn.fd < 0)
		return;
	if (conn.tls)
		plat_tls_close(conn.tls);
	net_sock_close(conn.fd);
	conn.fd = -1;
	conn.tls = NULL;
}

static int conn_open(uint64_t deadline)
{
	struct addrinfo hints, *res = NULL;
	uint8_t ip[4];
	int rv;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	if (getaddrinfo(server_host, NULL, &hints, &res) || !res)
		return 0;
	memcpy(ip, &((struct sockaddr_in *)res->ai_addr)->sin_addr, 4);
	freeaddrinfo(res);
	if ((conn.fd = net_sock_open(SOCK_KIND_TCP)) < 0)
		return 0;
	for (;;) {
		rv = net_sock_connect(conn.fd, ip, server_port);
		if (rv == 0 || rv == -ERR_ISCONN)
			break;
		if ((rv != -ERR_INPROGRESS && rv != -ERR_ALREADY && rv != -ERR_AGAIN) || plat_wall_ms() > deadline) {
			conn_close();
			return 0;
		}
		plat_sleep_ms(RETRY_MS);
	}
	if (server_tls && !(conn.tls = plat_tls_open(conn.fd, server_host))) {
		conn_close();
		return 0;
	}
	return 1;
}

static int conn_write(const void *data, int len, uint64_t deadline)
{
	const char *p = data;
	while (len > 0) {
		int n = conn.tls ? plat_tls_write(conn.tls, p, len) : net_sock_send(conn.fd, p, len);
		if (n > 0) {
			p += n;
			len -= n;
		} else if ((n == -ERR_AGAIN || n == -ERR_NOTCONN) && plat_wall_ms() < deadline) {
			plat_sleep_ms(RETRY_MS);
		} else {
			return 0;
		}
	}
	return 1;
}

/* Bytes read, 0 at the end, -1 on failure */
static int conn_read(void *data, int len, uint64_t deadline)
{
	for (;;) {
		int n = conn.tls ? plat_tls_read(conn.tls, data, len) : net_sock_recv(conn.fd, data, len);
		if (n >= 0)
			return n;
		if (n != -ERR_AGAIN || plat_wall_ms() > deadline)
			return -1;
		plat_sleep_ms(RETRY_MS);
	}
}

static const char *header(const char *head, const char *name)
{
	size_t n = strlen(name);
	const char *line = strstr(head, "\r\n");
	while (line && line[2] != '\r') {
		line += 2;
		if (!strncasecmp(line, name, n) && line[n] == ':')
			return line + n + 1 + strspn(line + n + 1, " ");
		line = strstr(line, "\r\n");
	}
	return NULL;
}

/* One go at the request. *answered says whether any reply came back, as a
 * request on a connection the far end dropped can safely go again. */
static int http_once(int *answered)
{
	static char head[MAX_TARGET + 512];
	uint64_t deadline = plat_wall_ms() + TIMEOUT_MS;
	char *end, *dest;
	const char *value;
	long length = -1;
	int n, have = 0, minor = 0, keep, cap, put = 0;
	*answered = 0;
	if (conn.fd < 0 && !conn_open(deadline)) {
		snprintf(req.message, sizeof(req.message), "Dreambox could not reach %s", server_host);
		return 0;
	}
	n = snprintf(head, sizeof(head), "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Archicast-Dreambox/1.0\r\n"
		"Content-Type: application/octet-stream\r\nContent-Length: %d\r\n%s%s%s\r\n",
		req.post ? "POST" : "GET", req.target, server_host, req.body_len,
		token[0] ? "Authorization: Bearer " : "", token, token[0] ? "\r\n" : "");
	if (!conn_write(head, n, deadline) || (req.body_len && !conn_write(req.body, req.body_len, deadline)))
		goto failed;
	head[0] = 0;
	while (!(end = strstr(head, "\r\n\r\n"))) {
		if (have >= (int)sizeof(head) - 1 || (n = conn_read(head + have, (int)sizeof(head) - 1 - have, deadline)) <= 0)
			goto failed;
		*answered = 1;
		have += n;
		head[have] = 0;
	}
	if (sscanf(head, "HTTP/1.%d %d", &minor, &req.status) != 2)
		goto failed;
	keep = minor >= 1 && !((value = header(head, "Connection")) && !strncasecmp(value, "close", 5));
	if ((value = header(head, "Content-Length")))
		length = strtol(value, NULL, 10);
	else
		keep = 0;
	/* A 4xx carries one line for the user; anything else goes where the caller said */
	dest = req.status >= 300 ? req.message : (char *)req.out;
	cap = req.status >= 300 ? (int)sizeof(req.message) - 1 : req.out_cap;
	if (req.status < 300 && length > cap) {
		snprintf(req.message, sizeof(req.message), "Dreambox sent more than expected");
		goto failed;
	}
	end += 4;
	n = (int)(head + have - end);
	if (length >= 0 && n > length)
		n = (int)length;
	put = n < cap ? n : cap;
	memcpy(dest, end, (size_t)put);
	for (length = length >= 0 ? length - n : -1; length; ) {
		char *into = put < cap ? dest + put : head;
		int room = put < cap ? cap - put : (int)sizeof(head);
		if (length > 0 && room > length)
			room = (int)length;
		if ((n = conn_read(into, room, deadline)) < 0)
			goto failed;
		if (n == 0) {
			if (length > 0)
				goto failed;
			break;
		}
		if (into != head)
			put += n;
		if (length > 0)
			length -= n;
	}
	req.out_len = put;
	if (dest == req.message) {
		req.message[put] = 0;
		req.message[strcspn(req.message, "\r\n")] = 0;
	}
	if (!keep)
		conn_close();
	conn.used = plat_wall_ms();
	return 1;
failed:
	if (!req.message[0])
		snprintf(req.message, sizeof(req.message), "Dreambox lost touch with %s", server_host);
	conn_close();
	return 0;
}

static void *box_thread(void *arg)
{
	(void)arg;
	for (;;) {
		if (req.state == REQ_PENDING) {
			int answered, reused = conn.fd >= 0;
			req.status = req.out_len = 0;
			req.message[0] = 0;
			if (!http_once(&answered) && reused && !answered) {
				req.status = req.out_len = 0;
				req.message[0] = 0;
				if (!http_once(&answered))
					req.status = 0;
			}
			__sync_synchronize();
			req.state = REQ_DONE;
		} else if (conn.fd >= 0 && plat_wall_ms() - conn.used > IDLE_CLOSE_MS) {
			conn_close(); /* gives back the socket buffers and TLS state */
		}
		plat_sleep_ms(req.state == REQ_PENDING ? 0 : RETRY_MS);
	}
	return NULL;
}

/* --- requests, built on the main thread --- */

static void target(const char *endpoint)
{
	snprintf(req.target, sizeof(req.target), "/api/v1/drive/%s?", endpoint);
}

static void arg_text(const char *name, const char *value)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t n = strlen(req.target);
	n += (size_t)snprintf(req.target + n, sizeof(req.target) - n, "%s=", name);
	for (; *value && n + 4 < sizeof(req.target); value++) {
		unsigned char c = (unsigned char)*value;
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
			req.target[n++] = (char)c;
		} else {
			req.target[n++] = '%';
			req.target[n++] = hex[c >> 4];
			req.target[n++] = hex[c & 15];
		}
	}
	req.target[n++] = '&';
	req.target[n] = 0;
}

static void arg_num(const char *name, uint32_t value, int is_hex)
{
	size_t n = strlen(req.target);
	snprintf(req.target + n, sizeof(req.target) - n, is_hex ? "%s=%X&" : "%s=%u&", name, (unsigned)value);
}

static void send_request(int post, const void *body, int len, void *out, int cap)
{
	if (!started) {
		const char *base = plat_dreambox_server();
		started = 1;
		server_tls = !strncmp(base, "https://", 8);
		snprintf(server_host, sizeof(server_host), "%s", base + (server_tls ? 8 : 7));
		server_host[strcspn(server_host, "/")] = 0;
		server_port = server_tls ? 443 : 80;
		if (strchr(server_host, ':')) {
			server_port = atoi(strchr(server_host, ':') + 1);
			*strchr(server_host, ':') = 0;
		}
		plat_thread_start(box_thread);
	}
	req.post = post;
	req.body = body;
	req.body_len = len;
	req.out = out;
	req.out_cap = cap;
	__sync_synchronize();
	req.state = REQ_PENDING;
}

/* --- state kept between calls --- */

typedef struct {
	int used, mode;
	char path[MAX_PATH];
	uint32_t extent, load, exec, attr;
	uint32_t known;  /* bytes the server can give back: the size at open, then what we flushed */
	int staged;      /* a block has gone up since the open */
	int modified;
} handle_t;

static handle_t handles[HANDLES];
static uint8_t *window;
static int win_handle = -1, win_dirty;
static uint32_t win_block, win_valid;

static char *list_text;   /* the server's listing, one object per line */
static int list_len;
static char list_dir[MAX_PATH];
static uint64_t list_at;

static uint32_t workspace;
static const guest_mem_t *G;
static uint32_t *regs;
static char small[512];   /* short replies */
static int token_loaded;
static char pair_device[64], pair_code[16];
static uint64_t pair_until;

static struct {
	int active, entry, step, lstep;
	char path[MAX_PATH], path2[MAX_PATH];
	uint32_t at, size, block;
	handle_t *h;
} op;

/* Protothreads: AWAIT sends a request and returns; the next call resumes after it,
 * once the worker has the answer. Locals that must live across one go in op. */
#define AWAIT_(n, post, body, len, out, cap) do { send_request(post, body, len, out, cap); \
	op.step = n; return BOX_AGAIN; case n:; } while (0)
#define AWAIT(post, body, len, out, cap) AWAIT_(__COUNTER__ + 1, post, body, len, out, cap)

static int dreambox_error(const char *fmt, ...)
{
	char text[200];
	va_list ap;
	uint32_t number = ERR_BASE;
	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	if (!workspace)
		return BOX_ERROR;
	G->write(workspace, &number, 4);
	G->write(workspace + 4, text, (uint32_t)strlen(text) + 1);
	regs[0] = workspace;
	return BOX_ERROR;
}

/* A failed request or a 4xx, as an error for the user. A 401 means the link was
 * revoked on the website, so forget it and the next call starts linking again. */
static int request_error(void)
{
	if (req.status == 401) {
		token[0] = 0;
		plat_dreambox_save("");
		return dreambox_error("Dreambox was unlinked. Open it again to link this Dreamcast");
	}
	if (req.status)
		return dreambox_error("Dreambox: %s", req.message[0] ? req.message : "the server refused that");
	return dreambox_error("%s", req.message);
}

/* FileSwitch's names, with any disc name and $ taken off: "$.Docs.Letter" is "Docs.Letter" */
static void path_from(uint32_t addr, char *out)
{
	char name[MAX_PATH], *p = name;
	G->string(addr, name, sizeof(name));
	if (*p == ':') {
		p += strcspn(p, ".");
		p += *p == '.';
	}
	if (*p == '$')
		p += 1 + (p[1] == '.');
	snprintf(out, MAX_PATH, "%s", p);
}

static int window_get(void)
{
	if (!window && !(window = malloc(BLOCK)))
		return 0;
	return 1;
}

static void maybe_release(void)
{
	int n;
	if (list_text && (!list_at || plat_wall_ms() - list_at > LIST_FRESH_MS)) {
		free(list_text);
		list_text = NULL;
		list_at = 0;
	}
	for (n = 0; n < HANDLES; n++)
		if (handles[n].used)
			return;
	free(window);
	window = NULL;
	win_handle = -1;
	win_dirty = 0;
}

static void list_forget(void)
{
	list_dir[0] = 0;
	list_at = 0;
}

/* One object's line: "type load exec size attr name" */
typedef struct { int type; uint32_t load, exec, size, attr; char name[40]; } object_t;

static const char *parse_object(const char *line, object_t *o)
{
	char *p;
	int i = 0;
	o->type = (int)strtoul(line, &p, 10);
	o->load = (uint32_t)strtoul(p, &p, 16);
	o->exec = (uint32_t)strtoul(p, &p, 16);
	o->size = (uint32_t)strtoul(p, &p, 10);
	o->attr = (uint32_t)strtoul(p, &p, 16);
	p += *p == ' ';
	while (*p >= ' ' && i < (int)sizeof(o->name) - 1)
		o->name[i++] = *p++;
	o->name[i] = 0;
	while (*p && *p != '\n')
		p++;
	return *p ? p + 1 : p;
}

/* The listing's index'th object, or NULL past the end */
static const char *list_line(int index)
{
	const char *p = list_text, *end = list_text + list_len;
	while (p && p < end && index--) {
		p = memchr(p, '\n', (size_t)(end - p));
		p = p ? p + 1 : NULL;
	}
	return p && p < end ? p : NULL;
}

/* Catalogue info from a fresh listing of the parent, saving a round trip. 1 found,
 * 0 not there, -1 the listing doesn't say. */
static int list_find(const char *path, object_t *o)
{
	const char *dot = strrchr(path, '.'), *leaf = dot ? dot + 1 : path;
	size_t dir_len = dot ? (size_t)(dot - path) : 0;
	const char *line;
	int i;
	if (!list_at || plat_wall_ms() - list_at > LIST_FRESH_MS || strlen(list_dir) != dir_len ||
	    strncasecmp(list_dir, path, dir_len))
		return -1;
	for (i = 0; (line = list_line(i)); i++) {
		parse_object(line, o);
		if (!strcasecmp(o->name, leaf))
			return 1;
	}
	return 0;
}

static void set_info(const object_t *o)
{
	regs[0] = (uint32_t)o->type;
	regs[2] = o->load;
	regs[3] = o->exec;
	regs[4] = o->size;
	regs[5] = o->attr;
}

/* --- linking --- */

/* BOX_DONE once there's a token, else an error box with the code to type in */
static int link_step(void)
{
	switch (op.lstep) {
	case 0:
		if (!token_loaded) {
			token_loaded = 1;
			if (!plat_dreambox_load(token, sizeof(token)))
				token[0] = 0;
		}
		if (token[0])
			return BOX_DONE;
		if (pair_device[0] && plat_wall_ms() < pair_until) {
			target("pair/claim");
			arg_text("device", pair_device);
			op.lstep = __COUNTER__ + 1;
			send_request(1, NULL, 0, small, sizeof(small) - 1);
			return BOX_AGAIN;
	case __COUNTER__:
			if (req.status == 200) {
				small[req.out_len] = 0;
				small[strcspn(small, "\r\n")] = 0;
				snprintf(token, sizeof(token), "%.*s", (int)sizeof(token) - 1, small);
				pair_device[0] = 0;
				plat_dreambox_save(token);
				op.lstep = 0;
				return BOX_DONE;
			}
			if (req.status == 202) {
				op.lstep = 0;
				goto ask;
			}
			pair_device[0] = 0;
		}
		target("pair");
		op.lstep = __COUNTER__ + 1;
		send_request(1, NULL, 0, small, sizeof(small) - 1);
		return BOX_AGAIN;
	case __COUNTER__: {
		char *code, *seconds;
		op.lstep = 0;
		small[req.out_len] = 0;
		if (req.status != 200 || !(code = strchr(small, '\n')) || !(seconds = strchr(code + 1, '\n')))
			return req.status && req.status != 200 ? request_error() :
				dreambox_error("%s", req.message[0] ? req.message : "Dreambox could not start linking");
		*code++ = 0;
		*seconds++ = 0;
		snprintf(pair_device, sizeof(pair_device), "%.*s", (int)sizeof(pair_device) - 1, small);
		snprintf(pair_code, sizeof(pair_code), "%s", code);
		pair_until = plat_wall_ms() + (uint64_t)strtoul(seconds, NULL, 10) * 1000 - 30000;
	}
	ask:
		return dreambox_error("To link Dreambox, log in at dcvmu.com, open Account, Archicast and enter %s",
			pair_code);
	}
	return BOX_DONE;
}

/* --- the window --- */

/* The handle's block in the window, flushing whatever was there. Expands inline
 * in an operation, as it may wait twice. */
#define WINDOW_FOR(h_, block_) do { \
	op.block = (block_); \
	if (win_handle != (int)((h_) - handles) || win_block != op.block) { \
		if (win_dirty) { \
			FLUSH(); \
		} \
		if (!window_get()) \
			return dreambox_error("Not enough memory for Dreambox"); \
		win_handle = (int)((h_) - handles); \
		win_block = op.block; \
		win_valid = 0; \
		memset(window, 0, BLOCK); \
		if (op.block * BLOCK < (h_)->known) { \
			target("read"); \
			arg_text("path", (h_)->path); \
			arg_num("offset", op.block * BLOCK, 0); \
			arg_num("length", BLOCK, 0); \
			arg_num("staged", (uint32_t)(h_)->staged, 0); \
			win_handle = -1; \
			AWAIT(0, NULL, 0, window, BLOCK); \
			if (req.status != 200) \
				return request_error(); \
			win_handle = (int)((h_) - handles); \
			win_valid = (uint32_t)req.out_len; \
		} \
	} \
} while (0)

/* Sends the window's block to staging */
#define FLUSH() do { \
	handle_t *f_ = &handles[win_handle]; \
	target("write"); \
	arg_text("path", f_->path); \
	arg_num("block", win_block, 0); \
	arg_num("fresh", !f_->staged, 0); \
	AWAIT(1, window, (int)win_valid, small, sizeof(small) - 1); \
	if (req.status != 200) \
		return request_error(); \
	f_ = &handles[win_handle]; \
	f_->staged = 1; \
	if (win_block * BLOCK + win_valid > f_->known) \
		f_->known = win_block * BLOCK + win_valid; \
	win_dirty = 0; \
} while (0)

/* --- FileSwitch's entries --- */

static int handle_from(uint32_t value)
{
	return value >= 1 && value <= HANDLES && handles[value - 1].used ? (int)value - 1 : -1;
}

static int box_open(void)
{
	static object_t o;
	int n;
	switch (op.step) {
	case 0:
		path_from(regs[1], op.path);
		for (n = 0; n < HANDLES && handles[n].used; n++)
			;
		if (n == HANDLES)
			return dreambox_error("Dreambox has %d files open already", HANDLES);
		op.h = &handles[n];
		memset(op.h, 0, sizeof(*op.h));
		snprintf(op.h->path, sizeof(op.h->path), "%s", op.path);
		op.h->mode = regs[0] == 1 ? MODE_OUT : regs[0] == 2 ? MODE_UP : MODE_READ;
		if (op.h->mode == MODE_OUT) {
			/* Created now, so it's there for anyone who looks before the close */
			target("commit");
			arg_text("path", op.path);
			arg_num("size", 0, 0);
			arg_num("load", op.h->load = 0xFFFFFD00, 1);
			arg_num("exec", op.h->exec = 0, 1);
			arg_num("staged", 0, 0);
			list_forget();
			AWAIT(1, NULL, 0, small, sizeof(small) - 1);
			if (req.status != 200)
				return request_error();
			op.h->attr = 3;
		} else {
			if ((n = list_find(op.path, &o)) < 0) {
				target("info");
				arg_text("path", op.path);
				AWAIT(0, NULL, 0, small, sizeof(small) - 1);
				if (req.status != 200 && req.status != 404)
					return request_error();
				small[req.out_len] = 0;
				if (req.status == 404)
					o.type = TYPE_NONE;
				else
					parse_object(small, &o);
			} else if (!n) {
				o.type = TYPE_NONE;
			}
			if (o.type != TYPE_FILE) {
				regs[1] = 0; /* not found, and FileSwitch says so */
				return BOX_DONE;
			}
			op.h->extent = op.h->known = o.size;
			op.h->load = o.load;
			op.h->exec = o.exec;
			op.h->attr = o.attr;
		}
		op.h->used = 1;
		regs[0] = (op.h->mode == MODE_READ ? 0 : 1u << 31) | 1u << 30;
		regs[1] = (uint32_t)(op.h - handles) + 1;
		regs[2] = FS_BUFFER;
		regs[3] = op.h->extent;
		regs[4] = (op.h->extent + FS_BUFFER - 1) & ~(uint32_t)(FS_BUFFER - 1);
	}
	return BOX_DONE;
}

/* GetBytes, PutBytes and Args 7 (write zeros). Bytes take r2 memory, r3 count and
 * r4 offset; zeros take r2 offset and r3 count. */
static int box_bytes(void)
{
	int zeros = op.entry == ENTRY_ARGS, n = handle_from(regs[1]);
	uint32_t base = zeros ? regs[2] : regs[4], end = base + regs[3], take, in_block;
	switch (op.step) {
	case 0:
		if (n < 0)
			return dreambox_error("Dreambox has no such open file");
		op.h = &handles[n];
		for (op.at = base; op.at < end; op.at += take) {
			WINDOW_FOR(op.h, op.at / BLOCK);
			in_block = op.at % BLOCK;
			take = BLOCK - in_block < end - op.at ? BLOCK - in_block : end - op.at;
			if (op.entry == ENTRY_GET) {
				G->write(regs[2] + op.at - base, window + in_block, take);
				continue;
			}
			if (zeros)
				memset(window + in_block, 0, take);
			else
				G->read(regs[2] + op.at - base, window + in_block, take);
			win_dirty = op.h->modified = 1;
			if (in_block + take > win_valid)
				win_valid = in_block + take;
			if (op.at + take > op.h->extent)
				op.h->extent = op.at + take;
		}
	}
	return BOX_DONE;
}

static int box_args(void)
{
	int n = handle_from(regs[1]);
	if (n < 0)
		return dreambox_error("Dreambox has no such open file");
	switch (regs[0]) {
	case 3: /* write extent */
		handles[n].extent = regs[2];
		handles[n].modified = 1;
		break;
	case 4: /* read size allocated */
		regs[2] = (handles[n].extent + FS_BUFFER - 1) & ~(uint32_t)(FS_BUFFER - 1);
		break;
	case 6: /* ensure size: the server allocates on commit */
		break;
	case 7:
		return box_bytes();
	case 8: /* read datestamp */
		regs[2] = handles[n].load;
		regs[3] = handles[n].exec;
		break;
	}
	return BOX_DONE;
}

static int box_close(void)
{
	int n;
	switch (op.step) {
	case 0:
		if ((n = handle_from(regs[1])) < 0)
			return BOX_DONE;
		op.h = &handles[n];
		if (regs[2] || regs[3]) {
			if (op.h->load != regs[2] || op.h->exec != regs[3])
				op.h->modified = 1;
			op.h->load = regs[2];
			op.h->exec = regs[3];
		}
		if (op.h->modified) {
			if (win_dirty && win_handle == n)
				FLUSH();
			target("commit");
			arg_text("path", op.h->path);
			arg_num("size", op.h->extent, 0);
			arg_num("load", op.h->load, 1);
			arg_num("exec", op.h->exec, 1);
			arg_num("attr", op.h->attr, 1);
			arg_num("staged", (uint32_t)op.h->staged, 0);
			arg_num("keep", op.h->mode == MODE_UP, 0);
			list_forget();
			AWAIT(1, NULL, 0, small, sizeof(small) - 1);
			if (win_handle == (int)(op.h - handles))
				win_handle = -1;
			op.h->used = 0;
			if (req.status != 200)
				return request_error();
		}
		if (win_handle == (int)(op.h - handles)) {
			win_handle = -1;
			win_dirty = 0;
		}
		op.h->used = 0;
	}
	return BOX_DONE;
}

/* A whole-file load or save borrows the window, so whatever an open file left
 * there goes up first */
#define WINDOW_FREE() do { \
	if (win_dirty) \
		FLUSH(); \
	if (!window_get()) \
		return dreambox_error("Not enough memory for Dreambox"); \
	win_handle = -1; \
} while (0)

static int stamp_request(uint32_t which)
{
	target("stamp");
	arg_text("path", op.path);
	if (which & 1)
		arg_num("load", regs[2], 1);
	if (which & 2)
		arg_num("exec", regs[3], 1);
	if (which & 4)
		arg_num("attr", regs[5], 1);
	list_forget();
	return 0;
}

/* Catalogue info for op.path into o, from the listing if it knows. No case
 * labels inside other switches here: AWAIT's would land in the wrong one. */
#define INFO(o) do { \
	int found_ = list_find(op.path, &(o)); \
	if (found_ < 0) { \
		target("info"); \
		arg_text("path", op.path); \
		AWAIT(0, NULL, 0, small, sizeof(small) - 1); \
		if (req.status != 200 && req.status != 404) \
			return request_error(); \
		small[req.out_len] = 0; \
		memset(&(o), 0, sizeof(o)); \
		if (req.status == 200) \
			parse_object(small, &(o)); \
	} else if (!found_) { \
		memset(&(o), 0, sizeof(o)); \
	} \
} while (0)

static int box_file(void)
{
	static object_t o;
	uint32_t reason = regs[0], take;
	switch (op.step) {
	case 0:
		path_from(regs[1], op.path);
		if (reason == 255) { /* load: r2 address */
			INFO(o);
			if (o.type != TYPE_FILE)
				return dreambox_error(o.type ? "Dreambox: that's a directory" : "Dreambox: file not found");
			WINDOW_FREE();
			for (op.at = 0; op.at < o.size; op.at += take) {
				target("read");
				arg_text("path", op.path);
				arg_num("offset", op.at, 0);
				arg_num("length", BLOCK, 0);
				AWAIT(0, NULL, 0, window, BLOCK);
				if (req.status != 200)
					return request_error();
				take = (uint32_t)req.out_len < o.size - op.at ? (uint32_t)req.out_len : o.size - op.at;
				if (!take)
					return dreambox_error("Dreambox: that file changed while loading");
				G->write(regs[2] + op.at, window, take);
			}
			regs[2] = o.load;
			regs[3] = o.exec;
			regs[4] = o.size;
			regs[5] = o.attr;
			regs[6] = regs[1];
		} else if (reason == 0 || reason == 7) { /* save, or create: r2 load, r3 exec, r4 start, r5 end */
			list_forget();
			if (reason == 0) {
				WINDOW_FREE();
				for (op.at = 0; op.at < regs[5] - regs[4]; op.at += take) {
					take = regs[5] - regs[4] - op.at < BLOCK ? regs[5] - regs[4] - op.at : BLOCK;
					G->read(regs[4] + op.at, window, take);
					target("write");
					arg_text("path", op.path);
					arg_num("block", op.at / BLOCK, 0);
					arg_num("fresh", op.at == 0, 0);
					AWAIT(1, window, (int)take, small, sizeof(small) - 1);
					if (req.status != 200)
						return request_error();
					take = regs[5] - regs[4] - op.at < BLOCK ? regs[5] - regs[4] - op.at : BLOCK;
				}
			}
			target("commit");
			arg_text("path", op.path);
			arg_num("size", regs[5] - regs[4], 0);
			arg_num("load", regs[2], 1);
			arg_num("exec", regs[3], 1);
			arg_num("staged", reason == 0 && regs[5] > regs[4], 0);
			AWAIT(1, NULL, 0, small, sizeof(small) - 1);
			if (req.status != 200)
				return request_error();
			regs[6] = regs[1];
		} else if (reason >= 1 && reason <= 4) { /* catalogue info, load, exec, attributes */
			stamp_request(reason == 1 ? 7 : 1u << (reason - 2));
			AWAIT(1, NULL, 0, small, sizeof(small) - 1);
			if (req.status != 200 && req.status != 404)
				return request_error();
		} else if (reason == 5) { /* read catalogue info */
			if (op.path[0]) {
				INFO(o);
			} else {
				memset(&o, 0, sizeof(o));
				o.type = TYPE_DIR;
			}
			set_info(&o);
		} else if (reason == 6) { /* delete, saying what it was */
			target("delete");
			arg_text("path", op.path);
			list_forget();
			AWAIT(1, NULL, 0, small, sizeof(small) - 1);
			if (req.status != 200)
				return request_error();
			small[req.out_len] = 0;
			parse_object(small, &o);
			set_info(&o);
		} else if (reason == 8) { /* create a directory: r2 load, r3 exec */
			target("mkdir");
			arg_text("path", op.path);
			if (regs[2] || regs[3]) {
				arg_num("load", regs[2], 1);
				arg_num("exec", regs[3], 1);
			}
			list_forget();
			AWAIT(1, NULL, 0, small, sizeof(small) - 1);
			if (req.status != 200)
				return request_error();
		} else {
			return dreambox_error("Dreambox can't do that (file %u)", (unsigned)reason);
		}
	}
	return BOX_DONE;
}

/* Directory entries from the listing: Func 14 names, 15 and 19 with catalogue info.
 * r2 buffer, r3 count, r4 first entry, r5 buffer size; out r3 read, r4 next or -1. */
static void fill_entries(void)
{
	static uint8_t rec[80];
	object_t o;
	const char *line;
	uint32_t at = regs[2], end = regs[2] + regs[5], count = 0;
	int index = (int)regs[4];
	for (; count < regs[3] && (line = list_line(index)); index++) {
		uint32_t len, name_at;
		parse_object(line, &o);
		name_at = regs[0] == 14 ? 0 : regs[0] == 15 ? 20 : 29;
		len = (name_at + (uint32_t)strlen(o.name) + 1 + (regs[0] == 14 ? 0 : 3)) & (regs[0] == 14 ? ~0u : ~3u);
		if (at + len > end)
			break;
		memset(rec, 0, sizeof(rec));
		if (regs[0] != 14) {
			uint32_t w[6] = {o.load, o.exec, o.size, o.attr, (uint32_t)o.type, 0};
			memcpy(rec, w, regs[0] == 15 ? 20 : 24);
			if (regs[0] == 19) {
				memcpy(rec + 24, &o.exec, 4); /* five byte time, if stamped */
				rec[28] = (uint8_t)o.load;
			}
		}
		memcpy(rec + name_at, o.name, strlen(o.name) + 1);
		G->write(at, rec, len);
		at += len;
		count++;
	}
	regs[3] = count;
	regs[4] = list_line(index) ? (uint32_t)index : 0xFFFFFFFFu;
}

static int box_func(void)
{
	static const char name[] = "Dreambox";
	uint32_t reason = regs[0];
	uint8_t buf[16];
	switch (op.step) {
	case 0:
		if (reason == 8) { /* rename: r1 old, r2 new; out r1 = 0 if done */
			path_from(regs[1], op.path);
			path_from(regs[2], op.path2);
			target("rename");
			arg_text("from", op.path);
			arg_text("to", op.path2);
			list_forget();
			AWAIT(1, NULL, 0, small, sizeof(small) - 1);
			if (req.status != 200)
				return request_error();
			regs[1] = 0;
		} else if (reason == 11) { /* disc name and boot option: <length><name><option> */
			buf[0] = (uint8_t)strlen(name);
			memcpy(buf + 1, name, buf[0]);
			buf[1 + buf[0]] = 0;
			G->write(regs[2], buf, buf[0] + 2u);
		} else if (reason == 12 || reason == 13) { /* CSD and library: <0><length><name> */
			memcpy(buf, "\0\1$", 3);
			G->write(regs[2], buf, 3);
		} else if (reason == 14 || reason == 15 || reason == 19) {
			path_from(regs[1], op.path);
			/* The Filer pages through a directory; only a fresh start re-reads it */
			if (!list_at || strcasecmp(op.path, list_dir) ||
			    (!regs[4] && plat_wall_ms() - list_at > LIST_FRESH_MS)) {
				if (!list_text && !(list_text = malloc(LIST_MAX)))
					return dreambox_error("Not enough memory for Dreambox");
				target("list");
				arg_text("path", op.path);
				list_forget();
				AWAIT(0, NULL, 0, list_text, LIST_MAX);
				if (req.status != 200)
					return request_error();
				list_len = req.out_len;
				snprintf(list_dir, sizeof(list_dir), "%s", op.path);
				list_at = plat_wall_ms();
			}
			fill_entries();
		} else if (reason >= 2 && reason <= 6) { /* catalogue and examine */
			return dreambox_error("Open Dreambox from the Filer to see what's in it");
		}
		/* Anything else (directories, options, boot, shutdown) needs nothing */
	}
	return BOX_DONE;
}

void dreambox_init(uint32_t at)
{
	workspace = at;
}

int dreambox_fs(uint32_t r[13], const guest_mem_t *mem)
{
	int result;
	G = mem;
	regs = r;
	if (op.active && req.state == REQ_PENDING) {
		plat_sleep_ms(1); /* the worker needs the CPU, and this is a retry loop */
		return BOX_AGAIN;
	}
	if (!op.active) {
		memset(&op, 0, sizeof(op));
		op.active = 1;
		op.entry = (int)r[12];
#ifdef BOX_TRACE
		{
			char name[64] = "";
			if (op.entry == ENTRY_OPEN || op.entry == ENTRY_FILE || op.entry == ENTRY_FUNC)
				mem->string(r[1], name, sizeof(name));
			fprintf(stderr, "box: entry %d r0=%X r1=%X '%s' r2=%X r3=%X r4=%X r5=%X\n", op.entry,
				(unsigned)r[0], (unsigned)r[1], name, (unsigned)r[2], (unsigned)r[3], (unsigned)r[4], (unsigned)r[5]);
		}
#endif
	}
	req.state = REQ_IDLE;
	/* Open files keep going without a token check; anything else needs a link */
	if (op.entry == ENTRY_OPEN || op.entry == ENTRY_FILE || op.entry == ENTRY_FUNC) {
		if (op.entry == ENTRY_FUNC && r[0] != 8 && r[0] != 14 && r[0] != 15 && r[0] != 19)
			op.lstep = -1;
		if (op.lstep >= 0 && op.step == 0 && (result = link_step()) != BOX_DONE)
			goto finished;
	}
	switch (op.entry) {
	case ENTRY_OPEN: result = box_open(); break;
	case ENTRY_GET:
	case ENTRY_PUT: result = box_bytes(); break;
	case ENTRY_ARGS: result = r[0] == 7 ? box_bytes() : box_args(); break;
	case ENTRY_CLOSE: result = box_close(); break;
	case ENTRY_FILE: result = box_file(); break;
	case ENTRY_FUNC: result = box_func(); break;
	default: result = dreambox_error("Dreambox: bad entry %u", (unsigned)op.entry); break;
	}
finished:
	if (result != BOX_AGAIN) {
#ifdef BOX_TRACE
		fprintf(stderr, "box:  -> %d r0=%X r1=%X r2=%X r3=%X r4=%X%s%s\n", result, (unsigned)r[0], (unsigned)r[1],
			(unsigned)r[2], (unsigned)r[3], (unsigned)r[4], result == BOX_ERROR ? " " : "",
			result == BOX_ERROR && workspace ? "(error)" : "");
#endif
		op.active = 0;
		if (!win_dirty)
			maybe_release();
	}
	return result;
}
