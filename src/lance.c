/*
 * lance - the AMD Am7990 LANCE Ethernet controller.
 *
 * Ported to C for FreeBee from MAME's src/devices/machine/am79c90.cpp and
 * am79c90.h ("license:BSD-3-Clause, copyright-holders:Patrick Mackinlay"),
 * with the changes Visionik made for the AT&T 3B1 in
 * github.com/visionik/mame, branch att-3b1 (commit 4ca77493, 2026-10-20):
 * host frames are padded and get an FCS, and a missed packet raises MISS
 * without RINT. Only the Am7990 is here, not the Am79C90 C-LANCE.
 *
 * Changes in this port: plain C; no timers (the board polls the transmit
 * ring and reads the /INTR pin itself); a frame is sent at once and its
 * transmit interrupt comes right after; lance_rx_ready() tells the board
 * when the receive ring has room for a frame.
 *
 * Sources (from the MAME file):
 *  - Am7990 Local Area Network Controller for Ethernet (LANCE), Publication
 *    #05698, Rev. C, June 1990, Advanced Micro Devices
 *  - Am79C90 CMOS Local Area Network Controller for Ethernet (C-LANCE),
 *    Publication #17881, Rev. C, January 1998, Advanced Micro Devices
 *
 * Copyright (c) Patrick Mackinlay (MAME am79c90)
 * Copyright (c) 2026 Visionik (the att-3b1 changes)
 * Copyright (c) 2026 the FreeBee authors (this port)
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
#include <string.h>
#include <zlib.h>
#include "lance.h"

#define CSR0_INIT	LANCE_CSR0_INIT
#define CSR0_STRT	LANCE_CSR0_STRT
#define CSR0_STOP	LANCE_CSR0_STOP
#define CSR0_TDMD	LANCE_CSR0_TDMD
#define CSR0_TXON	LANCE_CSR0_TXON
#define CSR0_RXON	LANCE_CSR0_RXON
#define CSR0_INEA	LANCE_CSR0_INEA
#define CSR0_INTR	LANCE_CSR0_INTR
#define CSR0_IDON	LANCE_CSR0_IDON
#define CSR0_TINT	LANCE_CSR0_TINT
#define CSR0_RINT	LANCE_CSR0_RINT
#define CSR0_MERR	LANCE_CSR0_MERR
#define CSR0_MISS	LANCE_CSR0_MISS
#define CSR0_CERR	LANCE_CSR0_CERR
#define CSR0_BABL	LANCE_CSR0_BABL
#define CSR0_ERR	LANCE_CSR0_ERR
#define CSR0_ANY_INTR	(CSR0_BABL | CSR0_MISS | CSR0_MERR | CSR0_RINT | CSR0_TINT | CSR0_IDON)
#define CSR0_ANY_ERR	(CSR0_BABL | CSR0_CERR | CSR0_MISS | CSR0_MERR)

#define CSR3_BCON	0x0001	// byte control
#define CSR3_ACON	0x0002	// ale control
#define CSR3_BSWP	0x0004	// byte swap
#define CSR3_MASK	0x0007

#define MODE_DRX	0x0001	// disable the receiver
#define MODE_DTX	0x0002	// disable the transmitter
#define MODE_LOOP	0x0004	// loopback
#define MODE_DTCR	0x0008	// disable transmit crc
#define MODE_COLL	0x0010	// force collision
#define MODE_DRTY	0x0020	// disable retry
#define MODE_INTL	0x0040	// internal loopback
#define MODE_PROM	0x8000	// promiscuous

#define TMD1_HADR	0x00ff	// high order address
#define TMD1_ENP	0x0100	// end of packet
#define TMD1_STP	0x0200	// start of packet
#define TMD1_ADD_FCS	0x2000	// append fcs (C-LANCE only)
#define TMD1_ERR	0x4000	// error
#define TMD1_OWN	0x8000	// owned by lance

#define TMD3_RTRY	0x0400	// retry error
#define TMD3_LCAR	0x0800	// loss of carrier
#define TMD3_UFLO	0x4000	// underflow error
#define TMD3_BUFF	0x8000	// buffer error

#define RMD1_HADR	0x00ff	// high order address
#define RMD1_ENP	0x0100	// end of packet
#define RMD1_STP	0x0200	// start of packet
#define RMD1_BUFF	0x0400	// buffer error
#define RMD1_CRC	0x0800	// crc error
#define RMD1_ERR	0x4000	// error
#define RMD1_OWN	0x8000	// owned by lance

#define RMD3_MCNT	0x0fff	// message byte count

#define FCS_RESIDUE	0xdebb20e3
#define INIT_ADDR_MASK	0xfffffe
#define RING_ADDR_MASK	0xfffff8

static const uint8_t ETH_BROADCAST[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

// The Ethernet FCS, the same CRC-32 as zlib's (and MAME's crc32_creator)
static uint32_t fcs(const uint8_t *buf, int length)
{
	return (uint32_t)crc32(0L, buf, (uInt)length);
}

static void put_u16le(uint8_t *p, uint16_t v) { p[0] = v & 0xff; p[1] = v >> 8; }
static void put_u16be(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xff; }
static void put_u32le(uint8_t *p, uint32_t v)
{
	p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = v >> 24;
}
static uint16_t get_u16le(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint16_t get_u16be(const uint8_t *p) { return (p[0] << 8) | p[1]; }

static uint16_t rd(LANCE *l, uint32_t address)
{
	return l->dma_in(l->opaque, address & 0xfffffe);
}

static void wr(LANCE *l, uint32_t address, uint16_t data)
{
	l->dma_out(l->opaque, address & 0xfffffe, data, 0xffff);
}

// Am7990 buffer length: the two's complement of BCNT, 4096 for zero
static int get_buf_length(uint16_t data)
{
	return (data == 0xf000) ? 4096 : -(int16_t)(0xf000 | data);
}

static uint32_t rx_ring_address(const LANCE *l, uint8_t pos)
{
	return (l->rx_ring_base + ((uint32_t)pos << 3)) & RING_ADDR_MASK;
}

static uint32_t tx_ring_address(const LANCE *l, uint8_t pos)
{
	return (l->tx_ring_base + ((uint32_t)pos << 3)) & RING_ADDR_MASK;
}

void lance_init(LANCE *l, void *opaque,
		uint16_t (*dma_in)(void *opaque, uint32_t address),
		void (*dma_out)(void *opaque, uint32_t address, uint16_t data, uint16_t mem_mask),
		int (*send)(void *opaque, const uint8_t *buf, int length))
{
	memset(l, 0, sizeof(*l));
	l->opaque = opaque;
	l->dma_in = dma_in;
	l->dma_out = dma_out;
	l->send = send;
	lance_reset(l);
}

void lance_reset(LANCE *l)
{
	l->rap = 0;
	l->csr[0] = CSR0_STOP;
	l->csr[3] = 0;

	l->mode = 0;
	l->lb_length = 0;
	l->idon = false;
	l->tx_demand = false;
}

bool lance_intr(const LANCE *l)
{
	return (l->csr[0] & CSR0_INEA) && (l->csr[0] & CSR0_INTR);
}

static void dma_in_buf(LANCE *l, uint32_t address, uint8_t *buf, int length)
{
	// odd address start
	if (address & 1) {
		uint16_t const word = rd(l, address & ~1);

		if (l->csr[3] & CSR3_BSWP)
			buf[0] = word & 0xff;
		else
			buf[0] = word >> 8;

		buf++;
		address++;
		length--;
	}

	// word loop
	while (length > 1) {
		uint16_t const word = rd(l, address);

		if (l->csr[3] & CSR3_BSWP)
			put_u16be(&buf[0], word);
		else
			put_u16le(&buf[0], word);

		buf += 2;
		address += 2;
		length -= 2;
	}

	// trailing byte
	if (length) {
		uint16_t const word = rd(l, address);

		if (l->csr[3] & CSR3_BSWP)
			buf[0] = word >> 8;
		else
			buf[0] = word & 0xff;
	}
}

static void dma_out_buf(LANCE *l, uint32_t address, const uint8_t *buf, int length)
{
	// odd address start
	if (address & 1) {
		if (l->csr[3] & CSR3_BSWP)
			l->dma_out(l->opaque, address & ~1, buf[0], 0x00ff);
		else
			l->dma_out(l->opaque, address & ~1, buf[0] << 8, 0xff00);

		buf++;
		address++;
		length--;
	}

	// word loop
	while (length > 1) {
		uint16_t const word = (l->csr[3] & CSR3_BSWP) ? get_u16be(&buf[0]) : get_u16le(&buf[0]);

		wr(l, address, word);

		buf += 2;
		address += 2;
		length -= 2;
	}

	// trailing byte
	if (length) {
		if (l->csr[3] & CSR3_BSWP)
			l->dma_out(l->opaque, address, buf[0] << 8, 0xff00);
		else
			l->dma_out(l->opaque, address, buf[0], 0x00ff);
	}
}

static bool address_filter(LANCE *l, const uint8_t *buf)
{
	if (l->mode & MODE_PROM)
		return true;

	if (buf[0] & 1) {
		// broadcast
		if (!memcmp(ETH_BROADCAST, buf, 6))
			return true;

		// multicast
		/*
		 * Multicast address matching is performed by computing the fcs crc
		 * of the destination address, and then using the upper 6 bits as an
		 * index into the 64-bit logical address filter.
		 */
		uint32_t const crc = fcs(buf, 6);
		if ((l->logical_addr_filter >> (63 - (crc >> 26))) & 1)
			return true;
	} else if (!memcmp(l->physical_addr, buf, 6)) {
		// unicast
		return true;
	}

	return false;
}

