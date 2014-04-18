#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include "musashi/m68k.h"
#include "state.h"
#include "utils.h"
#include "memory.h"

// The value which will be returned if the CPU attempts to read from empty memory
// TODO (FIXME?) - need to figure out if R/W ops wrap around. This seems to appease the UNIX kernel and P4TEST.
#define EMPTY 0xFFFFFFFFUL
//#define EMPTY 0x55555555UL
//#define EMPTY 0x00000000UL

/******************
 * Memory mapping
 ******************/

#define MAPRAM(addr) (((uint16_t)state.map[addr*2] << 8) + ((uint16_t)state.map[(addr*2)+1]))

static uint32_t map_address_debug(uint32_t addr)
{
	uint16_t page = (addr >> 12) & 0x3FF;

	// Look it up in the map RAM and get the physical page address
	uint32_t new_page_addr = MAPRAM(page) & 0x3FF;
	return (new_page_addr << 12) + (addr & 0xFFF);
}

uint32_t mapAddr(uint32_t addr, bool writing)/*{{{*/
{
	if (addr < 0x400000) {
		// RAM access. Check against the Map RAM
		// Start by getting the original page address
		uint16_t page = (addr >> 12) & 0x3FF;

		// Look it up in the map RAM and get the physical page address
		uint32_t new_page_addr = MAPRAM(page) & 0x3FF;

		// Update the Page Status bits
		uint8_t pagebits = (MAPRAM(page) >> 13) & 0x03;
		// Pagebits --
		//   0 = not present
		//   1 = present but not accessed
		//   2 = present, accessed (read from)
		//   3 = present, dirty (written to)
		switch (pagebits) {
			case 0:
				// Page not present
				// This should cause a page fault
				LOGS("Whoa! Pagebit update, when the page is not present!");
				break;

			case 1:
				// Page present -- first access
				state.map[page*2] &= 0x9F;	// turn off "present" bit (but not write enable!)
				if (writing)
					state.map[page*2] |= 0x60;		// Page written to (dirty)
				else
					state.map[page*2] |= 0x40;		// Page accessed but not written
				break;

			case 2:
			case 3:
				// Page present, 2nd or later access
				if (writing)
					state.map[page*2] |= 0x60;		// Page written to (dirty)
				break;
		}

		// Return the address with the new physical page spliced in
		return (new_page_addr << 12) + (addr & 0xFFF);
	} else {
		// I/O, VRAM or MapRAM space; no mapping is performed or required
		// TODO: assert here?
		return addr;
	}
}/*}}}*/

MEM_STATUS checkMemoryAccess(uint32_t addr, bool writing, bool dma)/*{{{*/
{
	// Get the page bits for this page.
	uint16_t page = (addr >> 12) & 0x3FF;
	uint8_t pagebits = (MAPRAM(page) >> 13) & 0x07;

	// Check page is present (but only for RAM zone)
	if ((addr < 0x400000) && ((pagebits & 0x03) == 0)) {
		LOG("Page not mapped in: addr %08X, page %04X, mapbits %04X", addr, page, MAPRAM(page));
		return MEM_PAGEFAULT;
	}

	// Are we in Supervisor mode?
	if (dma || (m68k_get_reg(NULL, M68K_REG_SR) & 0x2000))
		// Yes. We can do anything we like.
		return MEM_ALLOWED;

	// If we're here, then we must be in User mode.
	// Check that the user didn't access memory outside of the RAM area
	if (addr >= 0x400000) {
		LOGS("User accessed privileged memory");
		return MEM_UIE;
	}

	// User attempt to access the kernel
	// A19, A20, A21, A22 low (kernel access): RAM addr before paging; not in Supervisor mode
	if (((addr >> 19) & 0x0F) == 0 && !(!writing && addr <= 0x1000)) {
		LOGS("Attempt by user code to access kernel space");
		return MEM_KERNEL;
	}

	// Check page is write enabled
	if (writing && ((pagebits & 0x04) == 0)) {
		LOG("Page not write enabled: inaddr %08X, page %04X, mapram %04X [%02X %02X], pagebits %d",
				addr, page, MAPRAM(page), state.map[page*2], state.map[(page*2)+1], pagebits);
		return MEM_PAGE_NO_WE;
	}
	// Page access allowed.
	return MEM_ALLOWED;
}/*}}}*/

