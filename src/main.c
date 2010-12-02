#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <malloc.h>
#include <string.h>

#include "SDL.h"

#include "musashi/m68k.h"
#include "version.h"
#include "state.h"

void FAIL(char *err)
{
	state_done();
	fprintf(stderr, "ERROR: %s\nExiting...\n", err);
	exit(EXIT_FAILURE);
}

/***********************************
 * Array read/write utility macros
 * "Don't Repeat Yourself" :)
 ***********************************/

/// Array read, 32-bit
#define RD32(array, address, andmask)							\
	(((uint32_t)array[(address + 0) & (andmask)] << 24) |		\
	 ((uint32_t)array[(address + 1) & (andmask)] << 16) |		\
	 ((uint32_t)array[(address + 2) & (andmask)] << 8)  |		\
	 ((uint32_t)array[(address + 3) & (andmask)]))

/// Array read, 16-bit
#define RD16(array, address, andmask)							\
	(((uint32_t)array[(address + 0) & (andmask)] << 8)  |		\
	 ((uint32_t)array[(address + 1) & (andmask)]))

/// Array read, 8-bit
#define RD8(array, address, andmask)							\
	((uint32_t)array[(address + 0) & (andmask)])

/// Array write, 32-bit
#define WR32(array, address, andmask, value) {					\
	array[(address + 0) & (andmask)] = (value >> 24) & 0xff;	\
	array[(address + 1) & (andmask)] = (value >> 16) & 0xff;	\
	array[(address + 2) & (andmask)] = (value >> 8)  & 0xff;	\
	array[(address + 3) & (andmask)] =  value        & 0xff;	\
}

/// Array write, 16-bit
#define WR16(array, address, andmask, value) {					\
	array[(address + 0) & (andmask)] = (value >> 8)  & 0xff;	\
	array[(address + 1) & (andmask)] =  value        & 0xff;	\
}

/// Array write, 8-bit
#define WR8(array, address, andmask, value)						\
	array[(address + 0) & (andmask)] =  value        & 0xff;

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

typedef enum {
	MEM_ALLOWED = 0,
	MEM_PAGEFAULT,		// Page fault -- page not present
	MEM_PAGE_NO_WE,		// Page not write enabled
	MEM_KERNEL,			// User attempted to access kernel memory
	MEM_UIE				// User Nonmemory Location Access
} MEM_STATUS;

// check memory access permissions
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
#define ACCESS_CHECK_WR() do {									\
		/* MEM_STATUS st; */									\
		switch (checkMemoryAccess(address, true)) {				\
			case MEM_ALLOWED:									\
				/* Access allowed */							\
				break;											\
			case MEM_PAGEFAULT:									\
				/* Page fault */								\
				state.genstat = 0x8FFF;							\
				m68k_pulse_bus_error();							\
				return;											\
			case MEM_UIE:										\
				/* User access to memory above 4MB */			\
				state.genstat = 0x9EFF;							\
				m68k_pulse_bus_error();							\
				return;											\
			case MEM_KERNEL:									\
			case MEM_PAGE_NO_WE:								\
				/* kernel access or page not write enabled */	\
				/* TODO: which regs need setting? */			\
				m68k_pulse_bus_error();							\
				return;											\
		}														\
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
#define ACCESS_CHECK_RD() do {									\
		/* MEM_STATUS st; */									\
		switch (checkMemoryAccess(address, false)) {			\
			case MEM_ALLOWED:									\
				/* Access allowed */							\
				break;											\
			case MEM_PAGEFAULT:									\
				/* Page fault */								\
				state.genstat = 0xCFFF;							\
				m68k_pulse_bus_error();							\
				return 0xFFFFFFFF;								\
			case MEM_UIE:										\
				/* User access to memory above 4MB */			\
				state.genstat = 0xDEFF;							\
				m68k_pulse_bus_error();							\
				return 0xFFFFFFFF;								\
			case MEM_KERNEL:									\
			case MEM_PAGE_NO_WE:								\
				/* kernel access or page not write enabled */	\
				/* TODO: which regs need setting? */			\
				m68k_pulse_bus_error();							\
				return 0xFFFFFFFF;								\
		}														\
	} while (false)

/**
 * @brief Read M68K memory, 32-bit
 */
