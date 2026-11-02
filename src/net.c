/* DreamNet SWIs: how RISC OS programs reach the native network stack. RISC OS 3.11
 * has no TCP/IP in ROM and Acorn's Internet module is an ARM binary we cannot run,
 * so BASIC programs call these directly. They are caught in swi_insn before the
 * kernel sees them. Results come back in r0 as a value or -errno; only the X form
 * exists, since there is nowhere to build an error block for the kernel. */
#include <string.h>
#include "cpu.h"
#include "net.h"
#include "print.h"
#include "web.h"

enum {
	DN_STATUS, DN_CONNECT, DN_DISCONNECT, DN_PING, DN_PING_RESULT, DN_RESOLVE,
	DN_RESOLVE_RESULT, DN_GET_CONFIG, DN_SAVE_CONFIG, DN_SOCK_OPEN, DN_SOCK_BIND,
	DN_SOCK_CONNECT, DN_SOCK_SEND, DN_SOCK_RECV, DN_SOCK_SENDTO, DN_SOCK_RECVFROM,
	DN_SOCK_CLOSE, DN_WEB_OPEN, DN_WEB_BACK, DN_WEB_STATUS, DN_WEB_INFO, DN_WEB_LINE, DN_WEB_CLICK, DN_WEB_FIELD_GET,
	DN_WEB_FIELD_SET, DN_PRINT_DUMP, DN_PRINT_GET_CONFIG, DN_PRINT_SET_CONFIG, DN_PRINT_SAVE_CONFIG,
	DN_PRINT_STATUS, DN_FIND, DN_FIND_LINE, DN_FIND_DRIVE,
};

#define ERR_INVAL 22
#define MAX_TRANSFER 1460 /* one segment per call keeps a SWI short */

static void guest_read(u32 addr, void *dest, u32 len)
{
	u8 *d = dest;
	while (len--)
		*d++ = (u8)ld8(addr++);
}

static void guest_write(u32 addr, const void *src, u32 len)
{
	const u8 *s = src;
	while (len--)
		st8(addr++, *s++);
}

static void guest_string(u32 addr, char *dest, u32 size)
{
	u32 i;
	for (i = 0; i + 1 < size; i++) {
		dest[i] = (char)ld8(addr + i);
		if (dest[i] < ' ')
			break;
	}
	dest[i] = 0;
}

/* Addresses travel in one register, most significant byte first */
static void ip_unpack(u32 v, u8 ip[4])
{
	ip[0] = (u8)(v >> 24);
	ip[1] = (u8)(v >> 16);
	ip[2] = (u8)(v >> 8);
	ip[3] = (u8)v;
}

static u32 ip_pack(const u8 ip[4])
{
	return ((u32)ip[0] << 24) | ((u32)ip[1] << 16) | ((u32)ip[2] << 8) | ip[3];
}

static int parse_dotted(const char *s, u8 ip[4])
{
	int part, value;
	for (part = 0; part < 4; part++) {
		if (*s < '0' || *s > '9')
			return 0;
		for (value = 0; *s >= '0' && *s <= '9'; s++)
			if ((value = value * 10 + (*s - '0')) > 255)
				return 0;
		ip[part] = (u8)value;
		if (part < 3 && *s++ != '.')
			return 0;
	}
	return !*s;
}

static u8 literal_ip[4];
static int have_literal;

static s32 sock_transfer(u32 op)
{
	static u8 buffer[MAX_TRANSFER];
	u8 ip[4];
	int len = (int)R(2), port = 0, got;
	if (len < 0)
		return -ERR_INVAL;
	if (len > MAX_TRANSFER)
		len = MAX_TRANSFER;
	switch (op) {
	case DN_SOCK_SEND:
		guest_read(R(1), buffer, (u32)len);
		return net_sock_send((int)R(0), buffer, len);
	case DN_SOCK_SENDTO:
		guest_read(R(1), buffer, (u32)len);
		ip_unpack(R(3), ip);
		return net_sock_sendto((int)R(0), buffer, len, ip, (int)R(4));
	case DN_SOCK_RECV:
		got = net_sock_recv((int)R(0), buffer, len);
		break;
	default:
		got = net_sock_recvfrom((int)R(0), buffer, len, ip, &port);
		if (got >= 0) {
			R(3) = ip_pack(ip);
			R(4) = (u32)port;
		}
		break;
	}
	if (got > 0)
		guest_write(R(1), buffer, (u32)got);
	return got;
}

