/* Console network backend. KOS drivers sit behind its netif_t already, so a link
 * here is only how to bring one up: the BBA and LAN adapter are Ethernet netifs,
 * the W5500 is one on the serial port, and the modem is PPP. DreamPi looks the
 * same as any other PPP peer. Everything blocking runs on one worker thread, which
 * must not touch machine state: r14 only holds it on the main thread. */
#include <kos.h>
#include <dc/fs_dcload.h>
#include <dc/net/broadband_adapter.h>
#include <dc/net/lan_adapter.h>
#include <dc/net/w5500_adapter.h>
#include <dc/vmu_pkg.h>
#include <arch/timer.h>
#include <malloc.h>
#include <netdb.h>
#include <ppp/ppp.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#include "../net.h"
#include "../print.h"

#define CONFIG_NAME "DRISCNET"
#define CONFIG_MAGIC 0x31544E44u
#define PRINT_CONFIG_NAME "DRISCPRN"
#define PRINT_CONFIG_MAGIC 0x31525044u
#define WORKER_STACK 0x8000
#define IDLE_MS 20

extern int net_dhcp_request(uint32_t required_address);
extern void arch_init_net_dcload_ip(void);
extern uint32_t plat_hd_sectors(void);

typedef struct {
	const char *device; /* netif name, NULL if the link is not a plain netif */
	const char *label;
	void (*add)(void);
} ether_t;

static void add_bba(void) { bba_init(); }
static void add_lan(void) { la_init(); }
static void add_w5500(void) { w5500_adapter_init(NULL, true); }

static const ether_t ethers[LINK_COUNT] = {
	[LINK_BBA] = {"bba", "Broadband Adapter", add_bba},
	[LINK_LAN] = {"la", "LAN Adapter", add_lan},
	[LINK_W5500] = {"w5500", "W5500", add_w5500},
};

static net_config_t config;
static volatile struct {
	uint32_t state, link;
	char text[32];
} live;
static netif_t *current;
static int stack_ready, ppp_ready, under_dcload;

/* Requests from the main thread, each one slot deep */
static volatile int want_connect, want_down, want_resolve;
static volatile int ping_pending;
static volatile uint32_t ping_seq;
static uint8_t ping_ip[4];
static net_config_t next_config;
static char resolve_name[64];
static volatile int resolve_state;
static uint8_t resolve_ip[4];

static volatile struct { uint32_t seq, rtt_us, ttl; int valid; } replies[NET_SEQS];

/* Replies arrive on the receive path, which must not print: under dcload a printf
 * travels through that same path and waits for itself. The worker logs them. */
static volatile typeof(replies[0]) *volatile last_reply;

static void say(uint32_t state, const char *text)
{
	strncpy((char *)live.text, text, sizeof(live.text) - 1);
	live.state = state;
	printf("net: %s (%d bytes free)\n", text, (int)(_arch_mem_top - (uintptr_t)sbrk(0)) + (int)mallinfo().fordblks);
}

static void echo_reply(const uint8_t *ip, uint16_t seq, uint64_t delta_us, uint8_t ttl,
                       const uint8_t *data, size_t len)
{
	volatile typeof(replies[0]) *r = &replies[seq % NET_SEQS];
	(void)ip;
	(void)data;
	(void)len;
	if (delta_us == (uint64_t)-1)
		return;
	r->seq = seq;
	r->rtt_us = (uint32_t)delta_us;
	r->ttl = ttl;
	r->valid = 1;
	last_reply = r;
}

static netif_t *find_netif(const char *name)
{
	netif_t *n;
	LIST_FOREACH(n, net_get_if_list(), if_list)
		if (!strcmp(n->name, name))
			return n;
	return NULL;
}

/* The stack comes up empty, as drivers only register when a link asks for them */
static void stack_start(void)
{
	if (stack_ready)
		return;
	net_init(0);
	net_icmp_echo_cb = echo_reply;
	stack_ready = 1;
}