uint32_t m68k_read_memory_32(uint32_t address)
{
	uint32_t data = 0xFFFFFFFF;

	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	// Check access permissions
	ACCESS_CHECK_RD();

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
		data = RD32(state.rom, address, ROM_SIZE - 1);
	} else if (address <= (state.ram_size - 1)) {
		// RAM access
		data = RD32(state.ram, mapAddr(address, false), state.ram_size - 1);
	} else if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O register space, zone A
//		printf("RD32 0x%08X ==> ??? %s\n", address, m68k_get_reg(NULL, M68K_REG_SR) & 0x2000 ? "[SV]" : "");
		switch (address & 0x0F0000) {
			case 0x000000:				// Map RAM access
				if (address > 0x4007FF) fprintf(stderr, "NOTE: RD32 from MapRAM mirror, addr=0x%08X\n", address);
				data = RD32(state.map, address, 0x7FF);
				break;
			case 0x010000:				// General Status Register
				data = ((uint32_t)state.genstat << 16) + (uint32_t)state.genstat;
				break;
			case 0x020000:				// Video RAM
				if (address > 0x427FFF) fprintf(stderr, "NOTE: RD32 from VideoRAM mirror, addr=0x%08X\n", address);
				data = RD32(state.vram, address, 0x7FFF);
				break;
			case 0x030000:				// Bus Status Register 0
				break;
			case 0x040000:				// Bus Status Register 1
				break;
			case 0x050000:				// Phone status
				break;
			case 0x060000:				// DMA Count
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
				break;
			case 0x0B0000:				// TM/DIALWR
				break;
			case 0x0C0000:				// CSR
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
//		printf("RD32 0x%08X ==> ??? %s\n", address, m68k_get_reg(NULL, M68K_REG_SR) & 0x2000 ? "[SV]" : "");
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
						break;
					case 0x020000:		// [ef][2a]xxxx ==> Miscellaneous Control Register 2
						break;
					case 0x030000:		// [ef][3b]xxxx ==> Real Time Clock data bits
						break;
					case 0x040000:		// [ef][4c]xxxx ==> General Control Register
						switch (address & 0x077000) {
							case 0x040000:		// [ef][4c][08]xxx ==> EE
								break;
							case 0x041000:		// [ef][4c][19]xxx ==> P1E
								break;
							case 0x042000:		// [ef][4c][2A]xxx ==> BP
								break;
							case 0x043000:		// [ef][4c][3B]xxx ==> ROMLMAP
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
					case 0x060000:		// [ef][6e]xxxx ==> Control regs
						switch (address & 0x07F000) {
							default:
								break;
						}
						break;
					case 0x070000:		// [ef][7f]xxxx ==> 6850 Keyboard Controller
					default:
						fprintf(stderr, "NOTE: RD32 from undefined E/F-block address 0x%08X", address);
				}
		}
	}
	return data;
}

/**
 * @brief Read M68K memory, 16-bit
 */
uint32_t m68k_read_memory_16(uint32_t address)
{
	uint16_t data = 0xFFFF;

	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	// Check access permissions
	ACCESS_CHECK_RD();

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
		data = RD16(state.rom, address, ROM_SIZE - 1);
	} else if (address <= (state.ram_size - 1)) {
		// RAM access
		data = RD16(state.ram, mapAddr(address, false), state.ram_size - 1);
	} else if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O register space, zone A
