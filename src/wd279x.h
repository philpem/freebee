#ifndef _WD279X_H
#define _WD279X_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum {
	WD279X_REG_STATUS = 0,
	WD279X_REG_COMMAND = 0,
	WD279X_REG_TRACK = 1,
	WD279X_REG_SECTOR = 2,
	WD279X_REG_DATA = 3
} WD279X_REG;

typedef struct {
	// Current track, head and sector
	int						track, head, sector;
	// Geometry of current disc
	int						geom_secsz, geom_spt, geom_heads, geom_tracks;
	// IRQ status, level and edge sensitive.
	// Edge sensitive is cleared when host polls the IRQ status.
	// Level sensitive is cleared when emulated CPU polls the status reg or writes a new cmnd.
	// No EDGE sensitive interrupts will be issued unless the LEVEL SENSITIVE IRQ is clear.
	bool					irql, irqe;
	// Status of last command
	uint8_t					status;
	// Last command uses DRQ bit?
	bool					cmd_has_drq;
	// The last value written to the data register
	uint8_t					data_reg;
	// Last step direction. -1 for "towards zero", 1 for "away from zero"
	int						last_step_dir;
	// Data buffer, current DRQ pointer and length
	uint8_t					data[1024];
	size_t					data_pos, data_len;
	// Current disc image file
	FILE					*disc_image;
} WD279X_CTX;

#endif
