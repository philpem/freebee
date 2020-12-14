#ifndef _WD2010_H
#define _WD2010_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/// WD2010 registers
typedef enum {
	WD2010_REG_ERROR					= 1, ///< Error register
	WD2010_REG_WRITE_PRECOMP_CYLINDER	= 1, ///< Write precompensation cylinder register
	WD2010_REG_SECTOR_COUNT				= 2, ///< Sector count register
	WD2010_REG_SECTOR_NUMBER			= 3, ///< Sector number register
	WD2010_REG_CYLINDER_LOW				= 4, ///< Low byte of cylinder
	WD2010_REG_CYLINDER_HIGH			= 5, ///< High byte of cylinder
	WD2010_REG_SDH						= 6, ///< Sector size, drive, and head
	WD2010_REG_STATUS					= 7, ///< Status register
	WD2010_REG_COMMAND					= 7, ///< Command register
	UNIXPC_REG_MCR2						= 255	///< UNIX-PC MCR2 register (special!)
} WD2010_REG;

/// WD2010 emulator error codes
typedef enum {
	WD2010_ERR_OK			= 0,		///< Operation succeeded
	WD2010_ERR_BAD_GEOM		= -1,		///< Bad geometry, or image file too small
	WD2010_ERR_NO_MEMORY	= -2,		///< Out of memory
	WD2010_ERR_IO_ERROR	= -3		///< I/O problem
} WD2010_ERR;

typedef struct {
	// Current track, head and sector
	int						track, head, sector;
	// Geometry of current disc
	int						geom_secsz, geom_spt, geom_heads, geom_tracks;
	// IRQ status
	bool					irq;
	// Status of last command
	uint8_t					status;
	// Error resgister
	uint8_t					error_reg;
	// Cylinder number registers
	uint8_t					cylinder_high_reg, cylinder_low_reg;
	// SDH register (sets sector size, drive number, and head number)
	uint8_t					sdh;
	// MCR2 register (LSB is HDSEL3 - head select bit 3)
	bool					mcr2_hdsel3, mcr2_ddrive1;
	// Sector number and count registers
	int						sector_number, sector_count;
	// Last command has the multiple sector flag set?
	bool					multi_sector;
	// Last command uses DRQ bit?
	bool					cmd_has_drq;
	// Current write is a format?
	bool					formatting;
	// Data buffer, current DRQ pointer and length
	uint8_t					*data;
	size_t					data_pos, data_len;
	// Current disc image file
	FILE					*disc_image;
	// LBA at which to start writing
	int						write_pos;
	// Flag to allow delaying DRQ
	bool					drq;
} WD2010_CTX;

/**
 * @brief	Initialise a WD2010 context.
 * @param	ctx		WD2010 context.
 *
 * This must be run once when the context is created.
 */
int wd2010_init(WD2010_CTX *ctx, FILE *fp, int secsz, int spt, int heads);

/**
 * @brief	Reset a WD2010 context.
 * @param	ctx		WD2010 context.
 *
 * This should be run if the WD2010 needs to be reset (MR/ line toggled).
 */
void wd2010_reset(WD2010_CTX *ctx);

/**
 * Deinitialise a WD2010 context.
 * @param	ctx		WD2010 context.
 */
void wd2010_done(WD2010_CTX *ctx);

/**
 * @brief	Read IRQ Rising Edge status.
 * @param	ctx		WD2010 context.
 */
bool wd2010_get_irq(WD2010_CTX *ctx);

/**
 * @brief	Read DRQ status.
 * @param	ctx		WD2010 context.
 */
bool wd2010_get_drq(WD2010_CTX *ctx);

/**
 * @brief	Read WD2010 register.
 * @param	ctx		WD2010 context
 * @param	addr	Register address (0, 1, 2, 3, 4, 5, 6, 7, or 8)
 */
uint8_t wd2010_read_reg(WD2010_CTX *ctx, uint8_t addr);

/**
 * @brief	Write WD2010 register
 * @param	ctx		WD2010 context
 * @param	addr	Register address (0, 1, 2, 3, 4, 5, 6, 7, or 8)
 * @param	val		Value to write
 */
void wd2010_write_reg(WD2010_CTX *ctx, uint8_t addr, uint8_t val);

/**
 * @brief   Read a data byte from the data buffer
 * @param	ctx		WD2010 context
 */
uint8_t wd2010_read_data(WD2010_CTX *ctx);

/**
 * @brief   Write a value to the data buffer
 * @param	ctx		WD2010 context
 * @param   val     Value to write
 */
void wd2010_write_data(WD2010_CTX *ctx, uint8_t val);

void wd2010_dma_miss(WD2010_CTX *ctx);
#endif
