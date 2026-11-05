/* Network interface between the guest's DreamNet SWIs (net.c) and whatever
 * carries packets. A platform provides the plat_net_* calls: KOS drivers on the
 * console (src/dc/net.c), a scripted network on the harness (src/host/net.c).
 * None of these block, so a SWI can never stall the desktop. This header stays
 * free of cpu.h: the console backend runs on threads where r14 is not the
 * machine state. */
#ifndef NET_H
#define NET_H
#include <stdint.h>

enum { LINK_AUTO, LINK_BBA, LINK_LAN, LINK_W5500, LINK_MODEM, LINK_COUNT };
enum { NET_DOWN, NET_CONNECTING, NET_UP, NET_FAILED };

/* Both structures are shared with BASIC, so the layouts are part of the SWI API */
typedef struct {
	uint32_t link;       /* LINK_*: Auto tries the adapters that are safe to probe */
	uint32_t dhcp;       /* non-zero to ask the network; ignored by the modem */
	uint8_t ip[4], mask[4], gateway[4], dns[4];
	char phone[24], user[16], pass[16]; /* modem only */
} net_config_t;

typedef struct {
	uint32_t state;      /* NET_* */
	uint32_t link;       /* LINK_* in use, or LINK_AUTO when down */
	uint8_t ip[4], mask[4], gateway[4], dns[4];
	uint8_t mac[6], pad[2];
	char device[16];
	char text[32];       /* one line of progress for the user */
} net_status_t;

/* !NetTime's settings, shared with BASIC like the two above */
typedef struct {
	int32_t offset_min;  /* local time minus UTC */
	uint32_t summer;     /* non-zero adds an hour */
	char server[56];     /* name or address, with :port if it isn't 123 */
} time_config_t;

#define NET_SEQS 16 /* ping replies remembered, by sequence number */

void plat_net_init(void);
void plat_sock_tune(int fd); /* a new TCP socket, before it connects */
void plat_net_status(net_status_t *status);
void plat_net_get_config(net_config_t *config);
int plat_net_save_config(const net_config_t *config); /* 0 once it will survive power off */
void plat_net_connect(const net_config_t *config);
void plat_net_disconnect(void);
void plat_net_ping(const uint8_t ip[4], uint32_t seq);
int plat_net_ping_result(uint32_t seq, uint32_t *rtt_us, uint32_t *ttl); /* 1 got it, 0 waiting */
void plat_net_resolve(const char *name);
int plat_net_resolve_result(uint8_t ip[4]); /* 1 got it, 0 waiting, -1 failed */
int plat_time_load(time_config_t *config);       /* 1 if there was one */
int plat_time_save(const time_config_t *config); /* 0 once it will survive power off */

/* Non-blocking sockets, shared between platforms (net_sock.c). A negative
 * result is a BSD errno as RISC OS numbers them, so 35 means try again. */
enum { SOCK_KIND_TCP, SOCK_KIND_UDP };
int net_sock_open(int kind);
int net_sock_bind(int fd, int port);
int net_sock_connect(int fd, const uint8_t ip[4], int port);
int net_sock_send(int fd, const void *data, int len);
int net_sock_recv(int fd, void *data, int len);
int net_sock_sendto(int fd, const void *data, int len, const uint8_t ip[4], int port);
int net_sock_recvfrom(int fd, void *data, int len, uint8_t ip[4], int *port);
int net_sock_close(int fd);
#endif
