#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include "musashi/m68k.h"
#include "state.h"
#include "memory.h"

/******************
 * Memory mapping
 ******************/

#define MAPRAM(addr) (((uint16_t)state.map[addr*2] << 8) + ((uint16_t)state.map[(addr*2)+1]))

uint32_t mapAddr(uint32_t addr, bool writing)
{
	if (addr < 0x400000) {
		// RAM access. Check against the Map RAM
		// Start by getting the original page address
		uint16_t page = (addr >> 12) & 0x3FF;

		// Look it up in the map RAM and get the physical page address
		uint32_t new_page_addr = MAPRAM(page) & 0x3FF;

		// Update the Page Status bits
		uint8_t pagebits = (MAPRAM(page) >> 13) & 0x03;
		if (pagebits != 0) {
			if (writing)
				state.map[page*2] |= 0x60;		// Page written to (dirty)
			else
				state.map[page*2] |= 0x40;		// Page accessed but not written
		}

		// Return the address with the new physical page spliced in
		return (new_page_addr << 12) + (addr & 0xFFF);
	} else {
		// I/O, VRAM or MapRAM space; no mapping is performed or required
		// TODO: assert here?
		return addr;
	}
}

MEM_STATUS checkMemoryAccess(uint32_t addr, bool writing)
{
	// Are we in Supervisor mode?
	if (m68k_get_reg(NULL, M68K_REG_SR) & 0x2000)
		// Yes. We can do anything we like.
		return MEM_ALLOWED;

	// If we're here, then we must be in User mode.
	// Check that the user didn't access memory outside of the RAM area
	if (addr >= 0x400000)
		return MEM_UIE;

	// This leaves us with Page Fault checking. Get the page bits for this page.
	uint16_t page = (addr >> 12) & 0x3FF;
	uint8_t pagebits = (MAPRAM(page) >> 13) & 0x07;

	// Check page is present
	if ((pagebits & 0x03) == 0)
		return MEM_PAGEFAULT;

	// User attempt to access the kernel
	// A19, A20, A21, A22 low (kernel access): RAM addr before paging; not in Supervisor mode
	if (((addr >> 19) & 0x0F) == 0)
		return MEM_KERNEL;

	// Check page is write enabled
	if ((pagebits & 0x04) == 0)
		return MEM_PAGE_NO_WE;

	// Page access allowed.
	return MEM_ALLOWED;
}

#undef MAPRAM


/********************************************************
 * m68k memory read/write support functions for Musashi
 ********************************************************/

/**
 * @brief	Check memory access permissions for a write operation.
 * @note	This used to be a single macro (merged with ACCESS_CHECK_RD), but
 * 			gcc throws warnings when you have a return-with-value in a void
 * 			function, even if the return-with-value is completely unreachable.
 * 			Similarly it doesn't like it if you have a return without a value
 * 			in a non-void function, even if it's impossible to ever reach the
 * 			return-with-no-value. UGH!
 */
#define ACCESS_CHECK_WR(address, bits) do {							\
		bool fault = false;											\
		/* MEM_STATUS st; */										\
		switch (checkMemoryAccess(address, true)) {					\
			case MEM_ALLOWED:										\
				/* Access allowed */								\
				break;												\
			case MEM_PAGEFAULT:										\
				/* Page fault */									\
				state.genstat = 0x8BFF | (state.pie ? 0x0400 : 0);	\
				fault = true;										\
				break;												\
			case MEM_UIE:											\
				/* User access to memory above 4MB */				\
				state.genstat = 0x9AFF | (state.pie ? 0x0400 : 0);	\
				fault = true;										\
				break;												\
			case MEM_KERNEL:										\
			case MEM_PAGE_NO_WE:									\
				/* kernel access or page not write enabled */		\
				/* TODO: which regs need setting? */				\
				fault = true;										\
				break;												\
		}															\
																	\
		if (fault) {												\
			if (bits >= 16)											\
				state.bsr0 = 0x7F00;								\
			else													\
				state.bsr0 = (address & 1) ? 0x7D00 : 0x7E00;		\
			state.bsr0 |= (address >> 16);							\
			state.bsr1 = address & 0xffff;							\
			printf("ERR: BusError WR\n");							\
			m68k_pulse_bus_error();									\
			return;													\
		}															\
	} while (false)

/**
 * @brief Check memory access permissions for a read operation.
 * @note	This used to be a single macro (merged with ACCESS_CHECK_WR), but
 * 			gcc throws warnings when you have a return-with-value in a void
 * 			function, even if the return-with-value is completely unreachable.
 * 			Similarly it doesn't like it if you have a return without a value
 * 			in a non-void function, even if it's impossible to ever reach the
 * 			return-with-no-value. UGH!
 */
#define ACCESS_CHECK_RD(address, bits) do {							\
		bool fault = false;											\
		/* MEM_STATUS st; */										\
		switch (checkMemoryAccess(address, false)) {				\
			case MEM_ALLOWED:										\
				/* Access allowed */								\
				break;												\
			case MEM_PAGEFAULT:										\
				/* Page fault */									\
				state.genstat = 0xCBFF | (state.pie ? 0x0400 : 0);	\
				fault = true;										\
				break;												\
			case MEM_UIE:											\
				/* User access to memory above 4MB */				\
				state.genstat = 0xDAFF | (state.pie ? 0x0400 : 0);	\
				fault = true;										\
				break;												\
			case MEM_KERNEL:										\
			case MEM_PAGE_NO_WE:									\
				/* kernel access or page not write enabled */		\
				/* TODO: which regs need setting? */				\
				fault = true;										\
				break;												\
		}															\
																	\
		if (fault) {												\
			if (bits >= 16)											\
				state.bsr0 = 0x7F00;								\
			else													\
				state.bsr0 = (address & 1) ? 0x7D00 : 0x7E00;		\
			state.bsr0 |= (address >> 16);							\
			state.bsr1 = address & 0xffff;							\
			printf("ERR: BusError RD\n");							\
			m68k_pulse_bus_error();									\
			return 0xFFFFFFFF;										\
		}															\
	} while (false)

