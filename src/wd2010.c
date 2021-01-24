#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include "SDL.h"
#include "musashi/m68k.h"
#include "wd2010.h"

//#define WD2010_DEBUG

#ifndef WD2010_DEBUG
#define NDEBUG
#endif
#include "utils.h"

#ifndef WD2010_SEEK_DELAY
#define WD2010_SEEK_DELAY 30
#endif

#define CMD_ENABLE_RETRY 0x01
#define CMD_LONG_MODE 0x02
#define CMD_MULTI_SECTOR 0x04
#define CMD_INTRQ_WHEN_COMPLETE 0x08

#define ER_BAD_BLOCK 0x80
#define ER_CRC 0x40
#define ER_ID_NOT_FOUND 0x10
#define ER_ABORTED_COMMAND 0x04
#define ER_NO_TK0 0x02
#define ER_NO_ADDRESS_MARK 0x01

#define SR_BUSY 0x80
#define SR_READY 0x40
#define SR_WRITE_FAULT 0x20
#define SR_SEEK_COMPLETE 0x10
#define SR_DRQ 0x08
#define SR_CORRECTED 0x04
#define SR_COMMAND_IN_PROGRESS 0x02
#define SR_ERROR 0x01

// Cylinder high mask.
// 3.51m Kernel uses the width of Cylinder High to identify whether the controller
// is WD1010 or WD2010.
#ifdef EMULATE_WD1010
# define CYLH_MASK 0x03
#else
# define CYLH_MASK 0x07
#endif

extern int cpu_log_enabled;
static int wd2010_default_init(WD2010_CTX *ctx, FILE *fp, int drivenum, int secsz, int spt, int heads);
static int wd2010_disk_label_init(WD2010_CTX *ctx, FILE *fp, int drivenum);
static int wd2010_pre_label_init(WD2010_CTX *ctx, FILE *fp, int drivenum);

/// WD2010 command constants
enum {
	CMD_MASK				= 0xF0,		///< Bit mask to detect command bits
	CMD_2010_EXT			= 0x00,		///< WD2010 extended commands (compute correction, set parameter)
	CMD_RESTORE				= 0x10,		///< Restore (recalibrate, seek to track 0)
	CMD_READ_SECTOR			= 0x20,		///< Read sector
	CMD_WRITE_SECTOR		= 0x30,		///< Write sector
	CMD_SCAN_ID				= 0x40,		///< Scan ID
	CMD_WRITE_FORMAT		= 0x50,		///< Write format
	CMD_SEEK				= 0x70,		///< Seek to given track
};


static int wd2010_default_init(WD2010_CTX *ctx, FILE *fp, int drivenum, int secsz, int spt, int heads)
{
	size_t filesize;

	// Start by finding out how big the image file is
	fseek(fp, 0, SEEK_END);
	filesize = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	// Now figure out how many tracks it contains
	unsigned int tracks = filesize / secsz / spt / heads;
	// Confirm...
	if (tracks < 1 || tracks > 1400) {
		fprintf(stderr, "ERROR loading disc image 'hd.img'.\n");
		if (tracks > 1400) {
			fprintf(stderr, "ERROR hard disk cylinders > 1400 unsupported by UNIX.\n");
		}
		return WD2010_ERR_BAD_GEOM;
	}

	drivenum = drivenum ? 1 : 0;	// force to 1 or 0
	// Load the geometry data
	ctx->geometry[drivenum].tracks = tracks;
	ctx->geometry[drivenum].secsz = secsz;
	ctx->geometry[drivenum].heads = heads;
	ctx->geometry[drivenum].spt = spt;

	return WD2010_ERR_OK;
}

