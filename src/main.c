#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <malloc.h>
#include <string.h>
#include "musashi/m68k.h"
#include "version.h"

#define ROM_SIZE (32768/4)

void state_done(void);

void FAIL(char *err)
{
	state_done();
	fprintf(stderr, "ERROR: %s\nExiting...\n", err);
	exit(EXIT_FAILURE);
}


struct {
	// Boot PROM can be up to 32Kbytes total size
	uint32_t	rom[ROM_SIZE];

	// Main system RAM
	uint32_t	*ram;
	size_t		ram_size;			// number of RAM bytes allocated
	uint32_t	ram_addr_mask;		// address mask

	// GENERAL CONTROL REGISTER
	bool		romlmap;
} state;

int state_init()
{
	// Free RAM if it's allocated
	if (state.ram != NULL)
		free(state.ram);

	// Initialise hardware registers
	state.romlmap = false;

	// Allocate RAM
	// TODO: make sure ram size selection is valid!
	state.ram = malloc(state.ram_size);
	if (state.ram == NULL)
		return -1;
	state.ram_addr_mask = state.ram_size - 1;

	// Load ROMs
	FILE *r14c, *r15c;
	r14c = fopen("roms/14c.bin", "rb");
	if (r14c == NULL) FAIL("unable to open roms/14c.bin");
	r15c = fopen("roms/15c.bin", "rb");
	if (r15c == NULL) FAIL("unable to open roms/15c.bin");

	// get ROM file size
	fseek(r14c, 0, SEEK_END);
	size_t romlen = ftell(r14c);
	fseek(r14c, 0, SEEK_SET);
	fseek(r15c, 0, SEEK_END);
	size_t romlen2 = ftell(r15c);
	fseek(r15c, 0, SEEK_SET);
	if (romlen2 != romlen) FAIL("ROMs are not the same size!");
	if ((romlen / 4) > (ROM_SIZE / 2)) FAIL("ROM 14C is too big!");
	if ((romlen2 / 4) > (ROM_SIZE / 2)) FAIL("ROM 15C is too big!");

	// sanity checks completed; load the ROMs!
	uint8_t *romdat1, *romdat2;
	romdat1 = malloc(romlen);
	romdat2 = malloc(romlen2);
	fread(romdat1, 1, romlen, r15c);
	fread(romdat2, 1, romlen2, r14c);

	// convert the ROM data
	for (size_t i=0; i<romlen; i+=2) {
		state.rom[i/2] = (
				(romdat1[i+0] << 24) |
				(romdat2[i+0] << 16) |
				(romdat1[i+1] << 8)  |
				(romdat2[i+1]));
	}

	for (int i=0; i<8; i++)
		printf("%02X %02X ", romdat1[i], romdat2[i]);
	printf("\n%08X %08X\n", state.rom[0], state.rom[1]);

	// free the data arrays and close the files
	free(romdat1);
	free(romdat2);
	fclose(r14c);
	fclose(r15c);

	return 0;
}

void state_done()
{
	if (state.ram != NULL)
		free(state.ram);
}

// read m68k memory
// TODO: refactor musashi to use stdint, and properly sized integers!
// TODO: find a way to make musashi use function pointers instead of hard coded callbacks, maybe use a context struct too
uint32_t m68k_read_memory_32(uint32_t address)
{
	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	if (address >= 0xC00000) {
		// I/O Registers B
		// TODO
	} else if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
		printf("%08X\n", state.rom[(address & (ROM_SIZE-1)) / 4]);
		return state.rom[(address & (ROM_SIZE-1)) / 4];
	} else if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O Registers A
		// TODO
	} else if (address <= 0x3FFFFF) {
		// RAM
		return state.ram[(address & state.ram_addr_mask) / 4];
	}
	return 0xffffffff;
}

uint32_t m68k_read_memory_16(uint32_t address)
{
	if (address & 2) {
		return m68k_read_memory_32(address) & 0xFFFF;
	} else {
		return (m68k_read_memory_32(address) >> 16) & 0xFFFF;
	}
}

uint32_t m68k_read_memory_8(uint32_t address)
{
	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	switch (address & 3) {
		case 3:		return m68k_read_memory_32(address)			& 0xFF;
		case 2:		return (m68k_read_memory_32(address) >> 8)	& 0xFF;
		case 1:		return (m68k_read_memory_32(address) >> 16)	& 0xFF;
		case 0:		return (m68k_read_memory_32(address) >> 24)	& 0xFF;
	}
	return 0xffffffff;
}

// write m68k memory
void m68k_write_memory_32(uint32_t address, uint32_t value)
{
	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	if (address >= 0xC00000) {
		// I/O Registers B
		// TODO
	} else if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
		// TODO: bus error here? can't write to rom!
	} else if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O Registers A
		// TODO
	} else if (address <= 0x3FFFFF) {
		// RAM
		state.ram[(address & state.ram_addr_mask) / 4] = value;
	}
}

void m68k_write_memory_16(uint32_t address, uint32_t value)
{
	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	if (address >= 0xC00000) {
		// I/O Registers B
		// TODO
	} else if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
		// TODO: bus error here? can't write to rom!
	} else if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O Registers A
		// TODO
	} else if (address <= 0x3FFFFF) {
		// RAM
		if (address & 2)
			state.ram[(address & state.ram_addr_mask) / 4] = (state.ram[(address & state.ram_addr_mask) / 4] & 0xFFFF0000) | (value & 0xFFFF);
		else
			state.ram[(address & state.ram_addr_mask) / 4] = (state.ram[(address & state.ram_addr_mask) / 4] & 0x0000FFFF) | ((value & 0xFFFF) << 16);
	}
}

void m68k_write_memory_8(uint32_t address, uint32_t value)
{
	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	if (address >= 0xC00000) {
		// I/O Registers B
		// TODO
	} else if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
		// TODO: bus error here? can't write to rom!
	} else if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O Registers A
		// TODO
	} else if (address <= 0x3FFFFF) {
		// RAM
		switch (address & 3) {
			case 3:		state.ram[(address & state.ram_addr_mask) / 4] = (state.ram[(address & state.ram_addr_mask) / 4] & 0xFFFFFF00) | (value & 0xFF);
			case 2:		state.ram[(address & state.ram_addr_mask) / 4] = (state.ram[(address & state.ram_addr_mask) / 4] & 0xFFFF00FF) | ((value & 0xFF) << 8);
			case 1:		state.ram[(address & state.ram_addr_mask) / 4] = (state.ram[(address & state.ram_addr_mask) / 4] & 0xFF00FFFF) | ((value & 0xFF) << 16);
			case 0:		state.ram[(address & state.ram_addr_mask) / 4] = (state.ram[(address & state.ram_addr_mask) / 4] & 0x00FFFFFF) | ((value & 0xFF) << 24);
		}
	}
}

int main(void)
{
	// copyright banner
	printf("FreeBee: A Quick-and-Dirty AT&T 3B1 Emulator\n");
	printf("Copyright (C) 2010 P. A. Pemberton.\n");
	printf("Musashi M680x0 emulator engine developed by Karl Stenerud <kstenerud@gmail.com>\n");

	// set up system state
	// 512K of RAM
	state.ram_size = 512*1024;
	state_init();

	// set up musashi
	m68k_set_cpu_type(M68K_CPU_TYPE_68010);
	m68k_pulse_reset();

	// set up SDL

	// emulation loop!
	// repeat:
	// 		m68k_execute()
	// 		m68k_set_irq() every 60ms
	m68k_execute(100000);

	// shut down and exit

	return 0;
}