// Logging macros
#define LOG_NOT_HANDLED_R(bits)																	\
	do {																						\
		if (!handled)																			\
			printf("unhandled read%02d, addr=0x%08X\n", bits, address);							\
	} while (0);

#define LOG_NOT_HANDLED_W(bits)																	\
	do {																						\
		if (!handled)																			\
			printf("unhandled write%02d, addr=0x%08X, data=0x%08X\n", bits, address, value);	\
	} while (0);

/**
 * @brief Read M68K memory, 32-bit
 */
uint32_t m68k_read_memory_32(uint32_t address)
{
	uint32_t data = 0xFFFFFFFF;
	bool handled = false;

	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	// Check access permissions
	ACCESS_CHECK_RD(address, 32);

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
		data = RD32(state.rom, address, ROM_SIZE - 1);
		handled = true;
	} else if (address <= (state.ram_size - 1)) {
		// RAM access
		data = RD32(state.ram, mapAddr(address, false), state.ram_size - 1);
		handled = true;
	} else if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O register space, zone A
		switch (address & 0x0F0000) {
			case 0x000000:				// Map RAM access
				if (address > 0x4007FF) fprintf(stderr, "NOTE: RD32 from MapRAM mirror, addr=0x%08X\n", address);
				data = RD32(state.map, address, 0x7FF);
				handled = true;
				break;
			case 0x010000:				// General Status Register
				data = ((uint32_t)state.genstat << 16) + (uint32_t)state.genstat;
				handled = true;
				break;
			case 0x020000:				// Video RAM
				if (address > 0x427FFF) fprintf(stderr, "NOTE: RD32 from VideoRAM mirror, addr=0x%08X\n", address);
				data = RD32(state.vram, address, 0x7FFF);
				handled = true;
				break;
			case 0x030000:				// Bus Status Register 0
				data = ((uint32_t)state.bsr0 << 16) + (uint32_t)state.bsr0;
				handled = true;
				break;
			case 0x040000:				// Bus Status Register 1
				data = ((uint32_t)state.bsr1 << 16) + (uint32_t)state.bsr1;
				handled = true;
				break;
			case 0x050000:				// Phone status
				break;
			case 0x060000:				// DMA Count
				// TODO: U/OERR- is always inactive (bit set)... or should it be = DMAEN+?
				// Bit 14 is always unused, so leave it set
				data = (state.dma_count & 0x3fff) | 0xC000;
				handled = true;
				break;
			case 0x070000:				// Line Printer Status Register
				data = 0x00120012;	// no parity error, no line printer error, no irqs from FDD or HDD
				data |= (state.fdc_ctx.irql) ? 0x00080008 : 0;	// FIXME! HACKHACKHACK! shouldn't peek inside FDC structs like this
				break;
			case 0x080000:				// Real Time Clock
				break;
			case 0x090000:				// Phone registers
				switch (address & 0x0FF000) {
					case 0x090000:		// Handset relay
					case 0x098000:
						break;
					case 0x091000:		// Line select 2
					case 0x099000:
						break;
					case 0x092000:		// Hook relay 1
					case 0x09A000:
						break;
					case 0x093000:		// Hook relay 2
					case 0x09B000:
						break;
					case 0x094000:		// Line 1 hold
					case 0x09C000:
						break;
					case 0x095000:		// Line 2 hold
					case 0x09D000:
						break;
					case 0x096000:		// Line 1 A-lead
					case 0x09E000:
						break;
					case 0x097000:		// Line 2 A-lead
					case 0x09F000:
						break;
				}
				break;
			case 0x0A0000:				// Miscellaneous Control Register -- write only!
				handled = true;
				break;
			case 0x0B0000:				// TM/DIALWR
				break;
			case 0x0C0000:				// Clear Status Register -- write only!
				handled = true;
				break;
			case 0x0D0000:				// DMA Address Register
				break;
			case 0x0E0000:				// Disk Control Register
				break;
			case 0x0F0000:				// Line Printer Data Register
				break;
		}
	} else if ((address >= 0xC00000) && (address <= 0xFFFFFF)) {
		// I/O register space, zone B
		switch (address & 0xF00000) {
			case 0xC00000:				// Expansion slots
			case 0xD00000:
				switch (address & 0xFC0000) {
					case 0xC00000:		// Expansion slot 0
					case 0xC40000:		// Expansion slot 1
					case 0xC80000:		// Expansion slot 2
					case 0xCC0000:		// Expansion slot 3
					case 0xD00000:		// Expansion slot 4
					case 0xD40000:		// Expansion slot 5
					case 0xD80000:		// Expansion slot 6
					case 0xDC0000:		// Expansion slot 7
						fprintf(stderr, "NOTE: RD32 from expansion card space, addr=0x%08X\n", address);
						break;
				}
				break;
			case 0xE00000:				// HDC, FDC, MCR2 and RTC data bits
			case 0xF00000:
				switch (address & 0x070000) {
					case 0x000000:		// [ef][08]xxxx ==> WD1010 hard disc controller
						break;
					case 0x010000:		// [ef][19]xxxx ==> WD2797 floppy disc controller
						data = wd2797_read_reg(&state.fdc_ctx, (address >> 1) & 3);
						printf("WD279X: rd32 %02X ==> %02X\n", (address >> 1) & 3, data);
						handled = true;
						break;
					case 0x020000:		// [ef][2a]xxxx ==> Miscellaneous Control Register 2
						break;
					case 0x030000:		// [ef][3b]xxxx ==> Real Time Clock data bits
						break;
					case 0x040000:		// [ef][4c]xxxx ==> General Control Register
						switch (address & 0x077000) {
							case 0x040000:		// [ef][4c][08]xxx ==> EE
							case 0x041000:		// [ef][4c][19]xxx ==> PIE
							case 0x042000:		// [ef][4c][2A]xxx ==> BP
							case 0x043000:		// [ef][4c][3B]xxx ==> ROMLMAP
							case 0x044000:		// [ef][4c][4C]xxx ==> L1 MODEM
							case 0x045000:		// [ef][4c][5D]xxx ==> L2 MODEM
							case 0x046000:		// [ef][4c][6E]xxx ==> D/N CONNECT
								// All write-only registers... TODO: bus error?
								handled = true;
								break;
							case 0x047000:		// [ef][4c][7F]xxx ==> Whole screen reverse video [FIXME: not in TRM]
								break;
						}
						break;
					case 0x050000:		// [ef][5d]xxxx ==> 8274
						break;
					case 0x060000:		// [ef][6e]xxxx ==> Control regs
						switch (address & 0x07F000) {
							default:
								break;
						}
						break;
					case 0x070000:		// [ef][7f]xxxx ==> 6850 Keyboard Controller
						break;
				}
		}
	}

	LOG_NOT_HANDLED_R(32);
	return data;
}

