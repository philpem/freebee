#ifndef _CLOSEAPP_H
#define _CLOSEAPP_H

#include <stdbool.h>

/*
 * Alt-F9 (Option-F9 on a Mac): close the program in the front window, also
 * one that hangs, as "kill -9" of its process group would. Never the
 * Office, the window manager or the status line.
 */

/// close the program in the window that has the keyboard, as
/// soon as the CPU is at a safe place (within 2 seconds)
void closeapp_request(void);
/// at every 60 Hz tick, between instructions
void closeapp_tick(void);

#endif
