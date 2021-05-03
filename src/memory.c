#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include "musashi/m68k.h"
#include "state.h"
#include "utils.h"
#include "memory.h"
#include "i8274.h"

// Memory access debugging options, to reduce logspam
#undef MEM_DEBUG_PAGEFAULTS
#undef MEM_DEBUG_KEYBOARD
#undef MEM_DEBUG_SERIAL

#ifdef MEM_DEBUG_PAGEFAULTS
# define LOG_PF LOG
#else
# define LOG_PF(x, ...)
#endif

#ifdef MEM_DEBUG_SERIAL
# define LOG_SERIAL LOG
#else
# define LOG_SERIAL(x, ...)
#endif

// The value which will be returned if the CPU attempts to read from empty memory
// TODO (FIXME?) - need to figure out if R/W ops wrap around. This seems to appease the UNIX kernel and P4TEST.
#define EMPTY 0xFFFFFFFFUL
//#define EMPTY 0x55555555UL
//#define EMPTY 0x00000000UL

#define SUPERVISOR_MODE ((m68k_get_reg(NULL, M68K_REG_SR) & 0x2000)==0x2000)
#define USER_MODE (!SUPERVISOR_MODE)
#define ZEROPAGE 0x1000

/******************
 * Memory mapping
 ******************/

#define MAPRAM(page) (((uint16_t)state.map[page*2] << 8) + ((uint16_t)state.map[(page*2)+1]))

static uint32_t map_address_debug(uint32_t addr)
{
	uint16_t page = (addr >> 12) & 0x3FF;

	// Look it up in the map RAM and get the physical page address
	uint32_t new_page_addr = MAPRAM(page) & 0x3FF;
	return (new_page_addr << 12) + (addr & 0xFFF);
}

uint32_t mapAddr(uint32_t addr, bool writing)/*{{{*/
{
	assert(addr < 0x400000);

	// RAM access. Check against the Map RAM
	// Start by getting the original page address
	uint16_t page = (addr >> 12) & 0x3FF;

	// Look it up in the map RAM and get the physical page
	uint32_t new_page = MAPRAM(page) & 0x3FF;

	// Update the Page Status bits
	uint8_t pagebits = (state.map[page*2] >> 5) & 0x03;
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
			// Page present, 2nd or later access
			if (writing)
				state.map[page*2] |= 0x60;		// Page written to (dirty)
			break;
		case 3:
			// Page already dirty, no change
			break;
	}

	// Return the address with the new physical page spliced in
	return (new_page << 12) + (addr & 0xFFF);
}/*}}}*/