/**
 * @brief Read M68K memory, 16-bit
 */
uint32_t m68k_read_memory_16(uint32_t address)
{
	uint16_t data = 0xFFFF;
	bool handled = false;

	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	// Check access permissions
	ACCESS_CHECK_RD(address, 16);

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
		data = RD16(state.rom, address, ROM_SIZE - 1);
		handled = true;
	} else if (address <= (state.ram_size - 1)) {
		// RAM access
		data = RD16(state.ram, mapAddr(address, false), state.ram_size - 1);
		handled = true;
	} else if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O register space, zone A
		switch (address & 0x0F0000) {
			case 0x000000:				// Map RAM access
				if (address > 0x4007FF) fprintf(stderr, "NOTE: RD16 from MapRAM mirror, addr=0x%08X\n", address);
				data = RD16(state.map, address, 0x7FF);
				handled = true;
				break;
			case 0x010000:				// General Status Register
				data = state.genstat;
				handled = true;
				break;
			case 0x020000:				// Video RAM
				if (address > 0x427FFF) fprintf(stderr, "NOTE: RD16 from VideoRAM mirror, addr=0x%08X\n", address);
				data = RD16(state.vram, address, 0x7FFF);
				handled = true;
				break;
			case 0x030000:				// Bus Status Register 0
				data = state.bsr0;
				handled = true;
				break;
			case 0x040000:				// Bus Status Register 1
				data = state.bsr1;
				handled = true;
				break;
			case 0x050000:				// Phone status
				break;
			case 0x060000:				// DMA Count
				// TODO: U/OERR- is always inactive (bit set)... or should it be = DMAEN+?
				// Bit 14 is always unused, so leave it set
				data = (state.dma_count & 0x3fff) | 0xC000;
				handled = true;
				break;
			case 0x070000:				// Line Printer Status Register
				data = 0x0012;	// no parity error, no line printer error, no irqs from FDD or HDD
				data |= (state.fdc_ctx.irql) ? 0x0008 : 0;	// FIXME! HACKHACKHACK! shouldn't peek inside FDC structs like this
				break;
			case 0x080000:				// Real Time Clock
				break;
			case 0x090000:				// Phone registers
				switch (address & 0x0FF000) {
					case 0x090000:		// Handset relay
					case 0x098000:
						break;
					case 0x091000:		// Line select 2
					case 0x099000:
						break;
					case 0x092000:		// Hook relay 1
					case 0x09A000:
						break;
					case 0x093000:		// Hook relay 2
					case 0x09B000:
						break;
					case 0x094000:		// Line 1 hold
					case 0x09C000:
						break;
					case 0x095000:		// Line 2 hold
					case 0x09D000:
						break;
					case 0x096000:		// Line 1 A-lead
					case 0x09E000:
						break;
					case 0x097000:		// Line 2 A-lead
					case 0x09F000:
						break;
				}
				break;
			case 0x0A0000:				// Miscellaneous Control Register -- write only!
				handled = true;
				break;
			case 0x0B0000:				// TM/DIALWR
				break;
			case 0x0C0000:				// Clear Status Register -- write only!
				handled = true;
				break;
			case 0x0D0000:				// DMA Address Register
				break;
			case 0x0E0000:				// Disk Control Register
				break;
			case 0x0F0000:				// Line Printer Data Register
				break;
		}
	} else if ((address >= 0xC00000) && (address <= 0xFFFFFF)) {
		// I/O register space, zone B
		switch (address & 0xF00000) {
			case 0xC00000:				// Expansion slots
			case 0xD00000:
				switch (address & 0xFC0000) {
					case 0xC00000:		// Expansion slot 0
					case 0xC40000:		// Expansion slot 1
					case 0xC80000:		// Expansion slot 2
					case 0xCC0000:		// Expansion slot 3
					case 0xD00000:		// Expansion slot 4
					case 0xD40000:		// Expansion slot 5
					case 0xD80000:		// Expansion slot 6
					case 0xDC0000:		// Expansion slot 7
						fprintf(stderr, "NOTE: RD16 from expansion card space, addr=0x%08X\n", address);
						break;
				}
				break;
			case 0xE00000:				// HDC, FDC, MCR2 and RTC data bits
			case 0xF00000:
				switch (address & 0x070000) {
					case 0x000000:		// [ef][08]xxxx ==> WD1010 hard disc controller
						break;
					case 0x010000:		// [ef][19]xxxx ==> WD2797 floppy disc controller
						data = wd2797_read_reg(&state.fdc_ctx, (address >> 1) & 3);
						printf("WD279X: rd16 %02X ==> %02X\n", (address >> 1) & 3, data);
						handled = true;
						break;
					case 0x020000:		// [ef][2a]xxxx ==> Miscellaneous Control Register 2
						break;
					case 0x030000:		// [ef][3b]xxxx ==> Real Time Clock data bits
						break;
					case 0x040000:		// [ef][4c]xxxx ==> General Control Register
						switch (address & 0x077000) {
							case 0x040000:		// [ef][4c][08]xxx ==> EE
							case 0x041000:		// [ef][4c][19]xxx ==> PIE
							case 0x042000:		// [ef][4c][2A]xxx ==> BP
							case 0x043000:		// [ef][4c][3B]xxx ==> ROMLMAP
							case 0x044000:		// [ef][4c][4C]xxx ==> L1 MODEM
							case 0x045000:		// [ef][4c][5D]xxx ==> L2 MODEM
							case 0x046000:		// [ef][4c][6E]xxx ==> D/N CONNECT
								// All write-only registers... TODO: bus error?
								handled = true;
								break;
							case 0x047000:		// [ef][4c][7F]xxx ==> Whole screen reverse video
								break;
						}
						break;
					case 0x050000:		// [ef][5d]xxxx ==> 8274
						break;
					case 0x060000:		// [ef][6e]xxxx ==> Control regs
						switch (address & 0x07F000) {
							default:
								break;
						}
						break;
					case 0x070000:		// [ef][7f]xxxx ==> 6850 Keyboard Controller
						break;
				}
		}
	}

	LOG_NOT_HANDLED_R(16);
	return data;
}

