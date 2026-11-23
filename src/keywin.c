/*
 * keywin - the 3B1 window that has the keyboard.
 *
 * The window that has the keyboard is keywp, in the window driver that
 * UNIX 3.51 loads at boot (/etc/lddrv/wind). Its wdtotty() is
 *	struct tty *wdtotty() { return keywp ? keywp->w_tp : NULL; }
 * and its code holds no address but keywp's, so FreeBee finds keywp by
 * looking for that code, wherever the driver was loaded. This is specific
 * to UNIX 3.51 (3.51m); with other software nothing is found, and the
 * features that use it do nothing.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "memory.h"
#include "keywin.h"

// wdtotty() in UNIX 3.51m's window driver; words 3-4 and 7-8 are keywp
static const uint16_t wdtotty[] = {
	0x4E56, 0xFFFC,			// link a6,#-4
	0x4AB9, 0, 0,			// tst.l keywp
	0x6710,				// beq.s
	0x2079, 0, 0,			// movea.l keywp,a0
	0x2068, 0x0020,			// movea.l w_tp(a0),a0
	0x2008, 0x4E5E, 0x4E75,		// move.l a0,d0; unlk a6; rts
	0x91C8, 0x60F6,			// suba.l a0,a0; bra.s
};
#define N_CODE (sizeof(wdtotty) / sizeof(wdtotty[0]))

// where wdtotty() and keywp are (0: not found)
static uint32_t code_at, keywp_at;

// keywp's address, if wdtotty() is at a; else 0
static uint32_t keywp_from(uint32_t a)
{
	uint16_t w[N_CODE];
	for (size_t i = 0; i < N_CODE; i++) {
		if (!mem_peek16(a + 2 * i, &w[i]))
			return 0;
		bool const addr = i == 3 || i == 4 || i == 7 || i == 8;
		if (!addr && w[i] != wdtotty[i])
			return 0;
	}
	if (w[3] != w[7] || w[4] != w[8])
		return 0;
	uint32_t const p = ((uint32_t)w[3] << 16) | w[4];
	return (p & 1) || p < 0x400 || p >= 0x80000 ? 0 : p;
}

uint32_t keywin_keywp(void)
{
	if (keywp_at && keywp_from(code_at) != keywp_at)
		keywp_at = 0;		// another kernel is running: look again
	if (keywp_at)
		return keywp_at;
	// look through the kernel (virtual 0 to 512K) for wdtotty()
	for (uint32_t a = 0x400; a < 0x80000; a += 2) {
		uint32_t const p = keywp_from(a);
		if (p) {
			code_at = a;
			keywp_at = p;
			break;
		}
	}
	return keywp_at;
}

uint32_t keywin_window(void)
{
	uint32_t const kw = keywin_keywp();
	uint16_t hi, lo;
	if (!kw || !mem_peek16(kw, &hi) || !mem_peek16(kw + 2, &lo))
		return 0;
	uint32_t const wp = ((uint32_t)hi << 16) | lo;
	return (!wp || (wp & 1) || wp >= 0x80000) ? 0 : wp;
}