static void initialize(LANCE *l)
{
	uint32_t const init_addr = (((uint32_t)l->csr[2] << 16) | l->csr[1]) & INIT_ADDR_MASK;
	uint16_t init_block[12];

	for (int i = 0; i < 12; i++)
		init_block[i] = rd(l, init_addr + i * 2);

	l->mode = init_block[0];

	put_u16le(&l->physical_addr[0], init_block[1]);
	put_u16le(&l->physical_addr[2], init_block[2]);
	put_u16le(&l->physical_addr[4], init_block[3]);

	l->logical_addr_filter = ((uint64_t)init_block[7] << 48) | ((uint64_t)init_block[6] << 32) |
		((uint32_t)init_block[5] << 16) | init_block[4];

	l->rx_ring_base = (((uint32_t)init_block[9] << 16) | init_block[8]) & RING_ADDR_MASK;
	l->tx_ring_base = (((uint32_t)init_block[11] << 16) | init_block[10]) & RING_ADDR_MASK;
	l->rx_ring_mask = (uint8_t)((1u << ((init_block[9] >> 13) & 7)) - 1);
	l->tx_ring_mask = (uint8_t)((1u << ((init_block[11] >> 13) & 7)) - 1);

	l->tx_ring_pos = 0;
	l->rx_ring_pos = 0;

	l->csr[0] |= CSR0_IDON | CSR0_INIT;
	l->csr[0] &= ~CSR0_STOP;

	l->idon = true;
}

