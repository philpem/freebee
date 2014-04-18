#ifndef _STATE_H
#define _STATE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "wd279x.h"
#include "wd2010.h"
#include "keyboard.h"
#include "tc8250.h"


// Maximum size of the Boot PROMs. Must be a binary power of two.
#define ROM_SIZE 32768

/**
 * State error codes
 */
typedef enum {
	STATE_E_OK				= 0,	///< Operation succeeded
	STATE_E_BAD_RAMSIZE		= -1,	///< Bad RAM size specified (not a multiple of 512K, or less than 512K)
	STATE_E_NO_MEMORY		= -2,	///< Out of memory while allocating state variables
	STATE_E_ROM_LOAD_FAIL	= -3	///< Error loading ROMs
} STATE_ERR;

/**
 * @brief Emulator state storage
 *
 * This structure stores the internal state of the emulator.
 */
typedef struct {
	// Boot PROM can be up to 32Kbytes total size
	uint8_t		rom[ROM_SIZE];		///< Boot PROM data buffer

	//// Main system RAM
	uint8_t		*base_ram;			///< Base RAM data buffer
	size_t		base_ram_size;		///< Size of Base RAM buffer in bytes
	uint8_t		*exp_ram;			///< Expansion RAM data buffer
	size_t		exp_ram_size;		///< Size of Expansion RAM buffer in bytes

	/// Video RAM
	uint8_t		vram[0x8000];

	/// Map RAM
	uint8_t		map[0x800];

	//// Registers
	uint16_t	genstat;			///< General Status Register
	uint16_t	bsr0;				///< Bus Status Register 0
	uint16_t	bsr1;				///< Bus Status Register 1

	//// MISCELLANEOUS CONTROL REGISTER
	bool		dma_reading;		///< True if Disc DMA reads from the controller, false otherwise
	uint8_t		leds;				///< LED status, 1=on, in order red3/green2/yellow1/red0 from bit3 to bit0

	bool		timer_enabled;
	bool		timer_asserted;

	//// GENERAL CONTROL REGISTER
	/// GENCON.ROMLMAP -- false ORs the address with 0x800000, forcing the
	/// 68010 to access ROM instead of RAM when booting. TRM page 2-36.
	bool		romlmap;
	/// GENCON.PIE -- Parity Error Check Enable
	bool		pie;
	/// GENCON.EE -- Error Enable
	bool		ee;

	/// DMA Address Register
	uint32_t	dma_address;

	/// DMA count
	uint32_t	dma_count;

	/// DMA direction
	bool		idmarw;
	/// DMA enable
	bool		dmaen;
	bool		dmaenb;

	/// DMA device selection flags
	bool		fd_selected;
	bool       	hd_selected;
	/// Floppy disc controller context
	WD2797_CTX	fdc_ctx;
	/// Current disc image file
	FILE *fdc_disc;

	/// Hard disc controller context
	WD2010_CTX  hdc_ctx;
	FILE *hdc_disc0;
	FILE *hdc_disc1;

	/// Keyboard controller context
	KEYBOARD_STATE	kbd;

	/// Real time clock context
	TC8250_CTX rtc_ctx;
} S_state;

// Global emulator state. Yes, I know global variables are evil, please don't
// email me and lecture me about it.  -philpem
#ifndef _STATE_C
extern S_state state;
#else
S_state state;
#endif

/**
 * @brief	Initialise system state
 *
 * @param	base_ram_size		Base RAM size in bytes -- must be a multiple of 512KiB, min 512KiB, max 2MiB.
 * @param	exp_ram_size		Expansion RAM size in bytes -- must be a multiple of 512KiB, min 0, max 2MiB.
 *
 * Initialises the emulator's internal state.
 */
int state_init(size_t base_ram_size, size_t exp_ram_size);

/**
 * @brief Deinitialise system state
 *
 * Deinitialises the saved state, and frees all memory. Call this function
 * before exiting your program to avoid memory leaks.
 */
void state_done();

#endif
