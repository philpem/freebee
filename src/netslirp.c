/*
 * netslirp - user mode networking for the Ethernet board, with libslirp.
 * The way QEMU's net/slirp.c drives it, in a few lines: FreeBee's main
 * loop calls netslirp_poll() about every millisecond, with no waiting.
 * On Windows the sockets are polled with WSAPoll.
 */

#ifdef HAVE_SLIRP

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <poll.h>
#include <arpa/inet.h>
#endif
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
#ifdef _WIN32
	static LARGE_INTEGER freq;
	LARGE_INTEGER c;

	if (freq.QuadPart == 0)
		QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&c);
	return c.QuadPart / freq.QuadPart * 1000000000LL +
		c.QuadPart % freq.QuadPart * 1000000000LL / freq.QuadPart;
#else
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
}

// an IPv4 address in dotted form (inet_aton is not on Windows)
static bool ip4(const char *s, struct in_addr *a)
{
	return inet_pton(AF_INET, s, a) == 1;
}

// libslirp 4.8 made the callbacks return slirp_ssize_t; before, ssize_t
#if !SLIRP_CHECK_VERSION(4, 8, 0)
typedef ssize_t slirp_ssize_t;
#endif

// A frame from slirp for the guest, as it is
static slirp_ssize_t send_packet(const void *buf, size_t len, void *opaque)
{
	(void)opaque;
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

#if SLIRP_CONFIG_VERSION_MAX >= 6
static int add_poll(slirp_os_socket fd, int events, void *opaque)
#else
static int add_poll(int fd, int events, void *opaque)
#endif
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
#ifndef _WIN32		// WSAPoll takes only POLLIN and POLLOUT
	if (events & SLIRP_POLL_PRI)
		ev |= POLLPRI;
	if (events & SLIRP_POLL_ERR)
		ev |= POLLERR;
	if (events & SLIRP_POLL_HUP)
		ev |= POLLHUP;
#endif
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
	if (*p && !ip4(p, &haddr))
		return false;
	hport = atoi(colon + 1);

	// guest side
	p = dash + 1;
	if ((colon = strrchr(p, ':')) == NULL)
		return false;
	*colon = '\0';
	ip4(GUEST_ADDR, &gaddr);
	if (*p && !ip4(p, &gaddr))
		return false;
	gport = atoi(colon + 1);

	if (hport < 1 || hport > 65535 || gport < 1 || gport > 65535)
		return false;
	if (slirp_add_hostfwd(slirp, is_udp, haddr, hport, gaddr, gport) < 0) {
		fprintf(stderr, "[network] hostfwd %s: could not forward host port %d (in use?)\n", rule, hport);
		return true;
	}
	char hbuf[INET_ADDRSTRLEN], gbuf[INET_ADDRSTRLEN];	// inet_ntoa has one buffer
	inet_ntop(AF_INET, &haddr, hbuf, sizeof(hbuf));
	inet_ntop(AF_INET, &gaddr, gbuf, sizeof(gbuf));
	printf("Ethernet: host %s port %s:%d goes to the guest's %s:%d\n", is_udp ? "udp" : "tcp",
			haddr.s_addr == INADDR_ANY ? "*" : hbuf, hport, gbuf, gport);
	return true;
}

bool netslirp_init(const char *hostfwd)
{
	SlirpConfig cfg;
	static SlirpCb cb;

#ifdef _WIN32
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		fprintf(stderr, "network: Windows sockets did not start\n");
		return false;
	}
#endif
	memset(&cfg, 0, sizeof(cfg));
#if SLIRP_CONFIG_VERSION_MAX >= 6
	cfg.version = 6;
#else
	cfg.version = SLIRP_CONFIG_VERSION_MAX;
#endif
	cfg.restricted = 0;
	cfg.in_enabled = true;
	ip4("10.0.2.0", &cfg.vnetwork);
	ip4("255.255.255.0", &cfg.vnetmask);
	ip4("10.0.2.2", &cfg.vhost);
	cfg.in6_enabled = false;
	ip4(GUEST_ADDR, &cfg.vdhcp_start);
	ip4("10.0.2.3", &cfg.vnameserver);
	// [network] emu: libslirp's own helpers for old protocols through its
	// NAT, which rewrite the guest's FTP PORT commands (the 3B1's ftp has
	// no passive mode), and look at rlogin and IRC. Off, as in QEMU.
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
#ifdef _WIN32
	int const r = npfd ? WSAPoll(pfd, npfd, 0) : 0;
#else
	int const r = npfd ? poll(pfd, npfd, 0) : 0;
#endif
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
	if (slirp != NULL) {
		slirp_cleanup(slirp);
#ifdef _WIN32
		WSACleanup();
#endif
	}
	slirp = NULL;
	free(pfd);
	pfd = NULL;
	npfd = maxpfd = 0;
}

#else
typedef int netslirp_not_built;	// ISO C wants a declaration in a file
#endif /* HAVE_SLIRP */
