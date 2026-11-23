/*
 * record - the record tool: the screen and the speaker as a video.
 *
 * The main thread hands over a copy of video RAM and the speaker's tone at
 * every 60 Hz guest tick; a writer thread turns them into ZMBV frames and
 * PCM sound in AVI files. So the video runs on guest time: 60 frames a
 * second of it, however fast or slow the host runs the guest.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

#include "mcp.h"

#define REC_RATE	44100
#define REC_SPF		(REC_RATE / TICK_HZ)	// samples a frame: 735
#define RQ			240						// frames queued for the writer
#define KEY_EVERY	600						// a key frame every 10 s
#define PART_BYTES	(1000ULL * 1024 * 1024)	// a new file after this
#define THUMB_W		240
#define THUMB_H		180
#define MAX_THUMBS	400
#define SHEET		16
#define SHEET_COLS	4

typedef struct {
	uint8_t vram[VRAM_USED];
	bool rev, tone;
	int lo, hi;
} rec_frame;

typedef struct {
	char *path;
	uint64_t first_frame, frames;
} rec_file;

typedef struct {
	uint8_t img[THUMB_W * THUMB_H];
	uint64_t ms;
} thumb;

typedef struct {
	char *base;
	int color;
	bool audio, freeze;
	long skip_ms;
	uint8_t pal[768];

	avi_file *avi;
	zmbv *z;
	rec_file files[64];
	int n_files;
	uint64_t frames, frames_in_file, skipped, still;
	uint8_t prev[VRAM_USED];
	bool prev_rev, have_prev;
	uint8_t recent[4][VRAM_USED];	// the last new pictures (for skip-still)
	bool recent_rev[4];
	int n_recent;
	uint8_t *pix;
	sbuf enc;
	double ph_lo, ph_hi;
	int16_t samples[REC_SPF];
	char *error;

	thumb *thumbs;
	int n_thumbs;
	uint64_t thumb_every;

	sbuf marks;		// JSON objects, comma separated
	int n_marks, n_chapters, n_captions;
	struct mark { uint64_t ms, dur; int kind; char *text; } list[1000];
	int n_list;
	double started;
} recorder;

static pthread_mutex_t rec_mu = PTHREAD_MUTEX_INITIALIZER;
// One record action at a time (tool calls run in threads of their own):
// without it, "record status" could read the recorder while "record stop"
// frees it. rec_finish takes it too.
static pthread_mutex_t rec_ctl_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rec_cv = PTHREAD_COND_INITIALIZER;
static recorder *rec;
static volatile bool rec_on;		// main thread hands frames over
static volatile bool rec_freeze;
static volatile int rec_hold;
static bool stopping;
static pthread_t writer;
static rec_frame *queue;
static int q_head, q_count;

bool rec_active(void)
{
	return rec_on;
}

bool rec_frozen_locked(void)
{
	return rec_on && rec_freeze && mcp.calls_running == 0 && rec_hold == 0;
}

bool rec_holding(void)
{
	return rec_hold > 0;
}

bool rec_freezing(void)
{
	return rec_on && rec_freeze;
}

void rec_tick(const uint8_t *vram, bool rev, bool tone, int lo, int hi)
{
	pthread_mutex_lock(&rec_mu);
	while (rec_on && q_count == RQ)
		pthread_cond_wait(&rec_cv, &rec_mu);	// the writer is behind
	if (rec_on) {
		rec_frame *f = &queue[(q_head + q_count) % RQ];
		memcpy(f->vram, vram, VRAM_USED);
		f->rev = rev;
		f->tone = tone;
		f->lo = lo;
		f->hi = hi;
		q_count++;
		pthread_cond_broadcast(&rec_cv);
	}
	pthread_mutex_unlock(&rec_mu);
}

/* ---- the writer ---- */

