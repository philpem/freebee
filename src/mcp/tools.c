#define _GNU_SOURCE
/*
 * tools - the MCP tools of a FreeBee VM.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <errno.h>
#include <regex.h>
#include <unistd.h>
#include <sys/stat.h>

#include "mcp.h"
#include "../fbmcp.h"
#include "../state.h"
#include "../keys.h"
#include "../keyboard.h"
#include "../control.h"
#include "../fbconfig.h"
#include "../musashi/m68k.h"

#define ASPECT 1.5517	// screen y to 4:3: (720 / 348) * (3 / 4)

/* ---- arguments ---- */

static bool arg_int(call *c, const json *a, const char *k, long def, long lo, long hi, long *out)
{
	json *v = json_get(a, k);
	if (!v || v->type == JSON_NULL) {
		*out = def;
		return true;
	}
	if (v->type != JSON_NUMBER || v->num != floor(v->num)) {
		call_error(c, "%s must be a whole number", k);
		return false;
	}
	if (v->num < lo || v->num > hi) {
		call_error(c, "%s must be from %ld to %ld", k, lo, hi);
		return false;
	}
	*out = (long)v->num;
	return true;
}

static bool arg_num(call *c, const json *a, const char *k, double def, double lo, double hi, double *out)
{
	json *v = json_get(a, k);
	if (!v || v->type == JSON_NULL) {
		*out = def;
		return true;
	}
	if (v->type != JSON_NUMBER || v->num < lo || v->num > hi) {
		call_error(c, "%s must be a number from %g to %g", k, lo, hi);
		return false;
	}
	*out = v->num;
	return true;
}

static bool arg_bool(call *c, const json *a, const char *k, bool def, bool *out)
{
	json *v = json_get(a, k);
	if (!v || v->type == JSON_NULL) {
		*out = def;
		return true;
	}
	if (v->type != JSON_BOOL) {
		call_error(c, "%s must be true or false", k);
		return false;
	}
	*out = v->b;
	return true;
}

static bool arg_str(call *c, const json *a, const char *k, bool required, const char **out)
{
	json *v = json_get(a, k);
	*out = NULL;
	if (!v || v->type == JSON_NULL) {
		if (required)
			call_error(c, "%s is missing", k);
		return !required;
	}
	if (v->type != JSON_STRING) {
		call_error(c, "%s must be a string", k);
		return false;
	}
	*out = v->str;
	return true;
}

// index into vals, def when missing
static bool arg_enum(call *c, const json *a, const char *k, const char *const *vals, int def, int *out)
{
	const char *s;
	if (!arg_str(c, a, k, def < 0, &s))
		return false;
	if (!s) {
		*out = def;
		return true;
	}
	for (int i = 0; vals[i]; i++) {
		if (strcmp(vals[i], s) == 0) {
			*out = i;
			return true;
		}
	}
	sbuf l;
	sb_init(&l);
	for (int i = 0; vals[i]; i++)
		sb_printf(&l, "%s%s", i ? ", " : "", vals[i]);
	call_error(c, "%s must be one of: %s", k, l.s);
	sb_free(&l);
	return false;
}

static bool get_rect(call *c, const json *v, const char *what, rect *r)
{
	long x, y, w, h;
	if (!v || v->type != JSON_OBJECT) {
		call_error(c, "%s must be an object with x, y, w and h", what);
		return false;
	}
	if (!json_get(v, "x") || !json_get(v, "y") || !json_get(v, "w") || !json_get(v, "h")) {
		call_error(c, "%s needs x, y, w and h", what);
		return false;
	}
	if (!arg_int(c, v, "x", 0, 0, SCR_W - 1, &x) || !arg_int(c, v, "y", 0, 0, SCR_H - 1, &y) ||
	    !arg_int(c, v, "w", 1, 1, SCR_W, &w) || !arg_int(c, v, "h", 1, 1, SCR_H, &h))
		return false;
	if (x + w > SCR_W)
		w = SCR_W - x;
	if (y + h > SCR_H)
		h = SCR_H - y;
	*r = (rect){ x, y, w, h };
	return true;
}

static bool parse_addr(call *c, const char *s, uint32_t *out)
{
	char *end;
	errno = 0;
	unsigned long v = strtoul(s, &end, 0);
	while (*end == ' ')
		end++;
	if (!*s || *end || errno || v > 0xffffffffUL) {
		call_error(c, "'%s' is not an address: write hex with 0x, or decimal", s);
		return false;
	}
	*out = v;
	return true;
}

/* ---- the screen ---- */

void frame_get(frame_t *f)
{
	pthread_mutex_lock(&mcp_mu);
	*f = mcp.frame;
	f->at = mcp.ticks;
	pthread_mutex_unlock(&mcp_mu);
}

static bool in_rects(const rect *r, int n, int x, int y)
{
	for (int i = 0; i < n; i++)
		if (x >= r[i].x && x < r[i].x + r[i].w && y >= r[i].y && y < r[i].y + r[i].h)
			return true;
	return false;
}

int frame_diff(const frame_t *a, const frame_t *b, const rect *region,
		const rect *ign, int n_ign, rect *bbox)
{
	int n = 0, x0 = SCR_W, y0 = SCR_H, x1 = -1, y1 = -1;
	uint8_t inv = a->rev != b->rev ? 0xff : 0;
	for (int y = 0; y < SCR_H; y++) {
		if (region && (y < region->y || y >= region->y + region->h))
			continue;
		const uint8_t *pa = a->vram + y * VRAM_ROW, *pb = b->vram + y * VRAM_ROW;
		for (int i = 0; i < VRAM_ROW; i++) {
			uint8_t d = pa[i] ^ pb[i] ^ inv;
			if (!d)
				continue;
			// even bytes hold pixels 8..15 of their word, odd ones 0..7
			int base = (i >> 1) * 16 + ((i & 1) ? 0 : 8);
			for (int bit = 0; bit < 8; bit++) {
				if (!(d & (1 << bit)))
					continue;
				int x = base + bit;
				if (region && (x < region->x || x >= region->x + region->w))
					continue;
				if (n_ign && in_rects(ign, n_ign, x, y))
					continue;
				n++;
				if (x < x0) x0 = x;
				if (y < y0) y0 = y;
				if (x > x1) x1 = x;
				if (y > y1) y1 = y;
			}
		}
	}
	if (bbox)
		*bbox = n ? (rect){ x0, y0, x1 - x0 + 1, y1 - y0 + 1 } : (rect){ 0, 0, 0, 0 };
	return n;
}

/* mouse pointer, as the mouse tools think it is */
static pthread_mutex_t mouse_mu = PTHREAD_MUTEX_INITIALIZER;
// The 3B1's window driver moves the pointer one pixel a mouse count, with
// no acceleration, and stops it at the screen's edges: so the tools home it
// to the top left corner once, then count.
static bool m_known;
static double m_x, m_y;
static int m_buttons;

static void put_image(call *c, const uint8_t *pix, int w, int h, const uint8_t *pal, int n)
{
	sbuf png, b64;
	sb_init(&png);
	sb_init(&b64);
	if (png_encode(&png, w, h, pix, w, pal, n)) {
		sb_base64(&b64, (const unsigned char *)png.s, png.len);
		free(c->image);
		c->image = sb_take(&b64);
	}
	sb_free(&png);
	sb_free(&b64);
}

/// A screenshot of f into the call's result.
static void shot(call *c, const frame_t *f, const rect *region, double scale, bool aspect, bool pointer)
{
	rect r = region ? *region : (rect){ 0, 0, SCR_W, SCR_H };
	double sx = scale, sy = scale * (aspect ? ASPECT : 1);
	int w = (int)lround(r.w * sx), h = (int)lround(r.h * sy);
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	uint8_t *img = malloc((size_t)w * h);
	if (!img)
		return;
	if (sx >= 1 && sy >= 1) {
		static const uint8_t pal[9] = { 0, 0, 0, 255, 255, 255, 255, 48, 48 };
		for (int y = 0; y < h; y++) {
			int fy = r.y + (int)(y / sy);
			if (fy >= r.y + r.h) fy = r.y + r.h - 1;
			for (int x = 0; x < w; x++) {
				int fx = r.x + (int)(x / sx);
				if (fx >= r.x + r.w) fx = r.x + r.w - 1;
				img[(size_t)y * w + x] = frame_pixel(f, fx, fy);
			}
		}
		if (pointer) {
			pthread_mutex_lock(&mouse_mu);
			bool known = m_known;
			int px = (int)lround((m_x - r.x) * sx), py = (int)lround((m_y - r.y) * sy);
			pthread_mutex_unlock(&mouse_mu);
			if (known) {
				for (int d = -6; d <= 6; d++) {
					if (px + d >= 0 && px + d < w && py >= 0 && py < h)
						img[(size_t)py * w + px + d] = 2;
					if (py + d >= 0 && py + d < h && px >= 0 && px < w)
						img[(size_t)(py + d) * w + px] = 2;
				}
			}
		}
		put_image(c, img, w, h, pal, 3);
	} else {
		// smaller: average the pixels under each one, in grey
		for (int y = 0; y < h; y++) {
			int fy0 = r.y + (int)(y / sy), fy1 = r.y + (int)((y + 1) / sy);
			if (fy1 <= fy0) fy1 = fy0 + 1;
			if (fy1 > r.y + r.h) fy1 = r.y + r.h;
			for (int x = 0; x < w; x++) {
				int fx0 = r.x + (int)(x / sx), fx1 = r.x + (int)((x + 1) / sx);
				if (fx1 <= fx0) fx1 = fx0 + 1;
				if (fx1 > r.x + r.w) fx1 = r.x + r.w;
				int on = 0, all = 0;
				for (int yy = fy0; yy < fy1; yy++)
					for (int xx = fx0; xx < fx1; xx++, all++)
						on += frame_pixel(f, xx, yy);
				img[(size_t)y * w + x] = all ? on * 255 / all : 0;
			}
		}
		put_image(c, img, w, h, NULL, 0);
	}
	free(img);
}

/* ---- waiting ---- */

typedef struct {
	bool met, changed;
	rect box;		// the last change
	int pixels;
	double secs;
} wait_result;

static const char *const wait_names[] = { "none", "change", "stable", "change-then-stable", NULL };
enum { W_NONE, W_CHANGE, W_STABLE, W_CHANGE_STABLE };

#define WHIST 10	// pictures a wait remembers

