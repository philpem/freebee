/*
 * expcard - the 3B1's expansion slots and the cards in them (expcard.h).
 *
 * The slot conventions (the identity bytes at the top of a window, and the
 * card reset there) follow Visionik's unixpc_slot.h in MAME's att-3b1
 * branch (license:BSD-3-Clause; see enet.c for the licence text).
 */

#include <stdio.h>
#include <string.h>
#include "expcard.h"
#include "enet.h"
#include "fbconfig.h"

// The identity is on the odd bytes at the top of a window: the id's low
// and high bytes, then their two's complements. The kernel adds each byte
// to its complement and wants 0 (an empty window reads 0xFF + 0xFF).
#define EXP_ID_LO		0x0003fff9
#define EXP_ID_HI		0x0003fffb
#define EXP_ID_LO_COMP		0x0003fffd
#define EXP_ID_HI_COMP		0x0003ffff
// A write anywhere in the last 8 bytes of a window resets the card
#define EXP_RESET		0x0003fff8

#define EMPTY			0xffffffffu

// The card types FreeBee knows
static const EXPCARD_TYPE *const types[] = {
	&enet_card,
};

static EXPCARD *slots[EXP_SLOTS];

static EXPCARD *card_at(uint32_t address)
{
	if (address < EXP_IO_BASE || address >= EXP_IO_BASE + EXP_SLOTS * EXP_IO_SIZE)
		return NULL;
	return slots[(address - EXP_IO_BASE) / EXP_IO_SIZE];
}

// The identity byte at this offset (odd byte lane), or -1
static int id_byte(uint16_t id, uint32_t offset)
{
	uint8_t const lo = id & 0xff, hi = id >> 8;

	switch (offset | 1) {
		case EXP_ID_LO:		return lo;
		case EXP_ID_HI:		return hi;
		case EXP_ID_LO_COMP:	return (uint8_t)(0u - lo);
		case EXP_ID_HI_COMP:	return (uint8_t)(0u - hi);
	}
	return -1;
}

void expcard_init(void)
{
	for (int s = 0; s < EXP_SLOTS; s++) {
		char key[8];
		snprintf(key, sizeof(key), "slot%d", s);
		const char *name = fbc_get_string("expansion", key);
		if (name == NULL || *name == '\0')
			continue;
		const EXPCARD_TYPE *t = NULL;
		for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
			if (strcmp(types[i]->name, name) == 0)
				t = types[i];
		if (t == NULL) {
			fprintf(stderr, "[expansion] %s: no card called \"%s\"; the slot stays empty\n", key, name);
			continue;
		}
		EXPCARD *c = t->create(s);
		if (c == NULL)
			continue;
		c->type = t;
		c->slot = s;
		slots[s] = c;
	}
}

void expcard_reset(void)
{
	for (int s = 0; s < EXP_SLOTS; s++)
		if (slots[s])
			slots[s]->type->reset(slots[s]);
}

void expcard_done(void)
{
	for (int s = 0; s < EXP_SLOTS; s++) {
		if (slots[s])
			slots[s]->type->destroy(slots[s]);
		slots[s] = NULL;
	}
}

bool expcard_present(uint32_t address)
{
	return card_at(address) != NULL;
}

// A word of the identity area, or of the card
static uint16_t read16(EXPCARD *c, uint32_t offset)
{
	int const id = id_byte(c->type->id, offset);
	if (id >= 0)
		return 0xff00 | id;
	if (offset >= EXP_RESET)
		return 0xffff;
	return c->type->read(c, offset, 16);
}

uint32_t expcard_read(uint32_t address, int bits)
{
	EXPCARD *c = card_at(address);
	if (c == NULL)
		return EMPTY & ((bits == 32) ? 0xffffffffu : ((1u << bits) - 1));

	uint32_t const offset = address & (EXP_IO_SIZE - 1);
	if (offset < EXP_RESET - 2)
		return c->type->read(c, offset, bits);
	// the top of the window: the identity area
	if (bits == 8) {
		uint16_t const word = read16(c, offset & ~1);
		return (offset & 1) ? (word & 0xff) : (word >> 8);
	}
	if (bits == 16)
		return read16(c, offset & ~1);
	return ((uint32_t)read16(c, offset & ~1) << 16) |
		read16(c, (offset + 2) & (EXP_IO_SIZE - 2));
}

void expcard_write(uint32_t address, uint32_t data, int bits)
{
	EXPCARD *c = card_at(address);
	if (c == NULL)
		return;

	uint32_t const offset = address & (EXP_IO_SIZE - 1);
	if (offset >= EXP_RESET || (bits == 32 && offset + 2 >= EXP_RESET)) {
		c->type->reset(c);
		return;
	}
	c->type->write(c, offset, data, bits);
}

void expcard_tick(uint32_t cycles)
{
	for (int s = 0; s < EXP_SLOTS; s++)
		if (slots[s] && slots[s]->type->tick)
			slots[s]->type->tick(slots[s], cycles);
}

int expcard_irq(void)
{
	int level = 0;
	for (int s = 0; s < EXP_SLOTS; s++) {
		if (slots[s] && slots[s]->type->irq) {
			int const l = slots[s]->type->irq(slots[s]);
			if (l > level)
				level = l;
		}
	}
	return level;
}

void expcard_host_poll(void)
{
	for (int s = 0; s < EXP_SLOTS; s++)
		if (slots[s] && slots[s]->type->host_poll)
			slots[s]->type->host_poll(slots[s]);
}
