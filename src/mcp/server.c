#define _GNU_SOURCE
/*
 * server - FreeBee's MCP server on its private socket.
 *
 * Threads: one accepts connections, one reads each connection, and each
 * tool call runs in a thread of its own, so a long wait does not hold up a
 * screenshot. The emulator's main thread owns the machine: tool threads
 * see it through mcp (a copy of the screen and some state, updated at
 * every 60 Hz guest tick under mcp_mu) and change it through queues the
 * main thread empties at the tick (keys, mouse packets, serial input) or
 * through mcp_main_call.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <libgen.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>

#include "mcp.h"
#include "../fbmcp.h"
#include "../state.h"
#include "../dialer.h"
#include "../fbconfig.h"
#include "../keyboard.h"

pthread_mutex_t mcp_mu = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t mcp_cv;
mcp_state mcp;
char *mcp_vm_name;
char *mcp_vm_dir;

static bool started;
static int listen_fd = -1;
static char sock_path[512], reg_path[512];

double wall_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

void mcp_wait_until(double until)
{
	// mcp_cv runs on CLOCK_MONOTONIC where the OS lets us choose (Linux);
	// macOS has pthread_cond_timedwait_relative_np instead
	double left = until - wall_now();
	if (left <= 0)
		return;
	if (left > 0.25)
		left = 0.25;
#ifdef __APPLE__
	struct timespec ts = { (time_t)left, (long)((left - (time_t)left) * 1e9) };
	pthread_cond_timedwait_relative_np(&mcp_cv, &mcp_mu, &ts);
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	ts.tv_sec += (time_t)left;
	ts.tv_nsec += (long)((left - (time_t)left) * 1e9);
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}
	pthread_cond_timedwait(&mcp_cv, &mcp_mu, &ts);
#endif
}

/* ---- main thread calls ---- */

typedef struct main_call {
	void (*fn)(void *);
	void *arg;
	bool done;
	struct main_call *next;
} main_call;

static main_call *mc_head, *mc_tail;

void mcp_main_call(void (*fn)(void *), void *arg)
{
	main_call m = { fn, arg, false, NULL };
	pthread_mutex_lock(&mcp_mu);
	if (mcp.quitting) {
		pthread_mutex_unlock(&mcp_mu);
		return;
	}
	if (mc_tail)
		mc_tail->next = &m;
	else
		mc_head = &m;
	mc_tail = &m;
	while (!m.done && !mcp.quitting)
		mcp_wait_until(wall_now() + 1);
	if (!m.done) {
		// unlink: the main loop has ended and will not run it
		main_call **pp = &mc_head;
		mc_tail = NULL;
		while (*pp) {
			if (*pp == &m) {
				*pp = m.next;
				continue;
			}
			mc_tail = *pp;
			pp = &(*pp)->next;
		}
	}
	pthread_mutex_unlock(&mcp_mu);
}

static void run_main_calls(void)
{
	pthread_mutex_lock(&mcp_mu);
	while (mc_head) {
		main_call *m = mc_head;
		mc_head = m->next;
		if (!mc_head)
			mc_tail = NULL;
		pthread_mutex_unlock(&mcp_mu);
		m->fn(m->arg);
		pthread_mutex_lock(&mcp_mu);
		m->done = true;
	}
	pthread_cond_broadcast(&mcp_cv);
	pthread_mutex_unlock(&mcp_mu);
}

/* ---- mouse ---- */

typedef struct { int dx, dy, buttons, wait; } mouse_pkt;
#define MQ 4096
static mouse_pkt mq[MQ];
static int mq_head, mq_count, mq_wait;
static uint64_t mq_pushed, mq_done;

