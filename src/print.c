/* URF raster over IPP, for printers that take no PostScript or PCL. The guest's
 * dumper calls run on the main thread and only ever encode into a ring. A worker
 * thread owns the socket, so a slow printer holds up the print job and nothing else.
 *
 * URF is what AirPrint printers take: a file header, then per page a 32 byte header
 * and lines of 8 bit grey. Each line starts with a repeat count, then packets:
 * 0..127 is one pixel repeated n+1 times, 129..255 is 257-n literal pixels. The page
 * count goes in the header and we don't know it yet. Printers take 0 there. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "net.h"
#include "print.h"
#include "web.h"

#define RING_SIZE 0x20000u
#define CHUNK 1400           /* one TCP segment or so */
#define RETRY_MS 5
#define STALL_MS 30000       /* no progress for this long fails the job */
#define REPLY_MS 60000       /* the printer answers once it has taken the whole job */
#define MAX_WIDTH 2550
#define MAX_ENCODED (MAX_WIDTH + MAX_WIDTH / 128 + 8)
#define WHITE 255
#define BLACK 0

#define ERR_AGAIN 35
#define ERR_INPROGRESS 36
#define ERR_ALREADY 37
#define ERR_ISCONN 56
#define ERR_NOTCONN 57

/* 300dpi sheets in pixels, and the name the printer knows each one by */
static const struct { int width, height; const char *media; } papers[PAPER_COUNT] = {
	{2550, 3300, "na_letter_8.5x11in"},
	{2480, 3508, "iso_a4_210x297mm"},
};

static print_config_t config;
static int have_config;
static print_status_t status;

static uint8_t *ring;
static volatile uint32_t ring_in, ring_out; /* free running; one writer each */
static volatile int input_done, input_abort, worker_running;

/* Set on the worker, read on the main thread */
static volatile int job_failed;
static char fail_text[48];

/* The page being encoded */
static int page_width, page_height, page_rows, page_left, page_open;
static uint8_t line[MAX_WIDTH], last[MAX_WIDTH], encoded[MAX_ENCODED];
static int last_repeat; /* 0 means nothing held in last */

static void ensure_config(void)
{
	if (have_config)
		return;
	have_config = 1;
	if (!plat_print_load(&config))
		memset(&config, 0, sizeof(config));
}

void print_get_config(print_config_t *c)
{
	ensure_config();
	memcpy(c, &config, sizeof(*c));
}

void print_set_config(const print_config_t *c)
{
	ensure_config();
	memcpy(&config, c, sizeof(config));
	config.host[sizeof(config.host) - 1] = 0;
}

static void say(const char *text)
{
	snprintf(status.text, sizeof(status.text), "%s", text);
}

static void fail(const char *text)
{
	snprintf(fail_text, sizeof(fail_text), "%s", text);
	job_failed = 1;
}

void print_status(print_status_t *s)
{
	if (job_failed && status.state == PRINT_BUSY)
		say(fail_text);
	memcpy(s, &status, sizeof(*s));
}

/* Host, port and path out of "host[:port][/path]" */
static void parse_target(char *host, int size, int *port, char *path, int path_size)
{
	const char *p = config.host, *stop;
	int n;
	if (!strncmp(p, "ipp://", 6))
		p += 6;
	for (stop = p; *stop && *stop != ':' && *stop != '/'; stop++)
		;
	n = (int)(stop - p);
	if (n >= size)
		n = size - 1;
	memcpy(host, p, (size_t)n);
	host[n] = 0;
	*port = 631;
	if (*stop == ':') {
		*port = atoi(stop + 1);
		while (*stop && *stop != '/')
			stop++;
	}
	snprintf(path, (size_t)path_size, "%s", *stop == '/' ? stop : "/ipp/print");
}

static int put_attr(uint8_t *out, int tag, const char *name, const char *value)
{
	int name_len = (int)strlen(name), value_len = (int)strlen(value);
	uint8_t *p = out;
	*p++ = (uint8_t)tag;
	*p++ = (uint8_t)(name_len >> 8);
	*p++ = (uint8_t)name_len;
	memcpy(p, name, (size_t)name_len);
	p += name_len;
	*p++ = (uint8_t)(value_len >> 8);
	*p++ = (uint8_t)value_len;
	memcpy(p, value, (size_t)value_len);
	return (int)(p + value_len - out);
}