/* The dumper's reason codes, from PDriverDP in r11 */
enum { DUMP_SET_DRIVER, DUMP_OUTPUT, DUMP_COLOUR_SET, DUMP_START_PAGE, DUMP_END_PAGE, DUMP_START_JOB, DUMP_TIDY_JOB };

/* One strip, as much as the ring has room for. r0 is the sprite data, r3 the width in
 * pixels, r4 the rows and r5 the bytes per row. The veneer calls again until it's all gone. */
static s32 print_strip(void)
{
	static u8 row[320];
	static u32 rows_done;
	u32 width = R(3), rows = R(4), stride = R(5);
	u32 bytes = (width + 7) >> 3;
	if (bytes > sizeof(row))
		bytes = sizeof(row);
	while (rows_done < rows) {
		if (!print_row_ready())
			return PRINT_WAIT;
		guest_read(R(0) + rows_done * stride, row, bytes);
		print_row(row, (int)(bytes * 8 < width ? bytes * 8 : width));
		rows_done++;
	}
	rows_done = 0;
	return PRINT_OK;
}

/* Called by the dumper veneer in the DreamApps module, with PDriverDP's registers */
static s32 print_dump(void)
{
	s32 result = PRINT_OK;
	switch (R(11)) {
	case DUMP_START_JOB:
		result = print_job_start();
		break;
	case DUMP_START_PAGE:
		/* PDriverDP takes r3 back as rows it should still send blank */
		result = print_page_start((int)R(3), (int)R(6));
		if (result == PRINT_OK)
			R(3) = 0;
		break;
	case DUMP_OUTPUT:
		result = print_strip();
		break;
	case DUMP_END_PAGE:
		result = print_page_end();
		break;
	case DUMP_TIDY_JOB:
		result = print_job_end(R(3) != 0);
		break;
	}
	if (result == PRINT_WAIT)
		plat_sleep_ms(1); /* the worker needs the CPU, and this is a retry loop */
	return result;
}

/* !Find's search, as BASIC would take seconds over the ~300KB index. Lines end in a
 * control character, and only the name matches: past the kind letter, up to a tab.
 * The pattern is already lower case. */
static u32 find_match(u32 at, u32 end, const char *pattern)
{
	u32 len = (u32)strlen(pattern);
	while (at < end) {
		u32 p = at + 1;
		for (; ld8(p) >= 32; p++) {
			u32 j = 0, q = p;
			while (j < len) {
				int d = ld8(q++);
				if (d >= 'A' && d <= 'Z')
					d |= 32;
				if (d != (u8)pattern[j])
					break;
				j++;
			}
			if (j == len)
				return at;
		}
		while (p < end && ld8(p) != 13)
			p++;
		at = p + 1;
	}
	return 0;
}

