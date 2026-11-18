/*
 * netslirp - user mode networking for the Ethernet board, with libslirp.
 * The way QEMU's net/slirp.c drives it, in a few lines: FreeBee's main
 * loop calls netslirp_poll() about every millisecond, with no waiting.
 */

#ifdef HAVE_SLIRP

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <time.h>
#include <arpa/inet.h>
#include <libslirp.h>
#include "netslirp.h"
#include "enet.h"
#include "fbconfig.h"

#define GUEST_ADDR	"10.0.2.15"

static Slirp *slirp;

static struct pollfd *pfd;
static int npfd, maxpfd;

typedef struct {
	SlirpTimerCb cb;
	void *cb_opaque;
	int64_t expire_ms;
	bool armed;
} TIMER;
#define MAX_TIMERS	8
static TIMER *timers[MAX_TIMERS];

static int64_t clock_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/*
 * FTP into the guest through a forwarded port. The 3B1's ftpd (1986) has
 * only active mode, and a client on the host's loopback tells it to
 * connect back to 127.0.0.1, which on the 3B1 is the 3B1. slirp rewrites
 * the guest's FTP commands, not the host's. So in a frame to the guest's
 * port 21, "PORT 127,0,0,1," becomes "PORT 10,0,2,02,": 10.0.2.2 is the
 * host as the guest sees it, and the same length keeps the TCP sequence
 * numbers right. Then the TCP checksum is made again.
 */
#define FTP_PORT_FROM	"PORT 127,0,0,1,"
#define FTP_PORT_TO	"PORT 10,0,2,02,"

static uint16_t tcp_checksum(const uint8_t *ip, const uint8_t *tcp, int tcp_len)
{
	uint32_t sum = 6 + tcp_len;		// pseudo header: protocol, length

	for (int i = 12; i < 20; i += 2)	// source and destination address
		sum += (ip[i] << 8) | ip[i + 1];
	for (int i = 0; i + 1 < tcp_len; i += 2)
		sum += (tcp[i] << 8) | tcp[i + 1];
	if (tcp_len & 1)
		sum += tcp[tcp_len - 1] << 8;
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return ~sum & 0xffff;
}

static void ftp_port_fixup(uint8_t *f, int len)
{
	if (len < 14 + 20 + 20 || f[12] != 0x08 || f[13] != 0x00)
		return;
	uint8_t *ip = f + 14;
	int const ihl = (ip[0] & 15) * 4;
	int const ip_len = (ip[2] << 8) | ip[3];
	if ((ip[0] >> 4) != 4 || ip[9] != 6 || ihl < 20 || ip_len > len - 14 || ip_len < ihl + 20)
		return;
	if ((((ip[6] << 8) | ip[7]) & 0x3fff) != 0)	// a fragment
		return;
	uint8_t *tcp = ip + ihl;
	int const tcp_len = ip_len - ihl;
	int const thl = (tcp[12] >> 4) * 4;
	if (((tcp[2] << 8) | tcp[3]) != 21 || thl < 20 || thl > tcp_len)
		return;

	uint8_t *data = tcp + thl;
	int const n = tcp_len - thl, k = sizeof(FTP_PORT_FROM) - 1;
	for (int i = 0; i + k <= n; i++) {
		if (memcmp(data + i, FTP_PORT_FROM, k) == 0) {
			memcpy(data + i, FTP_PORT_TO, k);
			tcp[16] = tcp[17] = 0;
			uint16_t const sum = tcp_checksum(ip, tcp, tcp_len);
			tcp[16] = sum >> 8;
			tcp[17] = sum & 0xff;
			return;
		}
	}
}

static slirp_ssize_t send_packet(const void *buf, size_t len, void *opaque)
{
	static uint8_t frame[1514];

	(void)opaque;
	if (len <= sizeof(frame)) {
		memcpy(frame, buf, len);
		ftp_port_fixup(frame, (int)len);
		buf = frame;
	}
	enet_host_receive(buf, (int)len);
	return len;
}

static void guest_error(const char *msg, void *opaque)
{
	(void)opaque;
	fprintf(stderr, "network: guest error: %s\n", msg);
}

static int64_t clock_get_ns(void *opaque)
{
	(void)opaque;
	return clock_ns();
}

