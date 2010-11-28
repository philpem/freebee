#include <stdio.h>
#include <stdint.h>
#include "musashi/m68k.h"

// m68k memory
//uint32_t rom[32768/4];
uint32_t ram[512*1024/4];

// read m68k memory
uint32_t m68k_read_memory_32(uint32_t address)
{
	return ram[address];
}

uint32_t m68k_read_memory_16(uint32_t address)
{
	return ram[address] & 0xFFFF;
}

uint32_t m68k_read_memory_8(uint32_t address)
{
	return ram[address] & 0xFF;
}

// write m68k memory
void m68k_write_memory_32(uint32_t address, uint32_t value)
{
	ram[address] = value;
}

void m68k_write_memory_16(uint32_t address, uint32_t value)
{
	ram[address] = (ram[address] & 0xFFFF0000) | (value & 0xFFFF);
}

void m68k_write_memory_8(uint32_t address, uint32_t value)
{
	ram[address] = (ram[address] & 0xFFFFFF00) | (value & 0xFF);
}


int main(void)
{
	return 0;
}