static int wd2010_disk_label_init(WD2010_CTX *ctx, FILE *fp, int drivenum)
{
	ssize_t count;
	/*
	 * As seen in the s4 utils, the UNIX PC was ahead of most of its
	 * contemporaries, sporting a disk label describing the disk's geometry.
	 * We read that label and pull the interestings bits out of it.
	 */
	struct s4_dswprt {
		char     magic[4];	/* magic number */
		int32_t	 checksum;
		char 	 name[6];	/* name, sort of */
		uint16_t cyls;		/* the number of cylinders for this disk */
		uint16_t heads;		/* number of heads per cylinder */
		uint16_t psectrk;	/* number of physical sectors per track */
		uint16_t pseccyl;	/* number of physical sectors per cylinder */
		char	 flags;		/* floppy density and high tech drive flags */
		char	 step;		/* stepper motor rate to controller */
		uint16_t sectorsz;	/* physical sector size in bytes */
	}  __attribute__((__packed__));
	struct s4_dswprt disk_label;

	(void) fseek(fp, 0L, SEEK_SET);
	if ((count = fread(& disk_label, 1, sizeof(disk_label), fp)) != sizeof(disk_label)) {
		fprintf(stderr, "I/O error reading disk image: %s\n", strerror(errno));
		return WD2010_ERR_IO_ERROR;
	}
	(void) fseek(fp, 0L, SEEK_SET);

	drivenum = drivenum ? 1 : 0;	// force to 1 or 0
	// convert big endian data to native data
	ctx->geometry[drivenum].tracks = ntohs(disk_label.cyls);
	ctx->geometry[drivenum].secsz = ntohs(disk_label.sectorsz);
	ctx->geometry[drivenum].heads = ntohs(disk_label.heads);
	ctx->geometry[drivenum].spt = ntohs(disk_label.psectrk);

	return WD2010_ERR_OK;
}

static int wd2010_pre_label_init(WD2010_CTX *ctx, FILE *fp, int drivenum)
{
	int numheads, numcyls, blocks_per_track, block_size;
	int count;
	char buffer[BUFSIZ];

	(void) fseek(fp, 0L, SEEK_SET);
	(void) fgets(buffer, sizeof(buffer), fp);	// skip magic

	if (fgets(buffer, sizeof(buffer), fp) == NULL)
		return WD2010_ERR_IO_ERROR;

	count = sscanf(buffer, "heads: %d cyls: %d bpt: %d blksiz: %d",
			& numheads, & numcyls, & blocks_per_track, & block_size);
	if (count != 4)
		return WD2010_ERR_BAD_GEOM;

	(void) fseek(fp, 0L, SEEK_SET);

	drivenum = drivenum ? 1 : 0;	// force to 1 or 0
	ctx->geometry[drivenum].tracks = numcyls;
	ctx->geometry[drivenum].secsz = block_size;
	ctx->geometry[drivenum].heads = numheads;
	ctx->geometry[drivenum].spt = blocks_per_track;

	return WD2010_ERR_OK;
}


int wd2010_init(WD2010_CTX *ctx, FILE *fp, int drivenum, int secsz, int spt, int heads)
{
	int result;
	char magic[4];

	wd2010_reset(ctx);

	// read first 4 bytes
	// if UNIX PC magic, get real geometry
	// else if early magic, get user-specified geometry
	// else do default settings
	(void) fseek(fp, 0L, SEEK_SET);
	if (fread(magic, 1, 4, fp) != 4)
		return WD2010_ERR_IO_ERROR;

	if (strncmp(magic, "UQVQ", 4) == 0) {
		result = wd2010_disk_label_init(ctx, fp, drivenum);
	} else if (strncmp(magic, "free", 4) == 0) {
		result = wd2010_pre_label_init(ctx, fp, drivenum);
	} else {
		result = wd2010_default_init(ctx, fp, drivenum, secsz, spt, heads);
	}

	drivenum = drivenum ? 1 : 0;	// force to 1 or 0
	printf("WD2010 initialised: %d cylinders, %d heads, %d sectors per track\n",
			ctx->geometry[drivenum].tracks, ctx->geometry[drivenum].heads,
			ctx->geometry[drivenum].spt);

	// Allocate enough memory to store one disc track
	if (ctx->data[drivenum]) {
		free(ctx->data[drivenum]);
	}
	ctx->data[drivenum] = malloc(ctx->geometry[drivenum].secsz * ctx->geometry[drivenum].spt);
	if (!ctx->data[drivenum])
		return WD2010_ERR_NO_MEMORY;

	(void) fseek(fp, 0L, SEEK_SET);
	ctx->disc_image[drivenum] = fp;

	return result;
}