/* The Print-Job request ahead of the document */
static int build_request(uint8_t *out, const char *uri, const char *media)
{
	static const uint8_t head[] = {2, 0, 0, 2, 0, 0, 0, 1, 1}; /* IPP 2.0, Print-Job, id 1 */
	int n = sizeof(head);
	memcpy(out, head, sizeof(head));
	n += put_attr(out + n, 0x47, "attributes-charset", "utf-8");
	n += put_attr(out + n, 0x48, "attributes-natural-language", "en");
	n += put_attr(out + n, 0x45, "printer-uri", uri);
	n += put_attr(out + n, 0x42, "requesting-user-name", "dreamcast");
	n += put_attr(out + n, 0x42, "job-name", "RISC OS");
	n += put_attr(out + n, 0x49, "document-format", "image/urf");
	out[n++] = 2; /* job attributes */
	n += put_attr(out + n, 0x44, "media", media);
	out[n++] = 3; /* end of attributes */
	return n;
}

static int connect_to(const char *host, int port)
{
	struct addrinfo hints, *res = NULL;
	uint64_t deadline;
	uint8_t ip[4];
	int fd, rv;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, NULL, &hints, &res) || !res) {
		fail("Can't find the printer");
		return -1;
	}
	memcpy(ip, &((struct sockaddr_in *)res->ai_addr)->sin_addr, 4);
	freeaddrinfo(res);
	fd = net_sock_open(SOCK_KIND_TCP);
	if (fd < 0) {
		fail("No network");
		return -1;
	}
	deadline = plat_wall_ms() + STALL_MS;
	for (;;) {
		rv = net_sock_connect(fd, ip, port);
		if (rv == 0 || rv == -ERR_ISCONN)
			return fd;
		if ((rv != -ERR_INPROGRESS && rv != -ERR_ALREADY && rv != -ERR_AGAIN) || plat_wall_ms() > deadline) {
			fail("The printer did not answer");
			net_sock_close(fd);
			return -1;
		}
		plat_sleep_ms(RETRY_MS);
	}
}

static int send_all(int fd, const void *data, int len)
{
	const uint8_t *p = data;
	uint64_t deadline = plat_wall_ms() + STALL_MS;
	while (len > 0) {
		int n = net_sock_send(fd, p, len);
		if (n > 0) {
			p += n;
			len -= n;
			deadline = plat_wall_ms() + STALL_MS;
		} else if ((n != -ERR_AGAIN && n != -ERR_NOTCONN) || plat_wall_ms() > deadline) {
			fail("Lost the printer");
			return 0;
		} else {
			plat_sleep_ms(RETRY_MS);
		}
	}
	return 1;
}

static int send_chunk(int fd, const void *data, int len)
{
	char head[16];
	int n = snprintf(head, sizeof(head), "%x\r\n", (unsigned)len);
	return send_all(fd, head, n) && send_all(fd, data, len) && send_all(fd, "\r\n", 2);
}

/* Everything up to the end of the ring's input, as HTTP chunks */
static int pump(int fd)
{
	static uint8_t chunk[CHUNK];
	for (;;) {
		uint32_t have = ring_in - ring_out;
		uint32_t at, n, first;
		if (input_abort)
			return 0;
		if (!have) {
			if (input_done)
				return 1;
			plat_sleep_ms(RETRY_MS);
			continue;
		}
		n = have < CHUNK ? have : CHUNK;
		at = ring_out % RING_SIZE;
		first = RING_SIZE - at < n ? RING_SIZE - at : n;
		memcpy(chunk, ring + at, first);
		memcpy(chunk + first, ring, n - first);
		ring_out += n;
		if (!send_chunk(fd, chunk, (int)n))
			return 0;
		status.bytes += n;
	}
}

