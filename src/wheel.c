/*
 * wheel - the mouse wheel scrolls a 3B1 window, as its scroll arrows do.
 *
 * The 3B1 mouse has no wheel. A window that can scroll has arrows in its
 * border (BORDVSCROLL in sys/window.h). A click on one sends the window's
 * program Roll Up or Roll Down, the keys Shift+Up and Shift+Down: the
 * Office's list moves one line, and the highlight stays where it was on
 * the screen. So each wheel step presses Shift+Up or Shift+Down, but only
 * while the window that has the keyboard has those arrows (keywin.c). A
 * shell window has no arrows, and there the keys would be typed into the
 * command line, so there the wheel does nothing.
 *
 * This is specific to UNIX 3.51: with other software keywin finds no
 * window, and the wheel does nothing.
 */
#include <stdbool.h>
#include <stdint.h>
#include "state.h"
#include "memory.h"
#include "keywin.h"
#include "wheel.h"

// 3B1 scan codes
#define KEY_SHIFT	0x48
#define KEY_UP		0x01
#define KEY_DOWN	0x0b

// Roll steps still to press: + up, - down. A few at most, so the window
// stops soon after the wheel does.
#define MAX_STEPS 3
static float steps;
// the ticks until the next look for the window driver
static int look_wait;
// the Roll key being pressed: 0 none; ticks since it started
static uint8_t roll_key;
static int roll_ticks;

// the window that has the keyboard has scroll arrows
static bool window_scrolls(void)
{
	if (!keywin_keywp()) {
		look_wait = 60;		// the next look in a second at the soonest
		return false;
	}
	uint32_t const wp = keywin_window();
	uint16_t uflags;
	return wp && mem_peek16(wp + WD_UFLAGS, &uflags) && (uflags & WD_BORDVSCROLL);
}

static void set_key(uint8_t code, bool down)
{
	state.kbd.keystate[code] = down;
	state.kbd.update_flag = true;
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
	// A Roll key: Shift goes down one tick early (the keyboard sends the
	// keys that are down in scan code order, and Up is below Shift), the
	// keys stay down two ticks, then one tick with none
	if (roll_key) {
		roll_ticks++;
		if (roll_ticks == 1)
			set_key(roll_key, true);
		else if (roll_ticks == 3) {
			set_key(roll_key, false);
			set_key(KEY_SHIFT, false);
		} else if (roll_ticks == 4)
			roll_key = 0;
		return;
	}
	if (steps > -1 && steps < 1)
		return;
	bool const up = steps > 0;
	if (look_wait > 0 || !window_scrolls()) {
		steps = 0;		// nothing here scrolls: forget the wheel
		return;
	}
	steps -= up ? 1 : -1;
	roll_key = up ? KEY_UP : KEY_DOWN;
	roll_ticks = 0;
	set_key(KEY_SHIFT, true);
}
