#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <malloc.h>
#include <string.h>

#include "musashi/m68k.h"

#include "version.h"
#include "state.h"

void FAIL(char *err)
{
	state_done();
	fprintf(stderr, "ERROR: %s\nExiting...\n", err);
	exit(EXIT_FAILURE);
}

// read m68k memory
uint32_t m68k_read_memory_32(uint32_t address)
{
	uint32_t data = 0xFFFFFFFF;

	printf("RD32 %08X %d", address, state.romlmap);

	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
		data = (((uint32_t)state.rom[(address + 0) & (ROM_SIZE - 1)] << 24) |
				((uint32_t)state.rom[(address + 1) & (ROM_SIZE - 1)] << 16) |
				((uint32_t)state.rom[(address + 2) & (ROM_SIZE - 1)] << 8)  |
				((uint32_t)state.rom[(address + 3) & (ROM_SIZE - 1)]));
	} else if (address < state.ram_size - 1) {
		// RAM
		data = (((uint32_t)state.ram[address + 0] << 24) |
				((uint32_t)state.ram[address + 1] << 16) |
				((uint32_t)state.ram[address + 2] << 8)  |
				((uint32_t)state.ram[address + 3]));
	}

	printf(" ==> %08X\n", data);
	return data;
}

uint32_t m68k_read_memory_16(uint32_t address)
{
	uint16_t data = 0xFFFF;

	printf("RD16 %08X %d", address, state.romlmap);

	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
		data = ((state.rom[(address + 0) & (ROM_SIZE - 1)] << 8) |
				(state.rom[(address + 1) & (ROM_SIZE - 1)]));
	} else if (address < state.ram_size - 1) {
		// RAM
		data = ((state.ram[address + 0] << 8) |
				(state.ram[address + 1]));
	}

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

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
		data = state.rom[(address + 0) & (ROM_SIZE - 1)];
	} else if (address < state.ram_size) {
		// RAM access
		data = state.ram[address + 0];
	}

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
	} else if (address < state.ram_size) {
		// RAM access
		state.ram[address + 0] = (value >> 24) & 0xff;
		state.ram[address + 1] = (value >> 16) & 0xff;
		state.ram[address + 2] = (value >> 8)  & 0xff;
		state.ram[address + 3] =  value        & 0xff;
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
	} else if (address < state.ram_size) {
		// RAM access
		state.ram[address + 0] = (value >> 8)  & 0xff;
		state.ram[address + 1] =  value        & 0xff;
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
	} else if (address < state.ram_size) {
		state.ram[address] = value & 0xff;
	} else {
		switch (address) {
			case 0xE43000:	state.romlmap = ((value & 0x80) == 0x80);
		}
	}
}

// for the disassembler
uint32_t m68k_read_disassembler_32(uint32_t addr) { return m68k_read_memory_32(addr); }
uint32_t m68k_read_disassembler_16(uint32_t addr) { return m68k_read_memory_16(addr); }
uint32_t m68k_read_disassembler_8 (uint32_t addr) { return m68k_read_memory_8 (addr); }

int main(void)
{
	// copyright banner
	printf("FreeBee: A Quick-and-Dirty AT&T 3B1 Emulator. Version %s, %s mode.\n", VER_FULLSTR, VER_BUILD_TYPE);
	printf("Copyright (C) 2010 P. A. Pemberton. All rights reserved.\nLicensed under the Apache License Version 2.0.\n");
	printf("Musashi M680x0 emulator engine developed by Karl Stenerud <kstenerud@gmail.com>\n");
	printf("Built %s by %s@%s.\n", VER_COMPILE_DATETIME, VER_COMPILE_BY, VER_COMPILE_HOST);
	printf("Compiler: %s\n", VER_COMPILER);
	printf("CFLAGS: %s\n", VER_CFLAGS);
	printf("\n");

	// set up system state
	// 512K of RAM
	state_init(512*1024);

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
	int32_t dwTimerTickCounter, dwCpuCycles;
	const int32_t CLOCKS_PER_TIMER_TICK = 10e6/60;		//< number of clocks per 60Hz timer tick

	// initialise emulation variables
	dwTimerTickCounter = CLOCKS_PER_TIMER_TICK;
	bool exitEmu = false;
	for (;;) {
		dwCpuCycles = m68k_execute(10e6/60);
		dwTimerTickCounter -= dwCpuCycles;

		// check for timer tick expiry
		if (dwTimerTickCounter <= 0) {
			// TODO: Timer Tick IRQ
			
		}

		if (exitEmu) break;
	}

	// shut down and exit

	return 0;
}