static wait_result wait_screen_core(call *c, int until, long stable_ms, long timeout_ms, long min_px,
		const rect *region, const rect *ign, int n_ign, const frame_t *before)
{
	wait_result res = { false, false, { 0, 0, 0, 0 }, 0, 0 };
	frame_t *ref = malloc(sizeof(*ref)), *cur = malloc(sizeof(*cur));
	frame_t *seen = malloc(sizeof(frame_t) * WHIST);
	double t0 = wall_now(), deadline = t0 + timeout_ms / 1000.0;
	bool need_change = until == W_CHANGE || until == W_CHANGE_STABLE;
	bool need_stable = until == W_STABLE || until == W_CHANGE_STABLE;

	if (before)
		*ref = *before;
	else
		frame_get(ref);
	pthread_mutex_lock(&mcp_mu);
	uint64_t ref_tick = mcp.ticks;
	pthread_mutex_unlock(&mcp_mu);
	double ref_wall = t0;
	// what the screen showed in the seconds before: a change back to one of
	// these pictures (a blinking cursor) is not news
	uint64_t since = ref->at > 4 * TICK_HZ ? ref->at - 4 * TICK_HZ : 0;
	int n_seen = frame_history(ref->seq, since, seen, WHIST - 1);
	seen[n_seen++] = *ref;

	for (;;) {
		pthread_mutex_lock(&mcp_mu);
		*cur = mcp.frame;
		uint64_t ticks = mcp.ticks;
		bool running = mcp_cpu_running_locked();
		pthread_mutex_unlock(&mcp_mu);
		double now = wall_now();
		rect box;
		int d = frame_diff(ref, cur, region, ign, n_ign, &box);
		if (d >= min_px) {
			bool repeat = false;
			for (int i = 0; i < n_seen && !repeat; i++)
				repeat = frame_diff(&seen[i], cur, region, ign, n_ign, NULL) < min_px;
			*ref = *cur;
			if (!repeat) {
				res.changed = true;
				res.box = box;
				res.pixels = d;
				ref_tick = ticks;
				ref_wall = now;
				if (n_seen == WHIST) {
					memmove(seen, seen + 1, sizeof(frame_t) * (WHIST - 1));
					n_seen--;
				}
				seen[n_seen++] = *cur;
				if (until == W_CHANGE) {
					res.met = true;
					break;
				}
			}
		}
		if (need_stable && (!need_change || res.changed)) {
			double guest_ms = (ticks - ref_tick) * 1000.0 / TICK_HZ;
			double wall_ms = (now - ref_wall) * 1000.0;
			if (guest_ms >= stable_ms || (!running && wall_ms >= stable_ms)) {
				res.met = true;
				break;
			}
		}
		if (now >= deadline || call_stopped(c))
			break;
		call_progress(c, need_change && !res.changed ? "waiting for the screen to change" :
			"waiting for the screen to settle");
		pthread_mutex_lock(&mcp_mu);
		mcp_wait_until(deadline);
		pthread_mutex_unlock(&mcp_mu);
	}
	res.secs = wall_now() - t0;
	free(ref);
	free(cur);
	free(seen);
	return res;
}

static void describe_wait(call *c, int until, const wait_result *w, long stable_ms)
{
	if (w->met) {
		if (until == W_CHANGE)
			call_printf(c, "The screen changed after %.1f s (%d pixels in %d,%d %dx%d).",
				w->secs, w->pixels, w->box.x, w->box.y, w->box.w, w->box.h);
		else
			call_printf(c, "The screen was stable for %ld ms after %.1f s%s.", stable_ms, w->secs,
				w->changed ? "" : " (it did not change)");
	} else {
		c->unmet = true;
		if (call_cancelled(c))
			call_printf(c, "The wait was cancelled after %.1f s.", w->secs);
		else if ((until == W_CHANGE || until == W_CHANGE_STABLE) && !w->changed)
			call_printf(c, "The screen did not change in %.1f s.", w->secs);
		else
			call_printf(c, "The screen did not settle in %.1f s; it still changes.", w->secs);
	}
}

static bool wait_ticks(call *c, int n)
{
	pthread_mutex_lock(&mcp_mu);
	uint64_t until = mcp.ticks + n;
	double deadline = wall_now() + 30;
	while (mcp.ticks < until && mcp_cpu_running_locked() && !mcp.quitting && !call_cancelled(c) &&
	       wall_now() < deadline)
		mcp_wait_until(deadline);
	bool ok = mcp.ticks >= until;
	pthread_mutex_unlock(&mcp_mu);
	return ok;
}

// wait until the guest has the keys, mouse packets and serial bytes sent so far
static bool wait_delivered(call *c, uint64_t kseq, uint64_t mseq, bool serial)
{
	double deadline = wall_now() + WAIT_MAX_MS / 1000.0;
	for (;;) {
		bool done = keys_done() >= kseq && mcp_mouse_done() >= mseq &&
			(!serial || mcp_serial_queued() == 0);
		if (done)
			return true;
		pthread_mutex_lock(&mcp_mu);
		bool running = mcp_cpu_running_locked();
		pthread_mutex_unlock(&mcp_mu);
		if (!running) {
			call_printf(c, "The VM is paused: the input waits until it runs. ");
			return false;
		}
		if (call_stopped(c) || wall_now() > deadline)
			return false;
		call_progress(c, "sending the input");
		pthread_mutex_lock(&mcp_mu);
		mcp_wait_until(wall_now() + 0.1);
		pthread_mutex_unlock(&mcp_mu);
	}
}

/* "then": wait and look after an input */
typedef struct {
	int wait;
	long delay_ms, stable_ms, timeout_ms, min_px;
	bool shot, aspect, has_region;
	double scale;
	rect region;
} then_t;

static bool parse_shot(call *c, const json *v, bool def, bool *on, rect *region, bool *has_region,
		double *scale, bool *aspect)
{
	*on = def;
	*has_region = false;
	*scale = 1;
	*aspect = false;
	if (!v || v->type == JSON_NULL)
		return true;
	if (v->type == JSON_BOOL) {
		*on = v->b;
		return true;
	}
	if (v->type != JSON_OBJECT) {
		call_error(c, "screenshot must be true, false or an object");
		return false;
	}
	*on = true;
	json *r = json_get(v, "region");
	if (r && r->type != JSON_NULL) {
		if (!get_rect(c, r, "region", region))
			return false;
		*has_region = true;
	}
	return arg_num(c, v, "scale", 1, 0.5, 4, scale) && arg_bool(c, v, "aspect", false, aspect);
}

static bool parse_then(call *c, const json *args, then_t *t)
{
	json *v = json_get(args, "then");
	memset(t, 0, sizeof(*t));
	t->scale = 1;
	if (!v || v->type == JSON_NULL)
		return true;
	if (v->type != JSON_OBJECT) {
		call_error(c, "then must be an object");
		return false;
	}
	return arg_enum(c, v, "wait", wait_names, W_NONE, &t->wait) &&
		arg_int(c, v, "delay-ms", 0, 0, 600000, &t->delay_ms) &&
		arg_int(c, v, "stable-ms", 1000, 0, WAIT_MAX_MS, &t->stable_ms) &&
		arg_int(c, v, "timeout-ms", 10000, 0, WAIT_MAX_MS, &t->timeout_ms) &&
		arg_int(c, v, "min-pixels", 16, 1, SCR_W * SCR_H, &t->min_px) &&
		parse_shot(c, json_get(v, "screenshot"), false, &t->shot, &t->region, &t->has_region,
			&t->scale, &t->aspect);
}

static void sleep_ms(call *c, long ms)
{
	double until = wall_now() + ms / 1000.0;
	pthread_mutex_lock(&mcp_mu);
	while (wall_now() < until && !call_cancelled(c) && !mcp.quitting)
		mcp_wait_until(until);
	pthread_mutex_unlock(&mcp_mu);
}

static void finish_then(call *c, const then_t *t, const frame_t *before)
{
	if (t->delay_ms)
		sleep_ms(c, t->delay_ms);
	if (t->wait != W_NONE) {
		wait_result w = wait_screen_core(c, t->wait, t->stable_ms, t->timeout_ms, t->min_px,
			NULL, NULL, 0, before);
		call_printf(c, " ");
		describe_wait(c, t->wait, &w, t->stable_ms);
	}
	if (t->shot) {
		frame_t *f = malloc(sizeof(*f));
		frame_get(f);
		shot(c, f, t->has_region ? &t->region : NULL, t->scale, t->aspect, false);
		free(f);
	}
}

/* ---- vm-info, vm-power, vm-speed ---- */

typedef struct {
	char floppy[600], hd2[600];
	uint32_t pc, sr;
} info_req;

static void mc_info(void *p)
{
	info_req *r = p;
	const char *f = freebee_floppy_path();
	const char *h = freebee_hd2_path();
	snprintf(r->floppy, sizeof(r->floppy), "%s", f ? f : "");
	snprintf(r->hd2, sizeof(r->hd2), "%s", h ? h : "");
	r->pc = m68k_get_reg(NULL, M68K_REG_PC);
	r->sr = m68k_get_reg(NULL, M68K_REG_SR);
}

static bool tool_vm_info(call *c, const json *a)
{
	(void)a;
	info_req r;
	memset(&r, 0, sizeof(r));
	mcp_main_call(mc_info, &r);
	pthread_mutex_lock(&mcp_mu);
	mcp_state s = mcp;
	bool frozen = rec_frozen_locked();
	pthread_mutex_unlock(&mcp_mu);
	char pty[512] = "";
	const char *link = fbc_get_string("serial", "symlink");
	if (link) {
		ssize_t n = readlink(link, pty, sizeof(pty) - 1);
		pty[n > 0 ? n : 0] = 0;
	}
	(void)frozen;
	const char *run = s.paused_break ? "stopped by the debugger" : s.paused_user ? "paused" :
		rec_freezing() ? "recording with freeze: it runs only during tool calls" : "running";
	double secs = s.ticks / (double)TICK_HZ;
	call_printf(c, "VM %s (FreeBee, AT&T UNIX PC 3B1), pid %d, folder %s\n", mcp_vm_name,
		(int)getpid(), mcp_vm_dir);
	call_printf(c, "State: %s, speed %s, guest time %d:%02d:%02d, PC %06x (%s mode)\n", run,
		control_speed_max ? "max" : "real (10 MHz)", (int)secs / 3600, (int)secs / 60 % 60,
		(int)secs % 60, r.pc & 0xffffff, (r.sr & 0x2000) ? "supervisor" : "user");
	call_printf(c, "Memory: %d KB on the main board, %d KB expansion\n",
		(int)(state.base_ram_size / 1024), (int)(state.exp_ram_size / 1024));
	call_printf(c, "Hard disk 1: %s\nHard disk 2: %s\nFloppy: %s\n",
		fbc_get_string("hard_disk", "disk1"), r.hd2[0] ? r.hd2 : "none",
		r.floppy[0] ? r.floppy : "none (empty drive)");
	call_printf(c, "Serial port tty000: pty %s (link %s); MCP serial tools reach it too\n",
		pty[0] ? pty : "?", link ? link : "?");
	call_printf(c, "Screen: 720x348, one bit%s; LEDs (red, green, yellow, red): %d%d%d%d\n",
		s.frame.rev ? ", reverse video" : "",
		s.leds & 1, (s.leds >> 1) & 1, (s.leds >> 2) & 1, (s.leds >> 3) & 1);
	pthread_mutex_lock(&mouse_mu);
	if (!s.mouse_enabled)
		call_printf(c, "Mouse: the guest has not turned it on\n");
	else if (m_known)
		call_printf(c, "Mouse: on; the pointer is thought to be at %d,%d\n",
			(int)lround(m_x), (int)lround(m_y));
	else
		call_printf(c, "Mouse: on; pointer position not known yet\n");
	pthread_mutex_unlock(&mouse_mu);
	call_printf(c, "Keys waiting: %d. Recording: %s.", keys_pending(), rec_active() ? "yes" : "no");
	return true;
}

