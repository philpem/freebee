/*
 * enet - the AT&T 3B1 Ethernet expansion board: an AMD Am7990 LANCE (see
 * lance.c), a control register, a station address PROM and a "status ring"
 * in main memory, card id 0x1003. The driver is Wollongong's WIN/3B
 * (/etc/lddrv/ether).
 *
 * Ported to C for FreeBee from Visionik's AT&T 3B1 work in MAME,
 * github.com/visionik/mame, branch att-3b1 (commit 4ca77493, 2026-10-20):
 *  - src/mame/att/unixpc_enet.h and unixpc_slot.h: the board registers,
 *    the station address and the card identity bytes (the identity bytes
 *    and the card reset are in expcard.c, for every card).
 *    license:BSD-3-Clause, copyright-holders:Visionik (licence text below).
 *  - src/mame/att/unixpc_m.cpp, unixpc_enet_device: how the board answers
 *    the LANCE's interrupt, its register latch and its DMA through the page
 *    map. license:GPL-2.0+, copyright-holders:Dirk Best, R. Belmont,
 *    Visionik.
 * As a whole this file is part of FreeBee, under its GNU GPL version 3.
 *
 * Changes from MAME, from the WIN/3B driver's own code (disassembled from
 * /etc/lddrv/ether on UNIX 3.51):
 *  - The status ring is 0x200 bytes: enintr wraps NextCSR at StatusRing +
 *    0x200 (eninit fills 0x400 bytes with 0xFFFF, but only that much is
 *    used). MAME wrapped at 0x400, so the two drifted apart after 256
 *    words, and it read NextCSR at a fixed address to make up for it.
 *    Here the board wraps where the driver does, and no driver address is
 *    needed (the driver loads at a different address on every system).
 *  - One ring word per cause. enrint takes exactly one frame per RINT word,
 *    and the TINT code reclaims exactly one frame per TINT word, so a word
 *    that covers two frames leaves a frame behind for good; an extra word
 *    makes it reset the board (RINT) or walk the whole ring (TINT). The
 *    LANCE here does one frame at a time, and the board posts each event
 *    before the next one starts.
 *  - The board's interrupt request is dropped when the driver has already
 *    taken every word: enintr returns at once, without reading the control
 *    register, when *NextCSR is empty, so a request left over from words
 *    it drained in the same pass would interrupt it forever.
 *  - Frames from the host wait on the host side until the receive ring has
 *    buffers for all of a frame (a frame needs up to 12 of the driver's
 *    128-byte buffers). The real board would miss or truncate it, and the
 *    driver resets the board on both.
 *
 * The BSD 3-Clause licence of Visionik's unixpc_enet.h and unixpc_slot.h:
 *
 * Copyright (c) 2026 Visionik
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "enet.h"
#include "expcard.h"
#include "lance.h"
#include "memory.h"
#include "fbconfig.h"
#ifdef HAVE_SLIRP
#include "netslirp.h"
#endif

// The Ethernet driver asks the kernel's slot table for this id
#define ENET_ID			0x1003

// Board registers, offsets in the window (unixpc_enet.h)
#define ENET_RDP		0x000000	// LANCE data port
#define ENET_RAP		0x000002	// LANCE address port
#define ENET_CTL		0x000004	// control
#define ENET_LATCH		0x000006	// data latch (read), status ring address (write)
#define ENET_MAC0		0x0003ffcb	// station address octet 0
#define ENET_MAC_LSB		0x0003ffc1	// octet 5
#define ENET_RING_SHIFT		9
#define ENET_STATUS_RING_BYTES	0x200

// Control register bits
#define ENET_DMAEN		0x01		// DMA enable
#define ENET_OTHER		0x02		// 0 = Ethernet, 1 = the other interface
#define ENET_INT1		0x04		// interrupt level 1 (0 = level 5)
#define ENET_CTL_MASK		0x0f

// CSR0 cause bits the board copies into the status ring
#define ENET_CSR0_CAUSES	0x7f00		// BABL|CERR|MISS|MERR|RINT|TINT|IDON

#define IO_OPEN			0xffff

// Frames from the host waiting for the guest's receive ring
#define RXQ_LEN			256
#define FRAME_MAX		1514

typedef struct {
	int length;
	uint8_t data[FRAME_MAX];
} FRAME;

static struct {
	EXPCARD card;			// first: the bus hands this back
	bool present;
	uint8_t mac[6];

	// the board (Visionik's enet_board)
	uint8_t control;
	uint16_t ring;			// status ring address >> 9
	uint16_t ring_prod;		// next word the board writes (byte offset)
	uint16_t ring_cons;		// first word the driver has not taken
	uint16_t latch;
	bool irq;

	LANCE lance;
	uint32_t tx_poll_cycles;

	FRAME *rxq;
	int rxq_head, rxq_count;

	bool host;			// the host network is up
	const char *backend;
	unsigned long tx_frames, rx_frames, rx_dropped, dma_faults;
	bool debug;			// FREEBEE_ENET_DEBUG=1: log frames and ring words
} enet;

// One line about a frame, for the debug log
static void log_frame(const char *what, const uint8_t *f, int length)
{
	if (length < 14) {
		fprintf(stderr, "enet: %s %d bytes\n", what, length);
		return;
	}
	unsigned const type = (f[12] << 8) | f[13];
	if (type == 0x0800 && length >= 34) {
		const uint8_t *ip = f + 14;
		int const hl = (ip[0] & 15) * 4;
		unsigned const proto = ip[9];
		if (proto == 6 && length >= 14 + hl + 20) {
			const uint8_t *t = ip + hl;
			unsigned const fl = t[13];
			fprintf(stderr, "enet: %s %d bytes tcp %d.%d.%d.%d:%u > %d.%d.%d.%d:%u seq %u ack %u win %u%s%s%s%s%s len %d\n",
					what, length, ip[12], ip[13], ip[14], ip[15], (t[0] << 8) | t[1],
					ip[16], ip[17], ip[18], ip[19], (t[2] << 8) | t[3],
					(unsigned)((t[4] << 24) | (t[5] << 16) | (t[6] << 8) | t[7]),
					(unsigned)((t[8] << 24) | (t[9] << 16) | (t[10] << 8) | t[11]),
					(t[14] << 8) | t[15],
					(fl & 2) ? " SYN" : "", (fl & 16) ? " ACK" : "", (fl & 8) ? " PSH" : "",
					(fl & 1) ? " FIN" : "", (fl & 4) ? " RST" : "",
					((ip[2] << 8) | ip[3]) - hl - (t[12] >> 4) * 4);
			return;
		}
		fprintf(stderr, "enet: %s %d bytes ip proto %u %d.%d.%d.%d > %d.%d.%d.%d\n", what, length, proto,
				ip[12], ip[13], ip[14], ip[15], ip[16], ip[17], ip[18], ip[19]);
		return;
	}
	if (type == 0x0806 && length >= 42) {
		const uint8_t *a = f + 14;
		fprintf(stderr, "enet: %s %d bytes arp %s %d.%d.%d.%d > %d.%d.%d.%d\n", what, length,
				a[7] == 1 ? "who-has" : "reply", a[14], a[15], a[16], a[17], a[24], a[25], a[26], a[27]);
		return;
	}
	fprintf(stderr, "enet: %s %d bytes type %04x\n", what, length, type);
}

/*
 * The station address PROM (unixpc_enet.h)
 */