static int ether_start(int link)
{
	const ether_t *e = &ethers[link];
	netif_t *n;
	char text[32];
	/* An SD card owns the serial port as a live hard disc */
	if (link == LINK_W5500 && plat_hd_sectors()) {
		say(NET_FAILED, "Serial port is in use by SD");
		return -2;
	}
	snprintf(text, sizeof(text), "Looking for %s", e->label);
	say(NET_CONNECTING, text);
	e->add();
	n = find_netif(e->device);
	if (!n || n->if_detect(n) < 0)
		return -1;
	if (n->if_init(n) < 0 || n->if_start(n) < 0) {
		say(NET_FAILED, "No link: check the cable");
		return -2;
	}
	current = n;
	live.link = (uint32_t)link;
	net_set_default(n);
	return 0;
}

static void apply_static(netif_t *n)
{
	int i;
	memcpy(n->ip_addr, config.ip, 4);
	memcpy(n->netmask, config.mask, 4);
	memcpy(n->gateway, config.gateway, 4);
	memcpy(n->dns, config.dns, 4);
	for (i = 0; i < 4; i++)
		n->broadcast[i] = (uint8_t)(config.ip[i] | ~config.mask[i]);
}

static void ether_configure(void)
{
	if (!config.dhcp) {
		apply_static(current);
		say(NET_UP, "Connected");
		return;
	}
	say(NET_CONNECTING, "Asking for an address");
	if (net_dhcp_request(0) < 0 || !current->ip_addr[0])
		say(NET_FAILED, "No DHCP answer");
	else
		say(NET_UP, "Connected");
}

static void modem_start(void)
{
	int rate = 0, rv, i, n = 0;
	char text[32], digits[sizeof(config.phone)];
	say(NET_CONNECTING, "Dialling");
	/* The dialler stops at anything it has no tone for, such as a dash */
	for (i = 0; config.phone[i]; i++)
		if (config.phone[i] >= '0' && config.phone[i] <= '9')
			digits[n++] = config.phone[i];
	digits[n] = 0;
	if (!ppp_ready) {
		if (ppp_init() < 0) {
			say(NET_FAILED, "PPP would not start");
			return;
		}
		ppp_ready = 1;
	}
	ppp_set_login(config.user, config.pass);
	/* DreamPi plays a line voltage trick, so there may be no real dial tone */
	rv = ppp_modem_init(digits, 1, &rate);
	if (rv < 0) {
		say(NET_FAILED, rv == -4 ? "No answer" : "No modem");
		return;
	}
	snprintf(text, sizeof(text), "Modem up at %d, logging in", rate);
	say(NET_CONNECTING, text);
	if (ppp_connect() < 0) {
		say(NET_FAILED, "PPP login failed");
		return;
	}
	current = net_default_dev;
	live.link = LINK_MODEM;
	say(NET_UP, "Connected");
}

static void link_down(void)
{
	if (live.link == LINK_MODEM) {
		ppp_shutdown();
		ppp_ready = 0;
	} else if (current && current->if_stop) {
		current->if_stop(current);
		memset(current->ip_addr, 0, 4);
	}
	current = NULL;
	net_set_default(NULL);
	live.link = LINK_AUTO;
	say(NET_DOWN, "Not connected");
}

static void connect_now(void)
{
	int link = (int)config.link, rv = -1;
	stack_start();
	if (current)
		link_down();
	if (link == LINK_MODEM) {
		modem_start();
		return;
	}
	/* The W5500 probe turns the serial port into SPI, which would upset a serial
	 * console or SD adapter, so it only runs when someone asks for it by name. */
	if (link == LINK_AUTO) {
		rv = ether_start(LINK_BBA);
		if (rv == -1)
			rv = ether_start(LINK_LAN);
	} else {
		rv = ether_start(link);
	}
	if (rv == 0)
		ether_configure();
	else if (rv == -1)
		say(link == LINK_AUTO ? NET_DOWN : NET_FAILED, "No adapter found");
}