uint16_t lance_read(LANCE *l, int port, bool *open)
{
	*open = false;
	if (port == 0) {
		if (l->rap && !(l->csr[0] & CSR0_STOP)) {
			*open = true;
			return 0xffff;
		}
		return l->csr[l->rap];
	}
	return l->rap;
}

void lance_write(LANCE *l, int port, uint16_t data)
{
	if (port != 0) {
		l->rap = data & 3;
		return;
	}

	switch (l->rap) {
	case 0: // Control/Status
		/*
		 * All bits are cleared by reset or STOP, except for STOP which is set.
		 *
		 *   INIT, STRT, STOP, TDMD - read/write with 1 only
		 *   TXON, RXON, INTR, ERR - read only
		 *   INEA - read/write
		 *   IDON, TINT, RINT, MERR, MISS, CERR, BABL - read/clear only
		 */

		// STOP takes priority over all other bits
		if (data & CSR0_STOP) {
			if (!(l->csr[0] & CSR0_STOP))
				lance_reset(l);
			break;
		}

		// interrupt/error flags are all cleared by writing 1
		l->csr[0] &= ~(data & (CSR0_BABL | CSR0_CERR | CSR0_MISS | CSR0_MERR | CSR0_RINT | CSR0_TINT | CSR0_IDON));

		// handle INIT
		if ((data & CSR0_INIT) && !(l->csr[0] & CSR0_INIT)) {
			if (l->csr[0] & CSR0_STOP)
				initialize(l);
			else
				l->csr[0] |= l->idon ? CSR0_IDON : CSR0_INIT;
		}

		/*
		 * From the Am7990 datasheet:
		 *
		 * The STOP bit must be set prior to setting the STRT bit. INIT and
		 * STRT must not be set at the same time. The LANCE must be
		 * initialized first and the user must wait for the IDON bit to be
		 * set (IDON=1) before setting the STRT bit.
		 *
		 * And:
		 *
		 * The STOP bit must be set prior to setting the INIT bit. Setting
		 * INIT clears the STOP bit.
		 *
		 * This is clearly contradictory; driver code sets the STRT bit
		 * after INIT, so assume STRT does not require STOP to be set.
		 * (The 3B1 driver sets INIT and STRT together.)
		 */
		// handle STRT
		if ((data & CSR0_STRT) && !(l->csr[0] & CSR0_STRT)) {
			l->csr[0] |= CSR0_STRT;
			l->csr[0] &= ~CSR0_STOP;

			if (l->mode & MODE_DRX)
				l->csr[0] &= ~CSR0_RXON;
			else
				l->csr[0] |= CSR0_RXON;

			if (l->mode & MODE_DTX)
				l->csr[0] &= ~CSR0_TXON;
			else
				l->csr[0] |= CSR0_TXON;

			// trigger an immediate transmit poll
			l->tx_demand = true;
		}

		// transmit demand
		if ((data & CSR0_TDMD) && !(l->csr[0] & CSR0_TDMD)) {
			l->csr[0] |= CSR0_TDMD;
			l->tx_demand = true;
		}

		// interrupt enable is read/write, but not while stopped (Am7990)
		if (!(l->csr[0] & CSR0_STOP)) {
			if (data & CSR0_INEA)
				l->csr[0] |= CSR0_INEA;
			else
				l->csr[0] &= ~CSR0_INEA;
		}

		// ERR == BABL || CERR || MISS || MERR
		if (l->csr[0] & CSR0_ANY_ERR)
			l->csr[0] |= CSR0_ERR;
		else
			l->csr[0] &= ~CSR0_ERR;

		// INTR == BABL || MISS || MERR || RINT || TINT || IDON
		if (l->csr[0] & CSR0_ANY_INTR)
			l->csr[0] |= CSR0_INTR;
		else
			l->csr[0] &= ~CSR0_INTR;
		break;

	case 1: // Least significant 15 bits of the Initialization Block
		if (l->csr[0] & CSR0_STOP)
			l->csr[1] = data;
		break;

	case 2: // Most significant 8 bits of the Initialization Block
		// the Am7990 keeps all the bits
		if (l->csr[0] & CSR0_STOP)
			l->csr[2] = data;
		break;

	case 3: // Bus master interface
		if (l->csr[0] & CSR0_STOP)
			l->csr[3] = data & CSR3_MASK;
		break;
	}
}