// Octet 0 is at ENET_MAC0, the next ones 2 bytes lower each. Returns -1 if
// this is not a station address byte.
static int enet_mac_octet(uint32_t offset)
{
	uint32_t const addr = offset | 1;

	if (addr > ENET_MAC0 || addr < ENET_MAC_LSB)
		return -1;
	uint32_t const delta = ENET_MAC0 - addr;
	if (delta & 1)
		return -1;
	int const n = delta / 2;
	return (n < 6) ? n : -1;
}

/*
 * The LANCE's bus: guest logical addresses through the page map, like disk
 * DMA (unixpc_enet_device::dma_r and dma_w)
 */

static void dma_fault(uint32_t address, bool writing)
{
	if (enet.dma_faults++ < 10)
		fprintf(stderr, "enet: LANCE %s at 0x%06X: page not present\n",
				writing ? "write" : "read", address);
}

static uint16_t dma_in(void *opaque, uint32_t address)
{
	uint16_t data;

	(void)opaque;
	if (!mem_dma_read16(address, &data)) {
		dma_fault(address, false);
		return 0xffff;
	}
	return data;
}

static void dma_out(void *opaque, uint32_t address, uint16_t data, uint16_t mem_mask)
{
	(void)opaque;
	if (!mem_dma_write16(address, data, mem_mask))
		dma_fault(address, true);
}

