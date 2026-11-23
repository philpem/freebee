#ifndef _WHEEL_H
#define _WHEEL_H

/*
 * The mouse wheel: it scrolls a 3B1 window that has scroll arrows in its
 * border (such as the Office), as a click on the arrows does.
 */

/// the wheel turned; steps > 0 up (away from the user)
void wheel_scroll(float steps);
/// at every 60 Hz tick: press or let go of the Roll keys
void wheel_tick(void);

#endif