static void mc_resume(void *p)
{
	(void)p;
	if (mcp_dbg.n_bp || mcp_dbg.steps) {
		mcp_dbg.skip_pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xffffff;
		mcp_dbg.skip_armed = true;
	}
}

static bool tool_vm_power(call *c, const json *a)
{
	static const char *const acts[] = { "pause", "resume", "reset", "quit", NULL };
	int act;
	if (!arg_enum(c, a, "action", acts, -1, &act))
		return false;
	switch (act) {
	case 0:
		pthread_mutex_lock(&mcp_mu);
		mcp.paused_user = true;
		pthread_cond_broadcast(&mcp_cv);
		pthread_mutex_unlock(&mcp_mu);
		call_printf(c, "Paused. Screenshots, registers and memory still work; vm-power resume goes on.");
		break;
	case 1:
		mcp_main_call(mc_resume, NULL);
		pthread_mutex_lock(&mcp_mu);
		mcp.paused_user = false;
		mcp.paused_break = false;
		pthread_cond_broadcast(&mcp_cv);
		pthread_mutex_unlock(&mcp_mu);
		call_printf(c, "Running.");
		break;
	case 2:
		keys_clear();
		pthread_mutex_lock(&mcp_mu);
		mcp.reset = true;
		mcp.paused_break = false;
		pthread_cond_broadcast(&mcp_cv);
		pthread_mutex_unlock(&mcp_mu);
		pthread_mutex_lock(&mouse_mu);
		m_known = false;
		m_buttons = 0;
		pthread_mutex_unlock(&mouse_mu);
		call_printf(c, "Reset: the machine starts again from its boot ROM.%s",
			fbmcp_paused() ? " It is paused: vm-power resume lets it run." : "");
		break;
	case 3:
		pthread_mutex_lock(&mcp_mu);
		mcp.quit = true;
		pthread_cond_broadcast(&mcp_cv);
		pthread_mutex_unlock(&mcp_mu);
		call_printf(c, "FreeBee is quitting.");
		break;
	}
	return true;
}

static bool tool_vm_speed(call *c, const json *a)
{
	static const char *const sp[] = { "real", "max", NULL };
	int s;
	if (!arg_enum(c, a, "speed", sp, control_speed_max ? 1 : 0, &s))
		return false;
	control_speed_max = s == 1;
	call_printf(c, "Speed: %s.", s ? "max (as fast as the host can)" : "real (10 MHz)");
	return true;
}

/* ---- screenshot, wait-screen ---- */

static bool tool_screenshot(call *c, const json *a)
{
	rect r;
	bool has_r = false, aspect, pointer;
	double scale;
	json *rv = json_get(a, "region");
	if (rv && rv->type != JSON_NULL) {
		if (!get_rect(c, rv, "region", &r))
			return false;
		has_r = true;
	}
	if (!arg_num(c, a, "scale", 1, 0.5, 4, &scale) || !arg_bool(c, a, "aspect", false, &aspect) ||
	    !arg_bool(c, a, "pointer", false, &pointer))
		return false;
	frame_t *f = malloc(sizeof(*f));
	frame_get(f);
	shot(c, f, has_r ? &r : NULL, scale, aspect, pointer);
	pthread_mutex_lock(&mcp_mu);
	bool running = mcp_cpu_running_locked();
	pthread_mutex_unlock(&mcp_mu);
	call_printf(c, "Screen %s%s.", has_r ? "region" : "720x348", running ? "" : " (the VM is paused)");
	free(f);
	return true;
}

static bool tool_wait_screen(call *c, const json *a)
{
	static const char *const untils[] = { "change", "stable", "change-then-stable", NULL };
	int until;
	long stable_ms, timeout_ms, min_px;
	rect region, ign[16];
	bool has_region = false, sh, has_sr, aspect;
	rect sr;
	double scale;
	int n_ign = 0;
	if (!arg_enum(c, a, "until", untils, -1, &until) ||
	    !arg_int(c, a, "stable-ms", 2000, 0, WAIT_MAX_MS, &stable_ms) ||
	    !arg_int(c, a, "timeout-ms", 60000, 0, WAIT_MAX_MS, &timeout_ms) ||
	    !arg_int(c, a, "min-pixels", 16, 1, SCR_W * SCR_H, &min_px) ||
	    !parse_shot(c, json_get(a, "screenshot"), true, &sh, &sr, &has_sr, &scale, &aspect))
		return false;
	until++;	// to W_*
	json *rv = json_get(a, "region");
	if (rv && rv->type != JSON_NULL) {
		if (!get_rect(c, rv, "region", &region))
			return false;
		has_region = true;
	}
	json *iv = json_get(a, "ignore");
	if (iv && iv->type != JSON_NULL) {
		if (iv->type != JSON_ARRAY || iv->n > 16) {
			call_error(c, "ignore must be a list of up to 16 rectangles");
			return false;
		}
		for (int i = 0; i < iv->n; i++)
			if (!get_rect(c, iv->items[i], "ignore", &ign[n_ign++]))
				return false;
	}
	wait_result w = wait_screen_core(c, until, stable_ms, timeout_ms, min_px,
		has_region ? &region : NULL, ign, n_ign, NULL);
	describe_wait(c, until, &w, stable_ms);
	if (sh) {
		frame_t *f = malloc(sizeof(*f));
		frame_get(f);
		shot(c, f, has_sr ? &sr : NULL, scale, aspect, false);
		free(f);
	}
	return true;
}

/* ---- keyboard ---- */

static unsigned rand_seed = 12345;

static int rnd(int lo, int hi)
{
	return lo + (int)(rand_r(&rand_seed) % (unsigned)(hi - lo + 1));
}

static int ms_ticks(long ms)
{
	int t = (int)lround(ms * TICK_HZ / 1000.0);
	return t < 1 ? 1 : t > 255 ? 255 : t;
}

static bool tool_key(call *c, const json *a)
{
	json *keys = json_get(a, "keys");
	long hold_ms, delay_ms;
	then_t t;
	if (!keys || keys->type != JSON_ARRAY || keys->n == 0 || keys->n > 500) {
		call_error(c, "keys must be a list of 1 to 500 keys or combinations");
		return false;
	}
	if (!arg_int(c, a, "hold-ms", 33, 1, 4000, &hold_ms) ||
	    !arg_int(c, a, "delay-ms", 17, 0, 4000, &delay_ms) || !parse_then(c, a, &t))
		return false;
	key_chord *ch = calloc(keys->n, sizeof(*ch));
	int *rep = calloc(keys->n, sizeof(*rep));
	for (int i = 0; i < keys->n; i++) {
		json *k = keys->items[i];
		if (k->type != JSON_STRING || !*k->str) {
			call_error(c, "keys must be strings");
			goto fail;
		}
		char spec[64];
		snprintf(spec, sizeof(spec), "%s", k->str);
		rep[i] = 1;
		char *star = strrchr(spec, '*');
		if (star && star != spec && star[1]) {
			char *end;
			long n = strtol(star + 1, &end, 10);
			if (*end || n < 1 || n > 100) {
				call_error(c, "'%s': a repeat count is *1 to *100", k->str);
				goto fail;
			}
			*star = 0;
			rep[i] = n;
		}
		if (!keys_parse_combo(&ch[i], spec)) {
			call_error(c, "unknown key '%s'. Keys: %s; join a combination with - or +", k->str,
				keys_name_list());
			goto fail;
		}
		ch[i].hold = ms_ticks(hold_ms);
		ch[i].gap = delay_ms ? ms_ticks(delay_ms) : 1;
	}
	frame_t *before = malloc(sizeof(*before));
	frame_get(before);
	uint64_t seq = 0;
	for (int i = 0; i < keys->n; i++) {
		for (int r = 0; r < rep[i]; r++) {
			uint64_t s = keys_push(&ch[i]);
			if (!s) {
				call_error(c, "the key queue is full");
				free(before);
				goto fail;
			}
			seq = s;
		}
	}
	bool ok = wait_delivered(c, seq, 0, false);
	call_printf(c, ok ? "Pressed" : "Queued");
	for (int i = 0; i < keys->n && i < 20; i++)
		call_printf(c, " %s", keys->items[i]->str);
	call_printf(c, keys->n > 20 ? " ...." : ".");
	if (ok)
		finish_then(c, &t, before);
	free(before);
	free(ch);
	free(rep);
	return true;
fail:
	free(ch);
	free(rep);
	return false;
}