void wd2010_reset(WD2010_CTX *ctx)
{
	// track, head and sector unknown
	ctx->track = ctx->head = ctx->sector = 0;

	// no IRQ pending
	ctx->irq = false;

	// no data available
	ctx->data_pos = ctx->data_len = 0;

	// Status register clear, not busy
	ctx->status = 0;

	ctx->sector_count = 0;
	ctx->sector_number = 0;
	ctx->cylinder_low_reg = 0;
	ctx->cylinder_high_reg = 0;
	ctx->sdh = 0;
	ctx->mcr2_hdsel3 = 0;
	ctx->mcr2_ddrive1 = 0;
}

void wd2010_done(WD2010_CTX *ctx)
{
	int i;

	// Reset the WD2010
	wd2010_reset(ctx);

	// Free any allocated memory
	for (i = 0; i < 2; i++) {
		if (ctx->data[i]) {
			free(ctx->data[i]);
			ctx->data[i] = NULL;
		}
	}
}


bool wd2010_get_irq(WD2010_CTX *ctx)
{
	return ctx->irq;
}

bool wd2010_get_drq(WD2010_CTX *ctx)
{
	return (ctx->drq && ctx->data_pos < ctx->data_len);
}

void wd2010_dma_miss(WD2010_CTX *ctx)
{
	ctx->data_pos = ctx->data_len;
	ctx->write_pos = 0;
	ctx->status = SR_READY | SR_SEEK_COMPLETE;
	ctx->irq = true;
}

uint8_t wd2010_read_data(WD2010_CTX *ctx)
{
	// If there's data in the buffer, return it. Otherwise return 0xFF.
	if (ctx->data_pos < ctx->data_len) {
		if (ctx->multi_sector && (ctx->data_pos > 0) && ((ctx->data_pos % ctx->geometry[ctx->mcr2_ddrive1].secsz) == 0)){
			ctx->sector_count--;
			ctx->sector_number++;
		}
		// set IRQ if this is the last data byte
		if (ctx->data_pos == (ctx->data_len-1)) {
			ctx->status = SR_READY | SR_SEEK_COMPLETE;
			// Set IRQ
			ctx->irq = true;
			ctx->drq = false;
			LOG("WD2010: read done");
		}
		// return data byte and increment pointer
		return ctx->data[ctx->mcr2_ddrive1][ctx->data_pos++];
	} else {
		// empty buffer (this shouldn't happen)
		LOGS("WD2010: attempt to read from empty data buffer");
		return 0xff;
	}
}

void wd2010_write_data(WD2010_CTX *ctx, uint8_t val)
{
	// If we're processing a write command, and there's space in the
	// buffer, allow the write.
	if (ctx->write_pos >= 0 && ctx->data_pos < ctx->data_len) {
		// store data byte and increment pointer
		if (ctx->multi_sector && (ctx->data_pos > 0) && ((ctx->data_pos % ctx->geometry[ctx->mcr2_ddrive1].secsz) == 0)){
			ctx->sector_count--;
			ctx->sector_number++;
		}
		ctx->data[ctx->mcr2_ddrive1][ctx->data_pos++] = val;
		// set IRQ and write data if this is the last data byte
		if (ctx->data_pos == ctx->data_len) {
			if (!ctx->formatting){
				fseek(ctx->disc_image[ctx->mcr2_ddrive1], ctx->write_pos, SEEK_SET);
				fwrite(ctx->data[ctx->mcr2_ddrive1], 1, ctx->data_len, ctx->disc_image[ctx->mcr2_ddrive1]);
				fflush(ctx->disc_image[ctx->mcr2_ddrive1]);
			}
			ctx->formatting = false;
			ctx->status = SR_READY | SR_SEEK_COMPLETE;
			// Set IRQ and reset write pointer
			ctx->irq = true;
			ctx->write_pos = -1;
			ctx->drq = false;
			LOG("WD2010: write done");
		}
	}else{
		LOGS("WD2010: attempt to write to data buffer without a write command in progress");
	}
}

uint32_t seek_complete(uint32_t interval, WD2010_CTX *ctx)
{
	/*m68k_end_timeslice();*/
	ctx->status = SR_READY | SR_SEEK_COMPLETE;
	ctx->irq = true;
	return (0);
}

