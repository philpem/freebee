#ifndef _MEMORY_H
#define _MEMORY_H

/***********************************
 * Array read/write utility macros
 * "Don't Repeat Yourself" :)
 ***********************************/

/// Array read, 32-bit
#define RD32(array, address, andmask)							\
	(((uint32_t)array[(address + 0) & (andmask)] << 24) |		\
	 ((uint32_t)array[(address + 1) & (andmask)] << 16) |		\
	 ((uint32_t)array[(address + 2) & (andmask)] << 8)  |		\
	 ((uint32_t)array[(address + 3) & (andmask)]))

/// Array read, 16-bit
#define RD16(array, address, andmask)							\
	(((uint32_t)array[(address + 0) & (andmask)] << 8)  |		\
	 ((uint32_t)array[(address + 1) & (andmask)]))

/// Array read, 8-bit
#define RD8(array, address, andmask)							\
	((uint32_t)array[(address + 0) & (andmask)])

/// Array write, 32-bit
#define WR32(array, address, andmask, value) do {				\
	array[(address + 0) & (andmask)] = (value >> 24) & 0xff;	\
	array[(address + 1) & (andmask)] = (value >> 16) & 0xff;	\
	array[(address + 2) & (andmask)] = (value >> 8)  & 0xff;	\
	array[(address + 3) & (andmask)] =  value        & 0xff;	\
} while (0)

/// Array write, 16-bit
#define WR16(array, address, andmask, value) do {				\
	array[(address + 0) & (andmask)] = (value >> 8)  & 0xff;	\
	array[(address + 1) & (andmask)] =  value        & 0xff;	\
} while (0)

/// Array write, 8-bit
#define WR8(array, address, andmask, value) do {				\
	array[(address + 0) & (andmask)] =  value        & 0xff;	\
} while (0)

/******************
 * Memory mapping
 ******************/

typedef enum {
	MEM_ALLOWED = 0,
	MEM_PAGEFAULT,		// Page fault -- page not present
	MEM_PAGE_NO_WE,		// Page not write enabled
	MEM_KERNEL,			// User attempted to access kernel memory
	MEM_UIE				// User Nonmemory Location Access
} MEM_STATUS;

/**
 * @brief 	Check memory access permissions for a given address.
 * @param	addr		Address.
 * @param	writing		true if writing to memory, false if reading.
 * @return	One of the MEM_STATUS constants, specifying whether the access is
 * 			permitted, or what error occurred.
 */
MEM_STATUS checkMemoryAccess(uint32_t addr, bool writing);

/**
 * @brief	Map a CPU memory address into physical memory space.
 * @param	addr		Address.
 * @param	writing		true if writing to memory, false if reading.
 * @return	Address, remapped into physical memory.
 */
uint32_t mapAddr(uint32_t addr, bool writing);

#endif