static bool tool_type_text(call *c, const json *a)
{
	const char *text;
	long delay_ms;
	bool human;
	then_t t;
	if (!arg_str(c, a, "text", true, &text) ||
	    !arg_int(c, a, "char-delay-ms", 17, 0, 4000, &delay_ms) ||
	    !arg_bool(c, a, "human", rec_active(), &human) || !parse_then(c, a, &t))
		return false;
	size_t n = strlen(text);
	if (n == 0) {
		call_printf(c, "Nothing to type.");
		return true;
	}
	if (n > 100000) {
		call_error(c, "the text is too long (at most 100000 characters)");
		return false;
	}
	key_chord *ch = calloc(n, sizeof(*ch));
	for (size_t i = 0; i < n; i++) {
		unsigned char u = text[i];
		if (u >= 0x80 || !keys_from_char(&ch[i], u)) {
			size_t chno = 1;	// count characters, not UTF-8 bytes
			for (size_t k = 0; k < i; k++)
				chno += ((unsigned char)text[k] & 0xc0) != 0x80;
			if (u >= 0x80)
				call_error(c, "character %zu is not ASCII: the 3B1 keyboard cannot type it; nothing was typed", chno);
			else
				call_error(c, "character %zu (code %d) cannot be typed; nothing was typed", chno, u);
			free(ch);
			return false;
		}
		if (human) {
			ch[i].hold = rnd(2, 4);
			ch[i].gap = rnd(3, 9);
			if (u == ' ' || u == '\n')
				ch[i].gap += rnd(2, 10);
			else if (strchr(".,;:!?", u))
				ch[i].gap += rnd(4, 14);
			if (rnd(0, 40) == 0)
				ch[i].gap += rnd(10, 30);	// a little hesitation
		} else {
			ch[i].hold = 2;
			ch[i].gap = delay_ms ? ms_ticks(delay_ms) : 1;
		}
	}
	frame_t *before = malloc(sizeof(*before));
	frame_get(before);
	uint64_t seq = 0;
	for (size_t i = 0; i < n; i++) {
		uint64_t s = keys_push(&ch[i]);
		if (!s) {
			call_error(c, "the key queue is full");
			free(ch);
			free(before);
			return false;
		}
		seq = s;
	}
	free(ch);
	bool ok = wait_delivered(c, seq, 0, false);
	call_printf(c, "%s %zu character%s%s.", ok ? "Typed" : "Queued", n, n == 1 ? "" : "s",
		human ? " at a person's pace" : "");
	if (ok)
		finish_then(c, &t, before);
	free(before);
	return true;
}

/* ---- mouse ---- */

static const char *const button_names[] = { "left", "middle", "right", NULL };
static const int button_bits[] = { MOUSE_BUTTON_LEFT, MOUSE_BUTTON_MIDDLE, MOUSE_BUTTON_RIGHT };

static bool mouse_on(call *c)
{
	pthread_mutex_lock(&mcp_mu);
	bool on = mcp.mouse_enabled;
	pthread_mutex_unlock(&mcp_mu);
	if (!on)
		call_error(c, "the guest has not turned the mouse on (the Office and other mouse programs do); use the keyboard");
	return on;
}

// move by counts, in steps of at most step counts, one packet a tick
static uint64_t send_counts(int cx, int cy, int step, bool human)
{
	uint64_t s = mcp_mouse_done();
	if (human) {
		double dist = hypot(cx, cy);
		int n = (int)(10 + dist / 10);
		if (n > 45) n = 45;
		int sx = 0, sy = 0;
		for (int i = 1; i <= n; i++) {
			double p = (double)i / n;
			p = p * p * (3 - 2 * p);	// ease in and out
			int tx = (int)lround(cx * p), ty = (int)lround(cy * p);
			if (tx != sx || ty != sy) {
				uint64_t q = mcp_mouse_push(tx - sx, ty - sy, m_buttons, 0);
				if (q) s = q;
			}
			sx = tx;
			sy = ty;
		}
		return s;
	}
	while (cx || cy) {
		int m = abs(cx) > abs(cy) ? abs(cx) : abs(cy);
		int k = m > step ? step : m;
		int dx = (int)lround((double)cx * k / m), dy = (int)lround((double)cy * k / m);
		if (!dx && !dy)
			dx = cx ? (cx > 0 ? 1 : -1) : 0, dy = cy ? (cy > 0 ? 1 : -1) : 0;
		uint64_t q = mcp_mouse_push(dx, dy, m_buttons, 0);
		if (q) s = q;
		cx -= dx;
		cy -= dy;
	}
	return s;
}

static void home_pointer(call *c)
{
	uint64_t s = 0;
	for (int i = 0; i < 10; i++)
		s = mcp_mouse_push(-127, -127, m_buttons, 0);
	wait_delivered(c, 0, s, false);
	wait_ticks(c, 3);
	m_x = m_y = 0;
	m_known = true;
}

// mouse_mu held
static void move_to(call *c, int x, int y, int home, int step, bool human)
{
	if (home == 1 || (home == 0 && !m_known))
		home_pointer(c);
	int cx = (int)lround(x - m_x), cy = (int)lround(y - m_y);
	wait_delivered(c, 0, send_counts(cx, cy, step, human), false);
	wait_ticks(c, 3);	// the guest draws the pointer at the next tick or two
	m_x += cx;
	m_y += cy;
	if (m_x < 0) m_x = 0;
	if (m_y < 0) m_y = 0;
	if (m_x > SCR_W - 1) m_x = SCR_W - 1;
	if (m_y > SCR_H - 1) m_y = SCR_H - 1;
}

static bool get_xy(call *c, const json *a, const char *kx, const char *ky, bool *has, long *x, long *y)
{
	*has = json_has(a, kx) || json_has(a, ky);
	if (!*has)
		return true;
	if (!json_has(a, kx) || !json_has(a, ky)) {
		call_error(c, "give both %s and %s", kx, ky);
		return false;
	}
	return arg_int(c, a, kx, 0, 0, SCR_W - 1, x) && arg_int(c, a, ky, 0, 0, SCR_H - 1, y);
}

static const char *const home_names[] = { "auto", "always", "never", NULL };

static bool tool_mouse_move(call *c, const json *a)
{
	bool has_xy, has_d = json_has(a, "dx") || json_has(a, "dy");
	long x = 0, y = 0, dx, dy, step;
	int home;
	then_t t;
	if (!get_xy(c, a, "x", "y", &has_xy, &x, &y) ||
	    !arg_int(c, a, "dx", 0, -SCR_W, SCR_W, &dx) || !arg_int(c, a, "dy", 0, -SCR_H, SCR_H, &dy) ||
	    !arg_enum(c, a, "home", home_names, 0, &home) ||
	    !arg_int(c, a, "step-px", 8, 1, 127, &step) || !parse_then(c, a, &t))
		return false;
	if (has_xy == has_d) {
		call_error(c, "give x and y, or dx and dy");
		return false;
	}
	if (!mouse_on(c))
		return false;
	frame_t *before = malloc(sizeof(*before));
	frame_get(before);
	pthread_mutex_lock(&mouse_mu);
	bool human = rec_active();
	if (has_xy) {
		move_to(c, x, y, home, step, human);
	} else {
		wait_delivered(c, 0, send_counts(dx, dy, step, human), false);
		wait_ticks(c, 3);
		m_x += dx;
		m_y += dy;
		if (m_x < 0) m_x = 0;
		if (m_y < 0) m_y = 0;
		if (m_x > SCR_W - 1) m_x = SCR_W - 1;
		if (m_y > SCR_H - 1) m_y = SCR_H - 1;
	}
	call_printf(c, "The pointer is at %d,%d.", (int)lround(m_x), (int)lround(m_y));
	pthread_mutex_unlock(&mouse_mu);
	finish_then(c, &t, before);
	free(before);
	return true;
}

static bool tool_mouse_click(call *c, const json *a)
{
	bool has_xy;
	long x = 0, y = 0, count, hold_ms, interval_ms;
	int button, home;
	then_t t;
	if (!get_xy(c, a, "x", "y", &has_xy, &x, &y) ||
	    !arg_enum(c, a, "button", button_names, 0, &button) ||
	    !arg_int(c, a, "count", 1, 1, 3, &count) ||
	    !arg_int(c, a, "hold-ms", 80, 1, 4000, &hold_ms) ||
	    !arg_int(c, a, "interval-ms", 120, 1, 4000, &interval_ms) ||
	    !arg_enum(c, a, "home", home_names, 0, &home) || !parse_then(c, a, &t))
		return false;
	if (!mouse_on(c))
		return false;
	frame_t *before = malloc(sizeof(*before));
	frame_get(before);
	pthread_mutex_lock(&mouse_mu);
	if (has_xy)
		move_to(c, x, y, home, 8, rec_active());
	uint64_t s = 0;
	for (int i = 0; i < count; i++) {
		mcp_mouse_push(0, 0, m_buttons | button_bits[button], ms_ticks(hold_ms));
		s = mcp_mouse_push(0, 0, m_buttons, i + 1 < count ? ms_ticks(interval_ms) : 1);
	}
	wait_delivered(c, 0, s, false);
	call_printf(c, "Clicked %s%s at %d,%d.", button_names[button], count == 2 ? " twice" :
		count == 3 ? " three times" : "", (int)lround(m_x), (int)lround(m_y));
	pthread_mutex_unlock(&mouse_mu);
	finish_then(c, &t, before);
	free(before);
	return true;
}

static bool tool_mouse_button(call *c, const json *a)
{
	static const char *const acts[] = { "down", "up", NULL };
	int act, button;
	then_t t;
	if (!arg_enum(c, a, "action", acts, -1, &act) ||
	    !arg_enum(c, a, "button", button_names, 0, &button) || !parse_then(c, a, &t))
		return false;
	if (!mouse_on(c))
		return false;
	frame_t *before = malloc(sizeof(*before));
	frame_get(before);
	pthread_mutex_lock(&mouse_mu);
	if (act == 0)
		m_buttons |= button_bits[button];
	else
		m_buttons &= ~button_bits[button];
	wait_delivered(c, 0, mcp_mouse_push(0, 0, m_buttons, 2), false);
	pthread_mutex_unlock(&mouse_mu);
	call_printf(c, "The %s button is %s.", button_names[button], act ? "up" : "down");
	finish_then(c, &t, before);
	free(before);
	return true;
}

static bool tool_mouse_drag(call *c, const json *a)
{
	bool h1, h2;
	long x, y, tx, ty;
	int button;
	then_t t;
	if (!get_xy(c, a, "x", "y", &h1, &x, &y) || !get_xy(c, a, "to-x", "to-y", &h2, &tx, &ty) ||
	    !arg_enum(c, a, "button", button_names, 0, &button) || !parse_then(c, a, &t))
		return false;
	if (!h1 || !h2) {
		call_error(c, "give x, y, to-x and to-y");
		return false;
	}
	if (!mouse_on(c))
		return false;
	frame_t *before = malloc(sizeof(*before));
	frame_get(before);
	pthread_mutex_lock(&mouse_mu);
	bool human = rec_active();
	move_to(c, x, y, 0, 8, human);
	m_buttons |= button_bits[button];
	wait_delivered(c, 0, mcp_mouse_push(0, 0, m_buttons, 4), false);
	move_to(c, tx, ty, 2, 4, human);
	m_buttons &= ~button_bits[button];
	wait_delivered(c, 0, mcp_mouse_push(0, 0, m_buttons, 2), false);
	call_printf(c, "Dragged from %ld,%ld to %d,%d.", x, y, (int)lround(m_x), (int)lround(m_y));
	pthread_mutex_unlock(&mouse_mu);
	finish_then(c, &t, before);
	free(before);
	return true;
}