uint64_t mcp_mouse_push(int dx, int dy, int buttons, int wait_ticks)
{
	uint64_t s = 0;
	pthread_mutex_lock(&mcp_mu);
	if (mq_count < MQ) {
		mq[(mq_head + mq_count) % MQ] = (mouse_pkt){ dx, dy, buttons, wait_ticks };
		mq_count++;
		s = ++mq_pushed;
	}
	pthread_mutex_unlock(&mcp_mu);
	return s;
}

uint64_t mcp_mouse_done(void)
{
	pthread_mutex_lock(&mcp_mu);
	uint64_t s = mq_done;
	pthread_mutex_unlock(&mcp_mu);
	return s;
}

// main thread, mcp_mu held: one packet a tick, then its wait
static void mouse_tick_locked(void)
{
	if (mq_wait > 0) {
		mq_wait--;
		return;
	}
	if (!mq_count)
		return;
	mouse_pkt p = mq[mq_head];
	mq_head = (mq_head + 1) % MQ;
	mq_count--;
	mouse_event(&state.kbd, p.dx, p.dy, p.buttons);
	mq_wait = p.wait;
	mq_done++;
}

/* ---- serial ---- */

static char *ser_ring;
static uint64_t ser_total;
uint64_t mcp_serial_cursor;

#define RXQ (1 << 16)
static char rxq[RXQ];
static size_t rxq_head, rxq_count;
// XON/XOFF: the guest (with stty ixoff) sends ^S when its input is full
static bool rx_stopped;
static uint64_t rx_stop_tick;

void fbmcp_serial_tx(uint8_t b)
{
	if (!started)
		return;
	pthread_mutex_lock(&mcp_mu);
	ser_ring[ser_total % SER_RING] = b;
	ser_total++;
	if (b == 0x13) {
		rx_stopped = true;
		rx_stop_tick = mcp.ticks;
	} else if (b == 0x11) {
		rx_stopped = false;
	}
	pthread_mutex_unlock(&mcp_mu);
	// waiters notice at the next tick
}

int fbmcp_serial_rx(uint8_t *buf, int max)
{
	if (!started)
		return 0;
	// about 19200 baud at the guest's real speed
	if (max > 32)
		max = 32;
	pthread_mutex_lock(&mcp_mu);
	if (rx_stopped && mcp.ticks - rx_stop_tick < 3 * TICK_HZ) {
		pthread_mutex_unlock(&mcp_mu);
		return 0;
	}
	rx_stopped = false;	// XON, or a ^S that was only output
	int n = 0;
	while (n < max && rxq_count) {
		buf[n++] = rxq[rxq_head];
		rxq_head = (rxq_head + 1) % RXQ;
		rxq_count--;
	}
	pthread_mutex_unlock(&mcp_mu);
	return n;
}

size_t mcp_serial_get(uint64_t pos, char *out, size_t max, uint64_t *start)
{
	pthread_mutex_lock(&mcp_mu);
	uint64_t oldest = ser_total > SER_RING ? ser_total - SER_RING : 0;
	if (pos < oldest)
		pos = oldest;
	if (pos > ser_total)
		pos = ser_total;
	size_t n = ser_total - pos;
	if (n > max)
		n = max;
	for (size_t i = 0; i < n; i++)
		out[i] = ser_ring[(pos + i) % SER_RING];
	if (start)
		*start = pos;
	pthread_mutex_unlock(&mcp_mu);
	return n;
}

uint64_t mcp_serial_total(void)
{
	pthread_mutex_lock(&mcp_mu);
	uint64_t t = ser_total;
	pthread_mutex_unlock(&mcp_mu);
	return t;
}

bool mcp_serial_put(const char *data, size_t n)
{
	pthread_mutex_lock(&mcp_mu);
	bool ok = rxq_count + n <= RXQ;
	if (ok) {
		for (size_t i = 0; i < n; i++) {
			rxq[(rxq_head + rxq_count) % RXQ] = data[i];
			rxq_count++;
		}
	}
	pthread_mutex_unlock(&mcp_mu);
	return ok;
}

