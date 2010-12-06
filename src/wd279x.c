#include <stdint.h>
#include <stdbool.h>
#include <malloc.h>
#include "musashi/m68k.h"
#include "wd279x.h"

/// WD2797 command constants
enum {
	CMD_MASK				= 0xF0,		///< Bit mask to detect command bits
	CMD_RESTORE				= 0x00,		///< Restore (recalibrate, seek to track 0)
	CMD_SEEK				= 0x10,		///< Seek to given track
	CMD_STEP				= 0x20,		///< Step
	CMD_STEP_TU				= 0x30,		///< Step and update track register
	CMD_STEPIN				= 0x40,		///< Step In
	CMD_STEPIN_TU			= 0x50,		///< Step In and update track register
	CMD_STEPOUT				= 0x60,		///< Step Out
	CMD_STEPOUT_TU			= 0x70,		///< Step Out and update track register
	CMD_READ_SECTOR			= 0x80,		///< Read Sector
	CMD_READ_SECTOR_MULTI	= 0x90,		///< Read Multiple Sectors
	CMD_WRITE_SECTOR		= 0xA0,		///< Write Sector
	CMD_WRITE_SECTOR_MULTI	= 0xB0,		///< Write Multiple Sectors
	CMD_READ_ADDRESS		= 0xC0,		///< Read Address (IDAM contents)
	CMD_FORCE_INTERRUPT		= 0xD0,		///< Force Interrupt
	CMD_READ_TRACK			= 0xE0,		///< Read Track
	CMD_FORMAT_TRACK		= 0xF0		///< Format Track
};


void wd2797_init(WD2797_CTX *ctx)
{
	// track, head and sector unknown
	ctx->track = ctx->head = ctx->sector = 0;

	// no IRQ pending
	ctx->irql = ctx->irqe = false;

	// no data available
	ctx->data_pos = ctx->data_len = 0;
	ctx->data = NULL;

	// Status register clear, not busy
	ctx->status = 0;

	// Clear data register
	ctx->data_reg = 0;

	// Last step direction
	ctx->last_step_dir = -1;

	// No disc image loaded
	ctx->disc_image = NULL;
	ctx->geom_secsz = ctx->geom_spt = ctx->geom_heads = ctx->geom_tracks = 0;
}


void wd2797_reset(WD2797_CTX *ctx)
{
	// track, head and sector unknown
	ctx->track = ctx->head = ctx->sector = 0;

	// no IRQ pending
	ctx->irql = ctx->irqe = false;

	// no data available
	ctx->data_pos = ctx->data_len = 0;

	// Status register clear, not busy
	ctx->status = 0;

	// Clear data register
	ctx->data_reg = 0;

	// Last step direction
	ctx->last_step_dir = -1;
}


void wd2797_done(WD2797_CTX *ctx)
{
	// Reset the WD2797
	wd2797_reset(ctx);

	// Free any allocated memory
	if (ctx->data) {
		free(ctx->data);
		ctx->data = NULL;
	}
}


bool wd2797_get_irq(WD2797_CTX *ctx)
{
	// If an IRQ is pending, clear it and return true, otherwise return false
	if (ctx->irqe) {
		ctx->irqe = false;
		return true;
	} else {
		return false;
	}
}


bool wd2797_get_drq(WD2797_CTX *ctx)
{
	return (ctx->data_pos < ctx->data_len);
}


WD2797_ERR wd2797_load(WD2797_CTX *ctx, FILE *fp, int secsz, int spt, int heads)
{
	size_t filesize;

	// Start by finding out how big the image file is
	fseek(fp, 0, SEEK_END);
	filesize = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	// Now figure out how many tracks it contains
	int tracks = filesize / secsz / spt / heads;
	// Confirm...
	if (tracks < 1) {
		return WD2797_ERR_BAD_GEOM;
	}

	// Allocate enough memory to store one disc track
	if (ctx->data) {
		free(ctx->data);
	}
	ctx->data = malloc(secsz * spt);
	if (!ctx->data)
		return WD2797_ERR_NO_MEMORY;

	// Load the image and the geometry data
	ctx->disc_image = fp;
	ctx->geom_tracks = tracks;
	ctx->geom_secsz = secsz;
	ctx->geom_heads = heads;
	ctx->geom_spt = spt;

	return WD2797_ERR_OK;
}


