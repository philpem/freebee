/*
 * wheel - the mouse wheel scrolls a 3B1 window, as its scroll arrows do.
 *
 * The 3B1 mouse has no wheel. A window that can scroll has arrows in its
 * border (BORDVSCROLL in sys/window.h). A click on one sends the window's
 * program Roll Up or Roll Down, the keys Shift+Up and Shift+Down: the
 * Office's list moves one line, and the highlight stays where it was on
 * the screen. So each wheel step presses Shift+Up or Shift+Down, but only
 * while the window that has the keyboard has those arrows and the pointer
 * is over it. A shell window has no arrows, and there the keys would be
 * typed into the command line.
 *
 * The window that has the keyboard is keywp, in the window driver that
 * UNIX 3.51 loads at boot (/etc/lddrv/wind). Its wdtotty() is
 *	struct tty *wdtotty() { return keywp ? keywp->w_tp : NULL; }
 * and its code holds no address but keywp's, so FreeBee finds keywp by
 * looking for that code, wherever the driver was loaded.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "memory.h"
#include "keys.h"
#include "pointer.h"
#include "wheel.h"

// wdtotty() in UNIX 3.51m's window driver; words 3-4 and 7-8 are keywp
static const uint16_t wdtotty[] = {
	0x4E56, 0xFFFC,			// link a6,#-4
	0x4AB9, 0, 0,			// tst.l keywp
	0x6710,				// beq.s
	0x2079, 0, 0,			// movea.l keywp,a0
	0x2068, 0x0020,			// movea.l w_tp(a0),a0
	0x2008, 0x4E5E, 0x4E75,		// move.l a0,d0; unlk a6; rts
	0x91C8, 0x60F6,			// suba.l a0,a0; bra.s
};
#define N_CODE (sizeof(wdtotty) / sizeof(wdtotty[0]))

// struct windef (sys/wd.h): w_rec (ulx, uly, lrx, lry) at 0, w_uflags at 42
#define W_UFLAGS	42
#define BORDVSCROLL	0x8

// Roll steps still to press: + up, - down. A few at most, so the window
// stops soon after the wheel does.
#define MAX_STEPS 3
static float steps;
// where wdtotty() and keywp are (0: not found), and the ticks until the
// next look for them
static uint32_t code_at, keywp_at;
static int look_wait;

// keywp's address, if wdtotty() is at a; else 0
static uint32_t keywp_from(uint32_t a)
{
	uint16_t w[N_CODE];
	for (size_t i = 0; i < N_CODE; i++) {
		if (!mem_peek16(a + 2 * i, &w[i]))
			return 0;
		bool const addr = i == 3 || i == 4 || i == 7 || i == 8;
		if (!addr && w[i] != wdtotty[i])
			return 0;
	}
	if (w[3] != w[7] || w[4] != w[8])
		return 0;
	uint32_t const p = ((uint32_t)w[3] << 16) | w[4];
	return (p & 1) || p < 0x400 || p >= 0x80000 ? 0 : p;
}

// look through the kernel (virtual 0 to 512K) for wdtotty()
static void find_keywp(void)
{
	for (uint32_t a = 0x400; a < 0x80000; a += 2) {
		uint32_t const p = keywp_from(a);
		if (p) {
			code_at = a;
			keywp_at = p;
			return;
		}
	}
}

// the window that has the keyboard has scroll arrows, and the pointer is
// over it
static bool window_scrolls(void)
{
	if (keywp_at && keywp_from(code_at) != keywp_at)
		keywp_at = 0;		// another kernel is running: look again
	if (!keywp_at) {
		if (look_wait > 0)
			return false;
		look_wait = 60;		// the next look in a second at the soonest
		find_keywp();
		if (!keywp_at)
			return false;
	}
	uint16_t hi, lo, uflags, r[4];
	if (!mem_peek16(keywp_at, &hi) || !mem_peek16(keywp_at + 2, &lo))
		return false;
	uint32_t const wp = ((uint32_t)hi << 16) | lo;
	if (!wp || (wp & 1) || wp >= 0x80000 ||
			!mem_peek16(wp + W_UFLAGS, &uflags) || !(uflags & BORDVSCROLL))
		return false;
	for (int i = 0; i < 4; i++)
		if (!mem_peek16(wp + 2 * i, &r[i]))
			return false;
	int x, y;
	return !pointer_get(&x, &y) ||
		(x >= r[0] && x < r[2] && y >= r[1] && y < r[3]);
}

void wheel_scroll(float n)
{
	steps += n;
	if (steps > MAX_STEPS)
		steps = MAX_STEPS;
	else if (steps < -MAX_STEPS)
		steps = -MAX_STEPS;
}

void wheel_tick(void)
{
	if (look_wait > 0)
		look_wait--;
	if (steps > -1 && steps < 1)
		return;
	// one Roll at a time, after the keys before it
	if (keys_pending())
		return;
	bool const up = steps > 0;
	if (!window_scrolls()) {
		steps = 0;		// nothing here scrolls: forget the wheel
		return;
	}
	steps -= up ? 1 : -1;
	key_chord const c = { .code = { KEY_SHIFT, up ? KEY_UP : KEY_DOWN }, .n = 2 };
	keys_push(&c);
}

void wheel_reset(void)
{
	steps = 0;
	code_at = keywp_at = 0;
	look_wait = 0;
}
