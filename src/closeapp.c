/*
 * closeapp - Alt-F9 (Option-F9 on a Mac) closes the program in the front
 * window, also one that hangs.
 *
 * The 3B1 has no key for that. Suspd only puts the window behind the
 * others, and Exit or Del reach a program only while it reads the
 * keyboard. So FreeBee does what "kill -9" would do:
 *
 * - The window that has the keyboard is keywp, in the window driver (see
 *   keywin.c). Its tty's t_pgrp is the process group of the window's
 *   program: the Office starts each program in a group of its own.
 * - FreeBee calls the kernel's own signal(pgrp, SIGKILL), as the tty driver
 *   does with SIGINT when Del is typed. It calls it the way an interrupt
 *   would come: only while the CPU is at IPL 0, with a frame on the
 *   supervisor stack, one instruction at a time until signal() returns.
 *   Then the CPU is put back exactly as it was.
 * - The kernel ends the processes, the window closes, the Office comes
 *   back.
 *
 * Never the Office itself, the window manager or the status line: if ua,
 * wmgr or smgr is in the group, nothing happens. The names are in the
 * processes' u areas. If one of the group's u areas is not in memory,
 * nothing happens either.
 *
 * signal() is found by its code, which holds no address but &proc and
 * &v.ve_proc (UNIX 3.51m: 0xb068):
 *	for (p = &proc[1]; p < v.ve_proc; p++)
 *		if (p->p_pgrp == pgrp)
 *			psignal(p, sig);
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "musashi/m68k.h"
#include "memory.h"
#include "keywin.h"
#include "fbmcp.h"
#include "closeapp.h"

// signal() in UNIX 3.51m; words 9-10 are &proc, 28-29 &v.ve_proc. 0x70 is
// sizeof(struct proc), 0x14 is p_pgrp.
static const uint16_t signal_code[] = {
	0x4E56, 0xFFF4,			// link a6,#-12
	0x48EE, 0x0404, 0xFFF8,		// movem.l d2/a2,-8(a6)
	0x242E, 0x0008,			// move.l 8(a6),d2		pgrp
	0x672E,				// beq.s out
	0x2079, 0, 0,			// movea.l proc,a0
	0x45E8, 0x0070,			// lea 0x70(a0),a2		&proc[1]
	0x601A,				// bra.s test
	0x302A, 0x0014,			// loop: move.w p_pgrp(a2),d0
	0x48C0, 0xB082, 0x660C,		// ext.l d0; cmp.l d2,d0; bne.s next
	0x2EAE, 0x000C,			// move.l 12(a6),(a7)		sig
	0x4852,				// pea (a2)
	0x4EBA, 0x001A,			// jsr psignal(pc)
	0x584F,				// addq.w #4,a7
	0x45EA, 0x0070,			// next: lea 0x70(a2),a2
	0xB5F9, 0, 0,			// test: cmpa.l v.ve_proc,a2
	0x65DE,				// bcs.s loop
	0x4CEE, 0x0404, 0xFFF8,		// out: movem.l -8(a6),d2/a2
	0x4E5E, 0x4E75,			// unlk a6; rts
};
#define N_CODE (sizeof(signal_code) / sizeof(signal_code[0]))

// struct proc (sys/proc.h)
#define PROC_SIZE	0x70
#define P_FLAG		4		// int
#define P_STAT		8		// char
#define P_PGRP		0x14		// short
#define P_ADDR		0x1A		// ushort[2]: the u area's physical pages
#define SLOAD		0x1		// p_flag: in memory
#define SZOMB		5		// p_stat: a zombie, it has no u area
// struct user (sys/user.h): u is at 0x70900, so 0x900 into its first page
#define U_OFFSET	0x900
#define U_PROCP		0xA6		// struct proc *
#define U_COMM		0x25A		// char[DIRSIZ]
#define DIRSIZ		14
// struct tty (sys/tty.h) t_pgrp
#define T_PGRP		64

#define SIGKILL		9
// signal() runs a few hundred instructions; past this, something is wrong
#define MAX_INSTRUCTIONS 200000

// processes whose group is never closed
static const char *const keep[] = { "ua", "wmgr", "smgr" };

static uint32_t signal_at, proc_var, end_var;
static bool pending;
static int wait_ticks;
static char result[200] = "nothing yet";

static bool peek32(uint32_t a, uint32_t *v)
{
	uint16_t hi, lo;
	if (!mem_peek16(a, &hi) || !mem_peek16(a + 2, &lo))
		return false;
	*v = (uint32_t)hi << 16 | lo;
	return true;
}

static bool kernel_ptr(uint32_t p)
{
	return p && !(p & 1) && p < 0x80000;
}

// signal() is at a: then &proc and &v.ve_proc
static bool signal_from(uint32_t a, uint32_t *procv, uint32_t *endv)
{
	uint16_t w[N_CODE];
	for (size_t i = 0; i < N_CODE; i++) {
		if (!mem_peek16(a + 2 * i, &w[i]))
			return false;
		bool const addr = i == 9 || i == 10 || i == 28 || i == 29;
		if (!addr && w[i] != signal_code[i])
			return false;
	}
	*procv = (uint32_t)w[9] << 16 | w[10];
	*endv = (uint32_t)w[28] << 16 | w[29];
	return kernel_ptr(*procv) && kernel_ptr(*endv);
}

// look through the kernel (virtual 0 to 512K) for signal(), unless it is
// still where it was
static bool find_signal(void)
{
	uint32_t p, e;
	if (signal_at && signal_from(signal_at, &p, &e) && p == proc_var && e == end_var)
		return true;
	signal_at = 0;
	for (uint32_t a = 0x400; a < 0x80000; a += 2) {
		if (signal_from(a, &p, &e)) {
			signal_at = a;
			proc_var = p;
			end_var = e;
			return true;
		}
	}
	return false;
}

// the process group of the program in the window that has the keyboard
static int front_pgrp(void)
{
	uint32_t const wp = keywin_window();
	uint32_t tp;
	uint16_t pg;
	if (!wp || !peek32(wp + WD_TP, &tp) || !kernel_ptr(tp) ||
			!mem_peek16(tp + T_PGRP, &pg))
		return 0;
	return (int16_t)pg;
}

// the name of the process whose proc entry is at p, from its u area; false
// if the u area is not in memory
static bool proc_name(uint32_t p, char name[DIRSIZ + 1])
{
	uint32_t flag;
	uint16_t page, hi, lo;
	if (!peek32(p + P_FLAG, &flag) || !(flag & SLOAD) || !mem_peek16(p + P_ADDR, &page))
		return false;
	uint32_t const u = ((uint32_t)page << 12) + U_OFFSET;
	// the u area points back at its proc entry: then it is one
	if (!mem_peek_phys16(u + U_PROCP, &hi) || !mem_peek_phys16(u + U_PROCP + 2, &lo) ||
			((uint32_t)hi << 16 | lo) != p)
		return false;
	for (int i = 0; i < DIRSIZ; i += 2) {
		uint16_t w;
		if (!mem_peek_phys16(u + U_COMM + i, &w))
			return false;
		name[i] = (char)(w >> 8);
		name[i + 1] = (char)w;
	}
	name[DIRSIZ] = 0;
	return true;
}

// write a long on the stack the call uses
static bool poke32(uint32_t a, uint32_t v)
{
	return mem_dma_write16(a, (uint16_t)(v >> 16), 0xffff) &&
		mem_dma_write16(a + 2, (uint16_t)v, 0xffff);
}

// signal(pgrp, SIGKILL) on the guest's CPU; then the CPU as it was
static bool call_signal(int pgrp)
{
	void *ctx = malloc(m68k_context_size());
	if (!ctx)
		return false;
	m68k_get_context(ctx);
	// the supervisor stack: the one in use (IPL 0 code of the kernel), or
	// the one an interrupt from a user program would use
	uint32_t const sp = m68k_get_reg(NULL, M68K_REG_ISP) - 12;
	bool done = false;
	// the return address is signal() itself: the call is over when the
	// CPU is back there with the arguments on top of the stack
	if (poke32(sp, signal_at) && poke32(sp + 4, (uint32_t)pgrp) && poke32(sp + 8, SIGKILL)) {
		m68k_set_reg(M68K_REG_SR, 0x2700);	// supervisor, no interrupts
		m68k_set_reg(M68K_REG_A7, sp);
		m68k_set_reg(M68K_REG_PC, signal_at);
		for (int n = 0; n < MAX_INSTRUCTIONS; n++) {
			m68k_execute(1);	// one instruction
			if (m68k_get_reg(NULL, M68K_REG_PC) == signal_at &&
					m68k_get_reg(NULL, M68K_REG_A7) == sp + 4) {
				done = true;
				break;
			}
		}
	}
	m68k_set_context(ctx);
	free(ctx);
	return done;
}

static void close_now(void)
{
	if (!find_signal()) {
		snprintf(result, sizeof(result), "no UNIX kernel with signal() found: nothing closed");
		return;
	}
	int const pgrp = front_pgrp();
	if (pgrp <= 1) {
		snprintf(result, sizeof(result), "no program in the front window: nothing closed");
		return;
	}
	uint32_t base, end;
	if (!peek32(proc_var, &base) || !peek32(end_var, &end) || !kernel_ptr(base) ||
			end <= base || end - base > 1000 * PROC_SIZE) {
		snprintf(result, sizeof(result), "no process table: nothing closed");
		return;
	}
	// the group's processes, as signal() goes through them
	char names[120] = "", name[DIRSIZ + 1];
	int n = 0;
	for (uint32_t p = base + PROC_SIZE; p < end; p += PROC_SIZE) {
		uint16_t st, pg;
		if (!mem_peek16(p + P_STAT, &st) || !mem_peek16(p + P_PGRP, &pg))
			break;
		if ((st >> 8) == 0 || (st >> 8) == SZOMB || (int16_t)pg != pgrp)
			continue;
		if (!proc_name(p, name)) {
			snprintf(result, sizeof(result), "process group %d: a process is not in memory, "
				"its name is not known: nothing closed", pgrp);
			return;
		}
		for (size_t k = 0; k < sizeof(keep) / sizeof(keep[0]); k++) {
			if (strcmp(name, keep[k]) == 0) {
				snprintf(result, sizeof(result), "the front window is %s's (process group %d): "
					"not closed", name, pgrp);
				return;
			}
		}
		if (strlen(names) + strlen(name) + 2 < sizeof(names)) {
			if (n)
				strcat(names, " ");
			strcat(names, name);
		}
		n++;
	}
	if (!n) {
		snprintf(result, sizeof(result), "process group %d has no processes: nothing closed", pgrp);
		return;
	}
	if (call_signal(pgrp))
		snprintf(result, sizeof(result), "closed process group %d (%s)", pgrp, names);
	else
		snprintf(result, sizeof(result), "signal() did not return: nothing closed");
}

void closeapp_request(void)
{
	pending = true;
	wait_ticks = 120;
}

void closeapp_tick(void)
{
	if (!pending)
		return;
	if (--wait_ticks < 0) {
		pending = false;
		snprintf(result, sizeof(result), "the CPU never ran at IPL 0: nothing closed");
		printf("Alt-F9: %s\n", result);
		return;
	}
	// not while the MCP debugger steps or has breakpoints: its hook
	// would stop the CPU inside the call
	if (fbmcp_dbg_active)
		return;
	// only where an interrupt could come
	if (m68k_get_reg(NULL, M68K_REG_SR) & 0x0700)
		return;
	pending = false;
	close_now();
	printf("Alt-F9: %s\n", result);
	fflush(stdout);
}

void closeapp_reset(void)
{
	pending = false;
	signal_at = proc_var = end_var = 0;
}

const char *closeapp_result(void)
{
	return pending ? "waiting for the CPU" : result;
}
