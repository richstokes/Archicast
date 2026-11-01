/* Harness network: a scripted link in virtual time, so replays stay deterministic
 * and screen checksums can cover the network apps. Comes up 3s after a connect, and
 * answers pings from every address except x.x.x.254. Sockets are the Mac's own. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../cpu.h"
#include <time.h>
#include <unistd.h>
#include "../net.h"
#include "../print.h"
#include "../web.h"

#define UP_DELAY_US 3000000u
#define RTT_BASE_US 12000u
#define RTT_STEP_US 3000u

static net_config_t config;
static net_status_t status;
static uint64_t up_at;
static struct { uint32_t seq; uint64_t at; uint32_t rtt; int valid; } replies[NET_SEQS];
static int resolve_state;
static uint8_t resolve_ip[4];

static void say(uint32_t state, const char *text)
{
	status.state = state;
	snprintf(status.text, sizeof(status.text), "%s", text);
}

void plat_net_init(void)
{
	static const uint8_t mask[4] = {255, 255, 255, 0};
	memset(&config, 0, sizeof(config));
	config.dhcp = 1;
	memcpy(config.mask, mask, 4);
	strcpy(config.phone, "111-1111");
	strcpy(config.user, "dream");
	strcpy(config.pass, "dreamcast");
	/* Like the console, try the network from power on. The clock is left alone here
	 * as reading it advances virtual time, which would move every other checksum. */
	say(NET_CONNECTING, "Asking for an address");
	up_at = UP_DELAY_US;
}

/* No VMU here. DREAM_PRINTER stands in for one, so tests can name a server. */
int plat_print_load(print_config_t *c)
{
	const char *host = getenv("DREAM_PRINTER");
	if (!host)
		return 0;
	memset(c, 0, sizeof(*c));
	snprintf(c->host, sizeof(c->host), "%s", host);
	return 1;
}

int plat_print_save(const print_config_t *c)
{
	(void)c;
	return 0;
}

void plat_sock_tune(int fd)
{
	(void)fd;
}

void plat_net_status(net_status_t *s)
{
	static const uint8_t ip[4] = {192, 168, 1, 50}, mask[4] = {255, 255, 255, 0},
		gateway[4] = {192, 168, 1, 1}, mac[6] = {2, 0, 0x44, 0x43, 0, 1};
	if (status.state == NET_CONNECTING && plat_time_us() >= up_at) {
		say(NET_UP, "Connected");
		status.link = config.link == LINK_AUTO ? LINK_BBA : config.link;
		if (config.dhcp || config.link == LINK_MODEM) {
			memcpy(status.ip, ip, 4);
			memcpy(status.mask, mask, 4);
			memcpy(status.gateway, gateway, 4);
			memcpy(status.dns, gateway, 4);
		} else {
			memcpy(status.ip, config.ip, 4);
			memcpy(status.mask, config.mask, 4);
			memcpy(status.gateway, config.gateway, 4);
			memcpy(status.dns, config.dns, 4);
		}
		memcpy(status.mac, mac, 6);
		strcpy(status.device, "sim");
	}
	*s = status;
}

void plat_net_get_config(net_config_t *c)
{
	*c = config;
}

int plat_net_save_config(const net_config_t *c)
{
	config = *c;
	return 0;
}

void plat_net_connect(const net_config_t *c)
{
	config = *c;
	memset(&status, 0, sizeof(status));
	say(NET_CONNECTING, c->link == LINK_MODEM ? "Dialling" : "Asking for an address");
	up_at = plat_time_us() + UP_DELAY_US;
}

void plat_net_disconnect(void)
{
	memset(&status, 0, sizeof(status));
	say(NET_DOWN, "Not connected");
}

void plat_net_ping(const uint8_t ip[4], uint32_t seq)
{
	int slot = (int)(seq % NET_SEQS);
	replies[slot].valid = status.state == NET_UP && ip[3] != 254;
	replies[slot].seq = seq;
	replies[slot].rtt = RTT_BASE_US + RTT_STEP_US * (seq % 5);
	replies[slot].at = plat_time_us() + replies[slot].rtt;
}

int plat_net_ping_result(uint32_t seq, uint32_t *rtt_us, uint32_t *ttl)
{
	int slot = (int)(seq % NET_SEQS);
	if (!replies[slot].valid || replies[slot].seq != seq || plat_time_us() < replies[slot].at)
		return 0;
	*rtt_us = replies[slot].rtt;
	*ttl = 64;
	return 1;
}

void plat_net_resolve(const char *name)
{
	static const uint8_t ip[4] = {93, 184, 216, 34};
	resolve_state = strcmp(name, "example.com") ? -1 : 1;
	memcpy(resolve_ip, ip, 4);
}

int plat_net_resolve_result(uint8_t ip[4])
{
	if (resolve_state > 0)
		memcpy(ip, resolve_ip, 4);
	return resolve_state;
}

/* The browser's thread. It only touches web.c's own state, so it doesn't disturb
 * replays, and TLS is left to the console. */
web_tls_t *plat_tls_open(int fd, const char *host)
{
	(void)fd;
	(void)host;
	return NULL;
}

int plat_tls_read(web_tls_t *tls, void *data, int len)
{
	(void)tls;
	(void)data;
	(void)len;
	return -1;
}

int plat_tls_write(web_tls_t *tls, const void *data, int len)
{
	(void)tls;
	(void)data;
	(void)len;
	return -1;
}

void plat_tls_close(web_tls_t *tls)
{
	(void)tls;
}

void plat_thread_start(void *(*fn)(void *))
{
	pthread_t t;
	pthread_create(&t, NULL, fn, NULL);
	pthread_detach(t);
}

void plat_sleep_ms(int ms)
{
	usleep((useconds_t)ms * 1000);
}

uint64_t plat_wall_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
