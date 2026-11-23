#ifndef _POINTER_H
#define _POINTER_H

#include <stdbool.h>

/*
 * Where the 3B1's mouse pointer is, and the seamless mouse: over the
 * window, the 3B1's pointer follows the host's, with no F10 grab.
 */

/// The kernel keeps the pointer in 0..PTR_XMAX across and 0..PTR_YMAX
/// down; from row 348 down it is below the screen.
#define PTR_XMAX 719
#define PTR_YMAX 694

/// mouse_event put a packet in the keyboard buffer
void pointer_moved(int dx, int dy, int buttons);
/// mouse bytes the guest had not read were thrown away
void pointer_forget(void);

/// where the pointer is; false while it is not known
bool pointer_get(int *x, int *y);

/// seamless: the host pointer is at x,y (screen pixels)
void pointer_track(int x, int y);
/// seamless: the host's buttons changed (MOUSE_BUTTON_*)
void pointer_buttons(int buttons);
/// stop following the host pointer
void pointer_stop(void);
/// at every 60 Hz tick: one packet toward the host pointer,
/// and a look at the kernel's own copy
void pointer_tick(void);

#endif