// The changed areas between two frames: pixels with gaps of up to 2
// pixels between them make one area. Returns how many (at most max).
static int diff_blobs(const frame_t *a, const frame_t *b, rect *out, int max)
{
	uint8_t *m = calloc(SCR_W * SCR_H, 1);
	int *stack = malloc(sizeof(int) * SCR_W * SCR_H);
	int n = 0;
	for (int y = 0; y < SCR_H; y++)
		for (int x = 0; x < SCR_W; x++)
			m[y * SCR_W + x] = frame_pixel(a, x, y) != frame_pixel(b, x, y);
	for (int p0 = 0; p0 < SCR_W * SCR_H && n < max; p0++) {
		if (m[p0] != 1)
			continue;
		int sp = 0, x0 = SCR_W, y0 = SCR_H, x1 = -1, y1 = -1;
		stack[sp++] = p0;
		m[p0] = 2;
		while (sp) {
			int p = stack[--sp], px = p % SCR_W, py = p / SCR_W;
			if (px < x0) x0 = px;
			if (px > x1) x1 = px;
			if (py < y0) y0 = py;
			if (py > y1) y1 = py;
			for (int dy = -2; dy <= 2; dy++) {
				for (int dx = -2; dx <= 2; dx++) {
					int qx = px + dx, qy = py + dy;
					if (qx < 0 || qy < 0 || qx >= SCR_W || qy >= SCR_H)
						continue;
					int q = qy * SCR_W + qx;
					if (m[q] == 1) {
						m[q] = 2;
						stack[sp++] = q;
					}
				}
			}
		}
		out[n++] = (rect){ x0, y0, x1 - x0 + 1, y1 - y0 + 1 };
	}
	free(m);
	free(stack);
	return n;
}

static bool tool_pointer_locate(call *c, const json *a)
{
	(void)a;
	if (!mouse_on(c))
		return false;
	frame_t *A = malloc(sizeof(*A)), *B = malloc(sizeof(*B)), *C = malloc(sizeof(*C));
	rect blobs[64];
	pthread_mutex_lock(&mouse_mu);
	// a short move sideways: a menu line under the pointer stays lit
	int w = m_known && m_x > SCR_W - 40 ? -8 : 8;
	// each picture once the guest has redrawn (the Office lights the menu
	// line under the pointer a moment after it moves)
	wait_screen_core(c, W_STABLE, 300, 3000, 1, NULL, NULL, 0, NULL);
	frame_get(A);
	wait_delivered(c, 0, send_counts(w, 0, 8, false), false);
	wait_screen_core(c, W_STABLE, 300, 3000, 1, NULL, NULL, 0, NULL);
	frame_get(B);
	wait_delivered(c, 0, send_counts(-w, 0, 8, false), false);
	wait_screen_core(c, W_STABLE, 300, 3000, 1, NULL, NULL, 0, NULL);
	frame_get(C);
	int n = diff_blobs(A, B, blobs, 64), best = -1;
	double best_d = 1e9;
	for (int i = 0; i < n; i++) {
		// the pointer, drawn twice 8 pixels apart: small
		if (blobs[i].w > 48 || blobs[i].h > 40 || blobs[i].w < 9)
			continue;
		double d = m_known ? hypot(blobs[i].x - m_x, blobs[i].y - m_y) : blobs[i].w * blobs[i].h;
		if (d < best_d) {
			best_d = d;
			best = i;
		}
	}
	if (best >= 0) {
		rect *r = &blobs[best];
		// the arrow's tip is its top left corner
		m_x = r->x - (w < 0 ? w : 0);
		m_y = r->y;
		m_known = true;
		call_printf(c, "The pointer's tip is at %d,%d (the red cross).", (int)m_x, (int)m_y);
	} else if (n) {
		call_printf(c, "The pointer was not found among %d changed areas (", n);
		for (int i = 0; i < n && i < 8; i++)
			call_printf(c, "%s%d,%d %dx%d", i ? "; " : "", blobs[i].x, blobs[i].y, blobs[i].w, blobs[i].h);
		call_printf(c, "); try again when the screen is still.");
		c->unmet = true;
	} else {
		call_printf(c, "Nothing moved on the screen: no pointer is drawn here.");
		c->unmet = true;
	}
	pthread_mutex_unlock(&mouse_mu);
	shot(c, C, NULL, 1, false, best >= 0);
	free(A);
	free(B);
	free(C);
	return true;
}

/* ---- serial ---- */

static void sb_visible(sbuf *b, const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		unsigned char ch = s[i];
		if (ch == '\r' || ch == 0x11 || ch == 0x13) {
			continue;	// and XON/XOFF: flow control, not output
		} else if (ch == '\n' || ch == '\t') {
			sb_putc(b, ch);
		} else if (ch == 0x1b) {
			sb_puts(b, "^[");
		} else if (ch < 0x20 || ch == 0x7f) {
			sb_printf(b, "^%c", ch == 0x7f ? '?' : ch + '@');
		} else if (ch >= 0x80) {
			sb_printf(b, "\\x%02x", ch);
		} else {
			sb_putc(b, ch);
		}
	}
}

static bool tool_serial_write(call *c, const json *a)
{
	const char *text;
	bool raw;
	if (!arg_str(c, a, "text", true, &text) || !arg_bool(c, a, "raw", false, &raw))
		return false;
	size_t n = strlen(text);
	char *buf = malloc(n + 1);
	for (size_t i = 0; i < n; i++)
		buf[i] = (!raw && text[i] == '\n') ? '\r' : text[i];
	bool ok = mcp_serial_put(buf, n);
	free(buf);
	if (!ok) {
		call_error(c, "the serial input queue is full; the guest is not reading tty000");
		return false;
	}
	if (wait_delivered(c, 0, 0, true))
		call_printf(c, "Sent %zu bytes to tty000.", n);
	else
		call_printf(c, "%zu bytes wait to be read on tty000.", n);
	return true;
}

static bool tool_serial_read(call *c, const json *a)
{
	bool all;
	long max;
	if (!arg_bool(c, a, "all", false, &all) || !arg_int(c, a, "max-bytes", 16384, 1, 262144, &max))
		return false;
	pthread_mutex_lock(&mcp_mu);
	uint64_t from = mcp_serial_cursor;
	pthread_mutex_unlock(&mcp_mu);
	uint64_t total = mcp_serial_total();
	if (all || total - from > (uint64_t)max)
		from = total > (uint64_t)max ? total - max : 0;
	char *buf = malloc(max);
	uint64_t start;
	size_t n = mcp_serial_get(from, buf, max, &start);
	pthread_mutex_lock(&mcp_mu);
	if (mcp_serial_cursor < start + n)
		mcp_serial_cursor = start + n;
	pthread_mutex_unlock(&mcp_mu);
	if (n == 0) {
		call_printf(c, "(no new output on tty000)");
	} else {
		sb_visible(&c->text, buf, n);
	}
	free(buf);
	return true;
}

// find text (or regex) in the output after the cursor; wait for it
static bool serial_wait_for(call *c, const char *text, bool is_regex, long timeout_ms,
		uint64_t from, sbuf *out, uint64_t *match_end, char *sub, size_t subsz)
{
	regex_t re;
	if (is_regex) {
		int e = regcomp(&re, text, REG_EXTENDED);
		if (e) {
			char msg[200];
			regerror(e, &re, msg, sizeof(msg));
			call_error(c, "bad regular expression: %s", msg);
			return false;
		}
	}
	double deadline = wall_now() + timeout_ms / 1000.0;
	char *buf = malloc(SER_RING + 1);
	bool found = false;
	size_t tl = strlen(text);
	uint64_t seen = (uint64_t)-1;
	for (;;) {
		uint64_t total = mcp_serial_total();
		if (total == seen) {
			if (wall_now() >= deadline || call_stopped(c)) {
				uint64_t start;
				size_t n = mcp_serial_get(from, buf, SER_RING, &start);
				sb_add(out, buf, n);
				*match_end = start + n;
				break;
			}
			pthread_mutex_lock(&mcp_mu);
			mcp_wait_until(deadline);
			pthread_mutex_unlock(&mcp_mu);
			continue;
		}
		seen = total;
		uint64_t start;
		size_t n = mcp_serial_get(from, buf, SER_RING, &start);
		for (size_t i = 0; i < n; i++)
			if (!buf[i])
				buf[i] = ' ';
		buf[n] = 0;
		size_t end = 0;
		if (is_regex) {
			regmatch_t m[2];
			if (n && regexec(&re, buf, 2, m, 0) == 0) {
				found = true;
				end = m[0].rm_eo;
				if (sub && m[1].rm_so >= 0) {
					size_t l = m[1].rm_eo - m[1].rm_so;
					if (l >= subsz) l = subsz - 1;
					memcpy(sub, buf + m[1].rm_so, l);
					sub[l] = 0;
				}
			}
		} else if (tl) {
			char *p = n >= tl ? strstr(buf, text) : NULL;
			if (p) {
				found = true;
				end = p - buf + tl;
			}
		}
		if (found) {
			sb_add(out, buf, end);
			*match_end = start + end;
			break;
		}
		if (wall_now() >= deadline || call_stopped(c)) {
			sb_add(out, buf, n);
			*match_end = start + n;
			break;
		}
		call_progress(c, "waiting for serial output");
		pthread_mutex_lock(&mcp_mu);
		mcp_wait_until(deadline);
		pthread_mutex_unlock(&mcp_mu);
	}
	free(buf);
	if (is_regex)
		regfree(&re);
	return found;
}

static bool tool_serial_expect(call *c, const json *a)
{
	const char *text;
	bool is_regex;
	long timeout_ms;
	if (!arg_str(c, a, "text", true, &text) || !arg_bool(c, a, "regex", false, &is_regex) ||
	    !arg_int(c, a, "timeout-ms", 30000, 0, WAIT_MAX_MS, &timeout_ms))
		return false;
	pthread_mutex_lock(&mcp_mu);
	uint64_t from = mcp_serial_cursor;
	pthread_mutex_unlock(&mcp_mu);
	sbuf out;
	sb_init(&out);
	uint64_t end;
	double t0 = wall_now();
	if (!is_regex && !*text) {
		call_error(c, "text is empty");
		sb_free(&out);
		return false;
	}
	bool found = serial_wait_for(c, text, is_regex, timeout_ms, from, &out, &end, NULL, 0);
	if (c->is_error) {
		sb_free(&out);
		return false;
	}
	if (found) {
		pthread_mutex_lock(&mcp_mu);
		mcp_serial_cursor = end;
		pthread_mutex_unlock(&mcp_mu);
		call_printf(c, "Found after %.1f s. Output up to the match:\n", wall_now() - t0);
	} else {
		c->unmet = true;
		call_printf(c, "Not found in %.1f s. The output since the last read:\n", wall_now() - t0);
	}
	sb_visible(&c->text, out.s ? out.s : "", out.len);
	sb_free(&out);
	return true;
}