size_t mcp_serial_queued(void)
{
	pthread_mutex_lock(&mcp_mu);
	size_t n = rxq_count;
	pthread_mutex_unlock(&mcp_mu);
	return n;
}

/* ---- sound: the dialer's tone through the speaker ---- */

#define TONES 1024
static tone_event tones[TONES];
static uint64_t n_tones;

int mcp_tone_events(uint64_t since_tick, tone_event *out, int max)
{
	int n = 0;
	pthread_mutex_lock(&mcp_mu);
	uint64_t first = n_tones > TONES ? n_tones - TONES : 0;
	for (uint64_t i = first; i < n_tones && n < max; i++)
		if (tones[i % TONES].tick >= since_tick)
			out[n++] = tones[i % TONES];
	pthread_mutex_unlock(&mcp_mu);
	return n;
}

/* ---- debugger ---- */

volatile int fbmcp_dbg_active;
dbg_state mcp_dbg;
static bool cpu_stopped_now;

#include "../musashi/m68k.h"

int fbmcp_dbg_hook(unsigned int pc)
{
	dbg_state *d = &mcp_dbg;
	pc &= 0xffffff;
	if (d->skip_armed) {
		// the first instruction after a resume runs, even at a breakpoint
		d->skip_armed = false;
		if (pc == d->skip_pc) {
			if (d->steps)
				d->stepped++;
			return 0;
		}
	}
	if (d->steps) {
		if (d->stepped >= d->steps)
			goto stop;
		d->stepped++;
		return 0;
	}
	for (int i = 0; i < d->n_bp; i++) {
		if (d->bp[i] != pc)
			continue;
		if (d->super >= 0) {
			bool s = (m68k_get_reg(NULL, M68K_REG_SR) & 0x2000) != 0;
			if (s != (d->super == 1))
				return 0;
		}
		if (++d->hits < d->count)
			return 0;
		goto stop;
	}
	return 0;
stop:
	d->stopped = true;
	d->stop_pc = pc;
	d->n_bp = 0;
	d->steps = 0;
	fbmcp_dbg_active = 0;
	cpu_stopped_now = true;
	return 1;
}

bool fbmcp_cpu_stopped(void)
{
	if (!cpu_stopped_now)
		return false;
	cpu_stopped_now = false;
	pthread_mutex_lock(&mcp_mu);
	mcp.paused_break = true;
	pthread_cond_broadcast(&mcp_cv);
	pthread_mutex_unlock(&mcp_mu);
	return true;
}

/* ---- the main loop's side ---- */

bool mcp_cpu_running_locked(void)
{
	return !(mcp.paused_user || mcp.paused_break || rec_frozen_locked());
}

bool fbmcp_paused(void)
{
	if (!started)
		return false;
	pthread_mutex_lock(&mcp_mu);
	bool p = !mcp_cpu_running_locked();
	pthread_mutex_unlock(&mcp_mu);
	return p;
}

// the last few pictures of the screen, to tell a blinking cursor from news
#define HIST 8
static frame_t hist[HIST];
static uint64_t hist_count;

int frame_history(uint64_t max_seq, uint64_t min_tick, frame_t *out, int max)
{
	int n = 0;
	pthread_mutex_lock(&mcp_mu);
	uint64_t first = hist_count > HIST ? hist_count - HIST : 0;
	for (uint64_t i = first; i < hist_count && n < max; i++) {
		const frame_t *f = &hist[i % HIST];
		if (f->seq <= max_seq && f->tick >= min_tick)
			out[n++] = *f;
	}
	pthread_mutex_unlock(&mcp_mu);
	return n;
}