#define _ACCESS_CHECK_WR_BYTE(address)								\
	do {															\
		switch (st = checkMemoryAccess(address, true, false)) {			\
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
				/* XXX: is this the correct value? */				\
				state.genstat = 0x9BFF | (state.pie ? 0x0400 : 0);	\
				fault = true;										\
				break;												\
		}															\
	}while (0)
	


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
/*{{{ macro: ACCESS_CHECK_WR(address, bits)*/
#define ACCESS_CHECK_WR(address, bits)								\
	do {															\
		bool fault = false;											\
		MEM_STATUS st;												\
		_ACCESS_CHECK_WR_BYTE(address);								\
		if (!fault && bits == 32									\
				&& ((address + 3) & ~0xfff) != ((address & ~0xfff))){	\
			_ACCESS_CHECK_WR_BYTE(address + 3);						\
		}															\
		if (fault) {												\
			if (bits >= 16)											\
				state.bsr0 = 0x7C00;								\
			else													\
				state.bsr0 = (address & 1) ? 0x7E00 : 0x7D00;		\
			state.bsr0 |= (address >> 16);							\
			state.bsr1 = address & 0xffff;							\
			LOG("Bus Error while writing, addr %08X, statcode %d", address, st);		\
			if (state.ee) m68k_pulse_bus_error();					\
			return;													\
		}															\
	} while (0)
/*}}}*/

#define _ACCESS_CHECK_RD_BYTE(address)									\
	do {															\
		switch (st = checkMemoryAccess(address, false, false)) {	\
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
				/* XXX: is this the correct value? */				\
				state.genstat = 0xDBFF | (state.pie ? 0x0400 : 0);	\
				fault = true;										\
				break;												\
		}															\
	} while (0)	

/**
 * @brief Check memory access permissions for a read operation.
 * @note	This used to be a single macro (merged with ACCESS_CHECK_WR), but
 * 			gcc throws warnings when you have a return-with-value in a void
 * 			function, even if the return-with-value is completely unreachable.
 * 			Similarly it doesn't like it if you have a return without a value
 * 			in a non-void function, even if it's impossible to ever reach the
 * 			return-with-no-value. UGH!
 */
/*{{{ macro: ACCESS_CHECK_RD(address, bits)*/
#define ACCESS_CHECK_RD(address, bits)								\
	do {															\
		bool fault = false;											\
		uint32_t faultAddr = address;								\
		MEM_STATUS st;												\
		_ACCESS_CHECK_RD_BYTE(address);								\
		if (!fault && bits == 32									\
				&& ((address + 2) & ~0xfff) != (address & ~0xfff)){	\
			_ACCESS_CHECK_RD_BYTE(address + 2);						\
			if (fault) faultAddr = address + 2;						\
		}															\
																	\
		if (fault) {												\
			if (bits >= 16)											\
				state.bsr0 = 0x7C00;								\
			else													\
				state.bsr0 = (faultAddr & 1) ? 0x7E00 : 0x7D00;		\
			state.bsr0 |= (faultAddr >> 16);							\
			state.bsr1 = faultAddr & 0xffff;							\
			LOG("Bus Error while reading, addr %08X, statcode %d", faultAddr, st);		\
			if (state.ee) m68k_pulse_bus_error();					\
			if (bits >= 32)											\
				return EMPTY & 0xFFFFFFFF;									\
			else													\
				return EMPTY & ((1ULL << bits)-1);								\
		}															\
	} while (0)
/*}}}*/