/**
 * @brief Read M68K memory, 8-bit
 */
uint32_t m68k_read_memory_8(uint32_t address)
{
	uint8_t data = 0xFF;
	bool handled = false;

	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	// Check access permissions
	ACCESS_CHECK_RD(address, 8);

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
		data = RD8(state.rom, address, ROM_SIZE - 1);
		handled = true;
	} else if (address <= (state.ram_size - 1)) {
		// RAM access
		data = RD8(state.ram, mapAddr(address, false), state.ram_size - 1);
		handled = true;
	} else if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O register space, zone A
		switch (address & 0x0F0000) {
			case 0x000000:				// Map RAM access
				if (address > 0x4007FF) fprintf(stderr, "NOTE: RD8 from MapRAM mirror, addr=0x%08X\n", address);
				data = RD8(state.map, address, 0x7FF);
				handled = true;
				break;
			case 0x010000:				// General Status Register
				if ((address & 1) == 0)
					data = (state.genstat >> 8) & 0xff;
				else
					data = (state.genstat)      & 0xff;
				handled = true;
				break;
			case 0x020000:				// Video RAM
				if (address > 0x427FFF) fprintf(stderr, "NOTE: RD8 from VideoRAM mirror, addr=0x%08X\n", address);
				data = RD8(state.vram, address, 0x7FFF);
				handled = true;
				break;
			case 0x030000:				// Bus Status Register 0
				if ((address & 1) == 0)
					data = (state.bsr0 >> 8) & 0xff;
				else
					data = (state.bsr0)      & 0xff;
				handled = true;
				break;
			case 0x040000:				// Bus Status Register 1
				if ((address & 1) == 0)
					data = (state.bsr1 >> 8) & 0xff;
				else
					data = (state.bsr1)      & 0xff;
				handled = true;
				break;
			case 0x050000:				// Phone status
				break;
			case 0x060000:				// DMA Count
				// TODO: how to handle this in 8bit mode?
				break;
			case 0x070000:				// Line Printer Status Register
				printf("\tLPSR RD8 fdc irql=%d, irqe=%d\n", state.fdc_ctx.irql, state.fdc_ctx.irqe);
				if (address & 1) {
					data = 0x12;	// no parity error, no line printer error, no irqs from FDD or HDD
					data |= (state.fdc_ctx.irql) ? 0x08 : 0;	// FIXME! HACKHACKHACK! shouldn't peek inside FDC structs like this
//					data |= 0x04; // HDD interrupt, i.e. command complete -- HACKHACKHACK!
				} else {
					data = 0;
				}
				handled = true;
				break;
			case 0x080000:				// Real Time Clock
				break;
			case 0x090000:				// Phone registers
				switch (address & 0x0FF000) {
					case 0x090000:		// Handset relay
					case 0x098000:
						break;
					case 0x091000:		// Line select 2
					case 0x099000:
						break;
					case 0x092000:		// Hook relay 1
					case 0x09A000:
						break;
					case 0x093000:		// Hook relay 2
					case 0x09B000:
						break;
					case 0x094000:		// Line 1 hold
					case 0x09C000:
						break;
					case 0x095000:		// Line 2 hold
					case 0x09D000:
						break;
					case 0x096000:		// Line 1 A-lead
					case 0x09E000:
						break;
					case 0x097000:		// Line 2 A-lead
					case 0x09F000:
						break;
				}
				break;
			case 0x0A0000:				// Miscellaneous Control Register -- write only!
				handled = true;
				break;
			case 0x0B0000:				// TM/DIALWR
				break;
			case 0x0C0000:				// Clear Status Register -- write only!
				handled = true;
				break;
			case 0x0D0000:				// DMA Address Register
				break;
			case 0x0E0000:				// Disk Control Register
				break;
			case 0x0F0000:				// Line Printer Data Register
				break;
		}
	} else if ((address >= 0xC00000) && (address <= 0xFFFFFF)) {
		// I/O register space, zone B
		switch (address & 0xF00000) {
			case 0xC00000:				// Expansion slots
			case 0xD00000:
				switch (address & 0xFC0000) {
					case 0xC00000:		// Expansion slot 0
					case 0xC40000:		// Expansion slot 1
					case 0xC80000:		// Expansion slot 2
					case 0xCC0000:		// Expansion slot 3
					case 0xD00000:		// Expansion slot 4
					case 0xD40000:		// Expansion slot 5
					case 0xD80000:		// Expansion slot 6
					case 0xDC0000:		// Expansion slot 7
						fprintf(stderr, "NOTE: RD8 from expansion card space, addr=0x%08X\n", address);
						break;
				}
				break;
			case 0xE00000:				// HDC, FDC, MCR2 and RTC data bits
			case 0xF00000:
				switch (address & 0x070000) {
					case 0x000000:		// [ef][08]xxxx ==> WD1010 hard disc controller
						break;
					case 0x010000:		// [ef][19]xxxx ==> WD2797 floppy disc controller
						data = wd2797_read_reg(&state.fdc_ctx, (address >> 1) & 3);
						printf("WD279X: rd8 %02X ==> %02X\n", (address >> 1) & 3, data);
						handled = true;
						break;
					case 0x020000:		// [ef][2a]xxxx ==> Miscellaneous Control Register 2
						break;
					case 0x030000:		// [ef][3b]xxxx ==> Real Time Clock data bits
						break;
					case 0x040000:		// [ef][4c]xxxx ==> General Control Register
						switch (address & 0x077000) {
							case 0x040000:		// [ef][4c][08]xxx ==> EE
							case 0x041000:		// [ef][4c][19]xxx ==> PIE
							case 0x042000:		// [ef][4c][2A]xxx ==> BP
							case 0x043000:		// [ef][4c][3B]xxx ==> ROMLMAP
							case 0x044000:		// [ef][4c][4C]xxx ==> L1 MODEM
							case 0x045000:		// [ef][4c][5D]xxx ==> L2 MODEM
							case 0x046000:		// [ef][4c][6E]xxx ==> D/N CONNECT
								// All write-only registers... TODO: bus error?
								handled = true;
								break;
							case 0x047000:		// [ef][4c][7F]xxx ==> Whole screen reverse video
								break;
						}
					case 0x050000:		// [ef][5d]xxxx ==> 8274
						break;
					case 0x060000:		// [ef][6e]xxxx ==> Control regs
						switch (address & 0x07F000) {
							default:
								break;
						}
						break;
					case 0x070000:		// [ef][7f]xxxx ==> 6850 Keyboard Controller
						break;
				}
		}
	}

	LOG_NOT_HANDLED_R(8);

	return data;
}