// mcp_mu held
static void frame_update_locked(bool vram_changed)
{
	if ((vram_changed || state.reverse_video != mcp.frame.rev) &&
	    (state.reverse_video != mcp.frame.rev ||
	     memcmp(mcp.frame.vram, state.vram, VRAM_USED))) {
		memcpy(mcp.frame.vram, state.vram, VRAM_USED);
		mcp.frame.rev = state.reverse_video;
		mcp.frame.seq++;
		mcp.frame.tick = mcp.ticks;
		mcp.change_wall = wall_now();
		hist[hist_count % HIST] = mcp.frame;
		hist_count++;
	}
}

void fbmcp_tick(bool vram_changed)
{
	if (!started)
		return;
	const DIALER_STATE *d = dialer_get_state();
	bool tone = d->active && d->to_speaker;

	pthread_mutex_lock(&mcp_mu);
	mcp.ticks++;
	frame_update_locked(vram_changed);
	mcp.leds = state.leds;
	mcp.mouse_enabled = state.kbd.mouse_enabled;
	if (tone != mcp.tone || (tone && (d->f_low != mcp.tone_lo || d->f_high != mcp.tone_hi))) {
		mcp.tone = tone;
		mcp.tone_lo = tone ? d->f_low : 0;
		mcp.tone_hi = tone ? d->f_high : 0;
		tones[n_tones % TONES] = (tone_event){ mcp.ticks, tone, mcp.tone_lo, mcp.tone_hi };
		n_tones++;
	}
	mouse_tick_locked();
	pthread_cond_broadcast(&mcp_cv);
	pthread_mutex_unlock(&mcp_mu);

	if (rec_active())
		rec_tick(state.vram, state.reverse_video, tone, d->f_low, d->f_high);
	run_main_calls();
}

void fbmcp_service(void)
{
	if (!started)
		return;
	run_main_calls();
	pthread_mutex_lock(&mcp_mu);
	// memory-write may have drawn on the screen
	frame_update_locked(state.vram_updated);
	mcp.leds = state.leds;
	pthread_cond_broadcast(&mcp_cv);
	pthread_mutex_unlock(&mcp_mu);
}

bool fbmcp_quit(void)
{
	if (!started)
		return false;
	pthread_mutex_lock(&mcp_mu);
	bool q = mcp.quit;
	pthread_mutex_unlock(&mcp_mu);
	return q;
}

bool fbmcp_reset_wanted(void)
{
	if (!started)
		return false;
	pthread_mutex_lock(&mcp_mu);
	bool r = mcp.reset;
	mcp.reset = false;
	pthread_mutex_unlock(&mcp_mu);
	return r;
}

/* ---- calls ---- */

struct conn {
	int fd;
	pthread_mutex_t wlock;
	int refs;
	bool closed;
	call *calls;
};

static pthread_mutex_t conn_mu = PTHREAD_MUTEX_INITIALIZER;

static void conn_unref(conn *cn)
{
	pthread_mutex_lock(&conn_mu);
	bool last = --cn->refs == 0;
	pthread_mutex_unlock(&conn_mu);
	if (last) {
		close(cn->fd);
		pthread_mutex_destroy(&cn->wlock);
		free(cn);
	}
}

static void conn_send(conn *cn, sbuf *msg)
{
	sb_putc(msg, '\n');
	pthread_mutex_lock(&cn->wlock);
	size_t off = 0;
	while (off < msg->len) {
		ssize_t w = write(cn->fd, msg->s + off, msg->len - off);
		if (w > 0) {
			off += w;
		} else if (w < 0 && errno == EINTR) {
			continue;
		} else {
			break;
		}
	}
	pthread_mutex_unlock(&cn->wlock);
}

static void send_result(conn *cn, const char *id, const char *result)
{
	sbuf m;
	sb_init(&m);
	sb_printf(&m, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}", id, result);
	conn_send(cn, &m);
	sb_free(&m);
}

static void send_error(conn *cn, const char *id, int code, const char *text)
{
	sbuf m;
	sb_init(&m);
	sb_printf(&m, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":", id ? id : "null", code);
	sb_jstr(&m, text);
	sb_puts(&m, "}}");
	conn_send(cn, &m);
	sb_free(&m);
}