bool access_check_dma(int reading)
{
	// Check memory access permissions
	bool access_ok = false;
	switch (checkMemoryAccess(state.dma_address, !reading, true)) {
		case MEM_PAGEFAULT:
			// Page fault
			state.genstat = 0xABFF
				| (reading ? 0x4000 : 0)
				| (state.pie ? 0x0400 : 0);
			access_ok = false;
			break;

		case MEM_UIE:
			// User access to memory above 4MB
			// FIXME? Shouldn't be possible with DMA... assert this?
			state.genstat = 0xBAFF
				| (reading ? 0x4000 : 0)
				| (state.pie ? 0x0400 : 0);
			access_ok = false;
			break;

		case MEM_KERNEL:
		case MEM_PAGE_NO_WE:
			// Kernel access or page not write enabled
			/* XXX: is this correct? */
			state.genstat = 0xBBFF
				| (reading ? 0x4000 : 0)
				| (state.pie ? 0x0400 : 0);
			access_ok = false;
			break;

		case MEM_ALLOWED:
			access_ok = true;
			break;
	}
	if (!access_ok) {
		state.bsr0 = 0x3C00;
		state.bsr0 |= (state.dma_address >> 16);
		state.bsr1 = state.dma_address & 0xffff;
		if (state.ee) m68k_set_irq(7);
		printf("BUS ERROR FROM DMA: genstat=%04X, bsr0=%04X, bsr1=%04X\n", state.genstat, state.bsr0, state.bsr1);
	}
	return (access_ok);
}

// Logging macros
#define LOG_NOT_HANDLED_R(bits)															\
	if (!handled) printf("unhandled read%02d, addr=0x%08X\n", bits, address);

#define LOG_NOT_HANDLED_W(bits)															\
	if (!handled) printf("unhandled write%02d, addr=0x%08X, data=0x%08X\n", bits, address, data);

/********************************************************
 * I/O read/write functions
 ********************************************************/

/**
 * Issue a warning if a read operation is made with an invalid size
 */
inline static void ENFORCE_SIZE(int bits, uint32_t address, bool read, int allowed, char *regname)
{
	assert((bits == 8) || (bits == 16) || (bits == 32));
	if ((bits & allowed) == 0) {
		printf("WARNING: %s 0x%08X (%s) with invalid size %d!\n", read ? "read from" : "write to", address, regname, bits);
	}
}

inline static void ENFORCE_SIZE_R(int bits, uint32_t address, int allowed, char *regname)
{
	ENFORCE_SIZE(bits, address, true, allowed, regname);
}

inline static void ENFORCE_SIZE_W(int bits, uint32_t address, int allowed, char *regname)
{
	ENFORCE_SIZE(bits, address, false, allowed, regname);
}