//		printf("RD16 0x%08X ==> ??? %s\n", address, m68k_get_reg(NULL, M68K_REG_SR) & 0x2000 ? "[SV]" : "");
		switch (address & 0x0F0000) {
			case 0x000000:				// Map RAM access
				if (address > 0x4007FF) fprintf(stderr, "NOTE: RD16 from MapRAM mirror, addr=0x%08X\n", address);
				data = RD16(state.map, address, 0x7FF);
				break;
			case 0x010000:				// General Status Register
				data = state.genstat;
				break;
			case 0x020000:				// Video RAM
				if (address > 0x427FFF) fprintf(stderr, "NOTE: RD16 from VideoRAM mirror, addr=0x%08X\n", address);
				data = RD16(state.vram, address, 0x7FFF);
				break;
			case 0x030000:				// Bus Status Register 0
				break;
			case 0x040000:				// Bus Status Register 1
				break;
			case 0x050000:				// Phone status
				break;
			case 0x060000:				// DMA Count
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
				break;
			case 0x0B0000:				// TM/DIALWR
				break;
			case 0x0C0000:				// CSR
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
//		printf("RD16 0x%08X ==> ??? %s\n", address, m68k_get_reg(NULL, M68K_REG_SR) & 0x2000 ? "[SV]" : "");
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
						break;
					case 0x020000:		// [ef][2a]xxxx ==> Miscellaneous Control Register 2
						break;
					case 0x030000:		// [ef][3b]xxxx ==> Real Time Clock data bits
						break;
					case 0x040000:		// [ef][4c]xxxx ==> General Control Register
						switch (address & 0x077000) {
							case 0x040000:		// [ef][4c][08]xxx ==> EE
								break;
							case 0x041000:		// [ef][4c][19]xxx ==> P1E
								break;
							case 0x042000:		// [ef][4c][2A]xxx ==> BP
								break;
							case 0x043000:		// [ef][4c][3B]xxx ==> ROMLMAP
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
					case 0x060000:		// [ef][6e]xxxx ==> Control regs
						switch (address & 0x07F000) {
							default:
								break;
						}
						break;
					case 0x070000:		// [ef][7f]xxxx ==> 6850 Keyboard Controller
					default:
						fprintf(stderr, "NOTE: RD16 from undefined E/F-block address 0x%08X", address);
				}
		}
	}
	return data;
}

/**
 * @brief Read M68K memory, 8-bit
 */
uint32_t m68k_read_memory_8(uint32_t address)
{
	uint8_t data = 0xFF;

	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	// Check access permissions
	ACCESS_CHECK_RD();

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
		data = RD8(state.rom, address, ROM_SIZE - 1);
	} else if (address <= (state.ram_size - 1)) {
		// RAM access
		data = RD8(state.ram, mapAddr(address, false), state.ram_size - 1);
	} else if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O register space, zone A
//		printf("RD8 0x%08X ==> ??? %s\n", address, m68k_get_reg(NULL, M68K_REG_SR) & 0x2000 ? "[SV]" : "");
		switch (address & 0x0F0000) {
			case 0x000000:				// Map RAM access
				if (address > 0x4007FF) fprintf(stderr, "NOTE: RD8 from MapRAM mirror, addr=0x%08X\n", address);
				data = RD8(state.map, address, 0x7FF);
				break;
			case 0x010000:				// General Status Register
				if ((address & 1) == 0)
					data = (state.genstat >> 8) & 0xff;
				else
					data = (state.genstat)      & 0xff;
				break;
			case 0x020000:				// Video RAM
				if (address > 0x427FFF) fprintf(stderr, "NOTE: RD8 from VideoRAM mirror, addr=0x%08X\n", address);
				data = RD8(state.vram, address, 0x7FFF);
				break;
			case 0x030000:				// Bus Status Register 0
				break;
			case 0x040000:				// Bus Status Register 1
				break;
			case 0x050000:				// Phone status
				break;
			case 0x060000:				// DMA Count
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
				break;
			case 0x0B0000:				// TM/DIALWR
				break;
			case 0x0C0000:				// CSR
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
//		printf("RD8 0x%08X ==> ??? %s\n", address, m68k_get_reg(NULL, M68K_REG_SR) & 0x2000 ? "[SV]" : "");
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
						break;
					case 0x020000:		// [ef][2a]xxxx ==> Miscellaneous Control Register 2
						break;
					case 0x030000:		// [ef][3b]xxxx ==> Real Time Clock data bits
						break;
					case 0x040000:		// [ef][4c]xxxx ==> General Control Register
						switch (address & 0x077000) {
							case 0x040000:		// [ef][4c][08]xxx ==> EE
								break;
							case 0x041000:		// [ef][4c][19]xxx ==> P1E
								break;
							case 0x042000:		// [ef][4c][2A]xxx ==> BP
								break;
							case 0x043000:		// [ef][4c][3B]xxx ==> ROMLMAP
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
					case 0x060000:		// [ef][6e]xxxx ==> Control regs
						switch (address & 0x07F000) {
							default:
								break;
						}
						break;
					case 0x070000:		// [ef][7f]xxxx ==> 6850 Keyboard Controller
					default:
						fprintf(stderr, "NOTE: RD8 from undefined E/F-block address 0x%08X", address);
				}
		}
	}
	return data;
}