static int lance_send(void *opaque, const uint8_t *buf, int length)
{
	(void)opaque;
	enet.tx_frames++;
	if (enet.debug)
		log_frame("out", buf, length);
#ifdef HAVE_SLIRP
	if (enet.host)
		netslirp_send(buf, length);
#else
	(void)buf;
#endif
	return length;
}

/*
 * The status ring (unixpc_enet_device::post_status_csr0)
 */

static uint32_t ring_address(void)
{
	return (uint32_t)enet.ring << ENET_RING_SHIFT;
}

static bool status_ring_armed(void)
{
	return enet.ring != 0 && (enet.control & ENET_DMAEN);
}

// Answer the LANCE's /INTR: copy CSR0 into the status ring, one word per
// cause, without INTR and ERR (enintr takes bit 7 for a fatal snapshot and
// matches the high byte exactly), then clear the causes, keeping INEA, and
// raise the board's interrupt. MISS alone is cleared without a word: the
// driver would reset the board for it.
static void service(void)
{
	static const uint16_t order[] = {
		LANCE_CSR0_IDON, LANCE_CSR0_TINT, LANCE_CSR0_RINT,
		LANCE_CSR0_BABL, LANCE_CSR0_MERR
	};
	LANCE *l = &enet.lance;

	if (!lance_intr(l) || !status_ring_armed())
		return;

	uint16_t const csr0 = l->csr[0];
	uint16_t const causes = csr0 & ENET_CSR0_CAUSES;
	bool posted = false;

	for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
		if (causes & order[i]) {
			if (enet.debug)
				fprintf(stderr, "enet: ring word %04x at +%03x\n", (csr0 & 0x007f) | order[i], enet.ring_prod);
			dma_out(NULL, ring_address() + enet.ring_prod, (csr0 & 0x007f) | order[i], 0xffff);
			enet.ring_prod = (enet.ring_prod + 2) & (ENET_STATUS_RING_BYTES - 1);
			posted = true;
		}
	}
	if (causes & LANCE_CSR0_IDON)
		fprintf(stderr, "enet: LANCE started, station %02x:%02x:%02x:%02x:%02x:%02x\n",
				l->physical_addr[0], l->physical_addr[1], l->physical_addr[2],
				l->physical_addr[3], l->physical_addr[4], l->physical_addr[5]);
	lance_ack(l, causes);
	if (posted)
		enet.irq = true;
}

// True while a cause the board has not posted yet is pending: the next
// frame waits, so that each one gets its own word
static bool cause_pending(void)
{
	return (enet.lance.csr[0] & (LANCE_CSR0_IDON | LANCE_CSR0_TINT | LANCE_CSR0_RINT)) != 0;
}

/*
 * Board registers (unixpc_enet_device::exp_r and exp_w)
 */

static void board_reset(void)
{
	enet.control = 0;
	enet.ring = 0;
	enet.ring_prod = enet.ring_cons = 0;
	enet.latch = 0;
	enet.irq = false;
}

static uint16_t read16(uint32_t offset)
{
	bool open;
	int const octet = enet_mac_octet(offset);
	if (octet >= 0)
		return 0xff00 | enet.mac[octet];

	// A read of RDP or RAP does not return the LANCE's value on that cycle:
	// it is captured into the data latch, and the next read of the latch
	// (+6) delivers it. Writes go straight to the chip.
	switch (offset) {
		case ENET_RDP:
			enet.latch = lance_read(&enet.lance, 0, &open);
			return IO_OPEN;
		case ENET_RAP:
			enet.latch = lance_read(&enet.lance, 1, &open);
			return IO_OPEN;
		case ENET_CTL:
			// reading the control register clears the board interrupt
			if (enet.debug)
				fprintf(stderr, "enet: control read, interrupt %s\n", enet.irq ? "cleared" : "was clear");
			enet.irq = false;
			return enet.control;
		case ENET_LATCH:
			return enet.latch;
	}
	return IO_OPEN;
}