void IoWrite(uint32_t address, uint32_t data, int bits)/*{{{*/
{
	bool handled = false;

	if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O register space, zone A
		switch (address & 0x0F0000) {
			case 0x010000:				// General Status Register
				if (bits == 16)
					state.genstat = (data & 0xffff);
				else if (bits == 8) {
					if (address & 0)
						state.genstat = data;
					else
						state.genstat = data << 8;
				}
				handled = true;
				break;
			case 0x030000:				// Bus Status Register 0
				break;
			case 0x040000:				// Bus Status Register 1
				break;
			case 0x050000:				// Phone status
				break;
			case 0x060000:				// DMA Count
				ENFORCE_SIZE_W(bits, address, 16, "DMACOUNT");
				state.dma_count = (data & 0x3FFF);
				state.idmarw = ((data & 0x4000) == 0x4000);
				state.dmaen = ((data & 0x8000) == 0x8000);
				// This handles the "dummy DMA transfer" mentioned in the docs
				// disabled because it causes the floppy test to fail
#if 0
				if (!state.idmarw){
					if (access_check_dma(true)){
						uint32_t newAddr = mapAddr(state.dma_address, true);
						// RAM access
						if (newAddr <= 0x1fffff)
							WR16(state.base_ram, newAddr, state.base_ram_size - 1, 0xFF);
						else if (address <= 0x3FFFFF)
							WR16(state.exp_ram, newAddr - 0x200000, state.exp_ram_size - 1, 0xFF);
					}
				}
#endif
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
				ENFORCE_SIZE_W(bits, address, 16, "MISCCON");
				// TODO: handle the ctrl bits properly
				if (data & 0x8000){
					state.timer_enabled = 1;
				}else{
					state.timer_enabled = 0;
					state.timer_asserted = 0;
				}
				state.dma_reading = (data & 0x4000);
				if (state.leds != ((~data & 0xF00) >> 8)) {
					state.leds = (~data & 0xF00) >> 8;
#ifdef SHOW_LEDS
					printf("LEDs: %s %s %s %s\n",
							(state.leds & 8) ? "R" : "-",
							(state.leds & 4) ? "G" : "-",
							(state.leds & 2) ? "Y" : "-",
							(state.leds & 1) ? "R" : "-");
#endif
				}
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
				handled = true;
				break;
			case 0x0E0000:				// Disk Control Register
				{
					bool fd_selected;
					bool hd_selected;
					ENFORCE_SIZE_W(bits, address, 16, "DISKCON");
					// B7 = FDD controller reset
					if ((data & 0x80) == 0) wd2797_reset(&state.fdc_ctx);
					// B6 = drive 0 select
					fd_selected = (data & 0x40) != 0;
					// B5 = motor enable -- TODO
					// B4 = HDD controller reset
					if ((data & 0x10) == 0) wd2010_reset(&state.hdc_ctx);
					// B3 = HDD0 select
					hd_selected = (data & 0x08) != 0;
					// B2,1,0 = HDD0 head select -- TODO?
					if (hd_selected && !state.hd_selected){
						state.fd_selected = false;
						state.hd_selected = true;
					}else if (fd_selected && !state.fd_selected){
						state.hd_selected = false;
						state.fd_selected = true;
					}
					handled = true;
					break;
				}
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
						fprintf(stderr, "NOTE: WR%d to expansion card space, addr=0x%08X, data=0x%08X\n", bits, address, data);
						handled = true;
						break;
				}
				break;
			case 0xE00000:				// HDC, FDC, MCR2 and RTC data bits
			case 0xF00000:
				switch (address & 0x070000) {
					case 0x000000:		// [ef][08]xxxx ==> WD2010 hard disc controller
						wd2010_write_reg(&state.hdc_ctx, (address >> 1) & 7, data);
						handled = true;
						break;
					case 0x010000:		// [ef][19]xxxx ==> WD2797 floppy disc controller
						/*ENFORCE_SIZE_W(bits, address, 16, "FDC REGISTERS");*/
						wd2797_write_reg(&state.fdc_ctx, (address >> 1) & 3, data);
						handled = true;
						break;
					case 0x020000:		// [ef][2a]xxxx ==> Miscellaneous Control Register 2
						// MCR2 - UNIX PC Rev. P5.1 HDD head select b3 and potential HDD#2 select
						wd2010_write_reg(&state.hdc_ctx, UNIXPC_REG_MCR2, data);
						handled = true;
						break;
					case 0x030000:		// [ef][3b]xxxx ==> Real Time Clock data bits
						break;
					case 0x040000:		// [ef][4c]xxxx ==> General Control Register
						switch (address & 0x077000) {
							case 0x040000:		// [ef][4c][08]xxx ==> EE
								// Error Enable. If =0, Level7 intrs and bus errors are masked.
								ENFORCE_SIZE_W(bits, address, 16, "EE");
								state.ee = ((data & 0x8000) == 0x8000);
								handled = true;
								break;
							case 0x041000:		// [ef][4c][19]xxx ==> PIE
								ENFORCE_SIZE_W(bits, address, 16, "PIE");
								state.pie = ((data & 0x8000) == 0x8000);
								handled = true;
								break;
							case 0x042000:		// [ef][4c][2A]xxx ==> BP
								break;
							case 0x043000:		// [ef][4c][3B]xxx ==> ROMLMAP
								ENFORCE_SIZE_W(bits, address, 16, "ROMLMAP");
								state.romlmap = ((data & 0x8000) == 0x8000);
								handled = true;
								break;
							case 0x044000:		// [ef][4c][4C]xxx ==> L1 MODEM
								ENFORCE_SIZE_W(bits, address, 16, "L1 MODEM");
								break;
							case 0x045000:		// [ef][4c][5D]xxx ==> L2 MODEM
								ENFORCE_SIZE_W(bits, address, 16, "L2 MODEM");
								break;
							case 0x046000:		// [ef][4c][6E]xxx ==> D/N CONNECT
								ENFORCE_SIZE_W(bits, address, 16, "D/N CONNECT");
								break;
							case 0x047000:		// [ef][4c][7F]xxx ==> Whole screen reverse video
								ENFORCE_SIZE_W(bits, address, 16, "WHOLE SCREEN REVERSE VIDEO");
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
						// TODO: figure out which sizes are valid (probably just 8 and 16)
						// ENFORCE_SIZE_W(bits, address, 16, "KEYBOARD CONTROLLER");
						if (bits == 8) {
							printf("KBD WR %02X => %02X\n", (address >> 1) & 3, data);
							keyboard_write(&state.kbd, (address >> 1) & 3, data);
							handled = true;
						} else if (bits == 16) {
							printf("KBD WR %02X => %04X\n", (address >> 1) & 3, data);
							keyboard_write(&state.kbd, (address >> 1) & 3, data >> 8);
							handled = true;
						}
						break;
				}
		}
	}

	LOG_NOT_HANDLED_W(bits);
}/*}}}*/