// queue text for tty000 and wait until the guest has it
static bool ser_send(call *c, const char *text)
{
	if (!mcp_serial_put(text, strlen(text))) {
		call_error(c, "the serial input queue is full; the guest is not reading tty000");
		return false;
	}
	wait_delivered(c, 0, 0, true);
	return true;
}

static bool tool_serial_run(call *c, const json *a)
{
	const char *cmd;
	long timeout_ms;
	if (!arg_str(c, a, "command", true, &cmd) ||
	    !arg_int(c, a, "timeout-ms", 60000, 1000, WAIT_MAX_MS, &timeout_ms))
		return false;
	size_t n = strlen(cmd);
	while (n && (cmd[n - 1] == '\n' || cmd[n - 1] == ' '))
		n--;
	if (!n) {
		call_error(c, "command is empty");
		return false;
	}
	static const char marker_typed[] = "echo \"<<FB\"\"END $?>>\"";
	uint64_t start = mcp_serial_total();
	double t0 = wall_now();
	// The tty's line editing eats # and @ (System V's erase and kill keys
	// until stty changes them), takes lines of at most 256 bytes, and drops
	// input when more than that waits. So a command with # or @, or a long
	// one, goes as a script: one line starts cat, stores the script, puts
	// back newline, # and @ (sent as ^A, ^B and ^E) and runs it; then the
	// script comes in short lines, with XON/XOFF flow control and no echo.
	bool script = n > 200 || memchr(cmd, '\n', n) || memchr(cmd, '#', n) || memchr(cmd, '@', n);
	sbuf in;
	sb_init(&in);
	if (script) {
		sb_puts(&in, "stty ixoff -echo; cat > /tmp/fbmcp.raw; stty echo; tr -d '\\012' < /tmp/fbmcp.raw"
			" | tr '\\001\\002\\005' '\\012\\043\\100' > /tmp/fbmcp.sh; echo >> /tmp/fbmcp.sh;"
			" . /tmp/fbmcp.sh; ");
		sb_puts(&in, marker_typed);
		sb_putc(&in, '\r');
		if (!ser_send(c, in.s))
			goto fail;
		wait_ticks(c, 30);	// the shell starts cat before the lines come
		for (size_t i = 0; i < n; i += 100) {
			in.len = 0;
			for (size_t k = i; k < n && k < i + 100; k++) {
				char ch = cmd[k];
				sb_putc(&in, ch == '\n' ? 1 : ch == '#' ? 2 : ch == '@' ? 5 : ch);
			}
			sb_putc(&in, '\r');
			if (!ser_send(c, in.s))
				goto fail;
			wait_ticks(c, 2);
			call_progress(c, "sending the script");
		}
		if (!ser_send(c, "\004"))
			goto fail;
		sb_free(&in);
	} else {
		sb_add(&in, cmd, n);
		sb_puts(&in, cmd[n - 1] == '&' ? " " : "; ");
		sb_puts(&in, marker_typed);
		sb_putc(&in, '\r');
		if (!ser_send(c, in.s))
			goto fail;
		sb_free(&in);
	}

	sbuf out;
	sb_init(&out);
	uint64_t end;
	char status[16] = "";
	bool found = serial_wait_for(c, "<<FBEND ([0-9]+)>>", true, timeout_ms, start, &out, &end,
		status, sizeof(status));
	pthread_mutex_lock(&mcp_mu);
	mcp_serial_cursor = end;
	pthread_mutex_unlock(&mcp_mu);
	// the output starts after the echo of the marker command, if the tty echoes
	const char *s = out.s ? out.s : "";
	size_t len = out.len;
	const char *p = s, *q;
	while ((q = memmem(p, s + len - p, "<<FB\"\"END", 9)) != NULL) {
		const char *nl = memchr(q, '\n', s + len - q);
		p = nl ? nl + 1 : s + len;
	}
	const char *stop = s + len;
	if (found) {
		const char *m = NULL, *r = p;
		while ((q = memmem(r, s + len - r, "<<FBEND ", 8)) != NULL) {
			m = q;
			r = q + 8;
		}
		if (m)
			stop = m;
	}
	if (found) {
		call_printf(c, "Exit status %s after %.1f s. Output:\n", status, wall_now() - t0);
	} else {
		c->unmet = true;
		call_printf(c, "Still running after %.1f s (or the shell did not answer; is a shell logged"
			" in on tty000?). Output so far:\n", wall_now() - t0);
	}
	size_t olen = stop - p;
	if (olen > 65536) {
		call_printf(c, "[... %zu bytes left out ...]\n", olen - 65536);
		p = stop - 65536;
		olen = 65536;
	}
	sb_visible(&c->text, p, olen);
	sb_free(&out);
	return true;
fail:
	sb_free(&in);
	return false;
}

/* ---- media ---- */

typedef struct {
	int action, drive;
	const char *file;
	bool ok;
	char floppy[600], hd2[600];
} media_req;

static void mc_media(void *p)
{
	media_req *r = p;
	if (r->action == 1) {
		if (r->drive == 0) {
			freebee_floppy_eject();
			r->ok = freebee_floppy_insert(r->file);
		} else {
			r->ok = freebee_hd2_insert(r->file);
		}
	} else if (r->action == 2) {
		freebee_floppy_eject();
		r->ok = true;
	}
	const char *f = freebee_floppy_path(), *h = freebee_hd2_path();
	snprintf(r->floppy, sizeof(r->floppy), "%s", f ? f : "");
	snprintf(r->hd2, sizeof(r->hd2), "%s", h ? h : "");
}

static bool tool_media(call *c, const json *a)
{
	static const char *const acts[] = { "status", "insert", "eject", NULL };
	static const char *const drives[] = { "floppy", "hd2", NULL };
	media_req r;
	memset(&r, 0, sizeof(r));
	if (!arg_enum(c, a, "action", acts, -1, &r.action) ||
	    !arg_enum(c, a, "drive", drives, 0, &r.drive) || !arg_str(c, a, "file", r.action == 1, &r.file))
		return false;
	if (r.action == 2 && r.drive == 1) {
		call_error(c, "the second hard disk cannot be ejected; insert another image instead");
		return false;
	}
	if (r.action == 1) {
		struct stat st;
		if (stat(r.file, &st) != 0 || !S_ISREG(st.st_mode)) {
			call_error(c, "%s: %s (paths are relative to %s)", r.file,
				errno ? strerror(errno) : "not a file", mcp_vm_dir);
			return false;
		}
	}
	mcp_main_call(mc_media, &r);
	if (r.action == 1 && !r.ok) {
		call_error(c, "FreeBee could not use %s: %s", r.file, r.drive == 0 ?
			"its size is not a floppy geometry FreeBee knows" :
			"it is not a disk image FreeBee can open for writing");
		return false;
	}
	if (r.action == 1)
		call_printf(c, "Inserted %s. ", r.file);
	else if (r.action == 2)
		call_printf(c, "Ejected. ");
	call_printf(c, "Floppy drive: %s. Hard disk 2: %s.", r.floppy[0] ? r.floppy : "empty",
		r.hd2[0] ? r.hd2 : "none");
	return true;
}

/* ---- sound ---- */

static uint64_t snd_tick;
static bool snd_on;

static void describe_tones(call *c, uint64_t since, int *n_sounds, bool *playing, uint64_t *last_off)
{
	tone_event ev[256];
	int n = mcp_tone_events(since, ev, 256);
	int count = 0;
	*playing = false;
	*last_off = 0;
	for (int i = 0; i < n; i++) {
		if (!ev[i].on)
			continue;
		uint64_t end = 0;
		for (int k = i + 1; k < n; k++) {
			if (!ev[k].on || ev[k].lo != ev[i].lo || ev[k].hi != ev[i].hi) {
				end = ev[k].tick;
				break;
			}
		}
		count++;
		if (c && count <= 50) {
			double at = (ev[i].tick - since) / (double)TICK_HZ;
			if (end)
				call_printf(c, "\n  at %.2f s: %.2f s, %d + %d Hz", at,
					(end - ev[i].tick) / (double)TICK_HZ, ev[i].lo, ev[i].hi);
			else
				call_printf(c, "\n  at %.2f s: still playing, %d + %d Hz", at, ev[i].lo, ev[i].hi);
		}
		if (!end)
			*playing = true;
		else
			*last_off = end;
	}
	*n_sounds = count;
}

static bool tool_sound(call *c, const json *a)
{
	static const char *const acts[] = { "start", "wait", "stop", NULL };
	int act;
	long timeout_ms, quiet_ms;
	if (!arg_enum(c, a, "action", acts, -1, &act) ||
	    !arg_int(c, a, "timeout-ms", 30000, 0, WAIT_MAX_MS, &timeout_ms) ||
	    !arg_int(c, a, "quiet-ms", 500, 0, 60000, &quiet_ms))
		return false;
	if (act == 0) {
		pthread_mutex_lock(&mcp_mu);
		snd_tick = mcp.ticks;
		snd_on = true;
		pthread_mutex_unlock(&mcp_mu);
		call_printf(c, "Listening to the speaker (beeper and dialer tones).");
		return true;
	}
	if (!snd_on) {
		call_error(c, "call sound start first");
		return false;
	}
	int n;
	bool playing;
	uint64_t last_off;
	if (act == 1) {
		double deadline = wall_now() + timeout_ms / 1000.0;
		for (;;) {
			describe_tones(NULL, snd_tick, &n, &playing, &last_off);
			pthread_mutex_lock(&mcp_mu);
			uint64_t now_tick = mcp.ticks;
			pthread_mutex_unlock(&mcp_mu);
			if (n && !playing && (now_tick - last_off) * 1000 / TICK_HZ >= (uint64_t)quiet_ms)
				break;
			if (wall_now() >= deadline || call_stopped(c))
				break;
			call_progress(c, "listening");
			pthread_mutex_lock(&mcp_mu);
			mcp_wait_until(deadline);
			pthread_mutex_unlock(&mcp_mu);
		}
	}
	call_printf(c, "Heard:");
	describe_tones(c, snd_tick, &n, &playing, &last_off);
	if (!n) {
		call_printf(c, " nothing.");
		if (act == 1)
			c->unmet = true;
	}
	if (act == 2)
		snd_on = false;
	return true;
}

/* ---- debugger ---- */