void lance_ack(LANCE *l, uint16_t bits)
{
	uint16_t const rap = l->rap;

	l->rap = 0;
	lance_write(l, 0, (bits & (CSR0_ANY_INTR | CSR0_CERR)) | (l->csr[0] & CSR0_INEA));
	l->rap = rap;
}

static int receive(LANCE *l, const uint8_t *buf, int length)
{
	// check receiver enabled
	if (!(l->csr[0] & CSR0_RXON))
		return -1;

	// address filter
	if (!address_filter(l, buf))
		return -1;

	// check we have a buffer
	uint32_t ring_address = rx_ring_address(l, l->rx_ring_pos);
	l->rx_md[1] = rd(l, ring_address | 2);

	if (!(l->rx_md[1] & RMD1_OWN))
		return -2;

	// flag start of packet
	l->rx_md[1] |= RMD1_STP;

	int offset = 0;
	while (offset < length) {
		// read rmd0 and rmd2
		l->rx_md[0] = rd(l, ring_address | 0);
		l->rx_md[2] = rd(l, ring_address | 4);

		uint32_t const rx_buf_address = ((uint32_t)(l->rx_md[1] & RMD1_HADR) << 16) | l->rx_md[0];
		int const rx_buf_length = get_buf_length(l->rx_md[2]);

		// write the data to memory
		int const count = (length - offset < rx_buf_length) ? length - offset : rx_buf_length;
		dma_out_buf(l, rx_buf_address, &buf[offset], count);
		offset += count;

		// clear ownership
		l->rx_md[1] &= ~RMD1_OWN;

		if (offset < length) {
			// look ahead to next descriptor
			uint8_t const next_ring_pos = (l->rx_ring_pos + 1) & l->rx_ring_mask;
			uint32_t const next_ring_address = rx_ring_address(l, next_ring_pos);

			// only read the next descriptor if there's more than one in the ring
			uint16_t const next_rmd1 = (next_ring_address != ring_address) ? rd(l, next_ring_address | 2) : l->rx_md[1];

			// check ownership of the next descriptor
			if (next_rmd1 & RMD1_OWN) {
				// update the descriptor
				wr(l, ring_address | 2, l->rx_md[1]);

				// advance the ring
				l->rx_ring_pos = next_ring_pos;
				ring_address = next_ring_address;
				l->rx_md[1] = next_rmd1;
			} else {
				// overflow error
				l->rx_md[1] |= RMD1_ERR | RMD1_BUFF;
				break;
			}
		}
	}

	if (offset == length) {
		// check fcs
		if (!(l->mode & MODE_LOOP) || (l->mode & MODE_DTCR)) {
			uint32_t const crc = fcs(buf, length);

			if (~crc != FCS_RESIDUE)
				l->rx_md[1] |= RMD1_ERR | RMD1_CRC;
		}

		l->rx_md[1] |= RMD1_ENP;
	}

	return offset;
}