uint32_t IoRead(uint32_t address, int bits)/*{{{*/
{
	bool handled = false;
	uint32_t data = EMPTY & 0xFFFFFFFF;

	if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O register space, zone A
		switch (address & 0x0F0000) {
			case 0x010000:				// General Status Register
				/* ENFORCE_SIZE_R(bits, address, 16, "GENSTAT"); */
				if (bits == 32) {
					return ((uint32_t)state.genstat << 16) + (uint32_t)state.genstat;
				} else if (bits == 16) {
					return (uint16_t)state.genstat;
				} else {
					return (uint8_t)(state.genstat & 0xff);
				}
				break;
			case 0x030000:				// Bus Status Register 0
				ENFORCE_SIZE_R(bits, address, 16, "BSR0");
				return ((uint32_t)state.bsr0 << 16) + (uint32_t)state.bsr0;
				break;
			case 0x040000:				// Bus Status Register 1
				ENFORCE_SIZE_R(bits, address, 16, "BSR1");
				return ((uint32_t)state.bsr1 << 16) + (uint32_t)state.bsr1;
				break;
			case 0x050000:				// Phone status
				ENFORCE_SIZE_R(bits, address, 8 | 16, "PHONE STATUS");
				break;
			case 0x060000:				// DMA Count
				// TODO: U/OERR- is always inactive (bit set)... or should it be = DMAEN+?
				// Bit 14 is always unused, so leave it set
				ENFORCE_SIZE_R(bits, address, 16, "DMACOUNT");
				return (state.dma_count & 0x3fff) | 0xC000;
				break;
			case 0x070000:				// Line Printer Status Register
				data = 0x00120012;	// no parity error, no line printer error, no irqs from FDD or HDD
				data |= wd2797_get_irq(&state.fdc_ctx) ? 0x00080008 : 0;
				data |= wd2010_get_irq(&state.hdc_ctx) ? 0x00040004 : 0;
				return data;
				break;
			case 0x080000:				// Real Time Clock
				printf("READ NOTIMP: Realtime Clock\n");
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
						fprintf(stderr, "NOTE: RD%d from expansion card space, addr=0x%08X\n", bits, address);
						handled = true;
						break;
				}
				break;
			case 0xE00000:				// HDC, FDC, MCR2 and RTC data bits
			case 0xF00000:
				switch (address & 0x070000) {
					case 0x000000:		// [ef][08]xxxx ==> WD1010 hard disc controller
						return (wd2010_read_reg(&state.hdc_ctx, (address >> 1) & 7));

						break;
					case 0x010000:		// [ef][19]xxxx ==> WD2797 floppy disc controller
						/*ENFORCE_SIZE_R(bits, address, 16, "FDC REGISTERS");*/
						return wd2797_read_reg(&state.fdc_ctx, (address >> 1) & 3);
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
						// TODO: figure out which sizes are valid (probably just 8 and 16)
						//ENFORCE_SIZE_R(bits, address, 16, "KEYBOARD CONTROLLER");
						{
							if (bits == 8) {
								return keyboard_read(&state.kbd, (address >> 1) & 3);
							} else {
								return keyboard_read(&state.kbd, (address >> 1) & 3) << 8;
							}
							return data;
						}
						break;
				}
		}
	}

	LOG_NOT_HANDLED_R(bits);

	return data;
}/*}}}*/