static void *timer_new(SlirpTimerCb cb, void *cb_opaque, void *opaque)
{
	(void)opaque;
	for (int i = 0; i < MAX_TIMERS; i++) {
		if (timers[i] == NULL) {
			timers[i] = calloc(1, sizeof(TIMER));
			if (timers[i] == NULL)
				return NULL;
			timers[i]->cb = cb;
			timers[i]->cb_opaque = cb_opaque;
			return timers[i];
		}
	}
	fprintf(stderr, "network: out of slirp timers\n");
	return NULL;
}

static void timer_free(void *timer, void *opaque)
{
	(void)opaque;
	for (int i = 0; i < MAX_TIMERS; i++) {
		if (timers[i] == timer) {
			free(timers[i]);
			timers[i] = NULL;
		}
	}
}

static void timer_mod(void *timer, int64_t expire_time, void *opaque)
{
	TIMER *t = timer;

	(void)opaque;
	if (t != NULL) {
		t->expire_ms = expire_time;
		t->armed = true;
	}
}

#if SLIRP_CONFIG_VERSION_MAX >= 6
static void register_poll_socket(slirp_os_socket fd, void *opaque) { (void)fd; (void)opaque; }
static void unregister_poll_socket(slirp_os_socket fd, void *opaque) { (void)fd; (void)opaque; }
#else
static void register_poll_fd(int fd, void *opaque) { (void)fd; (void)opaque; }
static void unregister_poll_fd(int fd, void *opaque) { (void)fd; (void)opaque; }
#endif

static void notify(void *opaque)
{
	(void)opaque;
}

static int add_poll(int fd, int events, void *opaque)
{
	(void)opaque;
	if (npfd == maxpfd) {
		int const n = maxpfd ? maxpfd * 2 : 32;
		struct pollfd *p = realloc(pfd, n * sizeof(*pfd));
		if (p == NULL)
			return -1;
		pfd = p;
		maxpfd = n;
	}
	short ev = 0;
	if (events & SLIRP_POLL_IN)
		ev |= POLLIN;
	if (events & SLIRP_POLL_OUT)
		ev |= POLLOUT;
	if (events & SLIRP_POLL_PRI)
		ev |= POLLPRI;
	if (events & SLIRP_POLL_ERR)
		ev |= POLLERR;
	if (events & SLIRP_POLL_HUP)
		ev |= POLLHUP;
	pfd[npfd].fd = fd;
	pfd[npfd].events = ev;
	pfd[npfd].revents = 0;
	return npfd++;
}

static int get_revents(int idx, void *opaque)
{
	(void)opaque;
	if (idx < 0 || idx >= npfd)
		return 0;
	short const r = pfd[idx].revents;
	int ev = 0;
	if (r & POLLIN)
		ev |= SLIRP_POLL_IN;
	if (r & POLLOUT)
		ev |= SLIRP_POLL_OUT;
	if (r & POLLPRI)
		ev |= SLIRP_POLL_PRI;
	if (r & (POLLERR | POLLNVAL))
		ev |= SLIRP_POLL_ERR;
	if (r & POLLHUP)
		ev |= SLIRP_POLL_HUP;
	return ev;
}

// "[tcp:|udp:][hostaddr]:hostport-[guestaddr]:guestport"
static bool add_hostfwd(const char *rule)
{
	char buf[160], *p, *dash, *colon;
	int is_udp = 0;
	struct in_addr haddr, gaddr;
	int hport, gport;

	snprintf(buf, sizeof(buf), "%s", rule);
	p = buf;
	if (strncmp(p, "tcp:", 4) == 0) {
		p += 4;
	} else if (strncmp(p, "udp:", 4) == 0) {
		is_udp = 1;
		p += 4;
	}
	if ((dash = strchr(p, '-')) == NULL)
		return false;
	*dash = '\0';

	// host side
	if ((colon = strrchr(p, ':')) == NULL)
		return false;
	*colon = '\0';
	haddr.s_addr = INADDR_ANY;
	if (*p && !inet_aton(p, &haddr))
		return false;
	hport = atoi(colon + 1);

	// guest side
	p = dash + 1;
	if ((colon = strrchr(p, ':')) == NULL)
		return false;
	*colon = '\0';
	inet_aton(GUEST_ADDR, &gaddr);
	if (*p && !inet_aton(p, &gaddr))
		return false;
	gport = atoi(colon + 1);

	if (hport < 1 || hport > 65535 || gport < 1 || gport > 65535)
		return false;
	if (slirp_add_hostfwd(slirp, is_udp, haddr, hport, gaddr, gport) < 0) {
		fprintf(stderr, "[network] hostfwd %s: could not forward host port %d (in use?)\n", rule, hport);
		return true;
	}
	printf("Ethernet: host %s port %s:%d goes to the guest's %s:%d\n", is_udp ? "udp" : "tcp",
			haddr.s_addr == INADDR_ANY ? "*" : inet_ntoa(haddr), hport, inet_ntoa(gaddr), gport);
	return true;
}

