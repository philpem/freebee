#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <malloc.h>
#include <string.h>

#include "SDL.h"

#include "musashi/m68k.h"
#include "version.h"
#include "state.h"
#include "memory.h"

void FAIL(char *err)
{
	state_done();
	fprintf(stderr, "ERROR: %s\nExiting...\n", err);
	exit(EXIT_FAILURE);
}

/**
 * @brief Set the pixel at (x, y) to the given value
 * @note The surface must be locked before calling this!
 * @param	surface		SDL surface upon which to draw
 * @param	x			X co-ordinate
 * @param	y			Y co-ordinate
 * @param	pixel		Pixel value (from SDL_MapRGB)
 */
void putpixel(SDL_Surface *surface, int x, int y, Uint32 pixel)
{
	int bpp = surface->format->BytesPerPixel;
	/* Here p is the address to the pixel we want to set */
	Uint8 *p = (Uint8 *)surface->pixels + y * surface->pitch + x * bpp;

	switch (bpp) {
		case 1:
			*p = pixel;
			break;

		case 2:
			*(Uint16 *)p = pixel;
			break;

		case 3:
			if (SDL_BYTEORDER == SDL_BIG_ENDIAN) {
				p[0] = (pixel >> 16) & 0xff;
				p[1] = (pixel >> 8) & 0xff;
				p[2] = pixel & 0xff;
			}
			else {
				p[0] = pixel & 0xff;
				p[1] = (pixel >> 8) & 0xff;
				p[2] = (pixel >> 16) & 0xff;
			}
			break;

		case 4:
			*(Uint32 *)p = pixel;
			break;

		default:
			break;           /* shouldn't happen, but avoids warnings */
	} // switch
}


/**
 * @brief	Refresh the screen.
 * @param	surface		SDL surface upon which to draw.
 */
void refreshScreen(SDL_Surface *s)
{
	// Lock the screen surface (if necessary)
	if (SDL_MUSTLOCK(s)) {
		if (SDL_LockSurface(s) < 0) {
			fprintf(stderr, "ERROR: Unable to lock screen!\n");
			exit(EXIT_FAILURE);
		}
	}

	// Map the foreground and background colours
	Uint32 fg = SDL_MapRGB(s->format, 0x00, 0xFF, 0x00);	// green foreground
//	Uint32 fg = SDL_MapRGB(s->format, 0xFF, 0xC1, 0x06);	// amber foreground
//	Uint32 fg = SDL_MapRGB(s->format, 0xFF, 0xFF, 0xFF);	// white foreground
	Uint32 bg = SDL_MapRGB(s->format, 0x00, 0x00, 0x00);	// black background

	// Refresh the 3B1 screen area first. TODO: only do this if VRAM has actually changed!
	uint32_t vram_address = 0;
	for (int y=0; y<348; y++) {
		for (int x=0; x<720; x+=16) {	// 720 pixels, monochrome, packed into 16bit words
			// Get the pixel
			uint16_t val = RD16(state.vram, vram_address, sizeof(state.vram)-1);
			vram_address += 2;
			// Now copy it to the video buffer
			for (int px=0; px<16; px++) {
				if (val & 1)
					putpixel(s, x+px, y, fg);
				else
					putpixel(s, x+px, y, bg);
				val >>= 1;
			}
		}
	}

	// TODO: blit LEDs and status info

	// Unlock the screen surface
	if (SDL_MUSTLOCK(s)) {
		SDL_UnlockSurface(s);
	}

	// Trigger a refresh -- TODO: partial refresh depending on whether we
	// refreshed the screen area, status area, both, or none. Use SDL_UpdateRect() for this.
	SDL_Flip(s);
}

/**
 * @brief	Handle events posted by SDL.
 */
bool HandleSDLEvents(SDL_Surface *screen)
{
	SDL_Event event;
	while (SDL_PollEvent(&event))
	{
		if ((event.type == SDL_KEYDOWN) || (event.type == SDL_KEYUP)) {
			keyboard_event(&state.kbd, &event);
		}

		switch (event.type) {
			case SDL_QUIT:
				// Quit button tagged. Exit.
				return true;
			case SDL_KEYDOWN:
				switch (event.key.keysym.sym) {
					case SDLK_F11:
						if (state.fdc_disc) {
							wd2797_unload(&state.fdc_ctx);
							fclose(state.fdc_disc);
							state.fdc_disc = NULL;
							fprintf(stderr, "Disc image unloaded.\n");
						} else {
							state.fdc_disc = fopen("discim", "rb");
							if (!state.fdc_disc) {
								fprintf(stderr, "ERROR loading disc image 'discim'.\n");
								state.fdc_disc = NULL;
							} else {
								wd2797_load(&state.fdc_ctx, state.fdc_disc, 512, 10, 2);
								fprintf(stderr, "Disc image loaded.\n");
							}
						}
						break;
					case SDLK_F12:
						if (event.key.keysym.mod & (KMOD_LALT | KMOD_RALT))
							// ALT-F12 pressed; exit emulator
							return true;
						break;
					default:
						break;
				}
				break;
			default:
				break;
		}
	}

	return false;
}