/********************************************************
 * m68k memory read/write support functions for Musashi
 ********************************************************/


static uint16_t ram_read_16(uint32_t address)
{
	if (address <= 0x1fffff) {
		// Base memory wraps around
		return RD16(state.base_ram, address, state.base_ram_size - 1);
	} else {
		if ((address <= (state.exp_ram_size + 0x200000 - 1)) && (address >= 0x200000)){
			return RD16(state.exp_ram, address - 0x200000, state.exp_ram_size - 1);
		}else
			return EMPTY & 0xffff;
	}
}

/**
 * @brief Read M68K memory, 32-bit
 */
uint32_t m68k_read_memory_32(uint32_t address)/*{{{*/
{
	uint32_t data = EMPTY & 0xFFFFFFFF;

	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	// Check access permissions
	ACCESS_CHECK_RD(address, 32);

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
		return RD32(state.rom, address, ROM_SIZE - 1);
	} else if (address <= 0x3fffff) {
		// RAM access
		uint32_t newAddr = mapAddr(address, false);
		// Base memory wraps around
		data = ((ram_read_16(newAddr) << 16) | 
			ram_read_16(mapAddr(address + 2, false)));

		return (data);
	} else if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O register space, zone A
		switch (address & 0x0F0000) {
			case 0x000000:				// Map RAM access
				if (address > 0x4007FF) fprintf(stderr, "NOTE: RD32 from MapRAM mirror, addr=0x%08X\n", address);
				return RD32(state.map, address, 0x7FF);
				break;
			case 0x020000:				// Video RAM
				if (address > 0x427FFF) fprintf(stderr, "NOTE: RD32 from VideoRAM mirror, addr=0x%08X\n", address);
				return RD32(state.vram, address, 0x7FFF);
				break;
			default:
				return IoRead(address, 32);
		}
	} else {
		return IoRead(address, 32);
	}

	return data;
}/*}}}*/

/**
 * @brief Read M68K memory, 16-bit
 */
uint32_t m68k_read_memory_16(uint32_t address)/*{{{*/
{
	uint16_t data = EMPTY & 0xFFFF;

	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	// Check access permissions
	ACCESS_CHECK_RD(address, 16);

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
		data = RD16(state.rom, address, ROM_SIZE - 1);
	} else if (address <= 0x3fffff) {
		// RAM access
		uint32_t newAddr = mapAddr(address, false);
		if (newAddr <= 0x1fffff) {
			// Base memory wraps around
			return RD16(state.base_ram, newAddr, state.base_ram_size - 1);
		} else {
			if ((newAddr <= (state.exp_ram_size + 0x200000 - 1)) && (newAddr >= 0x200000))
				return RD16(state.exp_ram, newAddr - 0x200000, state.exp_ram_size - 1);
			else
				return EMPTY & 0xffff;
		}
	} else if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O register space, zone A
		switch (address & 0x0F0000) {
			case 0x000000:				// Map RAM access
				if (address > 0x4007FF) fprintf(stderr, "NOTE: RD16 from MapRAM mirror, addr=0x%08X\n", address);
				data = RD16(state.map, address, 0x7FF);
				break;
			case 0x020000:				// Video RAM
				if (address > 0x427FFF) fprintf(stderr, "NOTE: RD16 from VideoRAM mirror, addr=0x%08X\n", address);
				data = RD16(state.vram, address, 0x7FFF);
				break;
			default:
				data = IoRead(address, 16);
		}
	} else {
		data = IoRead(address, 16);
	}

	return data;
}/*}}}*/

/**
 * @brief Read M68K memory, 8-bit
 */