static void recv_complete(LANCE *l, int result)
{
	switch (result) {
	case -2: // missed packet: MISS/ERR/INTR only (not RINT)
		// (Visionik) RINT means a descriptor was filled. Raising RINT on
		// MISS makes CSR0 look like ERR|MISS|RINT (high byte 0x94), which
		// the WIN/3B driver takes for a bad CSR and resets the board.
		l->csr[0] |= CSR0_ERR | CSR0_MISS | CSR0_INTR;
		return;

	case -1: // packet discarded or filtered
		return;

	default: // received something
		{
			// update the final descriptor
			uint32_t const ring_address = rx_ring_address(l, l->rx_ring_pos);

			wr(l, ring_address | 2, l->rx_md[1]);
			wr(l, ring_address | 6, (l->rx_md[1] & RMD1_ERR) ? 0 : (result & RMD3_MCNT));

			// advance the ring
			l->rx_ring_pos = (l->rx_ring_pos + 1) & l->rx_ring_mask;
		}
		break;
	}

	// generate receive interrupt
	l->csr[0] |= CSR0_RINT | CSR0_INTR;
}

// Pad a host frame to 60 bytes and append its FCS (Visionik). Returns the
// new length, or the old one if it does not fit.
static int pad_and_fcs(LANCE *l, const uint8_t **buf, int length)
{
	if (length > 0 && length + 4 <= (int)sizeof(l->rx_ext_buf)) {
		memcpy(l->rx_ext_buf, *buf, length);
		while (length < 60)
			l->rx_ext_buf[length++] = 0;
		put_u32le(&l->rx_ext_buf[length], fcs(l->rx_ext_buf, length));
		length += 4;
		*buf = l->rx_ext_buf;
	}
	return length;
}

