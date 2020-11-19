#include "diskimg.h"

//#define DISKIMD_DEBUG

#ifndef DISKIMD_DEBUG
#define NDEBUG
#endif
#include "utils.h"

#define IMD_END_OF_COMMENT 0x1A
#define IMD_HEAD_MASK 0x03
#define IMD_SDR_DATA 0x01
#define IMD_SDR_COMPRESSED 0x01

typedef struct
{
	uint8_t  data_mode; 		// data mode (5 = 250kbps DD, 4 = 300kbps DD)
	uint8_t  cyl;  				// cylinder
	uint8_t  head; 				// head, flags (cylinder map, head map)
	uint8_t  spt;  				// sectors/track
	uint8_t  secsz_code;  		// sector size code (secsz = 128 << secsz_code)
} IMD_TRACK_HEADER;

static int init_imd(struct disk_image *ctx, FILE *fp, int secsz, int heads, int tracks)
{
	uint8_t sdrType, track_sector_map[10], ch;
    IMD_TRACK_HEADER trackHeader;
    size_t filepos;
    
	// write out and advance past comments
    fseek(fp, 0, SEEK_SET);
	while (1) {
		ch = fgetc(fp);
		if (ch == IMD_END_OF_COMMENT || feof(fp))
		   break;
		else
		   printf("%c", ch);
	}
	if (feof(fp)) return -1;

	// probe first track header to get spt
	filepos = ftell(fp);
	if (!fread(&trackHeader, sizeof(trackHeader), 1, fp))
	{
		fprintf(stderr, "error reading IMD track header\n");
		return -1;
	}
	fseek(fp, filepos, SEEK_SET);
	ctx->spt = trackHeader.spt;
	
    ctx->fp = fp;
    ctx->secsz = secsz;
    ctx->heads = heads;

	// allocate sectorMap
	if (ctx->sectorMap) free(ctx->sectorMap);
	ctx->sectorMap = malloc(tracks*heads*ctx->spt*sizeof(uint32_t));
	if (!ctx->sectorMap) return -1;
    	
	// tracks start, build sector offset map, check for unexpected SDRs
	for (int track_i=0; track_i < tracks*heads; track_i++)
	{		
		if (!fread(&trackHeader, sizeof(trackHeader), 1, fp))
		{
			fprintf(stderr, "error reading IMD track header\n");
			return -1;
		}
		// data mode 4 and 5 supported, secsz = 128 << secsz_code, head map & cylinder map flags unsupported
		if (!(trackHeader.data_mode == 5 || trackHeader.data_mode == 4) || trackHeader.spt != ctx->spt ||
			  trackHeader.secsz_code != 2 || (trackHeader.head & ~IMD_HEAD_MASK))
		{
			fprintf(stderr, "unexpected IMD track header data, track %i\n", track_i+1);
			return -1;
		}
		if (!fread(track_sector_map, ctx->spt, 1, fp))
		{
			fprintf(stderr, "error reading IMD track sector map\n");
			return -1;
		}
		
		for (int sect_i=0;  sect_i < ctx->spt; sect_i++)
		{
			ctx->sectorMap[(track_i*ctx->spt) + track_sector_map[sect_i] - 1] = ftell(fp);
			sdrType = fgetc(fp);
			switch (sdrType) {
				case IMD_SDR_DATA:
					fseek(fp, secsz, SEEK_CUR);
					break;
				case (IMD_SDR_DATA + IMD_SDR_COMPRESSED):
					fgetc(fp);  // fill byte
					break;
				default:
					fprintf(stderr, "unexpected IMD sector data record: %i\n", sdrType);
					return -1;
				}
		}
	}
    LOG("IMD file size: %li", ftell(fp));
	return ctx->spt;
}

static void done_imd(struct disk_image *ctx)
{
	if (ctx->sectorMap) free(ctx->sectorMap);
	ctx->sectorMap = NULL;
	
	ctx->fp = NULL;
    ctx->secsz = 0;
    ctx->heads = 0;
    ctx->spt = 0;
}

static size_t read_sector_imd(struct disk_image *ctx, int cyl, int head, int sect, uint8_t *data)
{
	size_t bytes_read;
	uint8_t sdrType, fill;
	int lba;
	
	// Calculate the LBA address of the required sector
	// LBA = (C * nHeads * nSectors) + (H * nSectors) + S - 1
	lba = (cyl * ctx->heads * ctx->spt) + (head * ctx->spt) + sect - 1;

	LOG("\tREAD(IMD), lba: %i, sectorMap offset: %i", lba, ctx->sectorMap[lba]);
	fseek(ctx->fp, ctx->sectorMap[lba], SEEK_SET);
	sdrType = fgetc(ctx->fp);
	switch (sdrType) {
		case IMD_SDR_DATA:
			bytes_read = fread(data, 1, ctx->secsz, ctx->fp);
			LOG("\tREAD(IMD) len=%lu, ssz=%d", bytes_read, ctx->secsz);			
			break;
		case (IMD_SDR_DATA + IMD_SDR_COMPRESSED):
			fill = fgetc(ctx->fp);
			memset(data, fill, ctx->secsz);
			bytes_read = ctx->secsz;
			LOG("\tREAD(IMD, compressed) len=%lu, ssz=%d", bytes_read, ctx->secsz);
			break;
		default:
			fprintf(stderr, "unexpected sector data record: %i\n", sdrType);
			return -1;
		}
	return bytes_read;
}

static void write_sector_imd(struct disk_image *ctx, int cyl, int head, int sect, uint8_t *data)
{
	uint8_t sdrType, fill;
	int lba;
	
	// Calculate the LBA address of the required sector
	// LBA = (C * nHeads * nSectors) + (H * nSectors) + S - 1
	lba = (cyl * ctx->heads * ctx->spt) + (head * ctx->spt) + sect - 1;

	LOG("IMD write sector, lba: %i, sectorMap offset: %i", lba, ctx->sectorMap[lba]);
	
	// IMD writes only supported if sector was uncompressed, or write data is also compressable
	fseek(ctx->fp, ctx->sectorMap[lba], SEEK_SET);
	sdrType = fgetc(ctx->fp);
	switch (sdrType) {
		case IMD_SDR_DATA:
			fwrite(data, 1, ctx->secsz, ctx->fp);
			fflush(ctx->fp);
			LOG("WRITE(IMD), ssz=%i", ctx->secsz);
			break;
		case (IMD_SDR_DATA + IMD_SDR_COMPRESSED):
			fill = data[0];
			// confirm write data is all the same
			for (int i=0; i < ctx->secsz; i++)
			{
			    if (data[i] != fill)
			    {
					fprintf(stderr, "**unsupported IMD sector write**\n");
					return;
				}
			}
			fputc(fill, ctx->fp);
			LOG("WRITE(IMD, compressed), ssz=%i", ctx->secsz);
			break;
		default:
			fprintf(stderr, "**unsupported IMD sector write**\n");
	}
}

DISK_IMAGE imd_format = {
	.init = init_imd,
	.done = done_imd,
	.read_sector = read_sector_imd,
	.write_sector = write_sector_imd,
	.fp = NULL,
	.secsz = 0,
	.heads = 0,
	.spt = 0,
	.sectorMap = NULL
};