void call_printf(call *c, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	sb_vprintf(&c->text, fmt, ap);
	va_end(ap);
}

void call_error(call *c, const char *fmt, ...)
{
	va_list ap;
	if (c->text.len && c->text.s[c->text.len - 1] != '\n')
		sb_putc(&c->text, '\n');
	va_start(ap, fmt);
	sb_vprintf(&c->text, fmt, ap);
	va_end(ap);
	c->is_error = true;
}

bool call_stopped(call *c)
{
	if (call_cancelled(c))
		return true;
	pthread_mutex_lock(&mcp_mu);
	bool q = mcp.quitting;
	pthread_mutex_unlock(&mcp_mu);
	return q;
}

void call_progress(call *c, const char *what)
{
	double now = wall_now();
	if (!c->token || now - c->last_progress < 5)
		return;
	c->last_progress = now;
	sbuf m;
	sb_init(&m);
	sb_printf(&m, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\","
		"\"params\":{\"progressToken\":%s,\"progress\":%d,\"message\":", c->token, ++c->progress);
	sbuf t;
	sb_init(&t);
	sb_printf(&t, "%s (%.0f s)", what, now - c->started);
	sb_jstr(&m, t.s);
	sb_free(&t);
	sb_puts(&m, "}}");
	conn_send(c->cn, &m);
	sb_free(&m);
}

typedef struct {
	call *c;
	char *name;
	json *args;
} work;

static void *call_worker(void *p)
{
	work *w = p;
	call *c = w->c;

	pthread_mutex_lock(&mcp_mu);
	mcp.calls_running++;
	pthread_cond_broadcast(&mcp_cv);
	pthread_mutex_unlock(&mcp_mu);

	tool_fn fn = tool_find(w->name);
	if (!fn)
		call_error(c, "unknown tool '%s'", w->name);
	else if (!w->args || w->args->type != JSON_OBJECT)
		call_error(c, "the arguments must be an object");
	else
		fn(c, w->args);

	pthread_mutex_lock(&mcp_mu);
	mcp.calls_running--;
	pthread_cond_broadcast(&mcp_cv);
	pthread_mutex_unlock(&mcp_mu);

	if (!c->cancelled) {
		sbuf r;
		sb_init(&r);
		sb_puts(&r, "{\"content\":[{\"type\":\"text\",\"text\":");
		sb_jstr(&r, c->text.len ? c->text.s : "done");
		sb_putc(&r, '}');
		if (c->image) {
			sb_puts(&r, ",{\"type\":\"image\",\"mimeType\":\"image/png\",\"data\":\"");
			sb_puts(&r, c->image);
			sb_puts(&r, "\"}");
		}
		sb_printf(&r, "],\"isError\":%s}", c->is_error ? "true" : "false");
		send_result(c->cn, c->id, r.s);
		sb_free(&r);
	}

	pthread_mutex_lock(&conn_mu);
	for (call **pp = &c->cn->calls; *pp; pp = &(*pp)->next) {
		if (*pp == c) {
			*pp = c->next;
			break;
		}
	}
	pthread_mutex_unlock(&conn_mu);
	conn_unref(c->cn);
	free(c->id);
	free(c->token);
	free(c->image);
	sb_free(&c->text);
	free(c);
	free(w->name);
	json_free(w->args);
	free(w);
	return NULL;
}

