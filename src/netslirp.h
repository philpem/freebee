/*
 * netslirp - user mode networking for the Ethernet board, with libslirp
 * (the network QEMU calls "user"): the guest is 10.0.2.15, the host
 * 10.0.2.2, DNS 10.0.2.3; the guest reaches the host's networks through
 * NAT, and forwarded host ports reach the guest.
 */

#ifndef _NETSLIRP_H
#define _NETSLIRP_H

#include <stdint.h>
#include <stdbool.h>

// Start it. hostfwd: NULL or a comma separated list of forwards, as in
// QEMU: "tcp:127.0.0.1:2323-:23,udp::5000-10.0.2.15:5000" (an empty host
// address is every address of the host; an empty guest address is
// 10.0.2.15). Returns false if it could not start.
bool netslirp_init(const char *hostfwd);
// A frame from the guest (without its FCS)
void netslirp_send(const uint8_t *frame, int length);
// Service the sockets and timers; frames for the guest go to
// enet_host_receive()
void netslirp_poll(void);
void netslirp_done(void);

#endif
