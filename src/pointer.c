/*
 * pointer - where the 3B1's mouse pointer is, and the seamless mouse.
 *
 * The UNIX kernel's mscan adds each mouse packet to the pointer, one screen
 * pixel a count with no acceleration, and keeps it in 0..719 across and
 * 0..694 down. Every packet FreeBee sends goes through mouse_event and so
 * through pointer_moved, so the place is known once the pointer has been
 * pushed into a corner. Better still: mscan's code shows where the kernel
 * keeps its own copy (msinfo), and that copy is read back whenever every
 * packet has got there, in case one went missing.
 *
 * The seamless mouse: over the window the 3B1's pointer follows the host's,
 * so the mouse needs no F10 grab. pointer_tick sends one packet a 60 Hz
 * tick (127 counts at most each way) toward the host pointer. A button
 * change waits in a queue with the place it happened at, so a quick click
 * lands where it was made and is not lost.
 *
 * What this takes from the guest (UNIX 3.51): the pointer moves one pixel
 * a count, with no acceleration, and stops at the edges, so pushing it
 * into a corner makes its place known with any kernel that works so. The
 * look at the kernel's copy (msinfo) is only a check: if mscan's code is
 * not found, the corner and the counts are used alone.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "state.h"
#include "memory.h"
#include "keyboard.h"
#include "pointer.h"

static int clamp(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/* ---- the place ---- */

// Where the pointer can be: x in xlo..xhi, y in ylo..yhi. The kernel stops
// it at the edges, so a push into a corner makes both one point.
static int xlo, xhi = PTR_XMAX, ylo, yhi = PTR_YMAX;
// the buttons the guest got last, and the ticks since a
// packet was last on its way to the kernel
static int last_buttons, quiet;

void pointer_moved(int dx, int dy, int buttons)
{
	xlo = clamp(xlo + dx, 0, PTR_XMAX);
	xhi = clamp(xhi + dx, 0, PTR_XMAX);
	ylo = clamp(ylo + dy, 0, PTR_YMAX);
	yhi = clamp(yhi + dy, 0, PTR_YMAX);
	last_buttons = buttons;
	quiet = 0;
}

void pointer_forget(void)
{
	xlo = ylo = 0;
	xhi = PTR_XMAX;
	yhi = PTR_YMAX;
}

bool pointer_get(int *x, int *y)
{
	bool const known = xlo == xhi && ylo == yhi;
	*x = (xlo + xhi) / 2;
	*y = (ylo + yhi) / 2;
	return known;
}

static void set_place(int x, int y)
{
	xlo = xhi = clamp(x, 0, PTR_XMAX);
	ylo = yhi = clamp(y, 0, PTR_YMAX);
}

/* ---- the kernel's own copy ---- */

// mscan's clamps in UNIX 3.51m (at 0x1a040). They hold no addresses, so
// they are the same wherever the kernel was linked:
//	cmpi.l #719,(a2); ble; move.l #719,(a2)
//	tst.l 4(a2); bge; clr.l 4(a2); bra
//	cmpi.l #694,4(a2); ble; move.l #694,4(a2)
// a2 is msinfo { long x, y; char buttons; }, from a movea.l #msinfo,a2
// near mscan's top. klow, the keyboard interrupt, gives mscan each byte as
// it comes in, so msinfo is new as soon as the guest has read a packet.
static const uint16_t clamps[] = {
	0x0C92, 0x0000, 0x02CF, 0x6F06, 0x24BC, 0x0000, 0x02CF,
	0x4AAA, 0x0004, 0x6C06, 0x42AA, 0x0004, 0x6012,
	0x0CAA, 0x0000, 0x02B6, 0x0004, 0x6F08, 0x257C, 0x0000, 0x02B6, 0x0004,
};
#define N_CLAMPS (sizeof(clamps) / sizeof(clamps[0]))

static enum { MS_LOOK, MS_FOUND, MS_NONE } ms_state;
static uint32_t ms_code, ms_info;
static bool was_enabled;

static bool clamps_at(uint32_t a)
{
	uint16_t w;
	for (size_t i = 0; i < N_CLAMPS; i++)
		if (!mem_peek16(a + 2 * i, &w) || w != clamps[i])
			return false;
	return true;
}