/**
 * @brief Write M68K memory, 32-bit
 */
void m68k_write_memory_32(uint32_t address, uint32_t value)
{
	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	// Check access permissions
	ACCESS_CHECK_WR();

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
		WR32(state.rom, address, ROM_SIZE - 1, value);
	} else if (address <= (state.ram_size - 1)) {
		// RAM access
		WR32(state.ram, mapAddr(address, false), state.ram_size - 1, value);
	} else if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O register space, zone A
//		printf("WR32 0x%08X ==> 0x%08X %s\n", address, value, m68k_get_reg(NULL, M68K_REG_SR) & 0x2000 ? "[SV]" : "");
		switch (address & 0x0F0000) {
			case 0x000000:				// Map RAM access
				if (address > 0x4007FF) fprintf(stderr, "NOTE: WR32 to MapRAM mirror, addr=0x%08X, data=0x%08X\n", address, value);
				WR32(state.map, address, 0x7FF, value);
				break;
			case 0x010000:				// General Status Register
				state.genstat = (value & 0xffff);
				break;
			case 0x020000:				// Video RAM
				if (address > 0x427FFF) fprintf(stderr, "NOTE: WR32 to VideoRAM mirror, addr=0x%08X, data=0x%08X\n", address, value);
				WR32(state.vram, address, 0x7FFF, value);
				break;
			case 0x030000:				// Bus Status Register 0
				break;
			case 0x040000:				// Bus Status Register 1
				break;
			case 0x050000:				// Phone status
				break;
			case 0x060000:				// DMA Count
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
				break;
			case 0x0B0000:				// TM/DIALWR
				break;
			case 0x0C0000:				// CSR
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
//		printf("WR32 0x%08X ==> 0x%08X %s\n", address, value, m68k_get_reg(NULL, M68K_REG_SR) & 0x2000 ? "[SV]" : "");
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
						break;
				}
				break;
			case 0xE00000:				// HDC, FDC, MCR2 and RTC data bits
			case 0xF00000:
				switch (address & 0x070000) {
					case 0x000000:		// [ef][08]xxxx ==> WD1010 hard disc controller
						break;
					case 0x010000:		// [ef][19]xxxx ==> WD2797 floppy disc controller
						break;
					case 0x020000:		// [ef][2a]xxxx ==> Miscellaneous Control Register 2
						break;
					case 0x030000:		// [ef][3b]xxxx ==> Real Time Clock data bits
						break;
					case 0x040000:		// [ef][4c]xxxx ==> General Control Register
						switch (address & 0x077000) {
							case 0x040000:		// [ef][4c][08]xxx ==> EE
								break;
							case 0x041000:		// [ef][4c][19]xxx ==> P1E
								break;
							case 0x042000:		// [ef][4c][2A]xxx ==> BP
								break;
							case 0x043000:		// [ef][4c][3B]xxx ==> ROMLMAP
								state.romlmap = ((value & 0x8000) == 0x8000);
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
						fprintf(stderr, "NOTE: WR32 to undefined E/F-block space, addr=0x%08X, data=0x%08X\n", address, value);
				}
		}
	}
}

/**
 * @brief Write M68K memory, 16-bit
 */
void m68k_write_memory_16(uint32_t address, uint32_t value)
{
	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	// Check access permissions
	ACCESS_CHECK_WR();

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
		WR16(state.rom, address, ROM_SIZE - 1, value);
	} else if (address <= (state.ram_size - 1)) {
		// RAM access
		WR16(state.ram, mapAddr(address, false), state.ram_size - 1, value);
	} else if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O register space, zone A
