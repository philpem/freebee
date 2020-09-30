#ifndef _DISKIMG_H
#define _DISKIMG_H

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct disk_image {	
	int (*const init)(struct disk_image *ctx, FILE *fp, int secsz, int heads, int tracks);
	void (*const done)(struct disk_image *ctx);
	size_t (*const read_sector)(struct disk_image *ctx, int cyl, int head, int sect, uint8_t *data);
	void (*const write_sector)(struct disk_image *ctx, int cyl, int head, int sect, uint8_t *data);

	FILE *fp;
	int secsz, heads, spt;

	// IMD specific
	uint32_t *sectorMap;  		// sector offset map
} DISK_IMAGE;

typedef enum {
	DISK_IMAGE_RAW,
	DISK_IMAGE_IMD				// ImageDisk
} DISK_IMAGE_FORMAT;

extern DISK_IMAGE raw_format;
extern DISK_IMAGE imd_format;

#endif
