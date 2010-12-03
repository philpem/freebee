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
	state_init(512*1024);

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

	/***
	 * The 3B1 CPU runs at 10MHz, with DMA running at 1MHz and video refreshing at
	 * around 60Hz (???), with a 60Hz periodic interrupt.
	 */
	const uint32_t TIMESLOT_FREQUENCY = 240;	// Hz
	const uint32_t MILLISECS_PER_TIMESLOT = 1e3 / TIMESLOT_FREQUENCY;
	const uint32_t CLOCKS_PER_60HZ = (10e6 / 60);
	uint32_t next_timeslot = SDL_GetTicks() + MILLISECS_PER_TIMESLOT;
	uint32_t clock_cycles = 0;
	bool exitEmu = false;
	for (;;) {
		// Run the CPU for however many cycles we need to. CPU core clock is
		// 10MHz, and we're running at 240Hz/timeslot. Thus: 10e6/240 or
		// 41667 cycles per timeslot.
		clock_cycles += m68k_execute(10e6/TIMESLOT_FREQUENCY);

		// TODO: run DMA here

		// Is it time to run the 60Hz periodic interrupt yet?
		if (clock_cycles > CLOCKS_PER_60HZ) {
			// Refresh the screen
			refreshScreen(screen);
			// TODO: trigger periodic interrupt (if enabled)
			// decrement clock cycle counter, we've handled the intr.
			clock_cycles -= CLOCKS_PER_60HZ;
		}

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

	// shut down and exit
	SDL_Quit();

	return 0;
}
