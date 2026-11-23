/*
 * expcard - the 3B1's expansion slots and the cards in them.
 *
 * The slots are eight 256 KiB I/O windows from 0xC00000. A card type is a
 * struct of callbacks (EXPCARD_TYPE). At start, expcard_init() reads
 * [expansion] slot0 to slot7 in .freebee.toml, makes a card of each type
 * named there and plugs it into its slot. The bus answers the identity
 * bytes at the top of each window, where the kernel looks for cards, and
 * the card reset there; the rest of a window goes to the card in it. An
 * empty window reads 0xFF.
 */

#ifndef _EXPCARD_H
#define _EXPCARD_H

#include <stdint.h>
#include <stdbool.h>

#define EXP_SLOTS		8
#define EXP_IO_BASE		0x00c00000
#define EXP_IO_SIZE		0x00040000

typedef struct EXPCARD EXPCARD;

typedef struct {
	/// The name in [expansion] slotN = "name"
	const char *name;
	/// The card id, which the kernel reads to find the card
	uint16_t id;

	/// Make a card for this slot, from the card's own configuration
	/// section; NULL if it cannot be made (the slot stays empty)
	EXPCARD *(*create)(int slot);
	/// Free the card, before FreeBee exits
	void (*destroy)(EXPCARD *card);
	/// The machine's reset, or a write to the card reset at the top of
	/// the card's window
	void (*reset)(EXPCARD *card);
	/// CPU access to the card's window. offset is from the start of the
	/// window, bits is 8, 16 or 32; never in the identity area.
	uint32_t (*read)(EXPCARD *card, uint32_t offset, int bits);
	void (*write)(EXPCARD *card, uint32_t offset, uint32_t data, int bits);

	/// Optional. After each CPU slice of this many cycles
	void (*tick)(EXPCARD *card, uint32_t cycles);
	/// Optional. The interrupt level the card asks for: 0 for none, or
	/// 1 or 5 (the two levels the expansion bus has)
	int (*irq)(EXPCARD *card);
	/// Optional. The host side (a network, a file): called about every
	/// millisecond of real time
	void (*host_poll)(EXPCARD *card);
} EXPCARD_TYPE;

/// Every card starts with this
struct EXPCARD {
	const EXPCARD_TYPE *type;
	int slot;
};

/// Make the cards that [expansion] names and plug them in (once, at start)
void expcard_init(void);
/// The machine's reset
void expcard_reset(void);
/// Before FreeBee exits
void expcard_done(void);

/// CPU access to the expansion window at address (0xC00000-0xDFFFFF)
uint32_t expcard_read(uint32_t address, int bits);
void expcard_write(uint32_t address, uint32_t data, int bits);
/// True if a card is in the slot of this address
bool expcard_present(uint32_t address);

/// After each CPU slice of this many cycles
void expcard_tick(uint32_t cycles);
/// The highest interrupt level a card asks for, or 0
int expcard_irq(void);
/// The host side of the cards: call it about every millisecond
void expcard_host_poll(void);

#endif