static void handle_message(conn *cn, json *msg)
{
	const char *method = json_get_str(msg, "method");
	json *idv = json_get(msg, "id");
	json *params = json_get(msg, "params");
	if (!method)
		return;		// a response; we send no requests
	sbuf idb;
	sb_init(&idb);
	if (idv)
		json_dump(&idb, idv);
	const char *id = idv ? idb.s : NULL;

	if (!idv) {
		if (strcmp(method, "notifications/cancelled") == 0) {
			json *rid = json_get(params, "requestId");
			if (rid) {
				sbuf r;
				sb_init(&r);
				json_dump(&r, rid);
				pthread_mutex_lock(&conn_mu);
				for (call *c = cn->calls; c; c = c->next)
					if (strcmp(c->id, r.s) == 0)
						c->cancelled = true;
				pthread_mutex_unlock(&conn_mu);
				pthread_mutex_lock(&mcp_mu);
				pthread_cond_broadcast(&mcp_cv);
				pthread_mutex_unlock(&mcp_mu);
				sb_free(&r);
			}
		}
	} else if (strcmp(method, "initialize") == 0) {
		const char *asked = json_get_str(params, "protocolVersion");
		sbuf r;
		sb_init(&r);
		sb_puts(&r, "{\"protocolVersion\":");
		sb_jstr(&r, asked && mcp_protocol_supported(asked) ? asked : MCP_PROTOCOL_LATEST);
		sb_puts(&r, ",\"capabilities\":{\"tools\":{\"listChanged\":false}},"
			"\"serverInfo\":{\"name\":\"freebee\",\"version\":\"1.0\"},\"instructions\":");
		sb_jstr(&r, mcp_instructions());
		sb_putc(&r, '}');
		send_result(cn, id, r.s);
		sb_free(&r);
	} else if (strcmp(method, "ping") == 0) {
		send_result(cn, id, "{}");
	} else if (strcmp(method, "tools/list") == 0) {
		sbuf r;
		sb_init(&r);
		sb_printf(&r, "{\"tools\":%s}", mcp_tools_json());
		send_result(cn, id, r.s);
		sb_free(&r);
	} else if (strcmp(method, "tools/call") == 0) {
		const char *name = json_get_str(params, "name");
		if (!name) {
			send_error(cn, id, -32602, "tools/call needs a name");
			goto out;
		}
		call *c = calloc(1, sizeof(*c));
		work *w = calloc(1, sizeof(*w));
		c->cn = cn;
		c->id = strdup(id);
		c->started = c->last_progress = wall_now();
		sb_init(&c->text);
		json *meta = json_get(params, "_meta");
		json *tok = json_get(meta, "progressToken");
		if (tok) {
			sbuf t;
			sb_init(&t);
			json_dump(&t, tok);
			c->token = sb_take(&t);
		}
		w->c = c;
		w->name = strdup(name);
		// take the arguments out of the message
		json *args = NULL;
		for (int i = 0; params && i < params->n; i++) {
			if (strcmp(params->keys[i], "arguments") == 0) {
				args = params->items[i];
				params->items[i] = NULL;
				break;
			}
		}
		if (!args)
			args = json_parse("{}", 2, NULL);
		w->args = args;
		pthread_mutex_lock(&conn_mu);
		cn->refs++;
		c->next = cn->calls;
		cn->calls = c;
		pthread_mutex_unlock(&conn_mu);
		pthread_t th;
		pthread_attr_t at;
		pthread_attr_init(&at);
		pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
		pthread_attr_setstacksize(&at, 1 << 20);
		if (pthread_create(&th, &at, call_worker, w) != 0) {
			send_error(cn, id, -32603, "cannot start a thread");
			pthread_mutex_lock(&conn_mu);
			cn->calls = c->next;
			cn->refs--;
			pthread_mutex_unlock(&conn_mu);
			free(c->id);
			free(c->token);
			free(c);
			free(w->name);
			json_free(w->args);
			free(w);
		}
		pthread_attr_destroy(&at);
	} else {
		sbuf t;
		sb_init(&t);
		sb_printf(&t, "unknown method '%s'", method);
		send_error(cn, id, -32601, t.s);
		sb_free(&t);
	}
out:
	sb_free(&idb);
}