/* Pull the IPP status out of the reply. 1 if the printer took the job. */
static int read_reply(int fd)
{
	static char reply[1024];
	uint64_t deadline = plat_wall_ms() + REPLY_MS;
	int have = 0;
	for (;;) {
		char *body;
		int n;
		reply[have] = 0;
		body = strstr(reply, "\r\n\r\n");
		if (body && !strncmp(reply, "HTTP/1.1 100", 12)) {
			/* An interim reply; the real one follows it */
			have -= (int)(body + 4 - reply);
			memmove(reply, body + 4, (size_t)have + 1);
			continue;
		}
		if (body) {
			int http = have > 12 ? atoi(reply + 9) : 0;
			const uint8_t *ipp = (const uint8_t *)body + 4;
			int len = have - (int)((const char *)ipp - reply);
			if (http != 200) {
				char text[48];
				snprintf(text, sizeof(text), "Printer said HTTP %d", http);
				fail(text);
				return 0;
			}
			/* A chunked reply has its size line first */
			if (strstr(reply, "chunked") || strstr(reply, "Chunked")) {
				const char *eol = strstr((const char *)ipp, "\r\n");
				if (eol) {
					len -= (int)((const uint8_t *)eol + 2 - ipp);
					ipp = (const uint8_t *)eol + 2;
				}
			}
			if (len >= 4) {
				int code = ipp[2] << 8 | ipp[3];
				if (code >= 0x100) {
					char text[48];
					snprintf(text, sizeof(text), "Printer refused the job (%04x)", code);
					fail(text);
					return 0;
				}
				return 1;
			}
		}
		n = net_sock_recv(fd, reply + have, (int)sizeof(reply) - 1 - have);
		if (n > 0) {
			have += n;
		} else if (n == 0 || n != -ERR_AGAIN || plat_wall_ms() > deadline) {
			fail("The printer did not reply");
			return 0;
		} else {
			plat_sleep_ms(RETRY_MS);
		}
		if (have >= (int)sizeof(reply) - 1) {
			fail("The printer's reply made no sense");
			return 0;
		}
	}
}

static void *print_thread(void *arg)
{
	static char host[64], path[64], uri[128], head[256];
	static uint8_t request[512];
	int port, fd, n;
	(void)arg;
	parse_target(host, sizeof(host), &port, path, sizeof(path));
	snprintf(uri, sizeof(uri), "ipp://%s:%d%s", host, port, path);
	fd = connect_to(host, port);
	if (fd >= 0) {
		n = snprintf(head, sizeof(head),
			"POST %s HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: application/ipp\r\n"
			"Transfer-Encoding: chunked\r\n\r\n", path, host, port);
		if (send_all(fd, head, n)) {
			n = build_request(request, uri, papers[config.paper].media);
			if (send_chunk(fd, request, n) && pump(fd) && send_all(fd, "0\r\n\r\n", 5))
				read_reply(fd);
		}
		net_sock_close(fd);
	}
	worker_running = 0;
	return NULL;
}

/* Every put checks for space first, and a failed job drops its output */
static uint32_t ring_room(void)
{
	return RING_SIZE - (ring_in - ring_out);
}

static void ring_put(const void *data, uint32_t len)
{
	const uint8_t *p = data;
	uint32_t at = ring_in % RING_SIZE;
	uint32_t first = RING_SIZE - at < len ? RING_SIZE - at : len;
	memcpy(ring + at, p, first);
	memcpy(ring, p + first, len - first);
	ring_in += len;
}