void wd2797_unload(WD2797_CTX *ctx)
{
	// Free memory buffer
	if (ctx->data) {
		free(ctx->data);
		ctx->data = NULL;
	}

	// Clear file pointer
	ctx->disc_image = NULL;

	// Clear the disc geometry
	ctx->geom_tracks = ctx->geom_secsz = ctx->geom_spt = ctx->geom_heads = 0;
}


uint8_t wd2797_read_reg(WD2797_CTX *ctx, uint8_t addr)
{
	uint8_t temp = 0;

	switch (addr & 0x03) {
		case WD2797_REG_STATUS:		// Status register
			// Read from status register clears IRQ
			ctx->irql = false;
			ctx->irqe = false;

			// Get current status flags (set by last command)
			// DRQ bit
			if (ctx->cmd_has_drq) {
				printf("\tWDFDC rd sr, has drq, pos=%lu len=%lu\n", ctx->data_pos, ctx->data_len);
				temp = ctx->status & ~0x03;
				temp |= (ctx->data_pos < ctx->data_len) ? 0x02 : 0x00;
			} else {
				temp = ctx->status & ~0x01;
			}
			// FDC is busy if there is still data in the buffer
			temp |= (ctx->data_pos < ctx->data_len) ? 0x01 : 0x00;	// if data in buffer, then DMA hasn't copied it yet, and we're still busy!
																	// TODO: also if seek delay / read delay hasn't passed (but that's for later)
			return temp;

		case WD2797_REG_TRACK:		// Track register
			return ctx->track;

		case WD2797_REG_SECTOR:		// Sector register
			return ctx->sector;

		case WD2797_REG_DATA:		// Data register
			// If there's data in the buffer, return it. Otherwise return 0xFF.
			if (ctx->data_pos < ctx->data_len) {
				// set IRQ if this is the last data byte
				if (ctx->data_pos == (ctx->data_len-1)) {
					// Set IRQ only if IRQL has been cleared (no pending IRQs)
					ctx->irqe = ctx->irql ? ctx->irqe : true;
					ctx->irql = true;
				}
				// return data byte and increment pointer
				return ctx->data[ctx->data_pos++];
			} else {
				// command finished
				return 0xff;
			}

		default:
			// shut up annoying compilers which don't recognise unreachable code when they see it
			// (here's looking at you, gcc!)
			return 0xff;
	}
}


