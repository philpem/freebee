#define _STATE_C
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include "wd279x.h"
#include "wd2010.h"
#include "keyboard.h"
#include "state.h"
#include "i8274.h"
#include "fbconfig.h"

int state_init(size_t base_ram_size, size_t exp_ram_size)
{
	// Free RAM if it's allocated
	if (state.base_ram != NULL)
		free(state.base_ram);
	if (state.exp_ram != NULL)
		free(state.exp_ram);

	// Initialise hardware registers
	state.romlmap = false;
	state.idmarw = state.dmaen = false;
	state.dma_count = state.dma_address = 0;
	state.pie = 0;
	state.ee = 0;
	state.leds = 0;
	state.genstat = 0;				// FIXME: check this
	state.bsr0 = state.bsr1 = 0;	// FIXME: check this
	state.timer_enabled = state.timer_asserted = false;
	state.dma_dev = DMA_DEV_UNDEF;
	state.mcr2mirror = 0;

	// Enable VIDPAL mod (allows user writing to VRAM), per config setting
	state.vidpal = fbc_get_bool("vidpal", "installed");

	// Allocate Base RAM, making sure the user has specified a valid RAM amount first
	// Basically: 512KiB minimum, 2MiB maximum, in increments of 512KiB.
	if ((base_ram_size < 512*1024) || (base_ram_size > 2048*1024) || ((base_ram_size % (512*1024)) != 0))
		return -1;
	state.base_ram = malloc(base_ram_size);
	if (state.base_ram == NULL)
		return -2;
	state.base_ram_size = base_ram_size;

	// Now allocate expansion RAM
	// The difference here is that we can have zero bytes of Expansion RAM; we're not limited to having a minimum of 512KiB.
	if ((exp_ram_size > 2048*1024) || ((exp_ram_size % (512*1024)) != 0))
		return -1;
	state.exp_ram = malloc(exp_ram_size);
	if (state.exp_ram == NULL)
		return -2;
	state.exp_ram_size = exp_ram_size;

	// Load ROMs
	const char *rom14c = fbc_get_string("roms", "rom_14c");
	const char *rom15c = fbc_get_string("roms", "rom_15c");
	FILE *r14c, *r15c;
	r14c = fopen(rom14c, "rb");
	if (r14c == NULL) {
		fprintf(stderr, "[state] Error loading roms/14c.bin.\n");
		return -3;
	}
	r15c = fopen(rom15c, "rb");
	if (r15c == NULL) {
		fprintf(stderr, "[state] Error loading roms/15c.bin.\n");
		return -3;
	}

	// get ROM file size
	fseek(r14c, 0, SEEK_END);
	size_t romlen = ftell(r14c);
	fseek(r14c, 0, SEEK_SET);
	fseek(r15c, 0, SEEK_END);
	size_t romlen2 = ftell(r15c);
	fseek(r15c, 0, SEEK_SET);
	if (romlen2 != romlen) {
		fprintf(stderr, "[state] ROMs are not the same size!\n");
		return -3;
	}
	if ((romlen + romlen2) > ROM_SIZE) {
		fprintf(stderr, "[state] ROM files are too large!\n");
		return -3;
	}

	// sanity checks completed; load the ROMs!
	uint8_t *romdat1, *romdat2;
	romdat1 = malloc(romlen);
	romdat2 = malloc(romlen2);
	if (fread(romdat1, 1, romlen, r15c) != romlen) {
		fprintf(stderr, "[state] Error reading ROM 15C.\n");
		return -3;
	}
	if (fread(romdat2, 1, romlen2, r14c) != romlen) {
		fprintf(stderr, "[state] Error reading ROM 14C.\n");
		return -3;
	}

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
	// Initialise the keyboard controller
	keyboard_init(&state.kbd);
	// Initialise the serial controller
	i8274_init(&state.serial_ctx);

	return 0;
}

void state_done()
{
	if (state.base_ram != NULL) {
		free(state.base_ram);
		state.base_ram = NULL;
	}

	if (state.exp_ram != NULL) {
		free(state.exp_ram);
		state.exp_ram = NULL;
	}

	// Deinitialise the disc controller
	wd2797_done(&state.fdc_ctx);
	wd2010_done(&state.hdc_ctx);
	// Deinitialise the serial controller
	i8274_done(&state.serial_ctx);
}


