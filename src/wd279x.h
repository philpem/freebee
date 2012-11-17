#ifndef _WD279X_H
#define _WD279X_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/// WD279x registers
typedef enum {
	WD2797_REG_STATUS		= 0,		///< Status register
	WD2797_REG_COMMAND		= 0,		///< Command register
	WD2797_REG_TRACK		= 1,		///< Track register
	WD2797_REG_SECTOR		= 2,		///< Sector register
	WD2797_REG_DATA			= 3			///< Data register
} WD2797_REG;

/// WD279x emulator error codes
typedef enum {
	WD2797_ERR_OK			= 0,		///< Operation succeeded
	WD2797_ERR_BAD_GEOM		= -1,		///< Bad geometry, or image file too small
	WD2797_ERR_NO_MEMORY	= -2		///< Out of memory
} WD2797_ERR;

typedef struct {
	// Current track, head and sector
	int						track, head, sector;
	// Track and sector registers
	int						track_reg, sector_reg;
	// Geometry of current disc
	int						geom_secsz, geom_spt, geom_heads, geom_tracks;
	// IRQ status
	bool					irq;
	// Status of last command
	uint8_t					status;
	// Last command uses DRQ bit?
	bool					cmd_has_drq;
	// The last value written to the data register
	uint8_t					data_reg;
	// Last step direction. -1 for "towards zero", 1 for "away from zero"
	int						last_step_dir;
	// Data buffer, current DRQ pointer and length
	uint8_t					*data;
	size_t					data_pos, data_len;
	// Current disc image file
	FILE					*disc_image;
	// Write protect flag
	int						writeable;
	// LBA at which to start writing
	int						write_pos;
	// True if a format command is in progress
	int						formatting;
} WD2797_CTX;

/**
 * @brief	Initialise a WD2797 context.
 * @param	ctx		WD2797 context.
 *
 * This must be run once when the context is created.
 */
void wd2797_init(WD2797_CTX *ctx);

/**
 * @brief	Reset a WD2797 context.
 * @param	ctx		WD2797 context.
 *
 * This should be run if the WD2797 needs to be reset (nRST line toggled).
 */
void wd2797_reset(WD2797_CTX *ctx);

/**
 * Deinitialise a WD2797 context.
 * @param	ctx		WD2797 context.
 */
void wd2797_done(WD2797_CTX *ctx);

/**
 * @brief	Read IRQ Rising Edge status. Clears Rising Edge status if it is set.
 * @note	No more IRQs will be sent until the Status Register is read, or a new command is written to the CR.
 * @param	ctx		WD2797 context.
 */
bool wd2797_get_irq(WD2797_CTX *ctx);

/**
 * @brief	Read DRQ status.
 * @param	ctx		WD2797 context.
 */
bool wd2797_get_drq(WD2797_CTX *ctx);

/**
 * @brief	Assign a disc image to the WD2797.
 * @param	ctx		WD2797 context.
 * @param	fp		Disc image file, already opened in "r+b" mode.
 * @param	secsz	Sector size: either 128, 256, 512 or 1024.
 * @param	spt		Sectors per track.
 * @param	heads	Number of heads (1 or 2).
 * @return	Error code; WD279X_E_OK if everything worked OK.
 */
WD2797_ERR wd2797_load(WD2797_CTX *ctx, FILE *fp, int secsz, int spt, int heads, int writeable);

/**
 * @brief	Deassign the current image file.
 * @param	ctx		WD2797 context.
 */
void wd2797_unload(WD2797_CTX *ctx);

/**
 * @brief	Read WD279x register.
 * @param	ctx		WD2797 context
 * @param	addr	Register address (0, 1, 2 or 3)
 */
uint8_t wd2797_read_reg(WD2797_CTX *ctx, uint8_t addr);

/**
 * @brief	Write WD279X register
 * @param	ctx		WD2797 context
 * @param	addr	Register address (0, 1, 2 or 3)
 * @param	val		Value to write
 */
void wd2797_write_reg(WD2797_CTX *ctx, uint8_t addr, uint8_t val);

void wd2797_dma_miss(WD2797_CTX *ctx);
#endif