static void *worker(void *arg)
{
	(void)arg;
	for (;;) {
		if (want_connect) {
			want_connect = 0;
			memcpy(&config, &next_config, sizeof(config));
			connect_now();
		}
		if (want_down) {
			want_down = 0;
			if (current)
				link_down();
		}
		if (ping_pending) {
			static uint8_t data[32];
			uint8_t ip[4];
			uint32_t seq = ping_seq;
			memcpy(ip, ping_ip, 4);
			ping_pending = 0;
			if (current)
				net_icmp_send_echo(current, ip, 0x4452, (uint16_t)seq, data, sizeof(data));
		}
		if (want_resolve) {
			struct addrinfo hints, *res = NULL;
			want_resolve = 0;
			memset(&hints, 0, sizeof(hints));
			hints.ai_family = AF_INET;
			if (current && !getaddrinfo(resolve_name, NULL, &hints, &res) && res) {
				memcpy(resolve_ip, &((struct sockaddr_in *)res->ai_addr)->sin_addr, 4);
				resolve_state = 1;
				freeaddrinfo(res);
			} else {
				resolve_state = -1;
			}
		}
		if (last_reply) {
			printf("net: reply seq %lu in %lu us\n", (unsigned long)last_reply->seq,
				(unsigned long)last_reply->rtt_us);
			last_reply = NULL;
		}
		thd_sleep(IDLE_MS);
	}
	return NULL;
}

/* Settings go in one VMU file. Any memory card will do. */
static int config_path(char *path, size_t size, int unit)
{
	maple_device_t *dev = maple_enum_type(unit, MAPLE_FUNC_MEMCARD);
	if (!dev)
		return 0;
	snprintf(path, size, "/vmu/%c%d/%s", 'a' + dev->port, dev->unit, CONFIG_NAME);
	return 1;
}

static int config_load(net_config_t *out)
{
	struct { uint32_t magic; net_config_t config; } saved;
	char path[32];
	int unit;
	for (unit = 0; config_path(path, sizeof(path), unit); unit++) {
		file_t f = fs_open(path, O_RDONLY);
		ssize_t got;
		if (f == FILEHND_INVALID)
			continue;
		got = fs_read(f, &saved, sizeof(saved));
		fs_close(f);
		if (got == sizeof(saved) && saved.magic == CONFIG_MAGIC && saved.config.link < LINK_COUNT) {
			memcpy(out, &saved.config, sizeof(*out));
			return 1;
		}
	}
	return 0;
}

static void config_defaults(net_config_t *c)
{
	static const uint8_t mask[4] = {255, 255, 255, 0};
	memset(c, 0, sizeof(*c));
	c->link = LINK_AUTO;
	c->dhcp = 1;
	memcpy(c->mask, mask, 4);
	/* What a stock DreamPi expects, and it takes any password */
	strcpy(c->phone, "111-1111");
	strcpy(c->user, "dream");
	strcpy(c->pass, "dreamcast");
}

void plat_net_init(void)
{
	kthread_attr_t attr;
	config_defaults(&config);
	config_load(&config);
	memcpy(&next_config, &config, sizeof(config));
	strcpy((char *)live.text, "Not connected");
	under_dcload = dcload_type == DCLOAD_TYPE_IP;
	if (under_dcload) {
		/* dcload-ip owns the BBA and serves /pc through it. KOS can only take over
		 * while nothing is using it, and then carries dcload's traffic itself. */
		bba_init();
		arch_init_net_dcload_ip();
		net_icmp_echo_cb = echo_reply;
		stack_ready = 1;
		current = net_default_dev;
		if (current) {
			live.link = LINK_BBA;
			say(current->ip_addr[0] ? NET_UP : NET_FAILED,
				current->ip_addr[0] ? "Connected through dcload" : "No address from dcload");
		}
	} else {
		/* dcload-serial talks over the same port, but Auto never probes it */
		want_connect = 1;
	}
	memset(&attr, 0, sizeof(attr));
	attr.stack_size = WORKER_STACK;
	attr.label = "dreamnet";
	thd_create_ex(&attr, worker, NULL);
}

/* KOS gives each TCP socket 64KB each way, which is more than the heap can spare
 * without an SD card. Asking for less before connecting works, except that it
 * allocates a buffer connect() then overwrites, leaking it. So for just that call
 * realloc hands back a placeholder: KOS keeps the size and never frees the pointer
 * for a socket that didn't connect. Link with --wrap=realloc. */
#define SOCK_RCVBUF (16 * 1024)
#define SOCK_SNDBUF (4 * 1024)

extern void *__real_realloc(void *ptr, size_t size);
static kthread_t *tuning;

void *__wrap_realloc(void *ptr, size_t size)
{
	static uint8_t placeholder[4];
	if (tuning && tuning == thd_get_current() && !ptr)
		return placeholder;
	return __real_realloc(ptr, size);
}

