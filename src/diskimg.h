#ifndef _DISKIMG_H
#define _DISKIMG_H

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define IMD_END_OF_COMMENT 0x1A
#define IMD_HEAD_MASK 0x03
#define IMD_SDR_DATA 0x01
#define IMD_SDR_COMPRESSED 0x01

typedef struct disk_image {	
	int (*init)(struct disk_image *ctx, FILE *fp, int secsz, int heads, int tracks);
	void (*done)(struct disk_image *ctx);
	size_t (*read_sector)(struct disk_image *ctx, int lba, uint8_t *data);
	void (*write_sector)(struct disk_image *ctx, int lba, uint8_t *data);

	uint32_t *sectorMap;  		// sector offset map, needed by IMD
	FILE *fp;
	int secsz;
} DISK_IMAGE;

typedef struct
{
	uint8_t  data_mode; 		// data mode (5 = 250kbps DD, 4 = 300kbps DD)
	uint8_t  cyl;  				// cylinder
	uint8_t  head; 				// head, flags (cylinder map, head map)
	uint8_t  spt;  				// sectors/track
	uint8_t  secsz_code;  		// sector size code (secsz = 128 << secsz_code)
} IMD_TRACK_HEADER;

typedef enum {
	DISK_IMAGE_RAW,
	DISK_IMAGE_IMD				// ImageDisk
} DISK_IMAGE_FORMAT;

// Raw image functions
int init_raw(struct disk_image *ctx, FILE *fp, int secsz, int heads, int tracks);
void done_raw(struct disk_image *ctx);
size_t read_sector_raw(struct disk_image *ctx, int lba, uint8_t *data);
void write_sector_raw(struct disk_image *ctx, int lba, uint8_t *data);

// IMD image functions
int init_imd(struct disk_image *ctx, FILE *fp, int secsz, int heads, int tracks);
void done_imd(struct disk_image *ctx);
size_t read_sector_imd(struct disk_image *ctx, int lba, uint8_t *data);
void write_sector_imd(struct disk_image *ctx, int lba, uint8_t *data);

extern DISK_IMAGE raw_format;
extern DISK_IMAGE imd_format;

#endif