/****************************
 * blessed be thy main()...
 ****************************/

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
	int i;
	if ((i = state_init(512*1024, 512*1024)) != STATE_E_OK) {
		fprintf(stderr, "ERROR: Emulator initialisation failed. Error code %d.\n", i);
		return i;
	}

	// set up musashi and reset the CPU
	m68k_set_cpu_type(M68K_CPU_TYPE_68010);
	m68k_pulse_reset();

	// Set up SDL
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) == -1) {
		printf("Could not initialise SDL: %s.\n", SDL_GetError());
		exit(EXIT_FAILURE);
	}

	// Make sure SDL cleans up after itself
	atexit(SDL_Quit);

	// Set up the video display
	SDL_Surface *screen = NULL;
	if ((screen = SDL_SetVideoMode(720, 384, 8, SDL_SWSURFACE | SDL_ANYFORMAT)) == NULL) {
		printf("Could not find a suitable video mode: %s.\n", SDL_GetError());
		exit(EXIT_FAILURE);
	}
	printf("Set %dx%d at %d bits-per-pixel mode\n\n", screen->w, screen->h, screen->format->BitsPerPixel);
	SDL_WM_SetCaption("FreeBee 3B1 emulator", "FreeBee");

	// Load a disc image
	state.fdc_disc = fopen("discim", "rb");
	if (!state.fdc_disc) {
		fprintf(stderr, "ERROR loading disc image 'discim'.\n");
		return -4;
	}
	wd2797_load(&state.fdc_ctx, state.fdc_disc, 512, 10, 2);

	/***
	 * The 3B1 CPU runs at 10MHz, with DMA running at 1MHz and video refreshing at
	 * around 60Hz (???), with a 60Hz periodic interrupt.
	 */
	const uint32_t SYSTEM_CLOCK = 10e6; // Hz
	const uint32_t TIMESLOT_FREQUENCY = 1000;//240;	// Hz
	const uint32_t MILLISECS_PER_TIMESLOT = 1e3 / TIMESLOT_FREQUENCY;
	const uint32_t CLOCKS_PER_60HZ = (SYSTEM_CLOCK / 60);
	uint32_t next_timeslot = SDL_GetTicks() + MILLISECS_PER_TIMESLOT;
	uint32_t clock_cycles = 0, tmp;
	bool exitEmu = false;
	bool lastirq_fdc = false;
	for (;;) {
		// Run the CPU for however many cycles we need to. CPU core clock is
		// 10MHz, and we're running at 240Hz/timeslot. Thus: 10e6/240 or
		// 41667 cycles per timeslot.
		tmp = m68k_execute(SYSTEM_CLOCK/TIMESLOT_FREQUENCY);
		clock_cycles += tmp;

		// Run the DMA engine
		if (state.dmaen) {
			// DMA ready to go -- so do it.
			size_t num = 0;
			while (state.dma_count < 0x4000) {
				uint16_t d = 0;

				// num tells us how many words we've copied. If this is greater than the per-timeslot DMA maximum, bail out!
				if (num > (1e6/TIMESLOT_FREQUENCY)) break;

				// Evidently we have more words to copy. Copy them.
				if (!wd2797_get_drq(&state.fdc_ctx)) {
					// Bail out, no data available. Try again later.
					// TODO: handle HDD controller too
					break;
				}

				// Check memory access permissions
				// TODO: enforce these!!!! use ACCESS_CHECK_* for guidance.
				bool access_ok;
				switch (checkMemoryAccess(state.dma_address, !state.dma_reading)) {
					case MEM_PAGEFAULT:
						// Page fault
						state.genstat = 0x8BFF
							| (state.dma_reading ? 0x4000 : 0)
							| (state.pie ? 0x0400 : 0);
						access_ok = false;
						break;

					case MEM_UIE:
						// User access to memory above 4MB
						// FIXME? Shouldn't be possible with DMA... assert this?
						state.genstat = 0x9AFF
							| (state.dma_reading ? 0x4000 : 0)
							| (state.pie ? 0x0400 : 0);
						access_ok = false;
						break;

					case MEM_KERNEL:
					case MEM_PAGE_NO_WE:
						// Kernel access or page not write enabled
						access_ok = false;
						break;

					case MEM_ALLOWED:
						access_ok = true;
						break;
				}
				if (!access_ok) {
					state.bsr0 = 0x3C00;
					state.bsr0 |= (state.dma_address >> 16);
					state.bsr1 = state.dma_address & 0xffff;
					m68k_pulse_bus_error();
					printf("BUS ERROR FROM DMA: genstat=%04X, bsr0=%04X, bsr1=%04X\n", state.genstat, state.bsr0, state.bsr1);

					// TODO: FIXME: if we get a pagefault, it NEEDS to be tagged as 'peripheral sourced'... this is a HACK!
					printf("REALLY BIG FSCKING HUGE ERROR: DMA Memory Access caused a FAULT!\n");
					exit(-1);
				}

				// Map logical address to a physical RAM address
				uint32_t newAddr = mapAddr(state.dma_address, !state.dma_reading);

				if (!state.dma_reading) {
					// Data available. Get it from the FDC. TODO: handle HDD too
					d = wd2797_read_reg(&state.fdc_ctx, WD2797_REG_DATA);
					d <<= 8;
					d += wd2797_read_reg(&state.fdc_ctx, WD2797_REG_DATA);

					if (newAddr <= 0x1FFFFF) {
						WR16(state.base_ram, newAddr, state.base_ram_size - 1, d);
					} else if (newAddr >= 0x200000) {
						WR16(state.exp_ram, newAddr - 0x200000, state.exp_ram_size - 1, d);
					}
					m68k_write_memory_16(state.dma_address, d);
				} else {
					// Data write to FDC. TODO: handle HDD too.

					// Get the data from RAM
					if (newAddr <= 0x1fffff) {
						d = RD16(state.base_ram, newAddr, state.base_ram_size - 1);
					} else {
						if (newAddr <= (state.exp_ram_size + 0x200000 - 1))
							d = RD16(state.exp_ram, newAddr - 0x200000, state.exp_ram_size - 1);
						else
							d = 0xffff;
					}

					// Send the data to the FDD
					wd2797_write_reg(&state.fdc_ctx, WD2797_REG_DATA, (d >> 8));
					wd2797_write_reg(&state.fdc_ctx, WD2797_REG_DATA, (d & 0xff));
				}

				// Increment DMA address
				state.dma_address+=2;
				// Increment number of words transferred
				num++; state.dma_count++;
			}

			// Turn off DMA engine if we finished this cycle
			if (state.dma_count >= 0x4000) {
				// FIXME? apparently this isn't required... or is it?
//				state.dma_count = 0;
				state.dmaen = false;
			}
		}

		// Any interrupts? --> TODO: masking
/*		if (!lastirq_fdc) {
			if (wd2797_get_irq(&state.fdc_ctx)) {
				lastirq_fdc = true;
				m68k_set_irq(2);
			}
*/
		if (keyboard_get_irq(&state.kbd)) {
			m68k_set_irq(3);
		} else {
			lastirq_fdc = wd2797_get_irq(&state.fdc_ctx);
			m68k_set_irq(0);
		}

		// Is it time to run the 60Hz periodic interrupt yet?
		if (clock_cycles > CLOCKS_PER_60HZ) {
			// Refresh the screen
			refreshScreen(screen);
			// TODO: trigger periodic interrupt (if enabled)
			// scan the keyboard
			keyboard_scan(&state.kbd);
			// decrement clock cycle counter, we've handled the intr.
			clock_cycles -= CLOCKS_PER_60HZ;
		}

		// handle SDL events -- returns true if we need to exit
		if (HandleSDLEvents(screen))
			exitEmu = true;

		// make sure frame rate is equal to real time
		uint32_t now = SDL_GetTicks();
		if (now < next_timeslot) {
			// timeslot finished early -- eat up some time
			SDL_Delay(next_timeslot - now);
		} else {
			// timeslot finished late -- skip ahead to gain time
			// TODO: if this happens a lot, we should let the user know
			// that their PC might not be fast enough...
			next_timeslot = now;
		}
		// advance to the next timeslot
		next_timeslot += MILLISECS_PER_TIMESLOT;

		// if we've been asked to exit the emulator, then do so.
		if (exitEmu) break;
	}

	// Release the disc image
	wd2797_unload(&state.fdc_ctx);
	fclose(state.fdc_disc);

	return 0;
}