// look through the kernel (virtual 0 to 512K) for mscan
static void find_msinfo(void)
{
	ms_state = MS_NONE;
	for (uint32_t a = 0x400; a < 0x80000; a += 2) {
		if (!clamps_at(a))
			continue;
		for (uint32_t b = a - 6; a - b < 0x100; b -= 2) {
			uint16_t op, hi, lo;
			if (!mem_peek16(b, &op) || op != 0x247C ||
					!mem_peek16(b + 2, &hi) || !mem_peek16(b + 4, &lo))
				continue;
			uint32_t const info = ((uint32_t)hi << 16) | lo;
			if (!(info & 1) && info < 0x80000) {
				ms_code = a;
				ms_info = info;
				ms_state = MS_FOUND;
			}
			return;
		}
		return;
	}
}

// Read the kernel's copy. Called once every packet has got there: it puts
// right a count that went wrong.
static void sync_kernel(void)
{
	if (ms_state == MS_LOOK)
		find_msinfo();
	if (ms_state != MS_FOUND)
		return;
	if (!clamps_at(ms_code)) {
		// another kernel is running: look again
		ms_state = MS_LOOK;
		return;
	}
	uint16_t w[4];
	for (int i = 0; i < 4; i++)
		if (!mem_peek16(ms_info + 2 * i, &w[i]))
			return;
	int32_t const x = (int32_t)(((uint32_t)w[0] << 16) | w[1]);
	int32_t const y = (int32_t)(((uint32_t)w[2] << 16) | w[3]);
	if (x >= 0 && x <= PTR_XMAX && y >= 0 && y <= PTR_YMAX)
		set_place(x, y);
}

/* ---- the seamless mouse ---- */

// a button change, and where the host pointer was
typedef struct { int x, y, buttons; } spot;
#define SPOTS 64
static spot spots[SPOTS];
static int spot_head, spot_count;
// the host pointer, in screen pixels, and its buttons
static int host_x, host_y, host_buttons;
// the host pointer moved: go after it
static bool following;

void pointer_track(int x, int y)
{
	host_x = clamp(x, 0, PTR_XMAX);
	host_y = clamp(y, 0, 347);
	following = true;
}

void pointer_buttons(int buttons)
{
	if (buttons == host_buttons)
		return;
	host_buttons = buttons;
	following = true;
	spot const s = { host_x, host_y, buttons };
	if (spot_count < SPOTS)
		spots[(spot_head + spot_count++) % SPOTS] = s;
	else
		spots[(spot_head + SPOTS - 1) % SPOTS] = s;	// full: the last one wins
}

void pointer_stop(void)
{
	following = false;
	spot_count = 0;
}

void pointer_tick(void)
{
	KEYBOARD_STATE *const ks = &state.kbd;
	bool const on = ks->mouse_enabled;

	// a program turned the mouse on: if mscan was not found, look again
	if (on && !was_enabled && ms_state == MS_NONE)
		ms_state = MS_LOOK;
	was_enabled = on;
	// the kernel's copy, 2 ticks after the guest read the last packet
	if (ks->mouse_left)
		quiet = 0;
	else if (quiet < 1000)
		quiet++;
	if (quiet >= 2 && (ms_state == MS_FOUND || on))
		sync_kernel();

	if (!on) {
		// nobody listens: nothing to follow
		pointer_stop();
		return;
	}
	// one packet a tick: a move toward the next button change, or toward
	// the host pointer; then the button change
	while (spot_count || following) {
		int tx = host_x, ty = host_y, x, y;
		if (spot_count) {
			tx = spots[spot_head].x;
			ty = spots[spot_head].y;
		}
		if (!pointer_get(&x, &y)) {
			// not known: wait for the kernel's copy, or else push the
			// pointer into the top left corner
			if (ms_state != MS_FOUND || quiet >= 2)
				mouse_event(ks, -127, -127, last_buttons);
			return;
		}
		int const dx = clamp(tx - x, -127, 127), dy = clamp(ty - y, -127, 127);
		if (dx || dy) {
			mouse_event(ks, dx, dy, last_buttons);	// a full buffer: next tick
			return;
		}
		if (!spot_count) {
			following = false;
			return;
		}
		int const b = spots[spot_head].buttons;
		bool const change = b != last_buttons;
		if (change && !mouse_event(ks, 0, 0, b))
			return;
		spot_head = (spot_head + 1) % SPOTS;
		spot_count--;
		if (change)
			return;
	}
}