void wd2797_write_reg(WD2797_CTX *ctx, uint8_t addr, uint8_t val)
{
	uint8_t cmd = val & CMD_MASK;
	size_t lba;
	bool is_type1 = false;
	int temp;

	m68k_end_timeslice();

	switch (addr) {
		case WD2797_REG_COMMAND:	// Command register
			// write to command register clears interrupt request
			ctx->irql = false;

			// Is the drive ready?
			if (ctx->disc_image == NULL) {
				// No disc image, thus the drive is busy.
				ctx->status = 0x80;
				return;
			}

			// Handle Type 1 commands
			switch (cmd) {
				case CMD_RESTORE:
					// Restore. Set track to 0 and throw an IRQ.
					is_type1 = true;
					ctx->track = 0;
					break;

				case CMD_SEEK:
					// Seek. Seek to the track specced in the Data Register.
					is_type1 = true;
					if (ctx->data_reg < ctx->geom_tracks) {
						ctx->track = ctx->data_reg;
					} else {
						// Seek error. :(
						ctx->status = 0x10;
					}

				case CMD_STEP:
					// TODO! deal with trk0!
					// Need to keep a copy of the track register; when it hits 0, set the TRK0 flag.
					is_type1 = true;
					break;

				case CMD_STEPIN:
				case CMD_STEPOUT:
					// TODO! deal with trk0!
					// Need to keep a copy of the track register; when it hits 0, set the TRK0 flag.
					if (cmd == CMD_STEPIN) {
						ctx->last_step_dir = 1;
					} else {
						ctx->last_step_dir = -1;
					}
					is_type1 = true;
					break;

				case CMD_STEP_TU:
				case CMD_STEPIN_TU:
				case CMD_STEPOUT_TU:
					// if this is a Step In or Step Out cmd, set the step-direction
					if (cmd == CMD_STEPIN_TU) {
						ctx->last_step_dir = 1;
					} else if (cmd == CMD_STEPOUT_TU) {
						ctx->last_step_dir = -1;
					}

					// Seek one step in the last direction used.
					ctx->track += ctx->last_step_dir;
					if (ctx->track < 0) ctx->track = 0;
					if (ctx->track >= ctx->geom_tracks) {
						// Seek past end of disc... that'll be a Seek Error then.
						ctx->status = 0x10;
						ctx->track = ctx->geom_tracks - 1;
					}
					is_type1 = true;
					break;

				default:
					break;
			}

			if (is_type1) {
				// Terminate any sector reads or writes
				ctx->data_len = ctx->data_pos = 0;

				// No DRQ bit for these commands.
				ctx->cmd_has_drq = false;

				// Type1 status byte...
				ctx->status = 0;
				// S7 = Not Ready. Command executed, therefore the drive was ready... :)
				// S6 = Write Protect. TODO: add this
				// S5 = Head Loaded. For certain emulation-related reasons, the heads are always loaded...
				ctx->status |= 0x20;
				// S4 = Seek Error. Not bloody likely if we got down here...!
				// S3 = CRC Error. Not gonna happen on a disc image!
				// S2 = Track 0
				ctx->status |= (ctx->track == 0) ? 0x04 : 0x00;
				// S1 = Index Pulse. TODO -- need periodics to emulate this
				// S0 = Busy. We just exec'd the command, thus we're not busy.
				// 		TODO: Set a timer for seeks, and ONLY clear BUSY when that timer expires. Need periodics for that.
				
				// Set IRQ only if IRQL has been cleared (no pending IRQs)
				ctx->irqe = ctx->irql ? ctx->irqe : true;
				ctx->irql = true;
				return;
			}

			// That's the Type 1 (seek) commands sorted. Now for the others.

			// All these commands return the DRQ bit...
			ctx->cmd_has_drq = true;

			// If drive isn't ready, then set status B7 and exit
			if (ctx->disc_image == NULL) {
				ctx->status = 0x80;
				return;
			}

			// If this is a Write command, check write protect status too
			// TODO!
			if (false) {
				// Write protected disc...
				if ((cmd == CMD_WRITE_SECTOR) || (cmd == CMD_WRITE_SECTOR_MULTI) || (cmd == CMD_FORMAT_TRACK)) {
					// Set Write Protect bit and bail.
					ctx->status = 0x40;

					// Set IRQ only if IRQL has been cleared (no pending IRQs)
					ctx->irqe = ctx->irql ? ctx->irqe : true;
					ctx->irql = true;

					return;
				}
			}

			// Disc is ready to go. Parse the command word.
			switch (cmd) {
				case CMD_READ_ADDRESS:
					// Read Address

					// reset data pointers
					ctx->data_pos = ctx->data_len = 0;

					// load data buffer
					ctx->data[ctx->data_len++] = ctx->track;
					ctx->data[ctx->data_len++] = ctx->head;
					ctx->data[ctx->data_len++] = ctx->sector;
					switch (ctx->geom_secsz) {
						case 128:	ctx->data[ctx->data_len++] = 0; break;
						case 256:	ctx->data[ctx->data_len++] = 1; break;
						case 512:	ctx->data[ctx->data_len++] = 2; break;
						case 1024:	ctx->data[ctx->data_len++] = 3; break;
						default:	ctx->data[ctx->data_len++] = 0xFF; break;	// TODO: deal with invalid values better
					}
					ctx->data[ctx->data_len++] = 0;	// TODO: IDAM CRC!
					ctx->data[ctx->data_len++] = 0;

					ctx->status = 0;
					// B6, B5 = 0
					// B4 = Record Not Found. We're not going to see this... FIXME-not emulated
					// B3 = CRC Error. Not possible.
					// B2 = Lost Data. Caused if DRQ isn't serviced in time. FIXME-not emulated
					// B1 = DRQ. Data request.
					ctx->status |= (ctx->data_pos < ctx->data_len) ? 0x02 : 0x00;
					break;

				case CMD_READ_SECTOR:
				case CMD_READ_SECTOR_MULTI:
					printf("WD279X: READ SECTOR chs=%d:%d:%d\n", ctx->track, ctx->head, ctx->sector);
					// Read Sector or Read Sector Multiple
					// reset data pointers
					ctx->data_pos = ctx->data_len = 0;

					// Calculate number of sectors to read from disc
					if (cmd == CMD_READ_SECTOR_MULTI)
						temp = ctx->geom_spt;
					else
						temp = 1;

					for (int i=0; i<temp; i++) {
						// Calculate the LBA address of the required sector
						lba = ((((ctx->track * ctx->geom_heads) + ctx->head) * ctx->geom_spt) + ((ctx->sector + i - 1) % ctx->geom_spt)) * ctx->geom_secsz;
						printf("\tREAD lba = %lu\n", lba);

						// Read the sector from the file
						fseek(ctx->disc_image, lba, SEEK_SET);
						ctx->data_len += fread(&ctx->data[ctx->data_len], 1, ctx->geom_secsz, ctx->disc_image);
						printf("\tREAD len=%lu, pos=%lu, ssz=%d\n", ctx->data_len, ctx->data_pos, ctx->geom_secsz);
						// TODO: check fread return value! if < secsz, BAIL! (call it a crc error or secnotfound maybe? also log to stderr)
					}

					ctx->status = 0;
					// B6 = 0
					// B5 = Record Type -- 1 = deleted, 0 = normal. We can't emulate anything but normal data blocks.
					// B4 = Record Not Found. We're not going to see this... FIXME-not emulated
					// B3 = CRC Error. Not possible.
					// B2 = Lost Data. Caused if DRQ isn't serviced in time. FIXME-not emulated
					// B1 = DRQ. Data request.
					ctx->status |= (ctx->data_pos < ctx->data_len) ? 0x02 : 0x00;
					break;

				case CMD_READ_TRACK:
					// Read Track
					// TODO! implement this
					ctx->status = 0;
					// B6, B5, B4, B3 = 0
					// B2 = Lost Data. Caused if DRQ isn't serviced in time. FIXME-not emulated
					// B1 = DRQ. Data request.
					ctx->status |= (ctx->data_pos < ctx->data_len) ? 0x02 : 0x00;
					break;

				case CMD_WRITE_SECTOR:
				case CMD_WRITE_SECTOR_MULTI:
					// Write Sector or Write Sector Multiple

					// reset data pointers
					ctx->data_pos = ctx->data_len = 0;

					// TODO: set "write pending" flag, and write LBA, and go from there.

					ctx->status = 0;
					// B6 = Write Protect. FIXME -- emulate this!
					// B5 = 0
					// B4 = Record Not Found. We're not going to see this... FIXME-not emulated
					// B3 = CRC Error. Not possible.
					// B2 = Lost Data. Caused if DRQ isn't serviced in time. FIXME-not emulated
					// B1 = DRQ. Data request.
					ctx->status |= (ctx->data_pos < ctx->data_len) ? 0x02 : 0x00;
					break;

				case CMD_FORMAT_TRACK:
					// Write Track (aka Format Track)
					ctx->status = 0;
					// B6 = Write Protect. FIXME -- emulate this!
					// B5, B4, B3 = 0
					// B2 = Lost Data. Caused if DRQ isn't serviced in time. FIXME-not emulated
					// B1 = DRQ. Data request.
					ctx->status |= (ctx->data_pos < ctx->data_len) ? 0x02 : 0x00;
					break;

				case CMD_FORCE_INTERRUPT:
					// Force Interrupt...
					// Terminates current operation and sends an interrupt
					// TODO!
					ctx->status = 0;
					ctx->data_pos = ctx->data_len = 0;
					// Set IRQ only if IRQL has been cleared (no pending IRQs)
					ctx->irqe = ctx->irql ? ctx->irqe : true;
					ctx->irql = true;
					break;
			}
			break;

		case WD2797_REG_TRACK:		// Track register
			ctx->track = val;
			break;

		case WD2797_REG_SECTOR:		// Sector register
			ctx->sector = val;
			break;

		case WD2797_REG_DATA:		// Data register
			// Save the value written into the data register
			ctx->data_reg = val;

			// If we're processing a write command, and there's space in the
			// buffer, allow the write.
			if (ctx->data_pos < ctx->data_len) {
				// set IRQ if this is the last data byte
				if (ctx->data_pos == (ctx->data_len-1)) {
					// Set IRQ only if IRQL has been cleared (no pending IRQs)
					ctx->irqe = ctx->irql ? ctx->irqe : true;
					ctx->irql = true;
				}

				// store data byte and increment pointer
				ctx->data[ctx->data_pos++] = val;
			}
			break;
	}
}

