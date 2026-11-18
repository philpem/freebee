/*
 * stub - the MCP calls for builds without the MCP server (Windows: it needs
 * Unix sockets). The rest of FreeBee calls these as if the server were off.
 */
#include <stdio.h>
#include "../fbmcp.h"

volatile int fbmcp_dbg_active = 0;

bool fbmcp_init(void) { return false; }
void fbmcp_done(void) { }
void fbmcp_tick(bool vram_changed) { (void)vram_changed; }
bool fbmcp_paused(void) { return false; }
void fbmcp_service(void) { }
bool fbmcp_quit(void) { return false; }
bool fbmcp_reset_wanted(void) { return false; }
void fbmcp_serial_tx(uint8_t b) { (void)b; }
int fbmcp_serial_rx(uint8_t *buf, int max) { (void)buf; (void)max; return 0; }
int fbmcp_dbg_hook(unsigned int pc) { (void)pc; return 0; }
bool fbmcp_cpu_stopped(void) { return false; }

int fbmcp_bridge_main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	fprintf(stderr, "freebee: this build has no MCP server\n");
	return 1;
}