static bool open_part(recorder *r)
{
	char path[1100];
	if (r->n_files == 0)
		snprintf(path, sizeof(path), "%s.avi", r->base);
	else
		snprintf(path, sizeof(path), "%s-%d.avi", r->base, r->n_files + 1);
	const char *err = NULL;
	r->avi = avi_create(path, SCR_W, SCR_H, TICK_HZ, r->audio ? REC_RATE : 0, 1, &err);
	if (!r->avi) {
		sbuf b;
		sb_init(&b);
		sb_printf(&b, "can't create %s: %s", path, err);
		r->error = sb_take(&b);
		return false;
	}
	rec_file *f = &r->files[r->n_files++];
	f->path = strdup(path);
	f->first_frame = r->frames;
	f->frames = 0;
	r->frames_in_file = 0;
	return true;
}

static void close_part(recorder *r)
{
	if (!r->avi)
		return;
	char *err = NULL;
	if (!avi_close(r->avi, &err) && !r->error)
		r->error = err;
	else
		free(err);
	r->avi = NULL;
}

static void render(recorder *r, const rec_frame *f)
{
	uint8_t *p = r->pix;
	uint8_t inv = f->rev ? 1 : 0;
	for (int y = 0; y < SCR_H; y++) {
		const uint8_t *v = f->vram + y * VRAM_ROW;
		for (int w = 0; w < VRAM_ROW; w += 2) {
			uint8_t lo = v[w + 1], hi = v[w];	// pixels 0..7, then 8..15
			for (int b = 0; b < 8; b++)
				*p++ = ((lo >> b) & 1) ^ inv;
			for (int b = 0; b < 8; b++)
				*p++ = ((hi >> b) & 1) ^ inv;
		}
	}
}

static void make_thumb(recorder *r)
{
	if (r->n_thumbs == MAX_THUMBS) {
		// keep every other one, and take them half as often
		for (int i = 0; i < MAX_THUMBS / 2; i++)
			r->thumbs[i] = r->thumbs[2 * i];
		r->n_thumbs = MAX_THUMBS / 2;
		r->thumb_every *= 2;
	}
	thumb *t = &r->thumbs[r->n_thumbs++];
	t->ms = r->frames * 1000 / TICK_HZ;
	for (int y = 0; y < THUMB_H; y++) {
		int y0 = y * SCR_H / THUMB_H, y1 = (y + 1) * SCR_H / THUMB_H;
		if (y1 <= y0) y1 = y0 + 1;
		for (int x = 0; x < THUMB_W; x++) {
			int x0 = x * SCR_W / THUMB_W, x1 = (x + 1) * SCR_W / THUMB_W;
			int on = 0, all = 0;
			for (int yy = y0; yy < y1; yy++)
				for (int xx = x0; xx < x1; xx++, all++)
					on += r->pix[yy * SCR_W + xx];
			t->img[y * THUMB_W + x] = 24 + on * 231 / all;
		}
	}
}

// whether f is a picture of the last few changes (a blinking cursor); if
// not, it is remembered
static bool seen_lately(recorder *r, const rec_frame *f)
{
	for (int i = 0; i < r->n_recent; i++)
		if (r->recent_rev[i] == f->rev && !memcmp(r->recent[i], f->vram, VRAM_USED))
			return true;
	if (r->n_recent == 4) {
		memmove(r->recent[0], r->recent[1], sizeof(r->recent[0]) * 3);
		memmove(r->recent_rev, r->recent_rev + 1, sizeof(r->recent_rev[0]) * 3);
		r->n_recent--;
	}
	memcpy(r->recent[r->n_recent], f->vram, VRAM_USED);
	r->recent_rev[r->n_recent++] = f->rev;
	return false;
}