int print_job_start(void)
{
	static const uint8_t file_head[12] = {'U', 'N', 'I', 'R', 'A', 'S', 'T', 0};
	ensure_config();
	memset(&status, 0, sizeof(status));
	job_failed = 0;
	if (!config.host[0]) {
		status.state = PRINT_FAILED;
		say("No printer set in !NetPrint");
		return PRINT_ERROR;
	}
	if (config.paper >= PAPER_COUNT)
		config.paper = PAPER_LETTER;
	ring = malloc(RING_SIZE);
	if (!ring) {
		status.state = PRINT_FAILED;
		say("Not enough memory to print");
		return PRINT_ERROR;
	}
	ring_in = ring_out = 0;
	input_done = input_abort = 0;
	worker_running = 1;
	status.state = PRINT_BUSY;
	say("Printing");
	ring_put(file_head, sizeof(file_head));
	plat_thread_start(print_thread);
	return PRINT_OK;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* Packets for one line of pixels */
static int encode_line(const uint8_t *p, int width, uint8_t *out)
{
	uint8_t *o = out;
	int x = 0;
	while (x < width) {
		int run = 1;
		while (x + run < width && run < 128 && p[x + run] == p[x])
			run++;
		if (run > 1) {
			*o++ = (uint8_t)(run - 1);
			*o++ = p[x];
			x += run;
		} else {
			int count = 1;
			while (x + count < width && count < 127 &&
				(x + count + 1 >= width || p[x + count] != p[x + count + 1]))
				count++;
			*o++ = (uint8_t)(257 - count);
			memcpy(o, p + x, (size_t)count);
			o += count;
			x += count;
		}
	}
	return (int)(o - out);
}

/* The held line and its repeats go out. Fits by print_row_ready(). */
static void flush_line(void)
{
	uint8_t count;
	int n;
	if (!last_repeat)
		return;
	count = (uint8_t)(last_repeat - 1);
	n = encode_line(last, page_width, encoded);
	ring_put(&count, 1);
	ring_put(encoded, (uint32_t)n);
	last_repeat = 0;
}

static void add_line(void)
{
	if (last_repeat && last_repeat < 256 && !memcmp(line, last, (size_t)page_width)) {
		last_repeat++;
	} else {
		flush_line();
		memcpy(last, line, (size_t)page_width);
		last_repeat = 1;
	}
	page_rows++;
}

int print_row_ready(void)
{
	if (job_failed)
		return 1; /* print_row drops it, and the caller sees the error next */
	return ring_room() >= 2 * (MAX_ENCODED + 1);
}

int print_page_start(int top_rows, int left_pixels)
{
	uint8_t head[32];
	int i;
	if (job_failed)
		return PRINT_ERROR;
	if (ring_room() < sizeof(head) + 2 * (MAX_ENCODED + 1))
		return PRINT_WAIT;
	page_width = papers[config.paper].width;
	page_height = papers[config.paper].height;
	page_rows = 0;
	page_left = left_pixels < 0 ? 0 : left_pixels;
	last_repeat = 0;
	memset(head, 0, sizeof(head));
	head[0] = 8;     /* bits per pixel */
	head[1] = 0;     /* sGray */
	head[2] = 1;     /* one sided */
	put32(head + 12, (uint32_t)page_width);
	put32(head + 16, (uint32_t)page_height);
	put32(head + 20, PRINT_DPI);
	ring_put(head, sizeof(head));
	page_open = 1;
	memset(line, WHITE, (size_t)page_width);
	for (i = 0; i < top_rows && i < page_height; i++)
		add_line(); /* a repeat count, so this needs no ring space to speak of */
	return PRINT_OK;
}

void print_row(const uint8_t *bits, int width)
{
	int x, stop;
	if (job_failed || !page_open || page_rows >= page_height)
		return;
	memset(line, WHITE, (size_t)page_width);
	stop = width;
	if (stop > page_width - page_left)
		stop = page_width - page_left;
	for (x = 0; x < stop; x += 8) {
		uint8_t byte = bits[x >> 3];
		uint8_t *to = line + page_left + x;
		int i, n = stop - x < 8 ? stop - x : 8;
		if (!byte)
			continue;
		for (i = 0; i < n; i++)
			if (byte >> i & 1)
				to[i] = BLACK;
	}
	add_line();
}

int print_page_end(void)
{
	if (job_failed)
		return PRINT_ERROR;
	if (!page_open)
		return PRINT_OK;
	if (ring_room() < 2 * (MAX_ENCODED + 1))
		return PRINT_WAIT;
	memset(line, WHITE, (size_t)page_width);
	while (page_rows < page_height) {
		if (ring_room() < 2 * (MAX_ENCODED + 1))
			return PRINT_WAIT;
		add_line();
	}
	flush_line();
	page_open = 0;
	status.pages++;
	return PRINT_OK;
}

int print_job_end(int abort)
{
	if (status.state != PRINT_BUSY)
		return status.state == PRINT_FAILED ? PRINT_ERROR : PRINT_OK;
	if (abort)
		input_abort = 1;
	input_done = 1;
	if (worker_running)
		return PRINT_WAIT;
	free(ring); /* !Web wants that heap back */
	ring = NULL;
	if (abort) {
		status.state = PRINT_IDLE;
		say("Cancelled");
		return PRINT_OK;
	}
	if (job_failed) {
		status.state = PRINT_FAILED;
		say(fail_text);
		return PRINT_ERROR;
	}
	status.state = PRINT_DONE;
	say("Sent to the printer");
	return PRINT_OK;
}
