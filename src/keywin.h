#ifndef _KEYWIN_H
#define _KEYWIN_H

#include <stdint.h>

/*
 * keywin - the 3B1 window that has the keyboard, in the window driver of
 * UNIX 3.51 (see keywin.c). For host keys and the mouse wheel that act on
 * the front window.
 */

/// The address of keywp, the window driver's pointer to the window that
/// has the keyboard (struct windef *); 0 if the driver was not found. A
/// look reads the whole kernel, so callers should not call it at every
/// tick while it returns 0.
uint32_t keywin_keywp(void);

/// The window that has the keyboard (its struct windef), or 0
uint32_t keywin_window(void);

/// struct windef (sys/wd.h): w_rec (ulx, uly, lrx, lry: shorts) at 0,
/// w_tp (struct tty *) at 32, w_uflags (short) at 42
#define WD_REC		0
#define WD_TP		32
#define WD_UFLAGS	42
/// w_uflags: the window has scroll arrows in its border
#define WD_BORDVSCROLL	0x8

#endif