static void write_frame(recorder *r, const rec_frame *f)
{
	bool changed = !r->have_prev || f->rev != r->prev_rev || memcmp(f->vram, r->prev, VRAM_USED);
	bool news = changed && (!r->skip_ms || !seen_lately(r, f));
	if (news || f->tone)
		r->still = 0;
	else
		r->still++;
	if (r->skip_ms && r->still * 1000 / TICK_HZ > (uint64_t)r->skip_ms) {
		r->skipped++;
		return;
	}
	if (changed) {
		render(r, f);
		memcpy(r->prev, f->vram, VRAM_USED);
		r->prev_rev = f->rev;
		r->have_prev = true;
	}
	if (r->error)
		return;
	if (!r->avi && !open_part(r))
		return;
	bool key = r->frames_in_file % KEY_EVERY == 0;
	if (!zmbv_encode(r->z, key, r->pix, r->pal, &r->enc) ||
	    !avi_add_video(r->avi, r->enc.s, r->enc.len, key)) {
		if (!r->error)
			r->error = strdup("can't encode or write a video frame");
		return;
	}
	if (r->audio) {
		if (f->tone) {
			double a = 32767 * 0.3;
			double slo = 2 * M_PI * f->lo / REC_RATE, shi = 2 * M_PI * f->hi / REC_RATE;
			for (int i = 0; i < REC_SPF; i++) {
				r->samples[i] = (int16_t)(a * sin(r->ph_lo) + a * sin(r->ph_hi));
				r->ph_lo = fmod(r->ph_lo + slo, 2 * M_PI);
				r->ph_hi = fmod(r->ph_hi + shi, 2 * M_PI);
			}
		} else {
			memset(r->samples, 0, sizeof(r->samples));
			r->ph_lo = r->ph_hi = 0;
		}
		avi_add_audio(r->avi, r->samples, REC_SPF);
	}
	if (r->frames % r->thumb_every == 0)
		make_thumb(r);
	r->frames++;
	r->frames_in_file++;
	r->files[r->n_files - 1].frames++;
	if (r->frames_in_file % TICK_HZ == 0)
		avi_sync(r->avi);
	if (avi_bytes(r->avi) > PART_BYTES && r->n_files < 64) {
		close_part(r);
		open_part(r);
	}
}

static void *writer_main(void *p)
{
	recorder *r = p;
	rec_frame *f = malloc(sizeof(*f));
	pthread_mutex_lock(&rec_mu);
	for (;;) {
		while (q_count == 0 && !stopping)
			pthread_cond_wait(&rec_cv, &rec_mu);
		if (q_count == 0 && stopping)
			break;
		*f = queue[q_head];
		q_head = (q_head + 1) % RQ;
		q_count--;
		pthread_cond_broadcast(&rec_cv);
		pthread_mutex_unlock(&rec_mu);
		write_frame(r, f);
		pthread_mutex_lock(&rec_mu);
		pthread_cond_broadcast(&rec_cv);
	}
	pthread_mutex_unlock(&rec_mu);
	free(f);
	return NULL;
}

/* ---- the tool ---- */

static const char *const kinds[] = { "chapter", "caption", "zoom", "note", NULL };
enum { K_CHAPTER, K_CAPTION, K_ZOOM, K_NOTE };

static void time_str(sbuf *b, uint64_t ms, bool frac)
{
	uint64_t s = ms / 1000;
	if (s >= 3600)
		sb_printf(b, "%d:%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
	else
		sb_printf(b, "%d:%02d", (int)(s / 60), (int)(s % 60));
	if (frac)
		sb_printf(b, ".%d", (int)(ms % 1000 / 100));
}

static void shell_quote(sbuf *b, const char *s)
{
	sb_putc(b, '\'');
	for (; *s; s++) {
		if (*s == '\'')
			sb_puts(b, "'\\''");
		else
			sb_putc(b, *s);
	}
	sb_putc(b, '\'');
}

// wait until the writer has every frame handed over so far
static void flush_queue(void)
{
	pthread_mutex_lock(&rec_mu);
	double deadline = wall_now() + 30;
	while (q_count > 0 && wall_now() < deadline) {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 50000000;
		if (ts.tv_nsec >= 1000000000L) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000L;
		}
		pthread_cond_timedwait(&rec_cv, &rec_mu, &ts);
	}
	pthread_mutex_unlock(&rec_mu);
}

