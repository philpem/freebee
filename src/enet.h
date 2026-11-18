/*
 * enet - the AT&T 3B1 Ethernet expansion board (an AMD LANCE, card id
 * 0x1003), for the Wollongong WIN/3B TCP/IP driver. See enet.c.
 */

#ifndef _ENET_H
#define _ENET_H

#include <stdint.h>
#include <stdbool.h>

// Set up the board from the [network] configuration (once, at start)
void enet_init(void);
// The machine's reset
void enet_reset(void);
// Before FreeBee exits
void enet_done(void);

// Is this address in the board's expansion slot window?
bool enet_window(uint32_t address);
// CPU access to the window (bits: 8, 16 or 32)
uint32_t enet_read(uint32_t address, int bits);
void enet_write(uint32_t address, uint32_t data, int bits);

// After each CPU slice of this many cycles: the LANCE's transmit poll, and
// a frame from the host when the receive ring has room for it
void enet_tick(uint32_t cycles);
// The interrupt level the board asks for: 0 (none), 1 or 5
int enet_irq(void);

// The host side of the network: call it often (about every millisecond)
void enet_host_poll(void);
// A frame from the host network for the guest (without its FCS)
void enet_host_receive(const uint8_t *frame, int length);

// One line about the board, for vm-info; NULL if there is no board
const char *enet_info(void);

#endif
