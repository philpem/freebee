#include "diskimg.h"

#define DISKRAW_DEBUG

#ifndef DISKRAW_DEBUG
#define NDEBUG
#endif
#include "utils.h"

static int init_raw(struct disk_image *ctx, FILE *fp, int secsz, int heads, int tracks)
{
	int spt;
	size_t filesize;
	
	ctx->fp = fp;
	ctx->secsz = secsz;
	
	// Start by finding out how big the image file is
	fseek(fp, 0, SEEK_END);
	filesize = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	
	// Calculate sectors per track
	spt = filesize / secsz / heads / tracks;
	
	return spt;
}

static void done_raw(struct disk_image *ctx)
{
	ctx->fp = NULL;
    ctx->secsz = 0;	
}

static size_t read_sector_raw(struct disk_image *ctx, int lba, uint8_t *data)
{
	size_t bytes_read;
	
	LOG("\tREAD(raw) lba = %i", lba);

	// convert LBA to byte address
	lba *= ctx->secsz;

	// Read the sector from the file
	fseek(ctx->fp, lba, SEEK_SET);
	
	// TODO: check fread return value! if < secsz, BAIL! (call it a crc error or secnotfound maybe? also log to stderr)
	bytes_read = fread(data, 1, ctx->secsz, ctx->fp);
	LOG("\tREAD(raw) len=%lu, ssz=%d", bytes_read, ctx->secsz);
	return bytes_read;	
}

static void write_sector_raw(struct disk_image *ctx, int lba, uint8_t *data)
{
	// convert LBA to byte address
	lba *= ctx->secsz;
	
	fseek(ctx->fp, lba, SEEK_SET);
	fwrite(data, 1, ctx->secsz, ctx->fp);
	fflush(ctx->fp);
}

DISK_IMAGE raw_format = {
	.init = init_raw,
	.done = done_raw,
	.read_sector = read_sector_raw,
	.write_sector = write_sector_raw,
	.fp = NULL,
	.secsz = 512,
	.sectorMap = NULL
};