static void write16(uint32_t offset, uint16_t data)
{
	switch (offset) {
		case ENET_RDP:
			lance_write(&enet.lance, 0, data);
			break;
		case ENET_RAP:
			lance_write(&enet.lance, 1, data);
			break;
		case ENET_CTL:
			enet.control = data & ENET_CTL_MASK;
			break;
		case ENET_LATCH:
			// the status ring address; the board starts at its first word
			enet.ring = data;
			enet.ring_prod = enet.ring_cons = 0;
			enet.irq = false;
			break;
		default:
			return;
	}
	service();
}

static uint32_t card_read(EXPCARD *card, uint32_t offset, int bits)
{
	(void)card;
	if (bits == 8) {
		uint16_t const word = read16(offset & ~1);
		return (offset & 1) ? (word & 0xff) : (word >> 8);
	}
	if (bits == 16)
		return read16(offset & ~1);
	return ((uint32_t)read16(offset & ~1) << 16) | read16((offset + 2) & (EXP_IO_SIZE - 2));
}

static void card_write(EXPCARD *card, uint32_t offset, uint32_t data, int bits)
{
	(void)card;
	if (bits == 8)
		write16(offset & ~1, (offset & 1) ? (data & 0xff) : ((data & 0xff) << 8));
	else if (bits == 16)
		write16(offset & ~1, data & 0xffff);
	else {
		write16(offset & ~1, data >> 16);
		write16((offset + 2) & (EXP_IO_SIZE - 2), data & 0xffff);
	}
}

/*
 * Time passes
 */

static void card_tick(EXPCARD *card, uint32_t cycles)
{
	LANCE *l = &enet.lance;

	(void)card;
	// transmit: on demand, and every 1.6 ms (the WIN/3B driver never sets
	// TDMD: it relies on the poll); back to back while there are frames
	enet.tx_poll_cycles += cycles;
	if (l->tx_demand || enet.tx_poll_cycles >= LANCE_TX_POLL_CYCLES) {
		enet.tx_poll_cycles = 0;
		for (int n = 0; n < 256 && !cause_pending(); n++) {
			bool const sent = lance_tx_poll(l);
			service();
			if (!sent)
				break;
		}
	}

	// receive: one frame per slice, once the ring has room for all of it
	if (enet.rxq_count && !cause_pending()) {
		FRAME *f = &enet.rxq[enet.rxq_head];
		int const ready = lance_rx_ready(l, f->data, f->length);
		if (ready != 0) {
			if (enet.debug)
				log_frame(ready > 0 ? "in" : "in (dropped)", f->data, f->length);
			if (ready > 0 && lance_receive(l, f->data, f->length) > 0)
				enet.rx_frames++;
			else
				enet.rx_dropped++;
			enet.rxq_head = (enet.rxq_head + 1) % RXQ_LEN;
			enet.rxq_count--;
			service();
		}
	}
}

static int card_irq(EXPCARD *card)
{
	(void)card;
	if (!enet.irq)
		return 0;

	// Skip the words the driver has taken (it writes 0xFFFF over each one).
	// None left: the request is stale, drop it.
	while (enet.ring_cons != enet.ring_prod) {
		uint16_t word;
		if (!mem_peek16(ring_address() + enet.ring_cons, &word) || word != 0xffff)
			break;
		enet.ring_cons = (enet.ring_cons + 2) & (ENET_STATUS_RING_BYTES - 1);
	}
	if (enet.ring_cons == enet.ring_prod) {
		if (enet.debug)
			fprintf(stderr, "enet: interrupt dropped, the driver took every word\n");
		enet.irq = false;
		return 0;
	}
	return (enet.control & ENET_INT1) ? 1 : 5;
}