uint32_t transfer_seek_complete(uint32_t interval, WD2010_CTX *ctx)
{
	/*m68k_end_timeslice();*/
	ctx->drq = true;
	return (0);
}

uint8_t wd2010_read_reg(WD2010_CTX *ctx, uint8_t addr)
{
	uint8_t temp = 0;

	/*cpu_log_enabled = 1;*/

	switch (addr & 0x07) {
		case WD2010_REG_ERROR:
			return ctx->error_reg;
		case WD2010_REG_SECTOR_COUNT:
			return ctx->sector_count;
		case WD2010_REG_SECTOR_NUMBER:
			return ctx->sector_number;
		case WD2010_REG_CYLINDER_HIGH:      // High byte of cylinder
			return ctx->cylinder_high_reg & CYLH_MASK;
		case WD2010_REG_CYLINDER_LOW:       // Low byte of cylinder
			return ctx->cylinder_low_reg;
		case WD2010_REG_SDH:
			return ctx->sdh;
		case WD2010_REG_STATUS:             // Status register
			// Read from status register clears IRQ
			ctx->irq = false;
			// Get current status flags (set by last command)
			// DRQ bit
			if (ctx->cmd_has_drq) {
				temp = ctx->status & ~(SR_BUSY & SR_DRQ);
				temp |= (ctx->data_pos < ctx->data_len) ? SR_DRQ : 0;
				LOG("\tWDFDC rd sr, has drq, pos=%zu len=%zu, sr=0x%02X", ctx->data_pos, ctx->data_len, temp);
			} else {
				temp = ctx->status & ~0x80;
			}
			/*XXX: where should 0x02 (command in progress) be set? should it be set here instead of 0x80 (busy)?*/
			// HDC is busy if there is still data in the buffer
			temp |= (ctx->data_pos < ctx->data_len) ? SR_BUSY : 0;	// if data in buffer, then DMA hasn't copied it yet, and we're still busy!
																	// TODO: also if seek delay / read delay hasn't passed (but that's for later)
			/*XXX: should anything else be set here?*/
			return temp;
		default:
			// shut up annoying compilers which don't recognise unreachable code when they see it
			// (here's looking at you, gcc!)
			return 0xff;
	}
}


