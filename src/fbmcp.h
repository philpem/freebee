/*
 * fbmcp - FreeBee's MCP server: what the rest of the emulator calls.
 *
 * Every FreeBee serves MCP (the Model Context Protocol) on a private Unix
 * socket, /tmp/freebee-<uid>/<pid>.sock, and describes itself next to it
 * in <pid>.json. "freebee -mcp-bridge" is the MCP server an agent starts:
 * it finds the running VMs there and passes each tool call to one.
 * See MCP.md.
 */
#ifndef _FBMCP_H
#define _FBMCP_H

#include <stdint.h>
#include <stdbool.h>

/// Start the server (after the disks are loaded). False if it is off.
bool fbmcp_init(void);
/// Finish a recording, remove the socket and the registry file.
void fbmcp_done(void);

/// The 60 Hz guest tick, from the main loop.
void fbmcp_tick(bool vram_changed);
/// The main loop must not run the CPU (paused, stopped by the debugger, or
/// frozen between calls while recording).
bool fbmcp_paused(void);
/// Called by the main loop every few milliseconds while paused.
void fbmcp_service(void);
/// Asked to quit.
bool fbmcp_quit(void);
/// Asked to reset the machine (true once per request).
bool fbmcp_reset_wanted(void);

/// The guest sent a byte on the serial port (tty000).
void fbmcp_serial_tx(uint8_t b);
/// Bytes for the guest to read from the serial port; returns the count.
int fbmcp_serial_rx(uint8_t *buf, int max);

/// The debugger's per-instruction hook (see m68kconf.h): nonzero to stop
/// before the instruction at pc.
extern volatile int fbmcp_dbg_active;
int fbmcp_dbg_hook(unsigned int pc);
/// A breakpoint or step stopped the CPU in this time slice.
bool fbmcp_cpu_stopped(void);

/// "freebee -mcp-bridge": the MCP server on stdin and stdout.
int fbmcp_bridge_main(int argc, char **argv);

/* in main.c */
int freebee_floppy_insert(const char *path);
void freebee_floppy_eject(void);
const char *freebee_floppy_path(void);
int freebee_hd2_insert(const char *path);
const char *freebee_hd2_path(void);
void freebee_reset(void);

#endif