static void *conn_reader(void *p)
{
	conn *cn = p;
	sbuf in;
	sb_init(&in);
	char buf[65536];
	for (;;) {
		ssize_t n = read(cn->fd, buf, sizeof(buf));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		sb_add(&in, buf, n);
		char *nl;
		while (in.len && (nl = memchr(in.s, '\n', in.len)) != NULL) {
			size_t len = nl - in.s;
			const char *err = NULL;
			bool blank = strspn(in.s, " \t\r") >= len;
			json *msg = blank ? NULL : json_parse(in.s, len, &err);
			if (msg && msg->type == JSON_OBJECT)
				handle_message(cn, msg);
			else if (!blank)
				send_error(cn, NULL, -32700, err ? err : "a message must be a JSON object");
			json_free(msg);
			memmove(in.s, nl + 1, in.len - len - 1);
			in.len -= len + 1;
		}
	}
	sb_free(&in);
	pthread_mutex_lock(&conn_mu);
	cn->closed = true;
	for (call *c = cn->calls; c; c = c->next)
		c->cancelled = true;
	pthread_mutex_unlock(&conn_mu);
	pthread_mutex_lock(&mcp_mu);
	pthread_cond_broadcast(&mcp_cv);
	pthread_mutex_unlock(&mcp_mu);
	conn_unref(cn);
	return NULL;
}

static void *acceptor(void *p)
{
	(void)p;
	for (;;) {
		int fd = accept(listen_fd, NULL, NULL);
		if (fd < 0) {
			if (errno == EINTR || errno == ECONNABORTED)
				continue;
			if (listen_fd < 0)
				return NULL;
			usleep(100000);
			continue;
		}
		conn *cn = calloc(1, sizeof(*cn));
		cn->fd = fd;
		cn->refs = 1;
		pthread_mutex_init(&cn->wlock, NULL);
		pthread_t th;
		pthread_attr_t at;
		pthread_attr_init(&at);
		pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
		if (pthread_create(&th, &at, conn_reader, cn) != 0) {
			close(fd);
			free(cn);
		}
		pthread_attr_destroy(&at);
	}
	return NULL;
}

/* ---- registry ---- */

const char *mcp_registry_dir(void)
{
	static char dir[256];
	if (!dir[0])
		snprintf(dir, sizeof(dir), "/tmp/freebee-%u", (unsigned)getuid());
	return dir;
}

bool mcp_registry_ok(const char *dir, bool create)
{
	struct stat st;
	if (create)
		mkdir(dir, 0700);
	if (lstat(dir, &st) != 0)
		return false;
	// ours, a directory, and nobody else's to look into
	return S_ISDIR(st.st_mode) && st.st_uid == getuid() && (st.st_mode & 077) == 0;
}

static void write_registry(void)
{
	char tmp[600];
	sbuf j;
	sb_init(&j);
	char when[64];
	time_t t = time(NULL);
	strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%S%z", localtime(&t));
	const char *agent = getenv("CLAUDECODE");
	bool by_agent = (agent && *agent && strcmp(agent, "0")) ||
		getenv("CLAUDE_CODE_SESSION_ID") || getenv("CLAUDE_CODE_ENTRYPOINT");
	char pty[512] = "";
	const char *link = fbc_get_string("serial", "symlink");
	if (link) {
		ssize_t n = readlink(link, pty, sizeof(pty) - 1);
		pty[n > 0 ? n : 0] = 0;
	}
	sb_printf(&j, "{\"pid\":%d,\"name\":", (int)getpid());
	sb_jstr(&j, mcp_vm_name);
	sb_puts(&j, ",\"dir\":");
	sb_jstr(&j, mcp_vm_dir);
	sb_puts(&j, ",\"socket\":");
	sb_jstr(&j, sock_path);
	sb_puts(&j, ",\"started\":");
	sb_jstr(&j, when);
	sb_printf(&j, ",\"by-agent\":%s,\"serial-pty\":", by_agent ? "true" : "false");
	sb_jstr(&j, pty);
	sb_puts(&j, ",\"disk1\":");
	sb_jstr(&j, fbc_get_string("hard_disk", "disk1"));
	sb_puts(&j, "}\n");
	snprintf(tmp, sizeof(tmp), "%s.tmp", reg_path);
	FILE *f = fopen(tmp, "w");
	if (f) {
		fputs(j.s, f);
		if (fclose(f) == 0)
			rename(tmp, reg_path);
		else
			unlink(tmp);
	}
	sb_free(&j);
}