/**
 * @brief Write M68K memory, 32-bit
 */
void m68k_write_memory_32(uint32_t address, uint32_t value)
{
	bool handled = false;

	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	// Check access permissions
	ACCESS_CHECK_WR(address, 32);

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
		handled = true;
	} else if (address <= (state.ram_size - 1)) {
		// RAM access
		WR32(state.ram, mapAddr(address, false), state.ram_size - 1, value);
		handled = true;
	} else if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O register space, zone A
		switch (address & 0x0F0000) {
			case 0x000000:				// Map RAM access
				if (address > 0x4007FF) fprintf(stderr, "NOTE: WR32 to MapRAM mirror, addr=0x%08X, data=0x%08X\n", address, value);
				WR32(state.map, address, 0x7FF, value);
				handled = true;
				break;
			case 0x010000:				// General Status Register
				state.genstat = (value & 0xffff);
				handled = true;
				break;
			case 0x020000:				// Video RAM
				if (address > 0x427FFF) fprintf(stderr, "NOTE: WR32 to VideoRAM mirror, addr=0x%08X, data=0x%08X\n", address, value);
				WR32(state.vram, address, 0x7FFF, value);
				handled = true;
				break;
			case 0x030000:				// Bus Status Register 0
				break;
			case 0x040000:				// Bus Status Register 1
				break;
			case 0x050000:				// Phone status
				break;
			case 0x060000:				// DMA Count
				printf("WR32 dmacount %08X\n", value);
				state.dma_count = (value & 0x3FFF);
				state.idmarw = ((value & 0x4000) == 0x4000);
				state.dmaen = ((value & 0x8000) == 0x8000);
				printf("\tcount %04X, idmarw %d, dmaen %d\n", state.dma_count, state.idmarw, state.dmaen);
				// This handles the "dummy DMA transfer" mentioned in the docs
				// TODO: access check, peripheral access
				if (!state.idmarw)
					WR32(state.ram, mapAddr(address, false), state.ram_size - 1, 0xDEAD);
				state.dma_count++;
				handled = true;
				break;
			case 0x070000:				// Line Printer Status Register
				break;
			case 0x080000:				// Real Time Clock
				break;
			case 0x090000:				// Phone registers
				switch (address & 0x0FF000) {
					case 0x090000:		// Handset relay
					case 0x098000:
						break;
					case 0x091000:		// Line select 2
					case 0x099000:
						break;
					case 0x092000:		// Hook relay 1
					case 0x09A000:
						break;
					case 0x093000:		// Hook relay 2
					case 0x09B000:
						break;
					case 0x094000:		// Line 1 hold
					case 0x09C000:
						break;
					case 0x095000:		// Line 2 hold
					case 0x09D000:
						break;
					case 0x096000:		// Line 1 A-lead
					case 0x09E000:
						break;
					case 0x097000:		// Line 2 A-lead
					case 0x09F000:
						break;
				}
				break;
			case 0x0A0000:				// Miscellaneous Control Register
				// TODO: handle the ctrl bits properly
				// TODO: &0x8000 --> dismiss 60hz intr
				state.dma_reading = (value & 0x4000);
				state.leds = (~value & 0xF00) >> 8;
				printf("LEDs: %s %s %s %s\n",
						(state.leds & 8) ? "R" : "-",
						(state.leds & 4) ? "G" : "-",
						(state.leds & 2) ? "Y" : "-",
						(state.leds & 1) ? "R" : "-");
				handled = true;
				break;
			case 0x0B0000:				// TM/DIALWR
				break;
			case 0x0C0000:				// Clear Status Register
				state.genstat = 0xFFFF;
				state.bsr0 = 0xFFFF;
				state.bsr1 = 0xFFFF;
				handled = true;
				break;
			case 0x0D0000:				// DMA Address Register
				if (address & 0x004000) {
					// A14 high -- set most significant bits
					state.dma_address = (state.dma_address & 0x1fe) | ((address & 0x3ffe) << 8);
				} else {
					// A14 low -- set least significant bits
					state.dma_address = (state.dma_address & 0x3ffe00) | (address & 0x1fe);
				}
				printf("WR32 DMA_ADDR %s, now %08X\n", address & 0x004000 ? "HI" : "LO", state.dma_address);
				handled = true;
				break;
			case 0x0E0000:				// Disk Control Register
				// B7 = FDD controller reset
				if ((value & 0x80) == 0) wd2797_reset(&state.fdc_ctx);
				// B6 = drive 0 select -- TODO
				// B5 = motor enable -- TODO
				// B4 = HDD controller reset -- TODO
				// B3 = HDD0 select -- TODO
				// B2,1,0 = HDD0 head select
				handled = true;
				break;
			case 0x0F0000:				// Line Printer Data Register
				break;
		}
	} else if ((address >= 0xC00000) && (address <= 0xFFFFFF)) {
		// I/O register space, zone B
		switch (address & 0xF00000) {
			case 0xC00000:				// Expansion slots
			case 0xD00000:
				switch (address & 0xFC0000) {
					case 0xC00000:		// Expansion slot 0
					case 0xC40000:		// Expansion slot 1
					case 0xC80000:		// Expansion slot 2
					case 0xCC0000:		// Expansion slot 3
					case 0xD00000:		// Expansion slot 4
					case 0xD40000:		// Expansion slot 5
					case 0xD80000:		// Expansion slot 6
					case 0xDC0000:		// Expansion slot 7
						fprintf(stderr, "NOTE: WR32 to expansion card space, addr=0x%08X, data=0x%08X\n", address, value);
						handled = true;
						break;
				}
				break;
			case 0xE00000:				// HDC, FDC, MCR2 and RTC data bits
			case 0xF00000:
				switch (address & 0x070000) {
					case 0x000000:		// [ef][08]xxxx ==> WD1010 hard disc controller
						break;
					case 0x010000:		// [ef][19]xxxx ==> WD2797 floppy disc controller
						printf("WD279X: wr32 %02X ==> %02X\n", (address >> 1) & 3, value);
						wd2797_write_reg(&state.fdc_ctx, (address >> 1) & 3, value);
						handled = true;
						break;
					case 0x020000:		// [ef][2a]xxxx ==> Miscellaneous Control Register 2
						break;
					case 0x030000:		// [ef][3b]xxxx ==> Real Time Clock data bits
						break;
					case 0x040000:		// [ef][4c]xxxx ==> General Control Register
						switch (address & 0x077000) {
							case 0x040000:		// [ef][4c][08]xxx ==> EE
								break;
							case 0x041000:		// [ef][4c][19]xxx ==> PIE
								state.pie = ((value & 0x8000) == 0x8000);
								handled = true;
								break;
							case 0x042000:		// [ef][4c][2A]xxx ==> BP
								break;
							case 0x043000:		// [ef][4c][3B]xxx ==> ROMLMAP
								state.romlmap = ((value & 0x8000) == 0x8000);
								handled = true;
								break;
							case 0x044000:		// [ef][4c][4C]xxx ==> L1 MODEM
								break;
							case 0x045000:		// [ef][4c][5D]xxx ==> L2 MODEM
								break;
							case 0x046000:		// [ef][4c][6E]xxx ==> D/N CONNECT
								break;
							case 0x047000:		// [ef][4c][7F]xxx ==> Whole screen reverse video
								break;
						}
					case 0x050000:		// [ef][5d]xxxx ==> 8274
						break;
					case 0x060000:		// [ef][6e]xxxx ==> Control regs
						switch (address & 0x07F000) {
							default:
								break;
						}
						break;
					case 0x070000:		// [ef][7f]xxxx ==> 6850 Keyboard Controller
						break;
				}
		}
	}

	LOG_NOT_HANDLED_W(32);
}