void wd2010_write_reg(WD2010_CTX *ctx, uint8_t addr, uint8_t val)
{
	uint8_t cmd = val & CMD_MASK;
	size_t lba;
	int new_track;
	int sector_count;

	m68k_end_timeslice();

	/*cpu_log_enabled = 1;*/

	if (addr == UNIXPC_REG_MCR2) {
		// The UNIX PC has an "MCR2" register with the following format:
		//   [ 7..2 ][1][0]
		//   Bits 7..2: Not used
		//   Bit 1:     DDRIVE1 (hard disk drive 1 select - not used?)
		//   Bit 0:     HDSEL3  (head-select bit 3)
		ctx->mcr2_hdsel3 = ((val & 1) == 1);
		ctx->mcr2_ddrive1 = ((val & 2) == 2);
		return;
	}

	switch (addr & 0x07) {
		case WD2010_REG_WRITE_PRECOMP_CYLINDER:
			break;
		case WD2010_REG_SECTOR_COUNT:
			ctx->sector_count = val;
			break;
		case WD2010_REG_SECTOR_NUMBER:
			// HDSEL3 is also in bit 5 of sector number
			ctx->sector_number = val & 0x1f;
			break;
		case WD2010_REG_CYLINDER_HIGH:		// High byte of cylinder
			ctx->cylinder_high_reg = val & CYLH_MASK;
			break;
		case WD2010_REG_CYLINDER_LOW:		// Low byte of cylinder
			ctx->cylinder_low_reg = val;
			break;
		case WD2010_REG_SDH:
			/*XXX: remove this once the DMA page fault test passes (unless this is actually the correct behavior here)*/
			//ctx->data_pos = ctx->data_len = 0;
			ctx->sdh = val;
			break;
		case WD2010_REG_COMMAND:	// Command register
			// write to command register clears interrupt request
			ctx->irq = false;
			ctx->error_reg = 0;

			/*cpu_log_enabled = 1;*/
			switch (cmd) {
				case CMD_RESTORE:
					// Restore. Set track to 0 and throw an IRQ.
					ctx->track = 0;
					SDL_AddTimer(WD2010_SEEK_DELAY, (SDL_TimerCallback)seek_complete, ctx);
					break;
				case CMD_SCAN_ID:
					ctx->cylinder_high_reg = (ctx->track >> 8) & CYLH_MASK;
					ctx->cylinder_low_reg = ctx->track & 0xff;
					ctx->sector_number = ctx->sector;
					ctx->sdh = (ctx->sdh & ~7) | (ctx->head & 7);
				case CMD_WRITE_FORMAT:
				case CMD_SEEK:
				case CMD_READ_SECTOR:
				case CMD_WRITE_SECTOR:
					// Seek. Seek to the track specced in the cylinder
					// registers.
					new_track = (ctx->cylinder_high_reg << 8) | ctx->cylinder_low_reg;
					if (new_track < ctx->geometry[ctx->mcr2_ddrive1].tracks) {
						ctx->track = new_track;
					} else {
						// Seek error. :(
						fprintf(stderr, "WD2010 ALERT: track %d out of range\n", new_track);
						ctx->status = SR_ERROR;
						ctx->error_reg = ER_ID_NOT_FOUND;
						ctx->irq = true;
						break;
					}
					// The SDH register provides 3 head select bits; the 4th comes from MCR2.
					ctx->head = (ctx->sdh & 0x07) + (ctx->mcr2_hdsel3 ? 8 : 0);
					ctx->sector = ctx->sector_number;

					ctx->formatting = cmd == CMD_WRITE_FORMAT;
					switch (cmd){
						case CMD_SEEK:
							SDL_AddTimer(WD2010_SEEK_DELAY, (SDL_TimerCallback)seek_complete, ctx);
							break;
						case CMD_READ_SECTOR:
							/*XXX: does a separate function to set the head have to be added?*/
							LOG("WD2010: READ SECTOR cmd=%02X chs=%d:%d:%d nsectors=%d", cmd, ctx->track, ctx->head, ctx->sector, ctx->sector_count);

							// Read Sector

							// Check to see if the cyl, hd and sec are valid
							if (cmd != CMD_WRITE_FORMAT && ((ctx->track > (ctx->geometry[ctx->mcr2_ddrive1].tracks-1)) || (ctx->head > (ctx->geometry[ctx->mcr2_ddrive1].heads-1)) || ((ctx->sector + ctx->sector_count - 1) > ctx->geometry[ctx->mcr2_ddrive1].spt-1))) {
								fprintf(stderr, "*** WD2010 ALERT: CHS parameter limit exceeded! CHS=%d:%d:%d, nSecs=%d, endSec=%d maxCHS=%d:%d:%d\n",
										ctx->track, ctx->head, ctx->sector,
										ctx->sector_count,
										ctx->sector + ctx->sector_count - 1,
										ctx->geometry[ctx->mcr2_ddrive1].tracks-1, ctx->geometry[ctx->mcr2_ddrive1].heads-1, ctx->geometry[ctx->mcr2_ddrive1].spt);
								// CHS parameters exceed limits
								ctx->status = SR_ERROR;
								ctx->error_reg = ER_ID_NOT_FOUND;
								// Set IRQ
								ctx->irq = true;
								break;
							}

							// reset data pointers
							ctx->data_pos = ctx->data_len = 0;

							if (val & CMD_MULTI_SECTOR){
								ctx->multi_sector = 1;
								sector_count = ctx->sector_count;
							}else{
								ctx->multi_sector = 0;
								sector_count = 1;
							}
							for (int i=0; i<sector_count; i++) {
								// Calculate the LBA address of the required sector
								// LBA = (C * nHeads * nSectors) + (H * nSectors) + S - 1
								lba = (((ctx->track * ctx->geometry[ctx->mcr2_ddrive1].heads * ctx->geometry[ctx->mcr2_ddrive1].spt) + (ctx->head * ctx->geometry[ctx->mcr2_ddrive1].spt) + ctx->sector) + i);
								// convert LBA to byte address
								lba *= ctx->geometry[ctx->mcr2_ddrive1].secsz;
								LOG("\tREAD lba = %zu", lba);

								// Read the sector from the file
								fseek(ctx->disc_image[ctx->mcr2_ddrive1], lba, SEEK_SET);
								// TODO: check fread return value! if < secsz, BAIL! (call it a crc error or secnotfound maybe? also log to stderr)
								ctx->data_len += fread(&ctx->data[ctx->mcr2_ddrive1][ctx->data_len], 1, ctx->geometry[ctx->mcr2_ddrive1].secsz, ctx->disc_image[ctx->mcr2_ddrive1]);
								LOG("\tREAD len=%zu, pos=%zu, ssz=%d", ctx->data_len, ctx->data_pos, ctx->geometry[ctx->mcr2_ddrive1].secsz);
							}

							ctx->status = 0;
							ctx->status |= (ctx->data_pos < ctx->data_len) ? SR_DRQ | SR_COMMAND_IN_PROGRESS | SR_BUSY : 0x00;
							/*SDL_AddTimer(WD2010_SEEK_DELAY, (SDL_TimerCallback)transfer_seek_complete, ctx);*/
							ctx->drq = true;

							break;
						case CMD_WRITE_FORMAT:
							ctx->sector = 0;
						case CMD_WRITE_SECTOR:
							LOG("WD2010: WRITE SECTOR cmd=%02X chs=%d:%d:%d nsectors=%d", cmd, ctx->track, ctx->head, ctx->sector, ctx->sector_count);
							// Write Sector

							// Check to see if the cyl, hd and sec are valid
							if (cmd != CMD_WRITE_FORMAT && ((ctx->track > (ctx->geometry[ctx->mcr2_ddrive1].tracks-1)) || (ctx->head > (ctx->geometry[ctx->mcr2_ddrive1].heads-1)) || ((ctx->sector + ctx->sector_count - 1) > ctx->geometry[ctx->mcr2_ddrive1].spt-1))) {
								fprintf(stderr, "*** WD2010 ALERT: CHS parameter limit exceeded! CHS=%d:%d:%d, nSecs=%d, endSec=%d maxCHS=%d:%d:%d\n",
										ctx->track, ctx->head, ctx->sector,
										ctx->sector_count,
										ctx->sector + ctx->sector_count - 1,
										ctx->geometry[ctx->mcr2_ddrive1].tracks-1, ctx->geometry[ctx->mcr2_ddrive1].heads-1, ctx->geometry[ctx->mcr2_ddrive1].spt);
								// CHS parameters exceed limits
								ctx->status = SR_ERROR;
								ctx->error_reg = ER_ID_NOT_FOUND;
								// Set IRQ
								ctx->irq = true;
								break;
							}

							// reset data pointers
							ctx->data_pos = ctx->data_len = 0;

							if (val & CMD_MULTI_SECTOR){
								ctx->multi_sector = 1;
								sector_count = ctx->sector_count;
							}else{
								ctx->multi_sector = 0;
								sector_count = 1;
							}
							ctx->data_len = ctx->geometry[ctx->mcr2_ddrive1].secsz * sector_count;
							lba = (((ctx->track * ctx->geometry[ctx->mcr2_ddrive1].heads * ctx->geometry[ctx->mcr2_ddrive1].spt) + (ctx->head * ctx->geometry[ctx->mcr2_ddrive1].spt) + ctx->sector));
							// convert LBA to byte address
							ctx->write_pos = (lba *= ctx->geometry[ctx->mcr2_ddrive1].secsz);
							LOG("\tWRITE lba = %zu", lba);

							ctx->status = 0;
							ctx->status |= (ctx->data_pos < ctx->data_len) ? SR_DRQ | SR_COMMAND_IN_PROGRESS | SR_BUSY : 0x00;
							/*SDL_AddTimer(WD2010_SEEK_DELAY, (SDL_TimerCallback)transfer_seek_complete, ctx);*/
							ctx->drq = true;

							break;
						default:
							LOG("WD2010: invalid seeking command %x (this shouldn't happen!)\n", cmd);
							break;
					}
					break;
				case CMD_2010_EXT: /* not implemented */
				default:
					LOG("WD2010: unknown command %x\n", cmd);
					ctx->status = SR_ERROR;
					ctx->error_reg = ER_ABORTED_COMMAND;
					ctx->irq = true;
					break;
			}
			break;

	}
}

