#ifndef _CONTROL_H
#define _CONTROL_H

#include <stdbool.h>

/*
 * control - a command socket for scripting FreeBee (typing, floppies,
 * screenshots, speed). See control.c for the commands.
 */

// Open the control socket if one is configured. Returns true if it is open.
bool control_init(void);
// Called at every 60Hz tick: reads commands, feeds queued keys.
void control_tick(void);
// Commands only, no keys (while MCP has the CPU paused).
void control_poll(void);
void control_done(void);

// true while "speed max" is on: the main loop does not wait for real time.
extern bool control_speed_max;
// set by the "quit" command
extern bool control_quit;

// in main.c
int freebee_floppy_insert(const char *path);
void freebee_floppy_eject(void);
const char *freebee_floppy_path(void);
int freebee_hd2_insert(const char *path);

#endif