uint32_t m68k_read_memory_8(uint32_t address)/*{{{*/
{
	uint8_t data = EMPTY & 0xFF;

	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	// Check access permissions
	ACCESS_CHECK_RD(address, 8);

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
		data = RD8(state.rom, address, ROM_SIZE - 1);
	} else if (address <= 0x3fffff) {
		// RAM access
		uint32_t newAddr = mapAddr(address, false);
		if (newAddr <= 0x1fffff) {
			// Base memory wraps around
			return RD8(state.base_ram, newAddr, state.base_ram_size - 1);
		} else {
			if ((newAddr <= (state.exp_ram_size + 0x200000 - 1)) && (newAddr >= 0x200000))
				return RD8(state.exp_ram, newAddr - 0x200000, state.exp_ram_size - 1);
			else
				return EMPTY & 0xff;
		}
	} else if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O register space, zone A
		switch (address & 0x0F0000) {
			case 0x000000:				// Map RAM access
				if (address > 0x4007FF) fprintf(stderr, "NOTE: RD8 from MapRAM mirror, addr=0x%08X\n", address);
				data = RD8(state.map, address, 0x7FF);
				break;
			case 0x020000:				// Video RAM
				if (address > 0x427FFF) fprintf(stderr, "NOTE: RD8 from VideoRAM mirror, addr=0x%08X\n", address);
				data = RD8(state.vram, address, 0x7FFF);
				break;
			default:
				data = IoRead(address, 8);
		}
	} else {
		data = IoRead(address, 8);
	}

	return data;
}/*}}}*/


static void ram_write_16(uint32_t address, uint32_t value)/*{{{*/
{
	if (address <= 0x1fffff) {
		if (address < state.base_ram_size) {
			WR16(state.base_ram, address, state.base_ram_size - 1, value);
		}
	} else {
		if ((address - 0x200000) < state.exp_ram_size) {
			WR16(state.exp_ram, address - 0x200000, state.exp_ram_size - 1, value);
		}
	}
}

/**
 * @brief Write M68K memory, 32-bit
 */
void m68k_write_memory_32(uint32_t address, uint32_t value)/*{{{*/
{
	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	// Check access permissions
	ACCESS_CHECK_WR(address, 32);
	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
	} else if (address <= 0x3FFFFF) {
		// RAM access
		uint32_t newAddr = mapAddr(address, true);
		ram_write_16(newAddr, (value & 0xffff0000) >> 16);
		ram_write_16(mapAddr(address + 2, true), (value & 0xffff));
	} else if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O register space, zone A
		switch (address & 0x0F0000) {
			case 0x000000:				// Map RAM access
				if (address > 0x4007FF) fprintf(stderr, "NOTE: WR32 to MapRAM mirror, addr=0x%08X\n", address);
				WR32(state.map, address, 0x7FF, value);
				break;
			case 0x020000:				// Video RAM
				if (address > 0x427FFF) fprintf(stderr, "NOTE: WR32 to VideoRAM mirror, addr=0x%08X\n", address);
				WR32(state.vram, address, 0x7FFF, value);
				break;
			default:
				IoWrite(address, value, 32);
		}
	} else {
		IoWrite(address, value, 32);
	}
}/*}}}*/

/**
 * @brief Write M68K memory, 16-bit
 */
void m68k_write_memory_16(uint32_t address, uint32_t value)/*{{{*/
{
	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	// Check access permissions
	ACCESS_CHECK_WR(address, 16);

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
	} else if (address <= 0x3FFFFF) {
		// RAM access
		uint32_t newAddr = mapAddr(address, true);

		if (newAddr <= 0x1fffff) {
			if (newAddr < state.base_ram_size) {
				WR16(state.base_ram, newAddr, state.base_ram_size - 1, value);
			}
		} else {
			if ((newAddr - 0x200000) < state.exp_ram_size) {
				WR16(state.exp_ram, newAddr - 0x200000, state.exp_ram_size - 1, value);
			}
		}
	} else if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O register space, zone A
		switch (address & 0x0F0000) {
			case 0x000000:				// Map RAM access
				if (address > 0x4007FF) fprintf(stderr, "NOTE: WR16 to MapRAM mirror, addr=0x%08X, data=0x%04X\n", address, value);
				WR16(state.map, address, 0x7FF, value);
				break;
			case 0x020000:				// Video RAM
				if (address > 0x427FFF) fprintf(stderr, "NOTE: WR16 to VideoRAM mirror, addr=0x%08X, data=0x%04X\n", address, value);
				WR16(state.vram, address, 0x7FFF, value);
				break;
			default:
				IoWrite(address, value, 16);
		}
	} else {
		IoWrite(address, value, 16);
	}
}/*}}}*/