MEM_STATUS checkMemoryAccess(uint32_t addr, bool writing, bool dma)/*{{{*/
{
	// Get the page bits for this page.
	uint16_t page = (addr >> 12) & 0x3FF;
	uint8_t pagebits = state.map[page*2] >> 5;

	// Check page is present (but only for RAM zone)
	if (addr < 0x400000) {
		if ((pagebits & 0x03) == 0)
		{
			LOG_PF("Page fault: addr 0x%06X, page %03X -> phys page %03X, pagebits %d",
					addr, page, MAPRAM(page) & 0x3FF, pagebits);
			return MEM_PAGEFAULT;
		}
		// early out valid user reads, and writes to write enabled pages,
		// not in kernel space, to avoid call to "expensive" supervisor mode check
		if (addr >= 0x080000 && (!writing || (pagebits & 0x04)))
		{
			return MEM_ALLOWED;
		}
	}

	// Are we in Supervisor mode?
	if (dma || SUPERVISOR_MODE)
		// Yes. We can do anything we like.
		return MEM_ALLOWED;

	// If we're here, then we must be in User mode.
	// Check that the user didn't access memory outside of the RAM area
	if (addr >= 0x400000) {
		if (state.vidpal && (addr >= 0x420000) && (addr <= 0x427FFF))
			return MEM_ALLOWED;
		else
		{
			LOG("User accessed privileged memory: %08X", addr);
			return MEM_UIE;
		}
	}

	// User attempt to access the kernel
	// A19, A20, A21, A22 low (kernel access): RAM addr before paging; not in Supervisor mode
	if (addr < 0x080000 && !(!writing && addr < ZEROPAGE)) {
		LOGS("Attempt by user code to access kernel space");
		return MEM_KERNEL;
	}

	// Check page is write enabled
	if (writing && ((pagebits & 0x04) == 0)) {
		LOG_PF("Page not write enabled: addr 0x%06X, page %03X -> phys page %03X, pagebits %d",
				addr, page, MAPRAM(page) & 0x3FF, pagebits);
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
				state.genstat = 0x01FF | (state.pie ? 0x8400 : 0);	\
				fault = true;										\
				break;												\
			case MEM_UIE:											\
				/* User access to memory above 4MB */				\
				state.genstat = 0x10FF | (state.pie ? 0x8400 : 0);	\
				fault = true;										\
				break;												\
			case MEM_KERNEL:										\
			case MEM_PAGE_NO_WE:									\
				/* kernel access or page not write enabled */		\
				/* XXX: is this the correct value? */				\
				state.genstat = 0x11FF | (state.pie ? 0x8400 : 0);	\
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
// TODO: 32-bit check is broken, problem with 32-bit write straddling 2 pages, proper page faults may not occur
//       if 1st word faults, 2nd page is never checked for PF (only an issue when straddling 2 pages)
#define ACCESS_CHECK_WR(address, bits)								\
	do {															\
		bool fault = false;											\
		uint32_t faultAddr = address;								\
		MEM_STATUS st;												\
		_ACCESS_CHECK_WR_BYTE(address);								\
		if (!fault && bits == 32									\
				&& ((address + 2) & ~0xfff) != (address & ~0xfff)){	\
			_ACCESS_CHECK_WR_BYTE(address + 2);						\
			if (fault) faultAddr = address + 2;						\
		}															\
		if (fault) {												\
			if (bits >= 16)											\
				state.bsr0 = 0x7C00;								\
			else													\
				state.bsr0 = (faultAddr & 1) ? 0x7E00 : 0x7D00;		\
			if (st==MEM_UIE) state.bsr0 |= 0x8000; 					\
			state.bsr0 |= (faultAddr >> 16);							\
			state.bsr1 = faultAddr & 0xffff;							\
			LOG_PF("Bus Error while writing, addr %08X, statcode %d", address, st);		\
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
				state.genstat = 0x41FF | (state.pie ? 0x8400 : 0);	\
				fault = true;										\
				break;												\
			case MEM_UIE:											\
				/* User access to memory above 4MB */				\
				state.genstat = 0x50FF | (state.pie ? 0x8400 : 0);	\
				fault = true;										\
				break;												\
			case MEM_KERNEL:										\
			case MEM_PAGE_NO_WE:									\
				/* kernel access or page not write enabled */		\
				/* XXX: is this the correct value? */				\
				state.genstat = 0x51FF | (state.pie ? 0x8400 : 0);	\
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
// TODO: 32-bit check is broken, problem with 32-bit read straddling 2 pages, if needed, 2nd page PF may not occur
//       if 1st word faults, 2nd page is never checked for PF? (only an issue when straddling 2 pages)
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
			if (st==MEM_UIE) state.bsr0 |= 0x8000;					\
			state.bsr0 |= (faultAddr >> 16);							\
			state.bsr1 = faultAddr & 0xffff;							\
			LOG_PF("Bus Error while reading, addr %08X, statcode %d", faultAddr, st);		\
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
			state.genstat = 0x21FF
				| (reading ? 0x4000 : 0)
				| (state.pie ? 0x8400 : 0);
			access_ok = false;
			break;

		case MEM_UIE:
			// User access to memory above 4MB
			// Shouldn't be possible with DMA, assert this
			assert(0);
			state.genstat = 0x30FF
				| (reading ? 0x4000 : 0)
				| (state.pie ? 0x8400 : 0);
			access_ok = false;
			break;

		case MEM_KERNEL:
		case MEM_PAGE_NO_WE:
			// Kernel access or page not write enabled
			// Shouldn't be possible with DMA, assert this
			assert(0);
			state.genstat = 0x31FF
				| (reading ? 0x4000 : 0)
				| (state.pie ? 0x8400 : 0);
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
		// trigger NMI (DMA Page Fault) kernel panic
		if (state.ee) m68k_set_irq(7);
		printf("DMA PAGE FAULT: genstat=%04X, bsr0=%04X, bsr1=%04X\n", state.genstat, state.bsr0, state.bsr1);
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
	static uint16_t dialerReg = 0;

	if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O register space, zone A
		switch (address & 0x0F0000) {
			case 0x010000:				// General Status Register (RD)
				break;
			case 0x030000:				// Bus Status Register 0 (RD)
				break;
			case 0x040000:				// Bus Status Register 1 (RD)
				break;
			case 0x050000:				// Phone status (RD)
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
			case 0x070000:				// Line Printer Status Register (RD)
				break;
			case 0x080000:				// Real Time Clock
				ENFORCE_SIZE_W(bits, address, 16, "RTCWRITE");
				/*printf("IoWrite RTCWRITE %x\n", data);*/
				tc8250_set_chip_enable(&state.rtc_ctx, data & 0x8000);
				tc8250_set_address_latch_enable(&state.rtc_ctx, data & 0x4000);
				tc8250_set_write_enable(&state.rtc_ctx, data & 0x2000);
				tc8250_write_reg(&state.rtc_ctx, (data & 0x0F00) >> 8);
				handled = true;
				break;
			case 0x090000:				// Telephony Control Register
				switch (address & 0x0FF000) {
					case 0x090000:		// Handset relay
					case 0x098000:
						LOG("TCR (%06X) Handset: %s", address, (data & 0x4000) ? "enabled" : "disabled");
						break;
					case 0x091000:		// Line select 2*
					case 0x099000:
						LOG("TCR (%06X) Line selected: %s", address, (data & 0x4000) ? "Line 1" : "Line 2");
						break;
					case 0x092000:		// Hook relay 1*
					case 0x09A000:
						LOG("TCR (%06X) Hook relay 1 set: %s", address, (data & 0x4000) ? "off" : "on");
						break;
					case 0x093000:		// Hook relay 2*
					case 0x09B000:
						LOG("TCR (%06X) Hook relay 2 set: %s", address, (data & 0x4000) ? "off" : "on");
						break;
					case 0x094000:		// Line 1 hold
					case 0x09C000:
						LOG("TCR (%06X) Line 1 hold set: %s", address, (data & 0x4000) ? "on" : "off");
						break;
					case 0x095000:		// Line 2 hold
					case 0x09D000:
						LOG("TCR (%06X) Line 2 hold set: %s", address, (data & 0x4000) ? "on" : "off");
						break;
					case 0x096000:		// Line 1 A-lead*
					case 0x09E000:
						LOG("TCR (%06X) Line 1 A-lead set: %s", address, (data & 0x4000) ? "off" : "on");
						break;
					case 0x097000:		// Line 2 A-lead*
					case 0x09F000:
						LOG("TCR (%06X) Line 2 A-lead set: %s", address, (data & 0x4000) ? "off" : "on");
						break;
					default:
						LOG("TCR (%06X) write, data: %i)", address, ((data & 0x4000) >> 14));
						break;
				}
				handled = true;
				break;
			case 0x0A0000:				// Miscellaneous Control Register (WR) high byte
				ENFORCE_SIZE_W(bits, address, 16, "MISCCON");
				// TODO: handle the ctrl bits properly (bit 13: LP strobe, bit 12: Chan B clock select (0 = modem clock [baud gen?], 1 = fixed 19.2k [uses 8274 16x divider for 1200baud, 64x divider for 300baud])
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
			case 0x0B0000:				// TM/DIALWR (838A)
				switch (address & 0x000C00) {
					case 0x000:
						// iohw.h: "baud generator for chan A (rs232): lower 3 nibble of the address is the counter value"
						// baud output TMOUT = [1/(4 x N)] x 1.2288 MHz
						handled = true;
						uint16_t baudgenN = (address & 0x1ff) << 3; // latch uses A1-A8 address lines, need to shift up 3 bits to get correct baud
						LOG("RS-232 baud (%06X) set to %i", address, baudgenN ? 1228800/(4*baudgenN) : 0);
						break;
					case 0x400:
						// DIALER TXD lower byte shift reg load
						dialerReg &= 0xff00;
						dialerReg |= address & 0xff;
						LOG("dialer reg low byte (%06X) now: %i", address, dialerReg);
						handled = true;
						break;
					case 0x800:
						// DIALER TXD upper byte shift reg load
						// and starts shifting data out of DIALER TXD at 4800 baud
						dialerReg &= 0xff;
						dialerReg |= (address & 0xff) << 8;
						LOG("dialer reg high byte (%06X) now: %i", address, dialerReg);
						handled = true;
						break;
					default:
						break;
					}
				break;
			case 0x0C0000:				// Clear Status Register
				// CSR is used to clear PERR* (main memory parity error), which is currently always returned as 'no parity error'
				// "If the current cycle causes a parity error, MMU error, or processor bus error, GSR is not updated at the following cycles until CSR"
				// clear MMU error in BSR0
				state.bsr0 |= 0x8000;
				// also disable PF- and UIE- in GSR
				state.genstat |= 0x1100;
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
			case 0x0E0000:				// Disk Control Register (WR)
				{
					uint8_t sdh;
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
					// B2,1,0 = HDD0 head select
					sdh = wd2010_read_reg(&state.hdc_ctx, WD2010_REG_SDH);
					sdh = (sdh & ~0x07) | (data & 0x07);
					wd2010_write_reg(&state.hdc_ctx, WD2010_REG_SDH, sdh);

					//if both devices are selected, whichever one was selected
					//last should be used
					if (hd_selected && !state.hd_selected){
						state.dma_dev = DMA_DEV_HD0;
					}else if (fd_selected && !state.fd_selected){
						state.dma_dev = DMA_DEV_FD;
					}else if (hd_selected && !fd_selected){
						state.dma_dev = DMA_DEV_HD0;
					}else if (fd_selected && !hd_selected){
						state.dma_dev = DMA_DEV_FD;
					}
					state.fd_selected = fd_selected;
					state.hd_selected = hd_selected;
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
						if ((address & 0x3FFF8) == 0x3FFF8)	// Software reset
							LOG("Expansion slot %i: Reset", ((address >> 18) & 7));
						else
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
						// P5.1 PAL - Save MCR2 bit 4 to mirror to Telephony Status bit 4
						state.mcr2mirror = ((data & 0x10) == 0x10);
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
								LOG("EE+ (%06X): %i", address, state.ee);
								handled = true;
								break;
							case 0x041000:		// [ef][4c][19]xxx ==> PIE
								ENFORCE_SIZE_W(bits, address, 16, "PIE");
								state.pie = ((data & 0x8000) == 0x8000);
								// update PIE+ (bit 10) in GSR, and mirror to bit 15 for P3 revlev detection
								state.genstat &= ~0x8400;
								if (state.pie) {
									state.genstat |= 0x8400;
								}
								LOG("PIE+ (%06X): %i", address, state.pie);
								handled = true;
								break;
							case 0x042000:		// [ef][4c][2A]xxx ==> BP
								break;
							case 0x043000:		// [ef][4c][3B]xxx ==> ROMLMAP
								ENFORCE_SIZE_W(bits, address, 16, "ROMLMAP");
								state.romlmap = ((data & 0x8000) == 0x8000);
								LOG("ROMLMAP (%06X): %i", address, state.romlmap);
								handled = true;
								break;
							case 0x044000:		// [ef][4c][4C]xxx ==> L1 MODEM*
								ENFORCE_SIZE_W(bits, address, 16, "L1 MODEM");
								LOG("L1 MODEM (%06X): Line 1 %s to modem", address, (data & 0x8000) ? "disconnected" : "connected");
								handled = true;
								break;
							case 0x045000:		// [ef][4c][5D]xxx ==> L2 MODEM*
								ENFORCE_SIZE_W(bits, address, 16, "L2 MODEM");
								LOG("L2 MODEM (%06X): Line 2 %s to modem", address, (data & 0x8000) ? "disconnected" : "connected");
								handled = true;
								break;
							case 0x046000:		// [ef][4c][6E]xxx ==> D/N CONNECT* (L1 connected to dial/network*)
								ENFORCE_SIZE_W(bits, address, 16, "D/N CONNECT");
								LOG("Dialer connected to (%06X): %s", address, (data & 0x8000) ? "Line 2" : "Line 1");
								handled = true;
								break;
							case 0x047000:		// [ef][4c][7F]xxx ==> Whole screen reverse video
								ENFORCE_SIZE_W(bits, address, 16, "WHOLE SCREEN REVERSE VIDEO");
								break;
						}
						break;
					case 0x050000:		// [ef][5d]xxxx ==> 8274 regs (chan A = rs232, chan B = modem), connected to D0-D7 data bus
						data &= 0xFF;
						switch (address & 0x6) {
							case 0x0:
								LOG_SERIAL("8274 (%06X) rs232 data WR%i: %X", address, bits, data);
								i8274_data_out(&state.serial_ctx, CHAN_A, data);
								handled = true;
								break;
							case 0x2:
								LOG_SERIAL("8274 (%06X) modem data WR%i: %X", address, bits, data);
								i8274_data_out(&state.serial_ctx, CHAN_B, data);
								handled = true;
								break;
							case 0x4:
								LOG_SERIAL("8274 (%06X) rs232 ctrl WR%i: %X", address, bits, data);
								i8274_control_write(&state.serial_ctx, CHAN_A, data);
								handled = true;
								break;
							case 0x6:
								LOG_SERIAL("8274 (%06X) modem ctrl WR%i: %X", address, bits, data);
								i8274_control_write(&state.serial_ctx, CHAN_B, data);
								handled = true;
								break;
						}
						break;
					case 0x060000:		// [ef][6e]xxxx ==> Modem (882A) regs
						ENFORCE_SIZE_W(bits, address, 16, "MODEM REGS");
						handled = true;
						switch (address & 0x00F000) {
							case 0x0000:
								LOG("Modem WR0 - Line control (%06X) write: %04X = talk mode: %i, offhook: %i, data mode: %i, DTR: %i, power reset: %i",
									address, data, ((data & 0x40)==0x40), ((data & 0x20)==0x20), ((data & 0x10)==0x10), ((data & 0x04)==0x04), ((data & 0x01)==0x01));
								break;
							case 0x1000:
								LOG("Modem WR1 - Loopback test (%06X) write: %04X = 1200 baud: %i, ext clock: %i, voice: %i", address, data, ((data & 0x10)==0x10), ((data & 0x40)==0x40), ((data & 0x20)==0x20));
								break;
							case 0x4000:
								LOG("Modem WR4 - Async/Sync & handshake options (%06X) write: %04X", address, data);
								break;
							case 0x5000:
								LOG("Modem WR5 - CCITT & disconnect options (%06X) write: %04X", address, data);
								break;
							case 0x6000:
								LOG("Modem WR6 - Rx/Tx control & chip test (%06X) write: %04X", address, data);
								break;
							case 0x8000:
								LOG("Modem WR8 - Transceiver control 1 (%06X) write: %04X", address, data);
								break;
							case 0x9000:
								LOG("Modem WR9 - Transceiver control 2 (%06X) write: %04X", address, data);
								break;
							default:
								handled = false;
								break;
						}
						break;
					case 0x070000:		// [ef][7f]xxxx ==> 6850 Keyboard Controller, connected to D8-D15 data bus
						// TODO: figure out which sizes are valid (probably just 8 and 16)
						// ENFORCE_SIZE_W(bits, address, 16, "KEYBOARD CONTROLLER");
						if (bits == 8) {
#ifdef MEM_DEBUG_KEYBOARD
							printf("KBD WR8 %02X => %04X\n", (address >> 1) & 3, data);
#endif
							keyboard_write(&state.kbd, (address >> 1) & 3, data);
							handled = true;
						} else if (bits == 16) {
#ifdef MEM_DEBUG_KEYBOARD
							printf("KBD WR %02X => %04X\n", (address >> 1) & 3, data);
#endif
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
			case 0x010000:				// General Status Register, connected to D8-D15 data bus
				// bit 11 = no connect, bit 09 = LPINT+, leave both low
				// bit 10 = PIE+, mirrored to bit 15 for P3 revlev detection
				ENFORCE_SIZE_R(bits, address, 8 | 16, "GENSTAT");
				if (bits == 8) {
					return state.genstat >> 8;
				} else {
					return state.genstat;
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
			case 0x050000:				// Telephony Status Register (RD), connected to D0-D7 data bus
				ENFORCE_SIZE_R(bits, address, 8 | 16, "PHONE STATUS");
				// b3: msg waiting*, b2: ring2*, b1: ring1*, b0: offhook*
				data = 0x0f;
				// The P5.1 PAL is detected by a "feedback signal" (bit 4) which mirrors the state of MCR2 bit 4
				if (state.mcr2mirror) {
					data |= 0x10;
				}
				LOG("phone status reg (%06X) RD%i: onhook, not ringing, no msg waiting, MCR2 bit 4 mirror: %i", address, bits, state.mcr2mirror);
				return data;
				break;
			case 0x060000:				// DMA Count
				// TODO: Bit 15 (U/OERR-) is always inactive (bit set)... or should it be = DMAEN+?
				// Bit 14 is always unused, so leave it set
				ENFORCE_SIZE_R(bits, address, 16, "DMACOUNT");
				return (state.dma_count & 0x3fff) | 0xC000;
				break;
			case 0x070000:				// Line Printer Status Register (RD)
				data = 0x00120012;	// no line printer error, no irqs from FDD or HDD, no parity error, dial tone
				data |= wd2797_get_irq(&state.fdc_ctx) ? 0x00080008 : 0;
				data |= wd2010_get_irq(&state.hdc_ctx) ? 0x00040004 : 0;
				return data;
				break;
			case 0x080000:				// Real Time Clock
				printf("READ NOTIMP: Realtime Clock\n");
				break;
			case 0x090000:				// Telephony Control Register -- write only!
				break;
			case 0x0A0000:				// Miscellaneous Control Register -- write only!
				break;
			case 0x0B0000:				// TM/DIALWR -- write only!
				break;
			case 0x0C0000:				// Clear Status Register -- write only!
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
						// 0x3FFFB: Board ID MSB, 0X3FFF9: Board ID LSB
						// 0x3FFFF: Two's complement of ID MSB, 0x3FFFD: Two's complement of ID LSB
						// low byte of 0x3FFFB + 0x3FFFF and 0x3FFF9 + 0x3FFFD should equal 0
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
						return (tc8250_read_reg(&state.rtc_ctx));
					case 0x040000:		// [ef][4c]xxxx ==> General Control Register (WR)
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
					case 0x050000:		// [ef][5d]xxxx ==> 8274 regs, connected to D0-D7 data bus
						switch (address & 0x6) {
							case 0x0:
								LOG_SERIAL("8274 (%06X) rs232 data RD%i", address, bits);
								data = i8274_data_in(&state.serial_ctx, CHAN_A);
								return data;
								break;
							case 0x2:
								LOG_SERIAL("8274 (%06X) modem data RD%i", address, bits);
								data = i8274_data_in(&state.serial_ctx, CHAN_B);
								return data;
								break;
							case 0x4:
								LOG_SERIAL("8274 (%06X) rs232 status RD%i", address, bits);
								data = i8274_status_read(&state.serial_ctx, CHAN_A);
								return data;
								break;
							case 0x6:
								LOG_SERIAL("8274 (%06X) modem status RD%i", address, bits);
								data = i8274_status_read(&state.serial_ctx, CHAN_B);
								return data;
								break;
							default:
								break;
							}
						break;
					case 0x060000:		// [ef][6e]xxxx ==> Modem (882A) regs
						switch (address & 0x00F000) {
							case 0x2000:  // modem.h: Modem status to terminal interface
								// 0x80: failed self test, 0x40: test mode, 0x20: data mode (incoming call answered), 0x10: DSR on
								// 0x04: 1200 baud, 0x02: data valid (set after modem handshake), 0x01: CTS on
								data = 0xFF; 	// FF interpreted as "no modem"
								LOG("Modem RR2 (%06X) - Modem status RD%i returning: no modem", address, bits);
								return data;
								break;
							case 0x3000: // modem.h: Modem status to lamps and relays
								LOG("Modem RR3 (%06X) - Modem status to lamps & relays RD%i returning: 0", address, bits);
								return (0);
								break;
							case 0xA000: // modem.h: Transceiver status
								LOG("Modem RR10 (%06X) - Transceiver status RD%i returning: 0", address, bits);
								return (0);
								break;
						}
						break;
					case 0x070000:		// [ef][7f]xxxx ==> 6850 Keyboard Controller, connected to D8-D15 data bus
						// TODO: figure out which sizes are valid (probably just 8 and 16)
						//ENFORCE_SIZE_R(bits, address, 16, "KEYBOARD CONTROLLER");
						if (bits == 8) {
							return keyboard_read(&state.kbd, (address >> 1) & 3);
						} else {
							return keyboard_read(&state.kbd, (address >> 1) & 3) << 8;
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
	if (address < ZEROPAGE && USER_MODE) {
		return (0);
	}
	if (address <= 0x1fffff) {
		// Base memory wraps around
		return RD16(state.base_ram, address, state.base_ram_size - 1);
	} else {
		if (address <= (state.exp_ram_size + 0x200000 - 1))
			return RD16(state.exp_ram, address - 0x200000, state.exp_ram_size - 1);
		else
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
	ACCESS_CHECK_RD(address, 16);
	uint32_t addr2 = address + 2;
	ACCESS_CHECK_RD(addr2, 16);

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
		return RD32(state.rom, address, ROM_SIZE - 1);
	} else if (address <= 0x3fffff) {
		// RAM access
		uint32_t newAddr = mapAddr(address, false);
		uint32_t newAddr2 = mapAddr(address + 2, false);
		// Base memory wraps around

		data = (((uint32_t)ram_read_16(newAddr) << 16) |
			 (uint32_t)ram_read_16(newAddr2));
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

		if (address < ZEROPAGE && USER_MODE) {
			return (0);
		}
		if (newAddr <= 0x1fffff) {
			// Base memory wraps around
			return RD16(state.base_ram, newAddr, state.base_ram_size - 1);
		} else {
			if (newAddr <= (state.exp_ram_size + 0x200000 - 1))
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

	// Musashi m68ki_exception_bus_error() check
	// If this read occurs, pulse_bus_error() was called when we were already processing
	//   a bus error, address error, or reset, this is a catastrophic failure
	// This occurs during Diagnostics:Processor:Page Protection Tests #2 (12,2) and #4 (12,4)
	assert(address != 0xFFFF01);

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

		if (address < ZEROPAGE && USER_MODE) {
			return (0);
		}
		if (newAddr <= 0x1fffff) {
			// Base memory wraps around
			return RD8(state.base_ram, newAddr, state.base_ram_size - 1);
		} else {
			if (newAddr <= (state.exp_ram_size + 0x200000 - 1))
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
	ACCESS_CHECK_WR(address, 16);
	uint32_t addr2 = address + 2;
	ACCESS_CHECK_WR(addr2, 16);

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
	} else if (address <= 0x3FFFFF) {
		// RAM access
		uint32_t newAddr = mapAddr(address, true);
		uint32_t newAddr2 = mapAddr(address + 2, true);

		ram_write_16(newAddr, (value & 0xffff0000) >> 16);
		ram_write_16(newAddr2, (value & 0xffff));
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
				state.vram_updated = true;
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
				state.vram_updated = true;
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
				state.vram_updated = true;
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
			if (newAddr <= (state.exp_ram_size + 0x200000 - 1))
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
			if (newAddr <= (state.exp_ram_size + 0x200000 - 1))
				return RD8(state.exp_ram, newAddr - 0x200000, state.exp_ram_size - 1);
			else
				return EMPTY & 0xff;
		}
	} else {
		printf(">>> WARNING Disassembler RD8 out of range 0x%08X\n", addr);
		return EMPTY & 0xff;
	}
}