static bool rec_start(call *c, const json *a)
{
	static const char *const colors[] = { "white", "green", "amber", NULL };
	const char *file;
	int color;
	bool audio, freeze;
	long skip;
	if (!json_get_str(a, "file") && json_has(a, "file")) {
		call_error(c, "file must be a string");
		return false;
	}
	file = json_get_str(a, "file");
	json *v;
	color = 0;
	if ((v = json_get(a, "color")) && v->type == JSON_STRING) {
		int i;
		for (i = 0; colors[i] && strcmp(colors[i], v->str); i++)
			;
		if (!colors[i]) {
			call_error(c, "color must be white, green or amber");
			return false;
		}
		color = i;
	}
	v = json_get(a, "audio");
	audio = !v || v->type != JSON_BOOL || v->b;
	v = json_get(a, "freeze");
	freeze = v && v->type == JSON_BOOL && v->b;
	v = json_get(a, "skip-still-ms");
	skip = v && v->type == JSON_NUMBER ? (long)v->num : 0;
	if (skip < 0 || skip > 3600000) {
		call_error(c, "skip-still-ms must be from 0 to 3600000");
		return false;
	}
	if (rec) {
		call_error(c, "already recording to %s.avi; stop it first", rec->base);
		return false;
	}
	char base[1024];
	if (file && *file) {
		snprintf(base, sizeof(base), "%s", file);
		size_t l = strlen(base);
		if (l > 4 && strcmp(base + l - 4, ".avi") == 0)
			base[l - 4] = 0;
	} else {
		time_t t = time(NULL);
		char when[32];
		strftime(when, sizeof(when), "%Y%m%d-%H%M%S", localtime(&t));
		snprintf(base, sizeof(base), "freebee-take-%s", when);
	}
	static const char *const ext[] = { ".avi", "-2.avi", ".json", ".srt", NULL };
	for (int i = 0; ext[i]; i++) {
		char p[1100];
		struct stat st;
		snprintf(p, sizeof(p), "%s%s", base, ext[i]);
		if (stat(p, &st) == 0) {
			call_error(c, "%s exists: choose another file", p);
			return false;
		}
	}

	recorder *r = calloc(1, sizeof(*r));
	r->base = strdup(base);
	r->color = color;
	r->audio = audio;
	r->freeze = freeze;
	r->skip_ms = skip;
	static const uint8_t fg[3][3] = { { 255, 255, 255 }, { 0x33, 0xff, 0x66 }, { 0xff, 0xb0, 0x00 } };
	memcpy(r->pal + 3, fg[color], 3);
	r->pix = calloc(SCR_W, SCR_H);
	r->z = zmbv_new(SCR_W, SCR_H, 4);
	r->thumbs = calloc(MAX_THUMBS, sizeof(thumb));
	r->thumb_every = 5 * TICK_HZ;
	r->started = wall_now();
	sb_init(&r->enc);
	sb_init(&r->marks);
	if (!r->pix || !r->z || !r->thumbs || !open_part(r)) {
		call_error(c, "%s", r->error ? r->error : "out of memory");
		zmbv_free(r->z);
		free(r->pix);
		free(r->thumbs);
		free(r->base);
		free(r->error);
		free(r);
		return false;
	}
	if (!queue)
		queue = malloc(sizeof(rec_frame) * RQ);
	pthread_mutex_lock(&rec_mu);
	q_head = q_count = 0;
	stopping = false;
	rec = r;
	pthread_mutex_unlock(&rec_mu);
	if (pthread_create(&writer, NULL, writer_main, r) != 0) {
		call_error(c, "cannot start the writer thread");
		pthread_mutex_lock(&rec_mu);
		rec = NULL;
		pthread_mutex_unlock(&rec_mu);
		close_part(r);
		return false;
	}
	pthread_mutex_lock(&mcp_mu);
	rec_freeze = freeze;
	rec_on = true;
	pthread_cond_broadcast(&mcp_cv);
	pthread_mutex_unlock(&mcp_mu);

	call_printf(c, "Recording to %s.avi: 720x348, 60 frames a second of guest time, %s phosphor, %s.",
		base, colors[color], audio ? "with the speaker's sound" : "no sound");
	if (freeze)
		call_printf(c, " The VM runs only during MCP calls (freeze): let it run with a wait or a"
			" mark's hold-ms.");
	if (skip)
		call_printf(c, " Still stretches longer than %ld ms are cut short.", skip);
	call_printf(c, " Mark chapters and captions with record mark; record stop finishes the files.");
	return true;
}