//		printf("WR16 0x%08X ==> 0x%04X %s\n", address, value, m68k_get_reg(NULL, M68K_REG_SR) & 0x2000 ? "[SV]" : "");
		switch (address & 0x0F0000) {
			case 0x000000:				// Map RAM access
				if (address > 0x4007FF) fprintf(stderr, "NOTE: WR16 to MapRAM mirror, addr=0x%08X, data=0x%04X\n", address, value);
				WR16(state.map, address, 0x7FF, value);
				break;
			case 0x010000:				// General Status Register
				state.genstat = (value & 0xffff);
				break;
			case 0x020000:				// Video RAM
				if (address > 0x427FFF) fprintf(stderr, "NOTE: WR16 to VideoRAM mirror, addr=0x%08X, data=0x%04X\n", address, value);
				WR16(state.vram, address, 0x7FFF, value);
				break;
			case 0x030000:				// Bus Status Register 0
				break;
			case 0x040000:				// Bus Status Register 1
				break;
			case 0x050000:				// Phone status
				break;
			case 0x060000:				// DMA Count
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
				break;
			case 0x0B0000:				// TM/DIALWR
				break;
			case 0x0C0000:				// CSR
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
//		printf("WR16 0x%08X ==> 0x%04X %s\n", address, value, m68k_get_reg(NULL, M68K_REG_SR) & 0x2000 ? "[SV]" : "");
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
						break;
					case 0x020000:		// [ef][2a]xxxx ==> Miscellaneous Control Register 2
						break;
					case 0x030000:		// [ef][3b]xxxx ==> Real Time Clock data bits
						break;
					case 0x040000:		// [ef][4c]xxxx ==> General Control Register
						switch (address & 0x077000) {
							case 0x040000:		// [ef][4c][08]xxx ==> EE
								break;
							case 0x041000:		// [ef][4c][19]xxx ==> P1E
								break;
							case 0x042000:		// [ef][4c][2A]xxx ==> BP
								break;
							case 0x043000:		// [ef][4c][3B]xxx ==> ROMLMAP
								state.romlmap = ((value & 0x8000) == 0x8000);
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
						fprintf(stderr, "NOTE: WR32 to undefined E/F-block space, addr=0x%08X, data=0x%08X\n", address, value);
				}
		}
	}
}

/**
 * @brief Write M68K memory, 8-bit
 */
void m68k_write_memory_8(uint32_t address, uint32_t value)
{
	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	// Check access permissions
	ACCESS_CHECK_WR();

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
		WR8(state.rom, address, ROM_SIZE - 1, value);
	} else if (address <= (state.ram_size - 1)) {
		// RAM access
		WR8(state.ram, mapAddr(address, false), state.ram_size - 1, value);
	} else if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O register space, zone A