int lance_rx_ready(LANCE *l, const uint8_t *buf, int length)
{
	if ((l->mode & MODE_LOOP) && (l->mode & MODE_INTL))
		return -1;
	if (!(l->csr[0] & CSR0_RXON) || length < 6 || !address_filter(l, buf))
		return -1;

	// the frame as stored: at least 60 bytes, and the FCS
	int need = (length < 60 ? 60 : length) + 4;
	uint8_t pos = l->rx_ring_pos;
	for (int i = 0; i <= l->rx_ring_mask; i++) {
		uint32_t const ring_address = rx_ring_address(l, pos);
		if (!(rd(l, ring_address | 2) & RMD1_OWN))
			return 0;
		need -= get_buf_length(rd(l, ring_address | 4));
		if (need <= 0)
			return 1;
		pos = (pos + 1) & l->rx_ring_mask;
	}
	// bigger than the whole ring: let the receiver report it
	return 1;
}

int lance_receive(LANCE *l, const uint8_t *buf, int length)
{
	// check internal loopback
	if ((l->mode & MODE_LOOP) && (l->mode & MODE_INTL))
		return 0;

	// Host frames omit the FCS and may come unpadded. The receive path
	// expects a frame of at least 64 bytes with its FCS: pad to 60 and
	// append a CRC, so the residue check matches a frame off the wire.
	length = pad_and_fcs(l, &buf, length);

	// discard runt packets
	if (length < 64)
		return 0;

	int const result = receive(l, buf, length);
	recv_complete(l, result);
	return result;
}

static void send_complete(LANCE *l, int result)
{
	uint32_t const ring_address = tx_ring_address(l, l->tx_ring_pos);

	// update tmd3 on error
	switch (result) {
	case -2: // invalid loopback packet
		l->tx_md[1] |= TMD1_ERR;
		break;

	case -1: // forced collision
		l->tx_md[1] |= TMD1_ERR;
		l->tx_md[3] |= TMD3_RTRY;
		wr(l, ring_address | 6, l->tx_md[3]);
		break;

	case 0: // failure to transmit (assume loss of carrier)
		l->tx_md[1] |= TMD1_ERR;
		l->tx_md[3] |= TMD3_LCAR;
		wr(l, ring_address | 6, l->tx_md[3]);
		break;
	}

	// update the last descriptor
	wr(l, ring_address | 2, l->tx_md[1]);

	// advance the ring
	l->tx_ring_pos = (l->tx_ring_pos + 1) & l->tx_ring_mask;

	// generate transmit interrupt
	l->csr[0] |= CSR0_TINT | CSR0_INTR;
}