static bool rec_mark(call *c, const json *a)
{
	int kind = K_CHAPTER;
	const char *text = json_get_str(a, "text");
	json *v = json_get(a, "kind");
	if (v && v->type == JSON_STRING) {
		int i;
		for (i = 0; kinds[i] && strcmp(kinds[i], v->str); i++)
			;
		if (!kinds[i]) {
			call_error(c, "kind must be chapter, caption, zoom or note");
			return false;
		}
		kind = i;
	}
	v = json_get(a, "duration-ms");
	long dur = v && v->type == JSON_NUMBER ? (long)v->num : 3000;
	v = json_get(a, "hold-ms");
	long hold = v && v->type == JSON_NUMBER ? (long)v->num : 0;
	if (dur < 100 || dur > 600000 || hold < 0 || hold > 60000) {
		call_error(c, "duration-ms must be from 100 to 600000 and hold-ms from 0 to 60000");
		return false;
	}
	if (kind != K_ZOOM && (!text || !*text)) {
		call_error(c, "a %s needs text", kinds[kind]);
		return false;
	}
	json *rv = json_get(a, "region");
	long rx = 0, ry = 0, rw = 0, rh = 0;
	if (kind == K_ZOOM) {
		if (!rv || rv->type != JSON_OBJECT || !json_get(rv, "x") || !json_get(rv, "y") ||
		    !json_get(rv, "w") || !json_get(rv, "h")) {
			call_error(c, "a zoom needs a region with x, y, w and h");
			return false;
		}
		rx = (long)json_get(rv, "x")->num;
		ry = (long)json_get(rv, "y")->num;
		rw = (long)json_get(rv, "w")->num;
		rh = (long)json_get(rv, "h")->num;
	}
	if (!rec) {
		call_error(c, "not recording: call record start first");
		return false;
	}
	flush_queue();
	recorder *r = rec;
	uint64_t ms = r->frames * 1000 / TICK_HZ;
	if (r->n_marks)
		sb_putc(&r->marks, ',');
	sb_printf(&r->marks, "{\"kind\":\"%s\",\"at-ms\":%llu", kinds[kind], (unsigned long long)ms);
	if (text) {
		sb_puts(&r->marks, ",\"text\":");
		sb_jstr(&r->marks, text);
	}
	if (kind == K_CAPTION || kind == K_ZOOM)
		sb_printf(&r->marks, ",\"duration-ms\":%ld", dur);
	if (kind == K_ZOOM)
		sb_printf(&r->marks, ",\"region\":{\"x\":%ld,\"y\":%ld,\"w\":%ld,\"h\":%ld}", rx, ry, rw, rh);
	sb_putc(&r->marks, '}');
	r->n_marks++;
	if (r->n_list < 1000) {
		struct mark *m = &r->list[r->n_list++];
		m->ms = ms;
		m->dur = dur;
		m->kind = kind;
		m->text = strdup(text ? text : "");
	}
	r->n_chapters += kind == K_CHAPTER;
	r->n_captions += kind == K_CAPTION;
	sbuf t;
	sb_init(&t);
	time_str(&t, ms, true);
	call_printf(c, "Marked a %s at %s of the video.", kinds[kind], t.s);
	sb_free(&t);
	if (hold) {
		rec_hold++;
		pthread_mutex_lock(&mcp_mu);
		uint64_t until = mcp.ticks + hold * TICK_HZ / 1000;
		double deadline = wall_now() + 600;
		while (mcp.ticks < until && !mcp.quitting && !call_cancelled(c) && wall_now() < deadline &&
		       !mcp.paused_user && !mcp.paused_break)
			mcp_wait_until(deadline);
		pthread_mutex_unlock(&mcp_mu);
		rec_hold--;
		call_printf(c, " Recorded %ld ms more.", hold);
	}
	return true;
}