void plat_sock_tune(int fd)
{
	uint32_t rcv = SOCK_RCVBUF, snd = SOCK_SNDBUF;
	tuning = thd_get_current();
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
	tuning = NULL;
}

void plat_net_status(net_status_t *s)
{
	netif_t *n = current;
	memset(s, 0, sizeof(*s));
	s->state = live.state;
	s->link = live.link;
	memcpy(s->text, (const char *)live.text, sizeof(s->text) - 1);
	if (n) {
		memcpy(s->ip, n->ip_addr, 4);
		memcpy(s->mask, n->netmask, 4);
		memcpy(s->gateway, n->gateway, 4);
		memcpy(s->dns, n->dns, 4);
		memcpy(s->mac, n->mac_addr, 6);
		strncpy(s->device, n->name, sizeof(s->device) - 1);
	}
}

void plat_net_get_config(net_config_t *c)
{
	memcpy(c, &next_config, sizeof(*c));
}

int plat_net_save_config(const net_config_t *c)
{
	struct { uint32_t magic; net_config_t config; } saved;
	char path[32];
	file_t f;
	ssize_t put;
	memcpy(&next_config, c, sizeof(*c));
	if (!config_path(path, sizeof(path), 0))
		return -1;
	saved.magic = CONFIG_MAGIC;
	memcpy(&saved.config, c, sizeof(*c));
	f = fs_open(path, O_WRONLY);
	if (f == FILEHND_INVALID)
		return -1;
	put = fs_write(f, &saved, sizeof(saved));
	fs_close(f);
	return put == sizeof(saved) ? 0 : -1;
}

int plat_print_load(print_config_t *c)
{
	struct { uint32_t magic; print_config_t config; } saved;
	maple_device_t *dev;
	char path[32];
	int unit;
	for (unit = 0; (dev = maple_enum_type(unit, MAPLE_FUNC_MEMCARD)); unit++) {
		file_t f;
		ssize_t got;
		snprintf(path, sizeof(path), "/vmu/%c%d/%s", 'a' + dev->port, dev->unit, PRINT_CONFIG_NAME);
		f = fs_open(path, O_RDONLY);
		if (f == FILEHND_INVALID)
			continue;
		got = fs_read(f, &saved, sizeof(saved));
		fs_close(f);
		if (got == sizeof(saved) && saved.magic == PRINT_CONFIG_MAGIC) {
			memcpy(c, &saved.config, sizeof(*c));
			return 1;
		}
	}
	return 0;
}

int plat_print_save(const print_config_t *c)
{
	struct { uint32_t magic; print_config_t config; } saved;
	maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
	char path[32];
	file_t f;
	ssize_t put;
	if (!dev)
		return -1;
	snprintf(path, sizeof(path), "/vmu/%c%d/%s", 'a' + dev->port, dev->unit, PRINT_CONFIG_NAME);
	saved.magic = PRINT_CONFIG_MAGIC;
	memcpy(&saved.config, c, sizeof(*c));
	f = fs_open(path, O_WRONLY);
	if (f == FILEHND_INVALID)
		return -1;
	put = fs_write(f, &saved, sizeof(saved));
	fs_close(f);
	return put == sizeof(saved) ? 0 : -1;
}

void plat_net_connect(const net_config_t *c)
{
	memcpy(&next_config, c, sizeof(*c));
	want_connect = 1;
}

void plat_net_disconnect(void)
{
	want_down = 1;
}

void plat_net_ping(const uint8_t ip[4], uint32_t seq)
{
	replies[seq % NET_SEQS].valid = 0;
	memcpy(ping_ip, ip, 4);
	ping_seq = seq;
	ping_pending = 1;
}

int plat_net_ping_result(uint32_t seq, uint32_t *rtt_us, uint32_t *ttl)
{
	volatile typeof(replies[0]) *r = &replies[seq % NET_SEQS];
	if (!r->valid || r->seq != (uint16_t)seq)
		return 0;
	*rtt_us = r->rtt_us;
	*ttl = r->ttl;
	return 1;
}

void plat_net_resolve(const char *name)
{
	strncpy(resolve_name, name, sizeof(resolve_name) - 1);
	resolve_state = 0;
	want_resolve = 1;
}

int plat_net_resolve_result(uint8_t ip[4])
{
	if (resolve_state > 0)
		memcpy(ip, resolve_ip, 4);
	return resolve_state;
}
