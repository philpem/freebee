/*
 * lance - the AMD Am7990 LANCE Ethernet controller.
 *
 * Ported to C for FreeBee from MAME's src/devices/machine/am79c90.cpp and
 * am79c90.h by Patrick Mackinlay, with the receive changes Visionik made
 * for the AT&T 3B1 in github.com/visionik/mame (branch att-3b1). The licence
 * of that code, and of this port, is the BSD 3-Clause licence in lance.c.
 */

#ifndef _LANCE_H
#define _LANCE_H

#include <stdint.h>
#include <stdbool.h>

// CSR0 bits
#define LANCE_CSR0_INIT	0x0001	// initialize
#define LANCE_CSR0_STRT	0x0002	// start
#define LANCE_CSR0_STOP	0x0004	// stop
#define LANCE_CSR0_TDMD	0x0008	// transmit demand
#define LANCE_CSR0_TXON	0x0010	// transmitter on
#define LANCE_CSR0_RXON	0x0020	// receiver on
#define LANCE_CSR0_INEA	0x0040	// interrupt enable
#define LANCE_CSR0_INTR	0x0080	// interrupt flag
#define LANCE_CSR0_IDON	0x0100	// initialization done
#define LANCE_CSR0_TINT	0x0200	// transmitter interrupt
#define LANCE_CSR0_RINT	0x0400	// receiver interrupt
#define LANCE_CSR0_MERR	0x0800	// memory error
#define LANCE_CSR0_MISS	0x1000	// missed packet
#define LANCE_CSR0_CERR	0x2000	// collision error
#define LANCE_CSR0_BABL	0x4000	// babble
#define LANCE_CSR0_ERR	0x8000	// error

typedef struct {
	// bus master access to the host's memory: a 16-bit word at an even
	// 24-bit address; mem_mask picks the bytes a write changes
	void *opaque;
	uint16_t (*dma_in)(void *opaque, uint32_t address);
	void (*dma_out)(void *opaque, uint32_t address, uint16_t data, uint16_t mem_mask);
	// a frame for the wire, without its FCS; returns its length, or 0 if
	// it could not be sent (the chip then reports loss of carrier)
	int (*send)(void *opaque, const uint8_t *buf, int length);

	uint16_t rap;
	uint16_t csr[4];
	uint16_t mode;
	uint64_t logical_addr_filter;
	uint8_t physical_addr[6];

	uint32_t rx_ring_base;
	uint8_t rx_ring_mask;
	uint8_t rx_ring_pos;
	uint16_t rx_md[4];

	uint32_t tx_ring_base;
	uint8_t tx_ring_mask;
	uint8_t tx_ring_pos;
	uint16_t tx_md[4];

	bool idon;
	// STRT or TDMD asked for a transmit poll now, not at the next 1.6 ms
	bool tx_demand;

	// internal loopback buffer
	uint8_t lb_buf[36];
	int lb_length;

	// Host frames come without the FCS and may be shorter than the minimum.
	// They are padded to 60 bytes and get an FCS here, as a frame off the
	// wire has, before the receive path checks it.
	uint8_t rx_ext_buf[1518];
} LANCE;

// The transmit ring is polled this often (in 10 MHz CPU cycles: 1.6 ms)
#define LANCE_TX_POLL_CYCLES	16000

void lance_init(LANCE *l, void *opaque,
		uint16_t (*dma_in)(void *opaque, uint32_t address),
		void (*dma_out)(void *opaque, uint32_t address, uint16_t data, uint16_t mem_mask),
		int (*send)(void *opaque, const uint8_t *buf, int length));

// Hardware reset (also what writing STOP does)
void lance_reset(LANCE *l);

// Register data port (port 0, RDP) and address port (port 1, RAP).
// A read of CSR1-3 while the chip runs is not decoded: *open is set.
uint16_t lance_read(LANCE *l, int port, bool *open);
void lance_write(LANCE *l, int port, uint16_t data);

// Clear CSR0 cause bits (IDON TINT RINT MERR MISS CERR BABL) as a driver
// does by writing them, leaving INEA and RAP as they are
void lance_ack(LANCE *l, uint16_t bits);

// The /INTR pin: true while it is asserted
bool lance_intr(const LANCE *l);

// Poll the transmit ring once. Returns true if a frame went out; poll again
// then, for the next one goes out back to back.
bool lance_tx_poll(LANCE *l);

// Can the receiver take this frame (without its FCS) now? 1 yes, 0 not yet
// (it needs more free receive buffers than the ring holds now), -1 never
// (the receiver is off, or the frame is not for this station)
int lance_rx_ready(LANCE *l, const uint8_t *buf, int length);

// A frame off the wire, without its FCS. Returns what the receiver did:
// > 0 stored (bytes), 0 dropped (runt or loopback), -1 discarded, -2 missed
int lance_receive(LANCE *l, const uint8_t *buf, int length);

#endif
