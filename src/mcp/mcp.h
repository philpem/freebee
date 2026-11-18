/*
 * mcp - FreeBee's MCP server, inside: what server.c, tools.c, record.c and
 * bridge.c share.
 */
#ifndef _FB_MCP_H
#define _FB_MCP_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#include "json.h"
#include "img.h"

#define SCR_W		720
#define SCR_H		348
#define VRAM_ROW	90		// bytes in a screen line
#define VRAM_USED	(VRAM_ROW * SCR_H)
#define TICK_HZ		60
#define WAIT_MAX_MS	(25 * 60 * 1000)

#define MCP_PROTOCOL_LATEST "2025-11-25"

typedef struct { int x, y, w, h; } rect;

/// A picture of the screen.
typedef struct {
	uint8_t vram[VRAM_USED];
	bool rev;			// whole screen reverse video
	uint64_t seq;		// counts changes
	uint64_t tick;		// guest tick of the last change
	uint64_t at;		// guest tick when this copy was taken
} frame_t;

/// What the server knows of the emulator; guarded by mcp_mu.
typedef struct {
	frame_t frame;
	uint64_t ticks;			// guest 60 Hz ticks so far
	double change_wall;		// wall time of the last change
	int calls_running;		// tool calls in progress
	bool paused_user;		// vm-power pause
	bool paused_break;		// debug-run or debug-step stopped the CPU
	bool quit;
	bool reset;
	bool quitting;			// fbmcp_done has begun
	uint8_t leds;
	bool mouse_enabled;
	// the dialer's tone through the speaker
	bool tone;
	int tone_lo, tone_hi;
} mcp_state;

extern pthread_mutex_t mcp_mu;
extern pthread_cond_t mcp_cv;	// broadcast at every tick and state change
extern mcp_state mcp;

double wall_now(void);	// seconds, monotonic
/// Wait on mcp_cv (mcp_mu held) until woken or the wall time "until".
void mcp_wait_until(double until);
/// Run fn(arg) on the emulator's main thread and wait for it.
void mcp_main_call(void (*fn)(void *), void *arg);

/// Whether the main loop runs the CPU now (mcp_mu held).
bool mcp_cpu_running_locked(void);

/* ---- mouse packets, sent one a tick ---- */
uint64_t mcp_mouse_push(int dx, int dy, int buttons, int wait_ticks);
uint64_t mcp_mouse_done(void);

/* ---- serial ---- */
#define SER_RING (1 << 20)
/// Bytes the guest sent: copy from absolute position pos, at most max.
/// Returns the count; *start is where they really start (old bytes are
/// overwritten after 1 MiB).
size_t mcp_serial_get(uint64_t pos, char *out, size_t max, uint64_t *start);
uint64_t mcp_serial_total(void);
/// Queue bytes for the guest to read.
bool mcp_serial_put(const char *data, size_t n);
/// Bytes queued for the guest and not read yet.
size_t mcp_serial_queued(void);
extern uint64_t mcp_serial_cursor;		// where the next read starts (mcp_mu)

/* ---- sound: the dialer's tone through the speaker ---- */
typedef struct { uint64_t tick; bool on; int lo, hi; } tone_event;
/// Tone changes since a guest tick.
int mcp_tone_events(uint64_t since_tick, tone_event *out, int max);

/* ---- debugger (the hook runs on the main thread) ---- */
#define DBG_MAX_BP 16
typedef struct {
	uint32_t bp[DBG_MAX_BP];
	int n_bp;
	int count, hits;	// stop at the count-th hit
	int super;			// -1 either, 0 user mode, 1 supervisor mode
	long steps;			// > 0: stop after this many instructions
	long stepped;
	uint32_t skip_pc;	// the first instruction after a resume runs
	bool skip_armed;
	bool stopped;
	uint32_t stop_pc;
} dbg_state;
extern dbg_state mcp_dbg;

/* ---- the registry of running VMs ---- */
const char *mcp_registry_dir(void);
/// Whether dir is ours and private (with create, make it first).
bool mcp_registry_ok(const char *dir, bool create);

/* ---- a tool call ---- */

typedef struct conn conn;
typedef struct call call;
struct call {
	conn *cn;
	char *id;				// the request's id, as JSON
	char *token;			// the progressToken, as JSON, or NULL
	volatile bool cancelled;
	double started, last_progress;
	int progress;
	sbuf text;				// the summary for the agent
	char *image;			// base64 PNG of the last screenshot, or NULL
	bool is_error;
	bool unmet;				// a wait did not get what it waited for
	call *parent;			// the steps call this step runs in
	call *next;
};

/// Whether the call, or the steps call it runs in, was cancelled.
static inline bool call_cancelled(const call *c)
{
	for (; c; c = c->parent)
		if (c->cancelled)
			return true;
	return false;
}

void call_printf(call *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void call_error(call *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/// Send a progress notification now and then, if the client asked.
void call_progress(call *c, const char *what);
/// Whether the call should stop (cancelled, VM quitting).
bool call_stopped(call *c);

typedef bool (*tool_fn)(call *c, const json *args);
tool_fn tool_find(const char *name);
/// Check a call's arguments without running it (for steps).
bool tool_check(call *c, const char *name, const json *args);

/// The tool list (JSON array text) and the instructions.
const char *mcp_tools_json(void);
const char *mcp_instructions(void);
bool mcp_protocol_supported(const char *v);

/* ---- recording (record.c) ---- */
bool rec_active(void);
bool rec_frozen_locked(void);		// mcp_mu held
bool rec_holding(void);
bool rec_freezing(void);			// recording with freeze
void rec_tick(const uint8_t *vram, bool rev, bool tone, int lo, int hi);	// main thread
bool tool_record(call *c, const json *args);
void rec_finish(void);				// at exit

/* ---- screen helpers (tools.c) ---- */
static inline int frame_pixel(const frame_t *f, int x, int y)
{
	const uint8_t *w = f->vram + y * VRAM_ROW + (x >> 4) * 2;
	int bit = x & 15;
	int v = bit < 8 ? (w[1] >> bit) & 1 : (w[0] >> (bit - 8)) & 1;
	return v ^ f->rev;
}

void frame_get(frame_t *f);
/// Recent pictures of the screen (the last few changes), oldest first: those
/// up to change max_seq that appeared at or after guest tick min_tick.
int frame_history(uint64_t max_seq, uint64_t min_tick, frame_t *out, int max);
/// Pixels that differ between a and b, inside region (or everywhere) and
/// outside the ignore rectangles; bbox gets their bounding box.
int frame_diff(const frame_t *a, const frame_t *b, const rect *region,
		const rect *ign, int n_ign, rect *bbox);

/// The VM's name (folder) and directory.
extern char *mcp_vm_name;
extern char *mcp_vm_dir;

#endif