static void rec_status(call *c)
{
	recorder *r = rec;
	sbuf t;
	sb_init(&t);
	time_str(&t, r->frames * 1000 / TICK_HZ, true);
	uint64_t bytes = 0;
	if (r->avi)
		bytes = avi_bytes(r->avi);
	call_printf(c, "Recording %s.avi: %s long, %llu frames, %d file%s, %.1f MB in the current file,"
		" %d marks", r->base, t.s, (unsigned long long)r->frames, r->n_files,
		r->n_files == 1 ? "" : "s", bytes / 1048576.0, r->n_marks);
	if (r->skip_ms)
		call_printf(c, ", %.1f s of stills left out", r->skipped / (double)TICK_HZ);
	call_printf(c, ".");
	if (r->error)
		call_printf(c, " Error: %s.", r->error);
	sb_free(&t);
}

/* the contact sheet's labels: a 5x7 font for digits and ':' */
static const uint8_t digits[11][7] = {
	{ 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
	{ 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
	{ 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
	{ 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
	{ 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
	{ 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 },
};

static void draw_text(uint8_t *img, int w, int h, int x, int y, const char *s)
{
	for (; *s; s++, x += 12) {
		int g = *s == ':' ? 10 : (*s >= '0' && *s <= '9') ? *s - '0' : -1;
		if (g < 0)
			continue;
		for (int r = 0; r < 7; r++)
			for (int col = 0; col < 5; col++)
				if (digits[g][r] & (0x10 >> col))
					for (int dy = 0; dy < 2; dy++)
						for (int dx = 0; dx < 2; dx++) {
							int px = x + col * 2 + dx, py = y + r * 2 + dy;
							if (px < w && py < h)
								img[py * w + px] = 255;
						}
	}
}

static void sheet(call *c, recorder *r)
{
	int n = r->n_thumbs < SHEET ? r->n_thumbs : SHEET;
	if (!n)
		return;
	int cols = n < SHEET_COLS ? n : SHEET_COLS, rows = (n + cols - 1) / cols;
	int gap = 6, label = 18;
	int w = gap + cols * (THUMB_W + gap), h = gap + rows * (THUMB_H + label + gap);
	uint8_t *img = malloc((size_t)w * h);
	memset(img, 40, (size_t)w * h);
	for (int i = 0; i < n; i++) {
		int k = n == 1 ? 0 : i * (r->n_thumbs - 1) / (n - 1);
		thumb *t = &r->thumbs[k];
		int x = gap + (i % cols) * (THUMB_W + gap), y = gap + (i / cols) * (THUMB_H + label + gap);
		for (int yy = 0; yy < THUMB_H; yy++)
			memcpy(img + (size_t)(y + yy) * w + x, t->img + yy * THUMB_W, THUMB_W);
		sbuf s;
		sb_init(&s);
		time_str(&s, t->ms, false);
		draw_text(img, w, h, x, y + THUMB_H + 2, s.s);
		sb_free(&s);
	}
	sbuf png, b64;
	sb_init(&png);
	sb_init(&b64);
	if (png_encode(&png, w, h, img, w, NULL, 0)) {
		sb_base64(&b64, (const unsigned char *)png.s, png.len);
		free(c->image);
		c->image = sb_take(&b64);
	}
	sb_free(&png);
	sb_free(&b64);
	free(img);
}

static void render_cmd(sbuf *s, recorder *r)
{
	// 4K: the 4:3 picture 2880x2160, blown up whole first for sharp pixels
	static const char vf[] = "scale=iw*7:ih*7:flags=neighbor,scale=2880:2160:flags=area,"
		"setsar=1,pad=3840:2160:480:0";
	sb_puts(s, "ffmpeg");
	for (int i = 0; i < r->n_files; i++) {
		sb_puts(s, " -i ");
		shell_quote(s, r->files[i].path);
	}
	if (r->n_files == 1) {
		sb_printf(s, " -vf '%s'", vf);
	} else {
		sb_puts(s, " -filter_complex '");
		for (int i = 0; i < r->n_files; i++)
			sb_printf(s, "[%d:v]%s[v%d];", i, vf, i);
		for (int i = 0; i < r->n_files; i++)
			sb_printf(s, r->audio ? "[v%d][%d:a]" : "[v%d]", i, i);
		sb_printf(s, "concat=n=%d:v=1:a=%d[v]%s' -map '[v]'%s", r->n_files, r->audio ? 1 : 0,
			r->audio ? "[a]" : "", r->audio ? " -map '[a]'" : "");
	}
	sb_puts(s, " -c:v libx264 -preset slow -crf 18 -pix_fmt yuv420p");
	if (r->audio)
		sb_puts(s, " -c:a aac -b:a 192k -ar 48000");
	sb_puts(s, " -movflags +faststart ");
	char mp4[1100];
	snprintf(mp4, sizeof(mp4), "%s.mp4", r->base);
	shell_quote(s, mp4);
}

static void srt_time(FILE *f, uint64_t ms)
{
	fprintf(f, "%02d:%02d:%02d,%03d", (int)(ms / 3600000), (int)(ms / 60000 % 60),
		(int)(ms / 1000 % 60), (int)(ms % 1000));
}

// stop the writer and finish the files; returns the recorder (no longer current)
static recorder *finish(void)
{
	pthread_mutex_lock(&mcp_mu);
	rec_on = false;
	rec_freeze = false;
	pthread_cond_broadcast(&mcp_cv);
	pthread_mutex_unlock(&mcp_mu);
	pthread_mutex_lock(&rec_mu);
	recorder *r = rec;
	stopping = true;
	pthread_cond_broadcast(&rec_cv);
	pthread_mutex_unlock(&rec_mu);
	if (!r)
		return NULL;
	pthread_join(writer, NULL);
	close_part(r);
	pthread_mutex_lock(&rec_mu);
	rec = NULL;
	pthread_mutex_unlock(&rec_mu);

	char path[1100];
	snprintf(path, sizeof(path), "%s.json", r->base);
	FILE *f = fopen(path, "w");
	if (f) {
		sbuf j;
		sb_init(&j);
		sb_printf(&j, "{\"recorder\":\"freebee record\",\"machine\":\"AT&T UNIX PC (3B1)\","
			"\"width\":%d,\"height\":%d,\"display-aspect\":\"4:3\",\"fps\":%d,\"audio-rate\":%d,"
			"\"color\":\"%s\",\"length-ms\":%llu,\"frames\":%llu,\"skipped-ms\":%llu,\"files\":[",
			SCR_W, SCR_H, TICK_HZ, r->audio ? REC_RATE : 0,
			r->color == 1 ? "green" : r->color == 2 ? "amber" : "white",
			(unsigned long long)(r->frames * 1000 / TICK_HZ), (unsigned long long)r->frames,
			(unsigned long long)(r->skipped * 1000 / TICK_HZ));
		for (int i = 0; i < r->n_files; i++) {
			sb_printf(&j, "%s{\"file\":", i ? "," : "");
			sb_jstr(&j, r->files[i].path);
			sb_printf(&j, ",\"start-ms\":%llu,\"length-ms\":%llu,\"frames\":%llu}",
				(unsigned long long)(r->files[i].first_frame * 1000 / TICK_HZ),
				(unsigned long long)(r->files[i].frames * 1000 / TICK_HZ),
				(unsigned long long)r->files[i].frames);
		}
		sb_printf(&j, "],\"marks\":[%s]}\n", r->marks.s ? r->marks.s : "");
		fputs(j.s, f);
		fclose(f);
		sb_free(&j);
	}
	if (r->n_captions) {
		snprintf(path, sizeof(path), "%s.srt", r->base);
		f = fopen(path, "w");
		if (f) {
			int k = 0;
			for (int i = 0; i < r->n_list; i++) {
				struct mark *m = &r->list[i];
				if (m->kind != K_CAPTION)
					continue;
				fprintf(f, "%d\n", ++k);
				srt_time(f, m->ms);
				fputs(" --> ", f);
				srt_time(f, m->ms + m->dur);
				fprintf(f, "\n%s\n\n", m->text);
			}
			fclose(f);
		}
	}
	return r;
}

static void free_rec(recorder *r)
{
	for (int i = 0; i < r->n_files; i++)
		free(r->files[i].path);
	for (int i = 0; i < r->n_list; i++)
		free(r->list[i].text);
	zmbv_free(r->z);
	free(r->pix);
	free(r->thumbs);
	sb_free(&r->enc);
	sb_free(&r->marks);
	free(r->error);
	free(r->base);
	free(r);
}

static bool rec_stop(call *c)
{
	if (!rec) {
		call_error(c, "not recording");
		return false;
	}
	recorder *r = finish();
	if (!r)
		return false;
	sbuf t;
	sb_init(&t);
	time_str(&t, r->frames * 1000 / TICK_HZ, true);
	call_printf(c, "Stopped: %s of video, %llu frames", t.s, (unsigned long long)r->frames);
	if (r->skipped)
		call_printf(c, " (%.1f s of stills left out)", r->skipped / (double)TICK_HZ);
	call_printf(c, ".\nFiles:");
	for (int i = 0; i < r->n_files; i++)
		call_printf(c, " %s", r->files[i].path);
	call_printf(c, "\nSidecar (files and marks, for the edit): %s.json", r->base);
	if (r->n_captions)
		call_printf(c, "\nSubtitles: %s.srt (%d captions)", r->base, r->n_captions);
	if (r->error)
		call_printf(c, "\nError: %s", r->error);
	if (r->n_chapters) {
		call_printf(c, "\nChapters, for the video's description on YouTube:\n");
		bool zero = false;
		for (int i = 0; i < r->n_list; i++)
			if (r->list[i].kind == K_CHAPTER && r->list[i].ms < 1000)
				zero = true;
		if (!zero)
			call_printf(c, "0:00 Start\n");
		for (int i = 0; i < r->n_list; i++) {
			if (r->list[i].kind != K_CHAPTER)
				continue;
			sbuf s;
			sb_init(&s);
			time_str(&s, r->list[i].ms < 1000 ? 0 : r->list[i].ms, false);
			call_printf(c, "%s %s\n", s.s, r->list[i].text);
			sb_free(&s);
		}
	}
	sbuf cmd;
	sb_init(&cmd);
	render_cmd(&cmd, r);
	call_printf(c, "\nRender for YouTube (4K, sharp pixels, 4:3), in %s:\n%s", mcp_vm_dir, cmd.s);
	sb_free(&cmd);
	if (r->n_thumbs)
		call_printf(c, "\nThe image is a contact sheet of the video.");
	sheet(c, r);
	sb_free(&t);
	free_rec(r);
	return true;
}

bool tool_record(call *c, const json *args)
{
	static const char *const acts[] = { "start", "mark", "status", "stop", NULL };
	const char *a = json_get_str(args, "action");
	int act;
	for (act = 0; a && acts[act] && strcmp(acts[act], a); act++)
		;
	if (!a || !acts[act]) {
		call_error(c, "action must be start, mark, status or stop");
		return false;
	}
	bool ok = true;
	pthread_mutex_lock(&rec_ctl_mu);
	switch (act) {
	case 0: ok = rec_start(c, args); break;
	case 1: ok = rec_mark(c, args); break;
	case 2:
		if (!rec)
			call_printf(c, "Not recording.");
		else
			rec_status(c);
		break;
	default: ok = rec_stop(c); break;
	}
	pthread_mutex_unlock(&rec_ctl_mu);
	return ok;
}

void rec_finish(void)
{
	pthread_mutex_lock(&rec_ctl_mu);
	recorder *r = rec ? finish() : NULL;
	pthread_mutex_unlock(&rec_ctl_mu);
	if (r) {
		fprintf(stderr, "MCP: the recording %s.avi was finished as FreeBee quit (%llu frames)\n",
			r->base, (unsigned long long)r->frames);
		free_rec(r);
	}
}
