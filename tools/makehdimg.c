/*
 * makehdimg.c --- create an initial "disk" image for use with the
 * freebee 3B1 emulator.
 */

/*
 * Copyright (C) 2020,
 * Arnold David Robbins
 *
 * MAKEHDIMG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * MAKEHDIMG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#define BLOCK_SIZE	512	// no other value makes sense, hardcode it
#define MAX_CYLS	1400	// OS doesn't allow more than this

/* usage --- print a usage message and exit */

void
usage(const char *progname)
{
	fprintf(stderr, "usage: %s [-H] -h numheads -c numcyls -b blocks_per_track [-o image]\n",
			progname);
	exit(EXIT_FAILURE);
}

/* main.c --- parse args, allocate and initialize memory, create the file */

int
main(int argc, char **argv)
{
	int c, fd;
	const char *outfile = "hd.img";
	char *buffer;
	size_t buffer_size;
	ssize_t count;
	int numheads, numcyls, blocks_per_track;

	buffer_size = numheads = numcyls = blocks_per_track = 0;

	while ((c = getopt(argc, argv, "Hh:c:b:o:")) != EOF) {
		switch (c) {
		case 'h':
			numheads = strtol(optarg, NULL, 10);
			break;
		case 'c':
			numcyls = strtol(optarg, NULL, 10);
			break;
		case 'b':
			blocks_per_track = strtol(optarg, NULL, 10);
			break;
		case 'o':
			outfile = optarg;
			break;
		case 'H':
		default:
			usage(argv[0]);
			break;
		}
	}

	if (numheads <= 0 || numcyls <= 0 || blocks_per_track <= 0) {
		fprintf(stderr, "error: invalid value supplied or value missing for "
				"one or more parameters\n");
		usage(argv[0]);
	}

	if (numcyls > MAX_CYLS) {
		fprintf(stderr, "error: number of cylinders cannot exceed %d\n", MAX_CYLS);
		exit(EXIT_FAILURE);
	}

	buffer_size = numheads * numcyls * blocks_per_track * BLOCK_SIZE;
	buffer = (char *) malloc(buffer_size);
	if (buffer == NULL) {
		fprintf(stderr, "error: could not allocate %lu bytes of memory: %s\n",
				buffer_size, strerror(errno));
		exit(EXIT_FAILURE);
	}

	/* write info into the buffer */
	memset(buffer, '\0', buffer_size);
	sprintf(buffer, "free\nheads: %d cyls: %d bpt: %d blksiz: %d\n",
			numheads, numcyls, blocks_per_track, BLOCK_SIZE);

	/* write to file */
	if ((fd = open(outfile, O_CREAT|O_WRONLY|O_TRUNC, 0644)) < 0) {
		fprintf(stderr, "error: %s: cannot open for writing: %s\n",
				outfile, strerror(errno));
		exit(EXIT_FAILURE);
	}

	if ((count = write(fd, buffer, buffer_size)) != buffer_size) {
		fprintf(stderr, "error: %s: cannot write data: %s\n",
				outfile, strerror(errno));
		(void) close(fd);
		(void) unlink(outfile);
		exit(EXIT_FAILURE);
	}

	(void) close(fd);
	(void) free(buffer);

	return EXIT_SUCCESS;
}
