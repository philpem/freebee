/*
 * netfix - two optional fixes for 1986 network programs, in the frames
 * the host network sends to the 3B1 (see netfix.c). Both are off by
 * default.
 */

#ifndef _NETFIX_H
#define _NETFIX_H

#include <stdbool.h>
#include <stdint.h>

/// Turn the fixes on or off: [network] fix_telnet_status, fix_ftp_port
void netfix_config(bool telnet_status, bool ftp_port);
/// Look at a frame for the guest, and change it in place if a fix
/// applies. The length never changes.
void netfix_frame(uint8_t *frame, int len);
/// Forget every TCP stream
void netfix_reset(void);

#endif