static uint32_t map_virt(uint32_t addr, bool *present)
{
	uint16_t page = (addr >> 12) & 0x3ff;
	uint16_t e = (state.map[page * 2] << 8) | state.map[page * 2 + 1];
	*present = ((state.map[page * 2] >> 5) & 3) != 0;
	return ((uint32_t)(e & 0x3ff) << 12) | (addr & 0xfff);
}

// main thread: a pointer to the byte, or NULL
static uint8_t *mem_byte(uint32_t addr, bool phys, bool for_write)
{
	addr &= 0xffffff;
	if (!phys && !state.romlmap && !for_write)
		addr |= 0x800000;	// before boot, the CPU sees the ROM everywhere
	if (!phys) {
		if (addr >= 0x800000 && addr <= 0xbfffff) {
			return for_write ? NULL : &state.rom[addr & (ROM_SIZE - 1)];
		} else if (addr < 0x400000) {
			bool present;
			addr = map_virt(addr, &present);
			if (!present)
				return NULL;
		} else if (addr >= 0x420000 && addr < 0x428000) {
			return &state.vram[addr & 0x7fff];
		} else if (addr >= 0x400000 && addr < 0x400800) {
			return &state.map[addr & 0x7ff];
		} else {
			return NULL;	// I/O: reading it would do something
		}
	}
	if (addr < 0x200000)
		return addr < state.base_ram_size ? &state.base_ram[addr] : NULL;
	if (addr - 0x200000 < state.exp_ram_size)
		return &state.exp_ram[addr - 0x200000];
	return NULL;
}

typedef struct {
	uint32_t addr;
	bool phys, write, check;	// check: only see whether it is RAM
	int len;
	uint8_t *data;
	bool *ok;
	int bad;
} mem_req;

static void mc_mem(void *p)
{
	mem_req *r = p;
	for (int i = 0; i < r->len; i++) {
		uint8_t *b = mem_byte(r->addr + i, r->phys, r->write || r->check);
		r->ok[i] = b != NULL;
		if (!b) {
			r->bad++;
			continue;
		}
		if (r->check)
			continue;
		if (r->write)
			*b = r->data[i];
		else
			r->data[i] = *b;
	}
	if (r->write && !r->check)
		state.vram_updated = true;
}

static bool tool_memory_read(call *c, const json *a)
{
	const char *as;
	long len;
	bool phys, str;
	uint32_t addr;
	if (!arg_str(c, a, "address", true, &as) || !parse_addr(c, as, &addr) ||
	    !arg_int(c, a, "length", 128, 1, 4096, &len) || !arg_bool(c, a, "physical", false, &phys) ||
	    !arg_bool(c, a, "string", false, &str))
		return false;
	if (str && !json_has(a, "length"))
		len = 256;
	mem_req r = { addr, phys, false, false, (int)len, calloc(len, 1), calloc(len, sizeof(bool)), 0 };
	mcp_main_call(mc_mem, &r);
	if (r.bad == len) {
		call_error(c, "nothing readable at 0x%06x%s", addr, phys ? " (physical)" :
			" (a page that is not present, or I/O)");
	} else if (str) {
		call_printf(c, "0x%06x: \"", addr);
		int i;
		for (i = 0; i < len && r.ok[i] && r.data[i]; i++) {
			if (r.data[i] == '"' || r.data[i] == '\\')
				call_printf(c, "\\%c", r.data[i]);
			else if (r.data[i] >= 0x20 && r.data[i] < 0x7f)
				call_printf(c, "%c", r.data[i]);
			else
				call_printf(c, "\\x%02x", r.data[i]);
		}
		call_printf(c, "\"%s", i == len ? " (no zero byte yet)" : "");
	} else {
		for (int i = 0; i < len; i += 16) {
			call_printf(c, "%06x: ", (addr + i) & 0xffffff);
			for (int k = 0; k < 16; k++) {
				if (i + k < len && r.ok[i + k])
					call_printf(c, "%02x", r.data[i + k]);
				else
					call_printf(c, i + k < len ? "--" : "  ");
				if (k & 1)
					call_printf(c, " ");
			}
			call_printf(c, " ");
			for (int k = 0; k < 16 && i + k < len; k++) {
				uint8_t b = r.data[i + k];
				call_printf(c, "%c", r.ok[i + k] && b >= 0x20 && b < 0x7f ? b : '.');
			}
			call_printf(c, "\n");
		}
		if (r.bad)
			call_printf(c, "(-- : not readable: a page that is not present, or I/O)");
	}
	free(r.data);
	free(r.ok);
	return true;
}

static int hexv(char ch)
{
	if (ch >= '0' && ch <= '9') return ch - '0';
	if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
	if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
	return -1;
}

static bool tool_memory_write(call *c, const json *a)
{
	const char *as, *hex, *text;
	bool phys;
	uint32_t addr;
	if (!arg_str(c, a, "address", true, &as) || !parse_addr(c, as, &addr) ||
	    !arg_str(c, a, "hex", false, &hex) || !arg_str(c, a, "text", false, &text) ||
	    !arg_bool(c, a, "physical", false, &phys))
		return false;
	if (!!hex == !!text) {
		call_error(c, "give hex or text");
		return false;
	}
	uint8_t *data = malloc(4096);
	int len = 0;
	if (hex) {
		int hi = -1;
		for (const char *p = hex; *p; p++) {
			if (*p == ' ' || *p == ',' || *p == ':')
				continue;
			if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && hi < 0) {
				p++;
				continue;
			}
			int v = hexv(*p);
			if (v < 0 || len >= 4096) {
				call_error(c, v < 0 ? "hex has a character that is not a hex digit" : "at most 4096 bytes");
				free(data);
				return false;
			}
			if (hi < 0) {
				hi = v;
			} else {
				data[len++] = hi << 4 | v;
				hi = -1;
			}
		}
		if (hi >= 0) {
			call_error(c, "hex has an odd number of digits");
			free(data);
			return false;
		}
	} else {
		len = strlen(text);
		if (len > 4096) {
			call_error(c, "at most 4096 bytes");
			free(data);
			return false;
		}
		memcpy(data, text, len);
	}
	if (!len) {
		call_error(c, "nothing to write");
		free(data);
		return false;
	}
	bool *ok = calloc(len, sizeof(bool));
	// check first: write only if all of it is RAM
	mem_req chk = { addr, phys, false, true, len, NULL, ok, 0 };
	mcp_main_call(mc_mem, &chk);
	for (int i = 0; i < len; i++) {
		if (!ok[i]) {
			call_error(c, "0x%06x is not RAM (or its page is not present): nothing written", addr + i);
			free(ok);
			free(data);
			return false;
		}
	}
	mem_req r = { addr, phys, true, false, len, data, ok, 0 };
	mcp_main_call(mc_mem, &r);
	call_printf(c, "Wrote %d bytes at 0x%06x%s.", len, addr, phys ? " (physical)" : "");
	free(ok);
	free(data);
	return true;
}

typedef struct {
	uint32_t addr;
	int count;
	sbuf *out;
} dis_req;

static void dis_lines(sbuf *out, uint32_t pc, int count)
{
	char buf[256];
	for (int i = 0; i < count; i++) {
		unsigned n = m68k_disassemble(buf, pc, M68K_CPU_TYPE_68010);
		if (n == 0)
			n = 2;
		sb_printf(out, "%06x  ", pc & 0xffffff);
		for (unsigned k = 0; k < 10; k += 2) {
			if (k < n)
				sb_printf(out, "%04x ", m68k_read_disassembler_16(pc + k) & 0xffff);
			else
				sb_puts(out, "     ");
		}
		sb_printf(out, " %s\n", buf);
		pc += n;
	}
}

static void mc_dis(void *p)
{
	dis_req *r = p;
	dis_lines(r->out, r->addr, r->count);
}

static void regs_text(sbuf *o, int ninstr)
{
	uint32_t sr = m68k_get_reg(NULL, M68K_REG_SR);
	uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
	sb_printf(o, "PC %06x  SR %04x (%s, T=%d, I=%d, X%d N%d Z%d V%d C%d)\n", pc & 0xffffff, sr,
		sr & 0x2000 ? "supervisor" : "user", (sr >> 15) & 1, (sr >> 8) & 7,
		(sr >> 4) & 1, (sr >> 3) & 1, (sr >> 2) & 1, (sr >> 1) & 1, sr & 1);
	for (int i = 0; i < 8; i++)
		sb_printf(o, "D%d %08x%s", i, m68k_get_reg(NULL, M68K_REG_D0 + i), i == 3 || i == 7 ? "\n" : "  ");
	for (int i = 0; i < 8; i++)
		sb_printf(o, "A%d %08x%s", i, m68k_get_reg(NULL, M68K_REG_A0 + i), i == 3 || i == 7 ? "\n" : "  ");
	sb_printf(o, "USP %08x  SSP %08x  VBR %08x  SFC %d  DFC %d\n",
		m68k_get_reg(NULL, M68K_REG_USP), m68k_get_reg(NULL, M68K_REG_ISP),
		m68k_get_reg(NULL, M68K_REG_VBR), m68k_get_reg(NULL, M68K_REG_SFC),
		m68k_get_reg(NULL, M68K_REG_DFC));
	if (ninstr)
		dis_lines(o, pc, ninstr);
}

typedef struct {
	sbuf *out;
	int n;
} regs_req;

static void mc_regs(void *p)
{
	regs_req *r = p;
	regs_text(r->out, r->n);
}

static bool tool_debug_registers(call *c, const json *a)
{
	long n;
	if (!arg_int(c, a, "instructions", 4, 0, 32, &n))
		return false;
	regs_req r = { &c->text, (int)n };
	pthread_mutex_lock(&mcp_mu);
	bool running = mcp_cpu_running_locked();
	pthread_mutex_unlock(&mcp_mu);
	if (running)
		call_printf(c, "(the CPU runs: a snapshot)\n");
	mcp_main_call(mc_regs, &r);
	return true;
}

static const struct { const char *name; int reg; } reg_names[] = {
	{ "d0", M68K_REG_D0 }, { "d1", M68K_REG_D1 }, { "d2", M68K_REG_D2 }, { "d3", M68K_REG_D3 },
	{ "d4", M68K_REG_D4 }, { "d5", M68K_REG_D5 }, { "d6", M68K_REG_D6 }, { "d7", M68K_REG_D7 },
	{ "a0", M68K_REG_A0 }, { "a1", M68K_REG_A1 }, { "a2", M68K_REG_A2 }, { "a3", M68K_REG_A3 },
	{ "a4", M68K_REG_A4 }, { "a5", M68K_REG_A5 }, { "a6", M68K_REG_A6 }, { "a7", M68K_REG_A7 },
	{ "sp", M68K_REG_A7 }, { "pc", M68K_REG_PC }, { "sr", M68K_REG_SR }, { "usp", M68K_REG_USP },
	{ "ssp", M68K_REG_ISP }, { "isp", M68K_REG_ISP }, { "vbr", M68K_REG_VBR },
	{ "sfc", M68K_REG_SFC }, { "dfc", M68K_REG_DFC }, { NULL, 0 }
};