void net_swi(u32 op)
{
	static net_status_t status;
	static net_config_t config;
	static char name[64];
	static char url[192];
	static web_info_t info;
	static u8 line[512];
	static print_config_t print_config;
	static print_status_t print_state;
	u8 ip[4];
	u32 a, b;
	s32 result = 0;
	switch (op) {
	case DN_STATUS:
		plat_net_status(&status);
		guest_write(R(0), &status, sizeof(status));
		break;
	case DN_CONNECT:
		guest_read(R(0), &config, sizeof(config));
		if (config.link >= LINK_COUNT)
			result = -ERR_INVAL;
		else
			plat_net_connect(&config);
		break;
	case DN_DISCONNECT:
		plat_net_disconnect();
		break;
	case DN_PING:
		ip_unpack(R(0), ip);
		plat_net_ping(ip, R(1));
		break;
	case DN_PING_RESULT:
		a = b = 0;
		result = plat_net_ping_result(R(0), &a, &b);
		R(1) = a;
		R(2) = b;
		break;
	case DN_RESOLVE:
		guest_string(R(0), name, sizeof(name));
		have_literal = parse_dotted(name, literal_ip);
		if (!have_literal)
			plat_net_resolve(name);
		break;
	case DN_RESOLVE_RESULT:
		if (have_literal) {
			result = 1;
			memcpy(ip, literal_ip, 4);
		} else {
			result = plat_net_resolve_result(ip);
		}
		if (result > 0)
			R(1) = ip_pack(ip);
		break;
	case DN_GET_CONFIG:
		plat_net_get_config(&config);
		guest_write(R(0), &config, sizeof(config));
		break;
	case DN_SAVE_CONFIG:
		guest_read(R(0), &config, sizeof(config));
		if (config.link >= LINK_COUNT)
			result = -ERR_INVAL;
		else
			result = plat_net_save_config(&config);
		break;
	case DN_SOCK_OPEN:
		result = net_sock_open((int)R(0));
		break;
	case DN_SOCK_BIND:
		result = net_sock_bind((int)R(0), (int)R(1));
		break;
	case DN_SOCK_CONNECT:
		ip_unpack(R(1), ip);
		result = net_sock_connect((int)R(0), ip, (int)R(2));
		break;
	case DN_SOCK_SEND:
	case DN_SOCK_RECV:
	case DN_SOCK_SENDTO:
	case DN_SOCK_RECVFROM:
		result = sock_transfer(op);
		break;
	case DN_SOCK_CLOSE:
		result = net_sock_close((int)R(0));
		break;
	case DN_WEB_OPEN:
		guest_string(R(0), url, sizeof(url));
		web_open(url);
		break;
	case DN_WEB_BACK:
		web_back();
		break;
	case DN_WEB_STATUS: {
		int lines, bytes;
		result = web_status(&lines, &bytes);
		R(1) = (u32)lines;
		R(2) = (u32)bytes;
		break;
	}
	case DN_WEB_INFO:
		web_info(&info);
		guest_write(R(0), &info, sizeof(info));
		break;
	case DN_WEB_LINE:
		result = web_line((int)R(0), line, sizeof(line));
		guest_write(R(1), line, (u32)result);
		break;
	case DN_WEB_CLICK:
		result = web_click((int)R(0), (int)R(1));
		break;
	case DN_WEB_FIELD_GET:
		web_field_get(url, sizeof(url));
		guest_write(R(0), url, (u32)strlen(url) + 1);
		break;
	case DN_WEB_FIELD_SET:
		guest_string(R(0), url, sizeof(url));
		web_field_set(url);
		break;
	case DN_PRINT_DUMP:
		result = print_dump();
		break;
	case DN_PRINT_GET_CONFIG:
		print_get_config(&print_config);
		guest_write(R(0), &print_config, sizeof(print_config));
		break;
	case DN_PRINT_SET_CONFIG:
		guest_read(R(0), &print_config, sizeof(print_config));
		if (print_config.paper >= PAPER_COUNT)
			result = -ERR_INVAL;
		else
			print_set_config(&print_config);
		break;
	case DN_PRINT_SAVE_CONFIG:
		guest_read(R(0), &print_config, sizeof(print_config));
		if (print_config.paper >= PAPER_COUNT) {
			result = -ERR_INVAL;
		} else {
			print_set_config(&print_config);
			result = plat_print_save(&print_config);
		}
		break;
	case DN_PRINT_STATUS:
		print_status(&print_state);
		guest_write(R(0), &print_state, sizeof(print_state));
		break;
	case DN_FIND:
		guest_string(R(2), name, sizeof(name));
		result = (s32)find_match(R(0), R(1), name);
		break;
	case DN_FIND_LINE:
		for (a = R(0), b = R(2); b && a < R(1); a++)
			b -= ld8(a) == 13;
		result = (s32)a;
		break;
	case DN_FIND_DRIVE: /* the archive is the drive that can't be written */
		result = -1;
		for (a = 0; a < HD_UNITS; a++)
			if (plat_hd_sectors((int)a) && !plat_hd_writable((int)a))
				result = 4 + (s32)a;
		break;
	default:
		result = -ERR_INVAL;
		break;
	}
	R(0) = (u32)result;
}