//		printf("WR8 0x%08X ==> %02X %s\n", address, value, m68k_get_reg(NULL, M68K_REG_SR) & 0x2000 ? "[SV]" : "");
		switch (address & 0x0F0000) {
			case 0x000000:				// Map RAM access
				if (address > 0x4007FF) fprintf(stderr, "NOTE: WR8 to MapRAM mirror, addr=%08X, data=%02X\n", address, value);
				WR8(state.map, address, 0x7FF, value);
				break;
			case 0x010000:				// General Status Register
				state.genstat = (value & 0xffff);
				break;
			case 0x020000:				// Video RAM
				if (address > 0x427FFF) fprintf(stderr, "NOTE: WR8 to VideoRAM mirror, addr=%08X\n, data=0x%02X", address, value);
				WR8(state.vram, address, 0x7FFF, value);
				break;
			case 0x030000:				// Bus Status Register 0
				break;
			case 0x040000:				// Bus Status Register 1
				break;
			case 0x050000:				// Phone status
				break;
			case 0x060000:				// DMA Count
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
				break;
			case 0x0B0000:				// TM/DIALWR
				break;
			case 0x0C0000:				// CSR
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
//		printf("WR8 0x%08X ==> 0x%08X %s\n", address, value, m68k_get_reg(NULL, M68K_REG_SR) & 0x2000 ? "[SV]" : "");
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
						break;
					case 0x020000:		// [ef][2a]xxxx ==> Miscellaneous Control Register 2
						break;
					case 0x030000:		// [ef][3b]xxxx ==> Real Time Clock data bits
						break;
					case 0x040000:		// [ef][4c]xxxx ==> General Control Register
						switch (address & 0x077000) {
							case 0x040000:		// [ef][4c][08]xxx ==> EE
								break;
							case 0x041000:		// [ef][4c][19]xxx ==> P1E
								break;
							case 0x042000:		// [ef][4c][2A]xxx ==> BP
								break;
							case 0x043000:		// [ef][4c][3B]xxx ==> ROMLMAP
								if ((address & 1) == 0)
								state.romlmap = ((value & 0x8000) == 0x8000);
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
}


// for the disassembler
uint32_t m68k_read_disassembler_32(uint32_t addr) { return m68k_read_memory_32(addr); }
uint32_t m68k_read_disassembler_16(uint32_t addr) { return m68k_read_memory_16(addr); }
uint32_t m68k_read_disassembler_8 (uint32_t addr) { return m68k_read_memory_8 (addr); }


/****************************
 * blessed be thy main()...
 ****************************/

int main(void)
{
	// copyright banner
	printf("FreeBee: A Quick-and-Dirty AT&T 3B1 Emulator. Version %s, %s mode.\n", VER_FULLSTR, VER_BUILD_TYPE);
	printf("Copyright (C) 2010 P. A. Pemberton. All rights reserved.\nLicensed under the Apache License Version 2.0.\n");
	printf("Musashi M680x0 emulator engine developed by Karl Stenerud <kstenerud@gmail.com>\n");
	printf("Built %s by %s@%s.\n", VER_COMPILE_DATETIME, VER_COMPILE_BY, VER_COMPILE_HOST);
	printf("Compiler: %s\n", VER_COMPILER);
	printf("CFLAGS: %s\n", VER_CFLAGS);
	printf("\n");

	// set up system state
	// 512K of RAM
	state_init(512*1024);

	// set up musashi and reset the CPU
	m68k_set_cpu_type(M68K_CPU_TYPE_68010);
	m68k_pulse_reset();

	// Set up SDL
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) == -1) {
		printf("Could not initialise SDL: %s.\n", SDL_GetError());
		exit(EXIT_FAILURE);
	}

	// Make sure SDL cleans up after itself
	atexit(SDL_Quit);

	// Set up the video display
	SDL_Surface *screen = NULL;
	if ((screen = SDL_SetVideoMode(720, 384, 8, SDL_SWSURFACE | SDL_ANYFORMAT)) == NULL) {
		printf("Could not find a suitable video mode: %s.\n", SDL_GetError());
		exit(EXIT_FAILURE);
	}
	printf("Set %dx%d at %d bits-per-pixel mode\n\n", screen->w, screen->h, screen->format->BitsPerPixel);
	SDL_WM_SetCaption("FreeBee 3B1 emulator", "FreeBee");

	/***
	 * The 3B1 CPU runs at 10MHz, with DMA running at 1MHz and video refreshing at
	 * around 60Hz (???), with a 60Hz periodic interrupt.
	 */
	const uint32_t TIMESLOT_FREQUENCY = 240;	// Hz
	const uint32_t MILLISECS_PER_TIMESLOT = 1e3 / TIMESLOT_FREQUENCY;
	const uint32_t CLOCKS_PER_60HZ = (10e6 / 60);
	uint32_t next_timeslot = SDL_GetTicks() + MILLISECS_PER_TIMESLOT;
	uint32_t clock_cycles = 0;
	bool exitEmu = false;
	for (;;) {
		// Run the CPU for however many cycles we need to. CPU core clock is
		// 10MHz, and we're running at 240Hz/timeslot. Thus: 10e6/240 or
		// 41667 cycles per timeslot.
		clock_cycles += m68k_execute(10e6/TIMESLOT_FREQUENCY);

		// TODO: run DMA here

		// Is it time to run the 60Hz periodic interrupt yet?
		if (clock_cycles > CLOCKS_PER_60HZ) {
			// TODO: refresh screen
			// TODO: trigger periodic interrupt (if enabled)
			// decrement clock cycle counter, we've handled the intr.
			clock_cycles -= CLOCKS_PER_60HZ;
		}

		// make sure frame rate is equal to real time
		uint32_t now = SDL_GetTicks();
		if (now < next_timeslot) {
			// timeslot finished early -- eat up some time
			SDL_Delay(next_timeslot - now);
		} else {
			// timeslot finished late -- skip ahead to gain time
			// TODO: if this happens a lot, we should let the user know
			// that their PC might not be fast enough...
			next_timeslot = now;
		}
		// advance to the next timeslot
		next_timeslot += MILLISECS_PER_TIMESLOT;

		// if we've been asked to exit the emulator, then do so.
		if (exitEmu) break;
	}

	// shut down and exit
	SDL_Quit();

	return 0;
}