static void remove_registry(void)
{
	if (reg_path[0])
		unlink(reg_path);
	if (sock_path[0])
		unlink(sock_path);
}

bool fbmcp_init(void)
{
	const char *env = getenv("FREEBEE_MCP");
	if (env && (strcmp(env, "0") == 0 || strcmp(env, "off") == 0 || strcmp(env, "no") == 0))
		return false;
	if (!env && !fbc_get_bool("mcp", "enabled"))
		return false;

	pthread_condattr_t ca;
	pthread_condattr_init(&ca);
#ifndef __APPLE__
	pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
#endif
	pthread_cond_init(&mcp_cv, &ca);
	pthread_condattr_destroy(&ca);

	char cwd[1024];
	if (!getcwd(cwd, sizeof(cwd)))
		strcpy(cwd, ".");
	mcp_vm_dir = strdup(cwd);
	const char *name = getenv("FREEBEE_NAME");
	if (!name || !*name)
		name = fbc_get_string("mcp", "name");
	if (!name || !*name) {
		char tmp[1024];
		strcpy(tmp, cwd);
		name = basename(tmp);
		mcp_vm_name = strdup(name);
	} else {
		mcp_vm_name = strdup(name);
	}

	ser_ring = calloc(1, SER_RING);
	if (!ser_ring)
		return false;

	const char *dir = mcp_registry_dir();
	if (!mcp_registry_ok(dir, true)) {
		fprintf(stderr, "MCP: %s is not a private directory of ours; no MCP server\n", dir);
		return false;
	}
	snprintf(sock_path, sizeof(sock_path), "%s/%d.sock", dir, (int)getpid());
	snprintf(reg_path, sizeof(reg_path), "%s/%d.json", dir, (int)getpid());

	struct sockaddr_un sa;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	if (strlen(sock_path) >= sizeof(sa.sun_path))
		return false;
	strcpy(sa.sun_path, sock_path);
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return false;
	unlink(sock_path);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(fd, 8) < 0) {
		fprintf(stderr, "MCP: cannot listen on %s: %s\n", sock_path, strerror(errno));
		close(fd);
		return false;
	}
	chmod(sock_path, 0600);
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	listen_fd = fd;
	signal(SIGPIPE, SIG_IGN);

	memcpy(mcp.frame.vram, state.vram, VRAM_USED);
	mcp.change_wall = wall_now();
	started = true;

	pthread_t th;
	pthread_attr_t at;
	pthread_attr_init(&at);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&th, &at, acceptor, NULL) != 0) {
		pthread_attr_destroy(&at);
		started = false;
		close(fd);
		unlink(sock_path);
		return false;
	}
	pthread_attr_destroy(&at);
	write_registry();
	atexit(remove_registry);
	printf("MCP server for '%s' on %s\n", mcp_vm_name, sock_path);
	return true;
}

void fbmcp_done(void)
{
	if (!started)
		return;
	rec_finish();
	pthread_mutex_lock(&mcp_mu);
	mcp.quitting = true;
	pthread_cond_broadcast(&mcp_cv);
	pthread_mutex_unlock(&mcp_mu);
	remove_registry();
	int fd = listen_fd;
	listen_fd = -1;
	if (fd >= 0) {
		shutdown(fd, SHUT_RDWR);
		close(fd);
	}
	// let calls that were waiting send their answers
	usleep(100000);
}