/*
 * The host side
 */

void enet_host_receive(const uint8_t *frame, int length)
{
	if (!enet.present)
		return;
	if (length < 14 || length > FRAME_MAX || enet.rxq_count == RXQ_LEN) {
		enet.rx_dropped++;
		return;
	}
	FRAME *f = &enet.rxq[(enet.rxq_head + enet.rxq_count) % RXQ_LEN];
	memcpy(f->data, frame, length);
	f->length = length;
	enet.rxq_count++;
}

static void card_host_poll(EXPCARD *card)
{
	(void)card;
#ifdef HAVE_SLIRP
	if (enet.host)
		netslirp_poll();
#endif
}

static bool parse_mac(const char *s, uint8_t mac[6])
{
	unsigned int b[6];
	char end;

	if (s == NULL || sscanf(s, "%x:%x:%x:%x:%x:%x%c", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &end) != 6)
		return false;
	for (int i = 0; i < 6; i++) {
		if (b[i] > 0xff)
			return false;
		mac[i] = b[i];
	}
	return true;
}

static EXPCARD *card_create(int slot)
{
	if (enet.present) {
		fprintf(stderr, "[expansion] slot%d: FreeBee has one Ethernet board, in slot %d; this slot stays empty\n",
				slot, enet.card.slot);
		return NULL;
	}
	memset(&enet, 0, sizeof(enet));
	const char *debug = getenv("FREEBEE_ENET_DEBUG");
	enet.debug = debug && *debug && strcmp(debug, "0");

	const char *mac = fbc_get_string("network", "mac");
	if (!parse_mac(mac, enet.mac)) {
		fprintf(stderr, "[network] mac \"%s\" is not like 02:00:00:3b:00:01; using that\n", mac ? mac : "");
		parse_mac("02:00:00:3b:00:01", enet.mac);
	}
	enet.rxq = calloc(RXQ_LEN, sizeof(FRAME));
	if (enet.rxq == NULL) {
		fprintf(stderr, "enet: out of memory\n");
		return NULL;
	}
	lance_init(&enet.lance, NULL, dma_in, dma_out, lance_send);
	board_reset();
	enet.present = true;

	enet.backend = fbc_get_string("network", "backend");
	if (enet.backend == NULL)
		enet.backend = "slirp";
	if (strcmp(enet.backend, "slirp") == 0) {
#ifdef HAVE_SLIRP
		enet.host = netslirp_init(fbc_get_string("network", "hostfwd"));
#else
		fprintf(stderr, "[network] backend slirp: this FreeBee was built without libslirp; the cable is unplugged\n");
#endif
	} else if (strcmp(enet.backend, "none") != 0) {
		fprintf(stderr, "[network] backend must be slirp or none (got \"%s\"); the cable is unplugged\n", enet.backend);
	}
	printf("Ethernet board in slot %d (0x%06X), station %02x:%02x:%02x:%02x:%02x:%02x, %s\n",
			slot, EXP_IO_BASE + slot * EXP_IO_SIZE,
			enet.mac[0], enet.mac[1], enet.mac[2], enet.mac[3], enet.mac[4], enet.mac[5],
			enet.host ? enet.backend : "no host network");
	return &enet.card;
}

// The machine's reset, or the card reset in the slot window. It also holds
// the LANCE in STOP, so CSR1-3 and INIT work.
static void card_reset(EXPCARD *card)
{
	(void)card;
	board_reset();
	lance_reset(&enet.lance);
	enet.rxq_head = enet.rxq_count = 0;
}

static void card_destroy(EXPCARD *card)
{
	(void)card;
#ifdef HAVE_SLIRP
	if (enet.host)
		netslirp_done();
#endif
	enet.host = false;
	free(enet.rxq);
	enet.rxq = NULL;
	enet.present = false;
}

const EXPCARD_TYPE enet_card = {
	.name = "ethernet",
	.id = ENET_ID,
	.create = card_create,
	.destroy = card_destroy,
	.reset = card_reset,
	.read = card_read,
	.write = card_write,
	.tick = card_tick,
	.irq = card_irq,
	.host_poll = card_host_poll,
};