/**
 * @brief Write M68K memory, 8-bit
 */
void m68k_write_memory_8(uint32_t address, uint32_t value)/*{{{*/
{
	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	// Check access permissions
	ACCESS_CHECK_WR(address, 8);

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access (read only!)
	} else if (address <= 0x3FFFFF) {
		// RAM access
		uint32_t newAddr = mapAddr(address, true);
		if (newAddr <= 0x1fffff) {
			if (newAddr < state.base_ram_size) {
				WR8(state.base_ram, newAddr, state.base_ram_size - 1, value);
			}
		} else {
			if ((newAddr - 0x200000) < state.exp_ram_size) {
				WR8(state.exp_ram, newAddr - 0x200000, state.exp_ram_size - 1, value);
			}
		}
	} else if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O register space, zone A
		switch (address & 0x0F0000) {
			case 0x000000:				// Map RAM access
				if (address > 0x4007FF) fprintf(stderr, "NOTE: WR8 to MapRAM mirror, addr=0x%08X, data=0x%04X\n", address, value);
				WR8(state.map, address, 0x7FF, value);
				break;
			case 0x020000:				// Video RAM
				if (address > 0x427FFF) fprintf(stderr, "NOTE: WR8 to VideoRAM mirror, addr=0x%08X, data=0x%04X\n", address, value);
				WR8(state.vram, address, 0x7FFF, value);
				break;
			default:
				IoWrite(address, value, 8);
		}
	} else {
		IoWrite(address, value, 8);
	}
}/*}}}*/


// for the disassembler
uint32_t m68k_read_disassembler_32(uint32_t addr)
{
	if (addr < 0x400000) {
		uint32_t newAddrHigh, newAddrLow;
		newAddrHigh = map_address_debug(addr);
		newAddrLow = map_address_debug(addr + 2);
		return ((ram_read_16(newAddrHigh) << 16) | 
			ram_read_16(newAddrLow));

	} else {
		printf(">>> WARNING Disassembler RD32 out of range 0x%08X\n", addr);
		return EMPTY;
	}
}

uint32_t m68k_read_disassembler_16(uint32_t addr)
{
	if (addr < 0x400000) {
		uint16_t page = (addr >> 12) & 0x3FF;
		uint32_t new_page_addr = MAPRAM(page) & 0x3FF;
		uint32_t newAddr = (new_page_addr << 12) + (addr & 0xFFF);
		if (newAddr <= 0x1fffff) {
			if (newAddr >= state.base_ram_size)
				return EMPTY & 0xffff;
			else
				return RD16(state.base_ram, newAddr, state.base_ram_size - 1);
		} else {
			if ((newAddr <= (state.exp_ram_size + 0x200000 - 1)) && (newAddr >= 0x200000))
				return RD16(state.exp_ram, newAddr - 0x200000, state.exp_ram_size - 1);
			else
				return EMPTY & 0xffff;
		}
	} else {
		printf(">>> WARNING Disassembler RD16 out of range 0x%08X\n", addr);
		return EMPTY & 0xffff;
	}
}

uint32_t m68k_read_disassembler_8 (uint32_t addr)
{
	if (addr < 0x400000) {
		uint16_t page = (addr >> 12) & 0x3FF;
		uint32_t new_page_addr = MAPRAM(page) & 0x3FF;
		uint32_t newAddr = (new_page_addr << 12) + (addr & 0xFFF);
		if (newAddr <= 0x1fffff) {
			if (newAddr >= state.base_ram_size)
				return EMPTY & 0xff;
			else
				return RD8(state.base_ram, newAddr, state.base_ram_size - 1);
		} else {
			if ((newAddr <= (state.exp_ram_size + 0x200000 - 1)) && (newAddr >= 0x200000))
				return RD8(state.exp_ram, newAddr - 0x200000, state.exp_ram_size - 1);
			else
				return EMPTY & 0xff;
		}
	} else {
		printf(">>> WARNING Disassembler RD8 out of range 0x%08X\n", addr);
		return EMPTY & 0xff;
	}
}

