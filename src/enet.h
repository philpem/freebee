/*
 * enet - the AT&T 3B1 Ethernet expansion board (an AMD LANCE, card id
 * 0x1003), for the Wollongong WIN/3B TCP/IP driver. See enet.c.
 */

#ifndef _ENET_H
#define _ENET_H

#include <stdint.h>
#include "expcard.h"

// The card type: [expansion] slotN = "ethernet"
extern const EXPCARD_TYPE enet_card;

// For MCP vm-info: the board, or NULL if there is none
const char *enet_info(void);

// A frame from the host network for the guest (without its FCS)
void enet_host_receive(const uint8_t *frame, int length);

#endif