static void transmit(LANCE *l)
{
	// check whether to append fcs (the Am7990 has no ADD_FCS bit)
	bool const append_fcs = !(l->mode & MODE_DTCR);
	l->tx_md[1] &= ~TMD1_ADD_FCS;

	uint32_t ring_address = tx_ring_address(l, l->tx_ring_pos);
	uint8_t buf[4096 + 4];
	int length = 0;

	for (;;) {
		// read tmd0 and tmd2
		l->tx_md[0] = rd(l, ring_address | 0);
		l->tx_md[2] = rd(l, ring_address | 4);
		l->tx_md[3] = 0;

		uint32_t const tx_buf_address = ((uint32_t)(l->tx_md[1] & TMD1_HADR) << 16) | l->tx_md[0];
		int tx_buf_length = get_buf_length(l->tx_md[2]);

		// clear ownership
		l->tx_md[1] &= ~TMD1_OWN;

		// read the data from memory (what fits: MAME has no check here)
		if (tx_buf_length > (int)sizeof(buf) - 4 - length)
			tx_buf_length = (int)sizeof(buf) - 4 - length;
		dma_in_buf(l, tx_buf_address, &buf[length], tx_buf_length);
		length += tx_buf_length;

		// check for end of packet
		if (l->tx_md[1] & TMD1_ENP)
			break;

		// look ahead to next descriptor
		uint8_t const next_ring_pos = (l->tx_ring_pos + 1) & l->tx_ring_mask;
		uint32_t const next_ring_address = tx_ring_address(l, next_ring_pos);

		// only read the next descriptor if there's more than one in the ring
		uint16_t const next_tmd1 = (next_ring_address != ring_address) ? rd(l, next_ring_address | 2) : l->tx_md[1];

		if (next_tmd1 & TMD1_OWN) {
			// update the descriptor
			wr(l, ring_address | 2, l->tx_md[1]);

			// advance the ring
			l->tx_ring_pos = next_ring_pos;
			ring_address = next_ring_address;
			l->tx_md[1] = next_tmd1;
		} else {
			// buffer error
			l->tx_md[1] |= TMD1_ERR;
			l->tx_md[3] |= TMD3_BUFF | TMD3_UFLO;
			wr(l, ring_address | 6, l->tx_md[3]);

			// turn off the transmitter
			l->csr[0] &= ~CSR0_TXON;
			break;
		}
	}

	// check for babble
	if (length > 1518)
		l->csr[0] |= CSR0_ERR | CSR0_BABL;

	// compute and append the fcs
	if (append_fcs) {
		put_u32le(&buf[length], fcs(buf, length));
		length += 4;
	}

	// handle loopback
	if (l->mode & MODE_LOOP) {
		// forced collision
		if ((l->mode & MODE_COLL) && (l->mode & MODE_INTL)) {
			send_complete(l, -1);
			return;
		}

		int const fcs_length = append_fcs ? 4 : 0;

		if ((length - fcs_length) < 8 || (length - fcs_length) > 32) {
			// don't know what to do, so just drop the packet
			send_complete(l, -2);
			return;
		}

		memcpy(l->lb_buf, buf, length);
		l->lb_length = length;

		if (l->mode & MODE_INTL) {
			send_complete(l, length);
			return;
		}
	}

	// the wire gets the frame without its 4 FCS bytes
	int const sent = l->send ? l->send(l->opaque, buf, length - 4) : length - 4;
	send_complete(l, sent > 0 ? length : 0);
}

bool lance_tx_poll(LANCE *l)
{
	bool sent = false;

	l->tx_demand = false;

	// check transmitter enabled
	if (l->csr[0] & CSR0_TXON) {
		// clear transmit demand
		l->csr[0] &= ~CSR0_TDMD;

		// read a transmit descriptor
		uint32_t const ring_address = tx_ring_address(l, l->tx_ring_pos);
		l->tx_md[1] = rd(l, ring_address | 2);

		// check ownership
		if (l->tx_md[1] & TMD1_OWN) {
			// check for start of packet
			if (!(l->tx_md[1] & TMD1_STP)) {
				// clear ownership
				wr(l, ring_address | 2, l->tx_md[1] & ~TMD1_OWN);

				// advance the ring
				l->tx_ring_pos = (l->tx_ring_pos + 1) & l->tx_ring_mask;
			} else {
				transmit(l);
				sent = true;
			}
		}
	}

	// receive pending loopback data
	if (l->lb_length && (l->mode & MODE_LOOP)) {
		int const result = receive(l, l->lb_buf, l->lb_length);
		l->lb_length = 0;
		recv_complete(l, result);
	}

	return sent;
}
