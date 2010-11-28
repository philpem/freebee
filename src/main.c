#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <malloc.h>
#include <string.h>
#include "musashi/m68k.h"
#include "version.h"

#define ROM_SIZE (32768/2)

void state_done(void);

void FAIL(char *err)
{
	state_done();
	fprintf(stderr, "ERROR: %s\nExiting...\n", err);
	exit(EXIT_FAILURE);
}


struct {
	// Boot PROM can be up to 32Kbytes total size
	uint16_t	rom[ROM_SIZE];

	// Main system RAM
	uint16_t	*ram;
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
	for (size_t i=0; i<romlen; i++) {
		state.rom[i] = ((romdat1[i] << 8) | (romdat2[i]));
	}

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
	uint32_t data = 0xFFFFFFFF;

	printf("RD32 %08X %d", address, state.romlmap);

	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
		data = ((state.rom[(address & (ROM_SIZE-1)) / 2] << 16) | (state.rom[((address & (ROM_SIZE-1)) / 2)+1]));
	} else if (address <= 0x3FFFFF) {
		// RAM
		data = state.ram[(address & state.ram_addr_mask) / 2];
	}

	printf(" ==> %04X\n", data);
	return data;
}

uint32_t m68k_read_memory_16(uint32_t address)
{
	uint16_t data = 0xFFFF;

	printf("RD16 %08X %d", address, state.romlmap);

	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	data = (m68k_read_memory_32(address) >> 16) & 0xFFFF;

	printf(" ==> %04X\n", data);
	return data;
}

uint32_t m68k_read_memory_8(uint32_t address)
{
	uint8_t data = 0xFF;

	printf("RD 8 %08X %d ", address, state.romlmap);

	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	data = m68k_read_memory_32(address) & 0xFF;

	printf("==> %02X\n", data);
	return data;
}

// write m68k memory
void m68k_write_memory_32(uint32_t address, uint32_t value)
{
	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	printf("WR32 %08X %d %02X\n", address, state.romlmap, value);

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
		// TODO: bus error here? can't write to rom!
	} else if (address <= 0x3FFFFF) {
		// RAM
		state.ram[(address & state.ram_addr_mask) / 2] = (value >> 16);
		state.ram[((address & state.ram_addr_mask) / 2)+1] = value & 0xffff;
	} else {
		switch (address) {
			case 0xE43000:	state.romlmap = ((value & 0x8000) == 0x8000);
		}
	}
}

void m68k_write_memory_16(uint32_t address, uint32_t value)
{
	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	printf("WR16 %08X %d %02X\n", address, state.romlmap, value);

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
		// TODO: bus error here? can't write to rom!
	} else if (address <= 0x3FFFFF) {
		// RAM
		state.ram[(address & state.ram_addr_mask) / 2] = value & 0xFFFF;
	} else {
		switch (address) {
			case 0xE43000:	state.romlmap = ((value & 0x8000) == 0x8000);
		}
	}
}

void m68k_write_memory_8(uint32_t address, uint32_t value)
{
	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	printf("WR 8 %08X %d %02X\n", address, state.romlmap, value);

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
		// TODO: bus error here? can't write to rom!
	} else if (address <= 0x3FFFFF) {
		// RAM
		switch (address & 3) {
			// FIXME
			case 3:		state.ram[(address & state.ram_addr_mask) / 4] = (state.ram[(address & state.ram_addr_mask) / 4] & 0xFFFFFF00) | (value & 0xFF);
			case 2:		state.ram[(address & state.ram_addr_mask) / 4] = (state.ram[(address & state.ram_addr_mask) / 4] & 0xFFFF00FF) | ((value & 0xFF) << 8);
			case 1:		state.ram[(address & state.ram_addr_mask) / 4] = (state.ram[(address & state.ram_addr_mask) / 4] & 0xFF00FFFF) | ((value & 0xFF) << 16);
			case 0:		state.ram[(address & state.ram_addr_mask) / 4] = (state.ram[(address & state.ram_addr_mask) / 4] & 0x00FFFFFF) | ((value & 0xFF) << 24);
		}
	} else {
		switch (address) {
			case 0xE43000:	state.romlmap = ((value & 0x80) == 0x80);
		}
	}
}

uint32_t m68k_read_disassembler_32(uint32_t addr) { return m68k_read_memory_32(addr); }
uint32_t m68k_read_disassembler_16(uint32_t addr) { return m68k_read_memory_16(addr); }
uint32_t m68k_read_disassembler_8 (uint32_t addr) { return m68k_read_memory_8 (addr); }

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

	char dasm[512];
	m68k_disassemble(dasm, 0x80001a, M68K_CPU_TYPE_68010);
	printf("%s\n", dasm);

	// set up SDL

	// emulation loop!
	// repeat:
	// 		m68k_execute()
	// 		m68k_set_irq() every 60ms
	printf("ran for %d cycles\n", m68k_execute(100000));

	// shut down and exit

	return 0;
}
