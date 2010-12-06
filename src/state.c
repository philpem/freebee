#define _STATE_C
#include <stddef.h>
#include <malloc.h>
#include <stdio.h>
#include "wd279x.h"
#include "state.h"

int state_init(size_t ramsize)
{
	// Free RAM if it's allocated
	if (state.ram != NULL)
		free(state.ram);

	// Initialise hardware registers
	state.romlmap = false;
	state.idmarw = state.dmaen = state.dmaenb = false;
	state.dma_count = state.dma_address = 0;
	state.pie = 0;
	state.leds = 0;
	state.genstat = 0;				// FIXME: check this
	state.bsr0 = state.bsr1 = 0;	// FIXME: check this

	// Allocate RAM, making sure the user has specified a valid RAM amount first
	// Basically: 512KiB minimum, 4MiB maximum, in increments of 512KiB.
	if ((ramsize < 512*1024) || ((ramsize % (512*1024)) != 0))
		return -1;
	state.ram = malloc(ramsize);
	if (state.ram == NULL)
		return -2;
	state.ram_size = ramsize;

	// Load ROMs
	FILE *r14c, *r15c;
	r14c = fopen("roms/14c.bin", "rb");
//	if (r14c == NULL) FAIL("unable to open roms/14c.bin");
	r15c = fopen("roms/15c.bin", "rb");
//	if (r15c == NULL) FAIL("unable to open roms/15c.bin");

	// get ROM file size
	fseek(r14c, 0, SEEK_END);
	size_t romlen = ftell(r14c);
	fseek(r14c, 0, SEEK_SET);
	fseek(r15c, 0, SEEK_END);
	size_t romlen2 = ftell(r15c);
	fseek(r15c, 0, SEEK_SET);
//	if (romlen2 != romlen) FAIL("ROMs are not the same size!");
//	if ((romlen + romlen2) > ROM_SIZE) FAIL("ROMs are too large to fit in memory!");

	// sanity checks completed; load the ROMs!
	uint8_t *romdat1, *romdat2;
	romdat1 = malloc(romlen);
	romdat2 = malloc(romlen2);
	fread(romdat1, 1, romlen, r15c);
	fread(romdat2, 1, romlen2, r14c);

	// convert the ROM data
	for (size_t i=0; i<(romlen + romlen2); i+=2) {
		state.rom[i+0] = romdat1[i/2];
		state.rom[i+1] = romdat2[i/2];
	}

	// TODO: if ROM buffer not filled, repeat the ROM data we read until it is (wraparound emulation)

	// free the data arrays and close the files
	free(romdat1);
	free(romdat2);
	fclose(r14c);
	fclose(r15c);

	// Initialise the disc controller
	wd2797_init(&state.fdc_ctx);

	return 0;
}

void state_done()
{
	if (state.ram != NULL) {
		free(state.ram);
		state.ram = NULL;
	}
	
	// Deinitialise the disc controller
	wd2797_done(&state.fdc_ctx);
}