bool netslirp_init(const char *hostfwd)
{
	SlirpConfig cfg;
	static SlirpCb cb;

	memset(&cfg, 0, sizeof(cfg));
#if SLIRP_CONFIG_VERSION_MAX >= 6
	cfg.version = 6;
#else
	cfg.version = SLIRP_CONFIG_VERSION_MAX;
#endif
	cfg.restricted = 0;
	cfg.in_enabled = true;
	inet_aton("10.0.2.0", &cfg.vnetwork);
	inet_aton("255.255.255.0", &cfg.vnetmask);
	inet_aton("10.0.2.2", &cfg.vhost);
	cfg.in6_enabled = false;
	inet_aton(GUEST_ADDR, &cfg.vdhcp_start);
	inet_aton("10.0.2.3", &cfg.vnameserver);
	// slirp's helpers for old protocols through NAT: FTP's PORT command
	// (the 3B1's ftp has no passive mode), rlogin, ident
	cfg.enable_emu = fbc_get_bool("network", "emu");

	memset(&cb, 0, sizeof(cb));
	cb.send_packet = send_packet;
	cb.guest_error = guest_error;
	cb.clock_get_ns = clock_get_ns;
	cb.timer_new = timer_new;
	cb.timer_free = timer_free;
	cb.timer_mod = timer_mod;
	cb.notify = notify;
#if SLIRP_CONFIG_VERSION_MAX >= 6
	cb.register_poll_socket = register_poll_socket;
	cb.unregister_poll_socket = unregister_poll_socket;
#else
	cb.register_poll_fd = register_poll_fd;
	cb.unregister_poll_fd = unregister_poll_fd;
#endif

	slirp = slirp_new(&cfg, &cb, NULL);
	if (slirp == NULL) {
		fprintf(stderr, "network: libslirp did not start\n");
		return false;
	}

	if (hostfwd != NULL && *hostfwd) {
		char *rules = strdup(hostfwd), *save = NULL;
		for (char *r = strtok_r(rules, ", ", &save); r != NULL; r = strtok_r(NULL, ", ", &save)) {
			if (!add_hostfwd(r))
				fprintf(stderr, "[network] hostfwd \"%s\" is not like tcp:127.0.0.1:2323-:23\n", r);
		}
		free(rules);
	}
	return true;
}

void netslirp_send(const uint8_t *frame, int length)
{
	if (slirp != NULL)
		slirp_input(slirp, frame, length);
}

void netslirp_poll(void)
{
	uint32_t timeout = 0;

	if (slirp == NULL)
		return;
	npfd = 0;
#if SLIRP_CONFIG_VERSION_MAX >= 6
	slirp_pollfds_fill_socket(slirp, &timeout, add_poll, NULL);
#else
	slirp_pollfds_fill(slirp, &timeout, add_poll, NULL);
#endif
	int const r = npfd ? poll(pfd, npfd, 0) : 0;
	slirp_pollfds_poll(slirp, r < 0, get_revents, NULL);

	int64_t const now = clock_ns() / 1000000;
	for (int i = 0; i < MAX_TIMERS; i++) {
		TIMER *t = timers[i];
		if (t != NULL && t->armed && t->expire_ms <= now) {
			t->armed = false;
			t->cb(t->cb_opaque);
		}
	}
}

void netslirp_done(void)
{
	if (slirp != NULL)
		slirp_cleanup(slirp);
	slirp = NULL;
	free(pfd);
	pfd = NULL;
	npfd = maxpfd = 0;
}

#else
typedef int netslirp_not_built;	// ISO C wants a declaration in a file
#endif /* HAVE_SLIRP */