/**
 * @brief Write M68K memory, 16-bit
 */
void m68k_write_memory_16(uint32_t address, uint32_t value)
{
	bool handled = false;

	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	// Check access permissions
	ACCESS_CHECK_WR(address, 16);

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
		handled = true;
	} else if (address <= (state.ram_size - 1)) {
		// RAM access
		WR16(state.ram, mapAddr(address, false), state.ram_size - 1, value);
		handled = true;
	} else if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O register space, zone A
		switch (address & 0x0F0000) {
			case 0x000000:				// Map RAM access
				if (address > 0x4007FF) fprintf(stderr, "NOTE: WR16 to MapRAM mirror, addr=0x%08X, data=0x%04X\n", address, value);
				WR16(state.map, address, 0x7FF, value);
				handled = true;
				break;
			case 0x010000:				// General Status Register (read only)
				handled = true;
				break;
			case 0x020000:				// Video RAM
				if (address > 0x427FFF) fprintf(stderr, "NOTE: WR16 to VideoRAM mirror, addr=0x%08X, data=0x%04X\n", address, value);
				WR16(state.vram, address, 0x7FFF, value);
				handled = true;
				break;
			case 0x030000:				// Bus Status Register 0 (read only)
				handled = true;
				break;
			case 0x040000:				// Bus Status Register 1 (read only)
				handled = true;
				break;
			case 0x050000:				// Phone status
				break;
			case 0x060000:				// DMA Count
				printf("WR16 dmacount %08X\n", value);
				state.dma_count = (value & 0x3FFF);
				state.idmarw = ((value & 0x4000) == 0x4000);
				state.dmaen = ((value & 0x8000) == 0x8000);
				printf("\tcount %04X, idmarw %d, dmaen %d\n", state.dma_count, state.idmarw, state.dmaen);
				// This handles the "dummy DMA transfer" mentioned in the docs
				// TODO: access check, peripheral access
				if (!state.idmarw)
					WR32(state.ram, mapAddr(address, false), state.ram_size - 1, 0xDEAD);
				state.dma_count++;
				handled = true;
				break;
			case 0x070000:				// Line Printer Status Register
				break;
			case 0x080000:				// Real Time Clock
				break;
			case 0x090000:				// Phone registers
				switch (address & 0x0FF000) {
					case 0x090000:		// Handset relay
					case 0x098000:
						break;
					case 0x091000:		// Line select 2
					case 0x099000:
						break;
					case 0x092000:		// Hook relay 1
					case 0x09A000:
						break;
					case 0x093000:		// Hook relay 2
					case 0x09B000:
						break;
					case 0x094000:		// Line 1 hold
					case 0x09C000:
						break;
					case 0x095000:		// Line 2 hold
					case 0x09D000:
						break;
					case 0x096000:		// Line 1 A-lead
					case 0x09E000:
						break;
					case 0x097000:		// Line 2 A-lead
					case 0x09F000:
						break;
				}
				break;
			case 0x0A0000:				// Miscellaneous Control Register
				// TODO: handle the ctrl bits properly
				// TODO: &0x8000 --> dismiss 60hz intr
				state.dma_reading = (value & 0x4000);
				state.leds = (~value & 0xF00) >> 8;
				printf("LEDs: %s %s %s %s\n",
						(state.leds & 8) ? "R" : "-",
						(state.leds & 4) ? "G" : "-",
						(state.leds & 2) ? "Y" : "-",
						(state.leds & 1) ? "R" : "-");
				handled = true;
				break;
			case 0x0B0000:				// TM/DIALWR
				break;
			case 0x0C0000:				// Clear Status Register
				state.genstat = 0xFFFF;
				state.bsr0 = 0xFFFF;
				state.bsr1 = 0xFFFF;
				handled = true;
				break;
			case 0x0D0000:				// DMA Address Register
				if (address & 0x004000) {
					// A14 high -- set most significant bits
					state.dma_address = (state.dma_address & 0x1fe) | ((address & 0x3ffe) << 8);
				} else {
					// A14 low -- set least significant bits
					state.dma_address = (state.dma_address & 0x3ffe00) | (address & 0x1fe);
				}
				printf("WR16 DMA_ADDR %s, now %08X\n", address & 0x004000 ? "HI" : "LO", state.dma_address);
				handled = true;
				break;
			case 0x0E0000:				// Disk Control Register
				// B7 = FDD controller reset
				if ((value & 0x80) == 0) wd2797_reset(&state.fdc_ctx);
				// B6 = drive 0 select -- TODO
				// B5 = motor enable -- TODO
				// B4 = HDD controller reset -- TODO
				// B3 = HDD0 select -- TODO
				// B2,1,0 = HDD0 head select
				handled = true;
				break;
			case 0x0F0000:				// Line Printer Data Register
				break;
		}
	} else if ((address >= 0xC00000) && (address <= 0xFFFFFF)) {
		// I/O register space, zone B
		switch (address & 0xF00000) {
			case 0xC00000:				// Expansion slots
			case 0xD00000:
				switch (address & 0xFC0000) {
					case 0xC00000:		// Expansion slot 0
					case 0xC40000:		// Expansion slot 1
					case 0xC80000:		// Expansion slot 2
					case 0xCC0000:		// Expansion slot 3
					case 0xD00000:		// Expansion slot 4
					case 0xD40000:		// Expansion slot 5
					case 0xD80000:		// Expansion slot 6
					case 0xDC0000:		// Expansion slot 7
						fprintf(stderr, "NOTE: WR16 to expansion card space, addr=0x%08X, data=0x%04X\n", address, value);
						break;
				}
				break;
			case 0xE00000:				// HDC, FDC, MCR2 and RTC data bits
			case 0xF00000:
				switch (address & 0x070000) {
					case 0x000000:		// [ef][08]xxxx ==> WD1010 hard disc controller
						break;
					case 0x010000:		// [ef][19]xxxx ==> WD2797 floppy disc controller
						printf("WD279X: wr16 %02X ==> %02X\n", (address >> 1) & 3, value);
						wd2797_write_reg(&state.fdc_ctx, (address >> 1) & 3, value);
						handled = true;
						break;
					case 0x020000:		// [ef][2a]xxxx ==> Miscellaneous Control Register 2
						break;
					case 0x030000:		// [ef][3b]xxxx ==> Real Time Clock data bits
						break;
					case 0x040000:		// [ef][4c]xxxx ==> General Control Register
						switch (address & 0x077000) {
							case 0x040000:		// [ef][4c][08]xxx ==> EE
								break;
							case 0x041000:		// [ef][4c][19]xxx ==> PIE
								state.pie = ((value & 0x8000) == 0x8000);
								handled = true;
								break;
							case 0x042000:		// [ef][4c][2A]xxx ==> BP
								break;
							case 0x043000:		// [ef][4c][3B]xxx ==> ROMLMAP
								state.romlmap = ((value & 0x8000) == 0x8000);
								handled = true;
								break;
							case 0x044000:		// [ef][4c][4C]xxx ==> L1 MODEM
								break;
							case 0x045000:		// [ef][4c][5D]xxx ==> L2 MODEM
								break;
							case 0x046000:		// [ef][4c][6E]xxx ==> D/N CONNECT
								break;
							case 0x047000:		// [ef][4c][7F]xxx ==> Whole screen reverse video
								break;
						}
					case 0x050000:		// [ef][5d]xxxx ==> 8274
						break;
					case 0x060000:		// [ef][6e]xxxx ==> Control regs
						switch (address & 0x07F000) {
							default:
								break;
						}
						break;
					case 0x070000:		// [ef][7f]xxxx ==> 6850 Keyboard Controller
						break;
				}
		}
	}

	LOG_NOT_HANDLED_W(16);
}