typedef struct {
	int reg;
	uint32_t value;
} regw_req;

static void mc_regw(void *p)
{
	regw_req *r = p;
	m68k_set_reg(r->reg, r->value);
}

static bool tool_register_write(call *c, const json *a)
{
	const char *name, *vs;
	uint32_t v;
	if (!arg_str(c, a, "register", true, &name) || !arg_str(c, a, "value", true, &vs) ||
	    !parse_addr(c, vs, &v))
		return false;
	for (int i = 0; reg_names[i].name; i++) {
		if (strcasecmp(reg_names[i].name, name) == 0) {
			regw_req r = { reg_names[i].reg, v };
			mcp_main_call(mc_regw, &r);
			call_printf(c, "%s = %08x.", reg_names[i].name, v);
			return true;
		}
	}
	call_error(c, "unknown register '%s': d0-d7, a0-a7, sp, pc, sr, usp, ssp, vbr, sfc, dfc", name);
	return false;
}

static bool tool_disassemble(call *c, const json *a)
{
	const char *as;
	long count;
	uint32_t addr = 0;
	if (!arg_str(c, a, "address", false, &as) || (as && !parse_addr(c, as, &addr)) ||
	    !arg_int(c, a, "count", 16, 1, 200, &count))
		return false;
	if (!as) {
		info_req r;
		mcp_main_call(mc_info, &r);
		addr = r.pc;
	}
	dis_req r = { addr, (int)count, &c->text };
	mcp_main_call(mc_dis, &r);
	return true;
}

typedef struct {
	uint32_t bp[DBG_MAX_BP];
	int n, count, super;
	long steps;
	bool stopped_before;
} run_req;

static void mc_dbg_setup(void *p)
{
	run_req *r = p;
	dbg_state *d = &mcp_dbg;
	memcpy(d->bp, r->bp, sizeof(d->bp));
	d->n_bp = r->n;
	d->count = r->count;
	d->hits = 0;
	d->super = r->super;
	d->steps = r->steps;
	d->stepped = 0;
	d->stopped = false;
	// leaving a stop: the instruction there runs first, breakpoint or not
	d->skip_pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xffffff;
	d->skip_armed = r->stopped_before && !r->steps;
	fbmcp_dbg_active = 1;
}

static void mc_dbg_clear(void *p)
{
	(void)p;
	mcp_dbg.n_bp = 0;
	mcp_dbg.steps = 0;
	fbmcp_dbg_active = 0;
}

static void report_stop(call *c)
{
	regs_req rr = { &c->text, 6 };
	mcp_main_call(mc_regs, &rr);
}

// resume the CPU and wait for a debugger stop
static bool dbg_go(call *c, run_req *r, long timeout_ms)
{
	pthread_mutex_lock(&mcp_mu);
	r->stopped_before = mcp.paused_break || mcp.paused_user;
	pthread_mutex_unlock(&mcp_mu);
	mcp_main_call(mc_dbg_setup, r);
	pthread_mutex_lock(&mcp_mu);
	mcp.paused_user = false;
	mcp.paused_break = false;
	pthread_cond_broadcast(&mcp_cv);
	double deadline = wall_now() + timeout_ms / 1000.0;
	while (!mcp.paused_break && !mcp.quitting && !call_cancelled(c) && wall_now() < deadline) {
		pthread_mutex_unlock(&mcp_mu);
		call_progress(c, r->steps ? "stepping" : "running to the breakpoint");
		pthread_mutex_lock(&mcp_mu);
		mcp_wait_until(deadline);
	}
	bool stopped = mcp.paused_break;
	pthread_mutex_unlock(&mcp_mu);
	if (!stopped)
		mcp_main_call(mc_dbg_clear, NULL);
	return stopped;
}

static bool tool_debug_run(call *c, const json *a)
{
	run_req r;
	memset(&r, 0, sizeof(r));
	long count, timeout_ms;
	json *av = json_get(a, "address");
	json *sv = json_get(a, "supervisor");
	if (!av) {
		call_error(c, "address is missing");
		return false;
	}
	if (av->type == JSON_STRING) {
		if (!parse_addr(c, av->str, &r.bp[0]))
			return false;
		r.n = 1;
	} else if (av->type == JSON_ARRAY && av->n >= 1 && av->n <= DBG_MAX_BP) {
		for (int i = 0; i < av->n; i++) {
			if (av->items[i]->type != JSON_STRING) {
				call_error(c, "address must be a string or a list of strings");
				return false;
			}
			if (!parse_addr(c, av->items[i]->str, &r.bp[i]))
				return false;
			r.bp[i] &= 0xffffff;
		}
		r.n = av->n;
	} else {
		call_error(c, "address must be an address or a list of 1 to %d", DBG_MAX_BP);
		return false;
	}
	r.bp[0] &= 0xffffff;
	r.super = -1;
	if (sv && sv->type != JSON_NULL) {
		if (sv->type != JSON_BOOL) {
			call_error(c, "supervisor must be true or false");
			return false;
		}
		r.super = sv->b;
	}
	if (!arg_int(c, a, "count", 1, 1, 1000000000, &count) ||
	    !arg_int(c, a, "timeout-ms", 30000, 0, WAIT_MAX_MS, &timeout_ms))
		return false;
	r.count = count;
	double t0 = wall_now();
	if (!dbg_go(c, &r, timeout_ms)) {
		c->unmet = true;
		call_printf(c, "Not reached in %.1f s; the breakpoints are gone and the CPU runs on.", wall_now() - t0);
		return true;
	}
	call_printf(c, "Stopped at %06x after %.1f s, before running the instruction there. vm-power"
		" resume lets it run; debug-step goes on one at a time.\n", mcp_dbg.stop_pc, wall_now() - t0);
	report_stop(c);
	return true;
}

static bool tool_debug_step(call *c, const json *a)
{
	long count;
	if (!arg_int(c, a, "count", 1, 1, 100000, &count))
		return false;
	pthread_mutex_lock(&mcp_mu);
	bool stopped = mcp.paused_break || mcp.paused_user;
	pthread_mutex_unlock(&mcp_mu);
	if (!stopped) {
		call_error(c, "the CPU runs: stop it first with debug-run or vm-power pause");
		return false;
	}
	run_req r;
	memset(&r, 0, sizeof(r));
	r.steps = count;
	r.super = -1;
	if (!dbg_go(c, &r, 60000)) {
		call_error(c, "the steps did not finish");
		return false;
	}
	call_printf(c, "Did %ld instruction%s.\n", count, count == 1 ? "" : "s");
	report_stop(c);
	return true;
}

/* ---- steps ---- */

static bool tool_steps(call *c, const json *a);

static const struct { const char *name; tool_fn fn; } tools[] = {
	{ "vm-info", tool_vm_info },
	{ "vm-power", tool_vm_power },
	{ "vm-speed", tool_vm_speed },
	{ "screenshot", tool_screenshot },
	{ "wait-screen", tool_wait_screen },
	{ "key", tool_key },
	{ "type-text", tool_type_text },
	{ "mouse-move", tool_mouse_move },
	{ "mouse-click", tool_mouse_click },
	{ "mouse-button", tool_mouse_button },
	{ "mouse-drag", tool_mouse_drag },
	{ "pointer-locate", tool_pointer_locate },
	{ "serial-write", tool_serial_write },
	{ "serial-read", tool_serial_read },
	{ "serial-expect", tool_serial_expect },
	{ "serial-run", tool_serial_run },
	{ "media", tool_media },
	{ "sound", tool_sound },
	{ "record", tool_record },
	{ "steps", tool_steps },
	{ "debug-registers", tool_debug_registers },
	{ "register-write", tool_register_write },
	{ "memory-read", tool_memory_read },
	{ "memory-write", tool_memory_write },
	{ "disassemble", tool_disassemble },
	{ "debug-run", tool_debug_run },
	{ "debug-step", tool_debug_step },
	{ NULL, NULL }
};

tool_fn tool_find(const char *name)
{
	for (int i = 0; tools[i].name; i++)
		if (strcmp(tools[i].name, name) == 0)
			return tools[i].fn;
	return NULL;
}

static bool tool_steps(call *c, const json *a)
{
	json *steps = json_get(a, "steps");
	if (!steps || steps->type != JSON_ARRAY || steps->n < 1 || steps->n > 50) {
		call_error(c, "steps must be a list of 1 to 50 steps");
		return false;
	}
	for (int i = 0; i < steps->n; i++) {
		json *s = steps->items[i];
		const char *name = json_get_str(s, "tool");
		json *args = json_get(s, "args");
		if (!name || !tool_find(name) || strcmp(name, "steps") == 0) {
			call_error(c, "step %d: %s", i + 1, name ? "no such tool (or steps inside steps)" : "tool is missing");
			return false;
		}
		if (args && args->type != JSON_OBJECT && args->type != JSON_NULL) {
			call_error(c, "step %d: args must be an object", i + 1);
			return false;
		}
	}
	json *empty = json_parse("{}", 2, NULL);
	for (int i = 0; i < steps->n; i++) {
		json *s = steps->items[i];
		const char *name = json_get_str(s, "tool");
		json *args = json_get(s, "args");
		if (!args || args->type != JSON_OBJECT)
			args = empty;
		call sub;
		memset(&sub, 0, sizeof(sub));
		sub.cn = c->cn;
		sub.id = c->id;
		sub.token = c->token;
		sub.started = c->started;
		sub.last_progress = c->last_progress;
		sub.parent = c;
		sb_init(&sub.text);
		tool_find(name)(&sub, args);
		c->last_progress = sub.last_progress;
		call_printf(c, "%d. %s: %s\n", i + 1, name, sub.text.len ? sub.text.s : "done");
		if (sub.image) {
			free(c->image);
			c->image = sub.image;
		}
		sb_free(&sub.text);
		if (sub.is_error || sub.unmet || call_stopped(c)) {
			call_printf(c, "Stopped at step %d of %d%s.", i + 1, steps->n,
				sub.is_error ? " (it failed)" : call_stopped(c) ? " (cancelled)" : " (its wait was not met)");
			c->unmet = !sub.is_error;
			c->is_error = sub.is_error;
			break;
		}
		if (i + 1 == steps->n)
			call_printf(c, "All %d steps done.", steps->n);
	}
	json_free(empty);
	return true;
}