/**
 * @brief Write M68K memory, 8-bit
 */
void m68k_write_memory_8(uint32_t address, uint32_t value)
{
	bool handled = false;

	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	// Check access permissions
	ACCESS_CHECK_WR(address, 8);

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access (read only!)
		handled = true;
	} else if (address <= (state.ram_size - 1)) {
		// RAM access
		WR8(state.ram, mapAddr(address, false), state.ram_size - 1, value);
		handled = true;
	} else if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O register space, zone A
		switch (address & 0x0F0000) {
			case 0x000000:				// Map RAM access
				if (address > 0x4007FF) fprintf(stderr, "NOTE: WR8 to MapRAM mirror, addr=%08X, data=%02X\n", address, value);
				WR8(state.map, address, 0x7FF, value);
				handled = true;
				break;
			case 0x010000:				// General Status Register
				handled = true;
				break;
			case 0x020000:				// Video RAM
				if (address > 0x427FFF) fprintf(stderr, "NOTE: WR8 to VideoRAM mirror, addr=%08X, data=0x%02X\n", address, value);
				WR8(state.vram, address, 0x7FFF, value);
				handled = true;
				break;
			case 0x030000:				// Bus Status Register 0
				handled = true;
				break;
			case 0x040000:				// Bus Status Register 1
				handled = true;
				break;
			case 0x050000:				// Phone status
				break;
			case 0x060000:				// DMA Count
				// TODO: how to handle this in 8bit mode?
				break;
			case 0x070000:				// Line Printer Status Register
				break;
			case 0x080000:				// Real Time Clock
				break;
			case 0x090000:				// Phone registers
				switch (address & 0x0FF000) {
					case 0x090000:		// Handset relay
					case 0x098000:
						break;
					case 0x091000:		// Line select 2
					case 0x099000:
						break;
					case 0x092000:		// Hook relay 1
					case 0x09A000:
						break;
					case 0x093000:		// Hook relay 2
					case 0x09B000:
						break;
					case 0x094000:		// Line 1 hold
					case 0x09C000:
						break;
					case 0x095000:		// Line 2 hold
					case 0x09D000:
						break;
					case 0x096000:		// Line 1 A-lead
					case 0x09E000:
						break;
					case 0x097000:		// Line 2 A-lead
					case 0x09F000:
						break;
				}
				break;
			case 0x0A0000:				// Miscellaneous Control Register
				// TODO: how to handle this in 8bit mode?
/*
				// TODO: handle the ctrl bits properly
				if ((address & 1) == 0) {
					// low byte
				} else {
					// hight byte
					// TODO: &0x8000 --> dismiss 60hz intr
					state.dma_reading = (value & 0x40);
					state.leds = (~value & 0xF);
				}
				printf("LEDs: %s %s %s %s\n",
						(state.leds & 8) ? "R" : "-",
						(state.leds & 4) ? "G" : "-",
						(state.leds & 2) ? "Y" : "-",
						(state.leds & 1) ? "R" : "-");
				handled = true;
*/
				break;
			case 0x0B0000:				// TM/DIALWR
				break;
			case 0x0C0000:				// Clear Status Register
				state.genstat = 0xFFFF;
				state.bsr0 = 0xFFFF;
				state.bsr1 = 0xFFFF;
				handled = true;
				break;
			case 0x0D0000:				// DMA Address Register
				if (address & 0x004000) {
					// A14 high -- set most significant bits
					state.dma_address = (state.dma_address & 0x1fe) | ((address & 0x3ffe) << 8);
				} else {
					// A14 low -- set least significant bits
					state.dma_address = (state.dma_address & 0x3ffe00) | (address & 0x1fe);
				}
				printf("WR08 DMA_ADDR %s, now %08X\n", address & 0x004000 ? "HI" : "LO", state.dma_address);
				handled = true;
				break;
			case 0x0E0000:				// Disk Control Register
				// B7 = FDD controller reset
				if ((value & 0x80) == 0) wd2797_reset(&state.fdc_ctx);
				// B6 = drive 0 select -- TODO
				// B5 = motor enable -- TODO
				// B4 = HDD controller reset -- TODO
				// B3 = HDD0 select -- TODO
				// B2,1,0 = HDD0 head select
				handled = true;
				break;
			case 0x0F0000:				// Line Printer Data Register
				break;
		}
	} else if ((address >= 0xC00000) && (address <= 0xFFFFFF)) {
		// I/O register space, zone B
		switch (address & 0xF00000) {
			case 0xC00000:				// Expansion slots
			case 0xD00000:
				switch (address & 0xFC0000) {
					case 0xC00000:		// Expansion slot 0
					case 0xC40000:		// Expansion slot 1
					case 0xC80000:		// Expansion slot 2
					case 0xCC0000:		// Expansion slot 3
					case 0xD00000:		// Expansion slot 4
					case 0xD40000:		// Expansion slot 5
					case 0xD80000:		// Expansion slot 6
					case 0xDC0000:		// Expansion slot 7
						fprintf(stderr, "NOTE: WR8 to expansion card space, addr=0x%08X, data=0x%08X\n", address, value);
						break;
				}
				break;
			case 0xE00000:				// HDC, FDC, MCR2 and RTC data bits
			case 0xF00000:
				switch (address & 0x070000) {
					case 0x000000:		// [ef][08]xxxx ==> WD1010 hard disc controller
						break;
					case 0x010000:		// [ef][19]xxxx ==> WD2797 floppy disc controller
						printf("WD279X: wr8 %02X ==> %02X\n", (address >> 1) & 3, value);
						wd2797_write_reg(&state.fdc_ctx, (address >> 1) & 3, value);
						handled = true;
						break;
					case 0x020000:		// [ef][2a]xxxx ==> Miscellaneous Control Register 2
						break;
					case 0x030000:		// [ef][3b]xxxx ==> Real Time Clock data bits
						break;
					case 0x040000:		// [ef][4c]xxxx ==> General Control Register
						switch (address & 0x077000) {
							case 0x040000:		// [ef][4c][08]xxx ==> EE
								break;
							case 0x041000:		// [ef][4c][19]xxx ==> PIE
								if ((address & 1) == 0)
									state.pie = ((value & 0x80) == 0x80);
								handled = true;
								break;
							case 0x042000:		// [ef][4c][2A]xxx ==> BP
								break;
							case 0x043000:		// [ef][4c][3B]xxx ==> ROMLMAP
								if ((address & 1) == 0)
									state.romlmap = ((value & 0x80) == 0x80);
								handled = true;
								break;
							case 0x044000:		// [ef][4c][4C]xxx ==> L1 MODEM
								break;
							case 0x045000:		// [ef][4c][5D]xxx ==> L2 MODEM
								break;
							case 0x046000:		// [ef][4c][6E]xxx ==> D/N CONNECT
								break;
							case 0x047000:		// [ef][4c][7F]xxx ==> Whole screen reverse video
								break;
						}
					case 0x050000:		// [ef][5d]xxxx ==> 8274
						break;
					case 0x060000:		// [ef][6e]xxxx ==> Control regs
						switch (address & 0x07F000) {
							default:
								break;
						}
						break;
					case 0x070000:		// [ef][7f]xxxx ==> 6850 Keyboard Controller
						break;
					default:
						fprintf(stderr, "NOTE: WR8 to undefined E/F-block space, addr=0x%08X, data=0x%08X\n", address, value);
						break;
				}
		}
	}

	LOG_NOT_HANDLED_W(8);
}


// for the disassembler
uint32_t m68k_read_disassembler_32(uint32_t addr) { return m68k_read_memory_32(addr); }
uint32_t m68k_read_disassembler_16(uint32_t addr) { return m68k_read_memory_16(addr); }
uint32_t m68k_read_disassembler_8 (uint32_t addr) { return m68k_read_memory_8 (addr); }


