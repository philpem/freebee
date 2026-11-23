#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "SDL.h"

#include "musashi/m68k.h"
#include "version.h"
#include "state.h"
#include "memory.h"
#include "fbconfig.h"
#include "utils.h"

#include "lightbar.c"
#include "i8274.h"
#include "dialer.h"

extern int cpu_log_enabled;

void FAIL(char *err)
{
	state_done();
	fprintf(stderr, "ERROR: %s\nExiting...\n", err);
	exit(EXIT_FAILURE);
}

static int load_fd()
{
	int writeable = 1;

	const char *discim = fbc_get_string("floppy", "disk");

	// disk = "" in [floppy]: the drive stays empty, with no error
	if (!discim || !*discim)
		return (0);
	state.fdc_disc = fopen(discim, "r+b");
	if (!state.fdc_disc){
		writeable = 0;
		state.fdc_disc = fopen(discim, "rb");
	}
	if (!state.fdc_disc){
		fprintf(stderr, "ERROR loading floppy image '%s'.\n", discim);
		state.fdc_disc = NULL;
		return (0);
	}else{
		if (wd2797_load(&state.fdc_ctx, state.fdc_disc, 512, 2, 40, writeable) != WD2797_ERR_OK) {
			fprintf(stderr, "ERROR: floppy image '%s' is too small, or of a geometry FreeBee does not know.\n", discim);
			fclose(state.fdc_disc);
			state.fdc_disc = NULL;
			return (0);
		}
		return (1);
	}
}

static int load_hd()
{
	int ret = 0;
	const char *disk1 = fbc_get_string("hard_disk", "disk1");
	const char *disk2 = fbc_get_string("hard_disk", "disk2");
	int sectors_per_track = fbc_get_int("hard_disk", "sectors_per_track");
	int heads = fbc_get_int("hard_disk", "heads");
	// bytes per sector is fixed at 512, not configurable, all hard drives of the 3B1
	// era used 512-byte sectors.
	const int bytes_per_sector = 512;

	state.hdc_disc0 = fopen(disk1, "r+b");
	if (!state.hdc_disc0){
		fprintf(stderr, "Drive 0: ERROR loading disc image '%s'.\n", disk1);
		state.hdc_disc0 = NULL;
		return (0);
	} else {
		if (wd2010_init(&state.hdc_ctx, state.hdc_disc0, 0, bytes_per_sector, sectors_per_track, heads) == WD2010_ERR_OK) {
			printf("Drive 0: Disc image '%s' loaded.\n", disk1);
			ret = 1;
		} else {
			fprintf(stderr, "Drive 0: ERROR loading disc image '%s'.\n", disk1);
			ret = 0;
		}
	}

	// disk2 = "" in [hard_disk]: no second disk, with no error
	state.hdc_disc1 = NULL;
	if (!disk2 || !*disk2)
		return ret;
	state.hdc_disc1 = fopen(disk2, "r+b");
	if (!state.hdc_disc1){
		fprintf(stderr, "Drive 1: ERROR loading disc image '%s'.\n", disk2);
		state.hdc_disc1 = NULL;
	} else {
		if (wd2010_init(&state.hdc_ctx, state.hdc_disc1, 1, bytes_per_sector, sectors_per_track, heads) == WD2010_ERR_OK) {
			printf("Drive 1: Disc image '%s' loaded.\n", disk2);
		} else {
			fprintf(stderr, "Drive 1: ERROR loading disc image '%s'.\n", disk2);
		}
	}
	return ret;
}



/**
 * @brief Set the pixel at (x, y) to the given value
 * @note The surface must be locked before calling this!
 * @param	surface		SDL surface upon which to draw
 * @param	x			X co-ordinate
 * @param	y			Y co-ordinate
 * @param	pixel		Pixel value (from SDL_MapRGB)
 */
static inline void putpixel(SDL_Surface *surface, int x, int y, Uint32 pixel)
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
 * @param	renderer	SDL renderer.
 * @param	texture		SDL texture to copy surface to.
 */
void refreshScreen(SDL_Surface *s, SDL_Renderer *r, SDL_Texture *t)
{
	// Lock the screen surface (if necessary)
	if (SDL_MUSTLOCK(s)) {
		if (SDL_LockSurface(s) < 0) {
			fprintf(stderr, "ERROR: Unable to lock screen!\n");
			exit(EXIT_FAILURE);
		}
	}

	static int red, green, blue;
	static bool inited = false;
	if (! inited) {
		inited = true;
		red = fbc_get_int("display", "red");
		green = fbc_get_int("display", "green");
		blue = fbc_get_int("display", "blue");
	}

	// Map the foreground and background colours
//	Uint32 fg = SDL_MapRGB(s->format, 0xFF, 0xC1, 0x06);	// amber foreground
//	Uint32 fg = SDL_MapRGB(s->format, 0xFF, 0xFF, 0xFF);	// white foreground
//	Uint32 fg = SDL_MapRGB(s->format, 0x50, 0xFF, 0xA0);	// minty foreground (possibly closer to actual color?)
//	Uint32 fg = SDL_MapRGB(s->format, 0x00, 0xFF, 0x00);	// green foreground
	Uint32 fg = SDL_MapRGB(s->format, red, green, blue);
	Uint32 bg = SDL_MapRGB(s->format, 0x00, 0x00, 0x00);	// black background

	// Whole screen reverse video just swaps the two -- no need to touch VRAM
	if (state.reverse_video) {
		Uint32 t = fg; fg = bg; bg = t;
	}

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

	// Unlock the screen surface
	if (SDL_MUSTLOCK(s)) {
		SDL_UnlockSurface(s);
	}

	// Update framebuffer texture. The caller draws it (in the main loop):
	// after SDL_RenderPresent the back buffer is undefined (on macOS/Metal it
	// is an empty buffer), so every frame must be drawn whole.
	SDL_UpdateTexture(t, NULL, s->pixels, s->pitch);
}

// set when the window needs drawing again (exposed, resized, restored)
static bool window_dirty = true;

#define LED_SIZE 8

void refreshStatusBar(SDL_Renderer *r, SDL_Texture *lightbar_tex)
{
	SDL_Rect red_led = 		{ 0, 				0, LED_SIZE, LED_SIZE };
	SDL_Rect green_led = 	{ LED_SIZE, 		0, LED_SIZE, LED_SIZE };
	SDL_Rect yellow_led = 	{ LED_SIZE*2, 		0, LED_SIZE, LED_SIZE };
	SDL_Rect inactive_led = { LED_SIZE*3, 		0, LED_SIZE, LED_SIZE };
	SDL_Rect dstrect = 		{ 720-LED_SIZE*4, 348-LED_SIZE, LED_SIZE, LED_SIZE };

	// LED bit values are inverse of documentation (leftmost LED is LSB)
	// Red user LED (leftmost LED) can be turned on using "syslocal(SYSL_LED, 1)" from sys/syslocal.h
	SDL_RenderCopy(r, lightbar_tex, (state.leds & 1) ? &red_led : &inactive_led, &dstrect);
	dstrect.x += LED_SIZE;
	SDL_RenderCopy(r, lightbar_tex, (state.leds & 2) ? &green_led : &inactive_led, &dstrect);
	dstrect.x += LED_SIZE;
	SDL_RenderCopy(r, lightbar_tex, (state.leds & 4) ? &yellow_led : &inactive_led, &dstrect);
	dstrect.x += LED_SIZE;
	SDL_RenderCopy(r, lightbar_tex, (state.leds & 8) ? &red_led : &inactive_led, &dstrect);
}

/**
 * @brief	Show in the title bar whether the 3B1 has the mouse, and what F10 does.
 */
static void show_mouse_state(SDL_Window *window, bool grabbed)
{
	SDL_SetWindowTitle(window, grabbed
		? "FreeBee 3B1 - Mouse: GRABBED - F10 to release"
		: "FreeBee 3B1 - Mouse: free - F10 to grab");
}

/**
 * @brief	Handle events posted by SDL.
 */
bool HandleSDLEvents(SDL_Window *window)
{
	SDL_Event event;
	static int mouse_grabbed = 0, mouse_buttons = 0;
	int dx = 0, dy = 0;

	while (SDL_PollEvent(&event))
	{
		if ((event.type == SDL_KEYDOWN) || (event.type == SDL_KEYUP)) {
			keyboard_event(&state.kbd, &event);
		}

		switch (event.type) {
			case SDL_QUIT:
				// Quit button tagged. Exit.
				return true;
			case SDL_WINDOWEVENT:
				window_dirty = true;
				if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
					keyboard_focus_lost(&state.kbd);	// no key-ups come while away
				break;
			case SDL_KEYDOWN:
				switch (event.key.keysym.sym) {
					case SDLK_F10:
						if (mouse_grabbed){
							SDL_SetRelativeMouseMode(SDL_FALSE);
							mouse_grabbed = 0;
						}else{
							SDL_SetRelativeMouseMode(SDL_TRUE);
							mouse_grabbed = 1;
						}
						show_mouse_state(window, mouse_grabbed);
						break;
					case SDLK_F11:
						if (state.fdc_disc) {
							wd2797_unload(&state.fdc_ctx);
							fclose(state.fdc_disc);
							state.fdc_disc = NULL;
							printf("Floppy image unloaded.\n");
						} else {
							load_fd();
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
			case SDL_MOUSEMOTION:
				SDL_GetRelativeMouseState(&dx, &dy);
				if (dx==0 && dy==0) break;  // sometimes SDL returns 0 for both, don't process
			case SDL_MOUSEBUTTONUP:
			case SDL_MOUSEBUTTONDOWN:
				if (mouse_grabbed){
					if (event.type == SDL_MOUSEBUTTONDOWN) {
						if (event.button.button == SDL_BUTTON_LEFT){
							mouse_buttons |= MOUSE_BUTTON_LEFT;
						}else if (event.button.button == SDL_BUTTON_MIDDLE){
							mouse_buttons |= MOUSE_BUTTON_MIDDLE;
						}else if (event.button.button == SDL_BUTTON_RIGHT){
							mouse_buttons |= MOUSE_BUTTON_RIGHT;
						}
					} else if (event.type == SDL_MOUSEBUTTONUP) {
						if (event.button.button == SDL_BUTTON_LEFT){
							mouse_buttons &= ~MOUSE_BUTTON_LEFT;
						}else if (event.button.button == SDL_BUTTON_MIDDLE){
							mouse_buttons &= ~MOUSE_BUTTON_MIDDLE;
						}else if (event.button.button == SDL_BUTTON_RIGHT){
							mouse_buttons &= ~MOUSE_BUTTON_RIGHT;
						}
					}
					mouse_event(&state.kbd, dx, dy, mouse_buttons);
					dx = 0;
					dy = 0;
				}
				break;
			default:
				break;
		}
	}

	return false;
}


/**
 * @brief	Validate the memory amounts requested.
 */

void validate_memory(int base_memory, int extended_memory)
{
	static const int base_memsizes_allowed[] = {
		512, 1024, 2048
	};
	static const int extended_memsizes_allowed[] = {
		0, 512, 1024, 1536, 2048
	};

	bool base_ok = false;
	bool extended_ok = false;

	int i;

	for (i = 0; i < NELEMS(base_memsizes_allowed); i++) {
		if (base_memory == base_memsizes_allowed[i]) {
			 base_ok = true;
			 break;
		}
	}

	for (i = 0; i < NELEMS(extended_memsizes_allowed); i++) {
		if (extended_memory == extended_memsizes_allowed[i]) {
			 extended_ok = true;
			 break;
		}
	}

	if (! base_ok) {
		fprintf(stderr, "Motherboard memory size %dK is invalid; it must be 512, 1024, or 2048.\n",
				base_memory);
		exit(EXIT_FAILURE);
	}

	if (! extended_ok) {
		fprintf(stderr, "Extension memory size %dK is invalid; it must be a multiple of 512K.\n",
				extended_memory);
		exit(EXIT_FAILURE);
	}

    printf("Memory config: %iKB On-board, %iKB Expansion\n", base_memory, extended_memory);
    if (base_memory + extended_memory < 1024)
       printf("*WARNING*: 1MB or higher RAM recommended for UNIX 3.51.\n\n");
}

/****************************
 * blessed be thy main()...
 ****************************/

int main(int argc, char *argv[])
{
	float scalex = fbc_get_double("display", "x_scale");
	float scaley = fbc_get_double("display", "y_scale");

	if (scalex <= 0 || scalex > 45 || scaley <= 0 || scaley > 45) {
		// 45 chosen as max because 45 * 720 < INT16_MAX
		fprintf(stderr, "scale factors must be greater than zero and less than or equal to 45\n");
		exit(EXIT_FAILURE);
	}

	// copyright banner
	printf("FreeBee: A perfectly cromulent AT&T 3B1 Emulator. Version %s, %s mode.\n", VER_FULLSTR, VER_BUILD_TYPE);
	printf("Copyright (C) 2010-2026 P. A. Pemberton. All rights reserved.\nLicensed under the Apache License Version 2.0.\n");
	printf("Musashi M680x0 emulator engine developed by Karl Stenerud <kstenerud@gmail.com> et al\n");
	printf("\n");

	// set up system state
	// RAM sizes come from config, default 2 Meg for each kind of memory
	int i;
	int base_memory = fbc_get_int("memory", "base_memory");
	int extended_memory = fbc_get_int("memory", "extended_memory");

	validate_memory(base_memory, extended_memory);	// exits if problem

	base_memory *= 1024;
	extended_memory *= 1024;
	if ((i = state_init(base_memory, extended_memory)) != STATE_E_OK) {
		fprintf(stderr, "ERROR: Emulator initialisation failed. Error code %d.\n", i);
		return i;
	}

	// set up musashi and reset the CPU
	m68k_init();
	m68k_set_cpu_type(M68K_CPU_TYPE_68010);
	m68k_pulse_reset();

	// Set up SDL
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) == -1) {
		fprintf(stderr, "Could not initialise SDL: %s.\n", SDL_GetError());
		exit(EXIT_FAILURE);
	}

	// Make sure SDL cleans up after itself
	atexit(SDL_Quit);

	// Set up the video display
	SDL_Window *window;
	if ((window = SDL_CreateWindow("FreeBee 3B1 Emulator", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
									(int) ceilf(720*scalex), (int) ceilf(348*scaley), 0)) == NULL) {
		fprintf(stderr, "Error creating SDL window: %s.\n", SDL_GetError());
		exit(EXIT_FAILURE);
	}
	show_mouse_state(window, false);
    // SDL default is "nearest", our default is "linear" if there's scaling
    if (scalex != 1.0 || scaley != 1.0)
	    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, fbc_get_string("display", "scale_quality"));
	SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, 0);
	SDL_RenderSetScale(renderer, scalex, scaley);
	if (!renderer){
		fprintf(stderr, "Error creating SDL renderer: %s.\n", SDL_GetError());
		exit(EXIT_FAILURE);
	}
	SDL_Texture *fbTexture = SDL_CreateTexture(renderer,
                               SDL_PIXELFORMAT_RGB888,
                               SDL_TEXTUREACCESS_STREAMING,
                               720, 348);
	if (!fbTexture){
		fprintf(stderr, "Error creating SDL FB texture: %s.\n", SDL_GetError());
		exit(EXIT_FAILURE);
	}
    SDL_Surface *screen = SDL_CreateRGBSurface(0, 720, 348, 32, 0, 0, 0, 0);
	if (!screen){
		fprintf(stderr, "Error creating SDL FB surface: %s.\n", SDL_GetError());
		exit(EXIT_FAILURE);
	}
	// Load in status LED sprites
    SDL_Surface *surf = SDL_CreateRGBSurfaceFrom((void*)lightbar.pixel_data, lightbar.width, lightbar.height,
														lightbar.bytes_per_pixel*8, lightbar.bytes_per_pixel*lightbar.width,
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
														0xFF000000, 0x00FF0000, 0x0000FF00, 0x000000FF
#else
														0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000
#endif
	);
	SDL_Texture *lightbarTexture = SDL_CreateTextureFromSurface(renderer, surf);
	SDL_FreeSurface(surf);

	printf("Set %dx%d at %d bits-per-pixel mode\n\n", (int) ceilf(720*scalex), (int) ceilf(348*scaley), screen->format->BitsPerPixel);

	// Set up the dialer's tone output (the system beep comes through it)
	dialer_init();

	// Load a disc image
	load_fd();

	load_hd();

	/***
	 * The 3B1 CPU runs at 10MHz, with DMA running at 1MHz and video refreshing at
	 * 60.821331Hz, with a 60Hz periodic interrupt.
	 */
	const uint32_t SYSTEM_CLOCK = 10e6; // Hz
	const uint32_t TIMESLOT_FREQUENCY = 100;//240;	// Hz
	const uint32_t MILLISECS_PER_TIMESLOT = 1e3 / TIMESLOT_FREQUENCY;
	const uint32_t CYCLES_PER_TIMESLOT = SYSTEM_CLOCK / TIMESLOT_FREQUENCY;
	const uint32_t CLOCKS_PER_60HZ = (SYSTEM_CLOCK / 60);
	const uint32_t NUM_CPU_TIMESLOTS = 500;
	uint32_t next_timeslot = SDL_GetTicks() + MILLISECS_PER_TIMESLOT;
	uint32_t clock_cycles = 0, cycles_run;

	// [cpu] turbo: the CPU runs that many times as fast as a real 3B1's.
	// Each time slot runs turbo times as many cycles, and the 60Hz tick
	// comes every turbo times as many cycles, so the clock stays real.
	const int TURBO_MAX = 32;
	int turbo_cfg = fbc_get_int("cpu", "turbo");
	if (turbo_cfg < 1 || turbo_cfg > TURBO_MAX) {
		fprintf(stderr, "[cpu] turbo must be 1 to %d (got %d); using 1\n", TURBO_MAX, turbo_cfg);
		turbo_cfg = 1;
	}
	const uint32_t turbo = (uint32_t)turbo_cfg;
	if (turbo > 1)
		printf("Turbo: the CPU runs %u times as fast as a real 3B1 (%u MHz)\n", turbo, turbo * 10);
	// the CPU cycles in one time slot
	const uint32_t slot_cycles = CYCLES_PER_TIMESLOT * turbo;
	bool exitEmu = false;
	uint8_t last_leds = 255;

	/*bool lastirq_fdc = false;*/
	for (;;) {
		for (i = 0; i < slot_cycles; i += cycles_run){
			// Run the CPU for however many cycles we need to. CPU core clock is
			// 10MHz, and we're running at 240Hz/timeslot. Thus: 10e6/240 or
			// 41667 cycles per timeslot.
			cycles_run = m68k_execute(CYCLES_PER_TIMESLOT / NUM_CPU_TIMESLOTS);
			clock_cycles += cycles_run;

			// Run the DMA engine
			if (state.dmaen) {
				// DMA ready to go -- so do it.
				size_t num = 0;
				while (state.dma_count < 0x4000) {
					uint16_t d = 0;

					// num tells us how many words we've copied. If this is greater than the per-timeslot DMA maximum, bail out!
					if (num > (1e6/TIMESLOT_FREQUENCY / NUM_CPU_TIMESLOTS)) break;

					// Evidently we have more words to copy. Copy them.
					if (state.dma_dev == DMA_DEV_FD){
						if (!wd2797_get_drq(&state.fdc_ctx)) {
							// Bail out, no data available. Try again later.
							break;
						}
					}else if (state.dma_dev == DMA_DEV_HD0){
						if (!wd2010_get_drq(&state.hdc_ctx)) {
							// Bail out, no data available. Try again later.
							break;
						}
					}else{
						fprintf(stderr, "ERROR: DMA attempt with no drive selected!\n");
					}
					if (!access_check_dma(state.dma_reading)) {
						break;
					}
					uint32_t newAddr;
					// Map logical address to a physical RAM address
					newAddr = mapAddr(state.dma_address, !state.dma_reading);

					if (!state.dma_reading) {
						// Data available. Get it from the FDC or HDC.
						if (state.dma_dev == DMA_DEV_FD) {
							d = wd2797_read_reg(&state.fdc_ctx, WD2797_REG_DATA);
							d <<= 8;
							d += wd2797_read_reg(&state.fdc_ctx, WD2797_REG_DATA);
						}else if (state.dma_dev == DMA_DEV_HD0) {
							d = wd2010_read_data(&state.hdc_ctx);
							d <<= 8;
							d += wd2010_read_data(&state.hdc_ctx);
						}
						if (newAddr <= 0x1FFFFF) {
							WR16(state.base_ram, newAddr, state.base_ram_size - 1, d);
						} else if (newAddr >= 0x200000) {
							WR16(state.exp_ram, newAddr - 0x200000, state.exp_ram_size - 1, d);
						}
					} else {
						// Data write to FDC or HDC.

						// Get the data from RAM
						if (newAddr <= 0x1fffff) {
							d = RD16(state.base_ram, newAddr, state.base_ram_size - 1);
						} else {
							if (newAddr <= (state.exp_ram_size + 0x200000 - 1))
								d = RD16(state.exp_ram, newAddr - 0x200000, state.exp_ram_size - 1);
							else
								d = 0xffff;
						}

						// Send the data to the FDD or HDD
						if (state.dma_dev == DMA_DEV_FD){
							wd2797_write_reg(&state.fdc_ctx, WD2797_REG_DATA, (d >> 8));
							wd2797_write_reg(&state.fdc_ctx, WD2797_REG_DATA, (d & 0xff));
						}else if (state.dma_dev == DMA_DEV_HD0){
							wd2010_write_data(&state.hdc_ctx, (d >> 8));
							wd2010_write_data(&state.hdc_ctx, (d & 0xff));
						}
					}

					// Increment DMA address
					state.dma_address+=2;
					// Increment number of words transferred
					num++; state.dma_count++;
				}

				// Turn off DMA engine if we finished this cycle
				if (state.dma_count >= 0x4000) {
					// FIXME? apparently this isn't required... or is it?
					state.dma_count = 0x3fff;
					/*state.dmaen = false;*/
				}
			}else if (wd2010_get_drq(&state.hdc_ctx)){
				wd2010_dma_miss(&state.hdc_ctx);
			}else if (wd2797_get_drq(&state.fdc_ctx)){
				wd2797_dma_miss(&state.fdc_ctx);
			}


			// Present the highest-priority pending interrupt to the CPU.
			// This is re-evaluated every slice, so a level which is still
			// pending is re-presented once a higher one has been serviced.
			//
			// Level 6 is latched in hardware and stays asserted until the
			// guest dismisses it via MCR.CLRSINT -- unlike the device
			// interrupts below, it must not be dropped just because the CPU
			// hasn't got around to taking it yet (e.g. it is sat at SPL6).
			//
			// TODO: MCR2.F_MASK (0x0400) masks the floppy interrupt
			int irq = 0;
			if (state.timer_int_latch) {
				irq = 6;
			} else if (i8274_get_irq(&state.serial_ctx)) {
				irq = 4;
			} else if (keyboard_get_irq(&state.kbd)) {
				irq = 3;
			} else if (wd2797_get_irq(&state.fdc_ctx) || wd2010_get_irq(&state.hdc_ctx)) {
				irq = 2;
			}
			m68k_set_irq(irq);

			// The idle skip. The kernel's idle loop waits for an interrupt,
			// has gone round IDLE_PASSES times with nothing to do, and
			// memory.c stopped the CPU at its top (the MCR write). With no
			// interrupt to take and no disk or floppy transfer waiting, the
			// CPU would only go round it until the next interrupt. So count
			// the rest of the time slot as run (the 60Hz clock stays
			// right), and sleep instead.
			if (state.idle_hint) {
				state.idle_hint = false;
				if (state.idle_skip && irq == 0 &&
						!wd2010_get_drq(&state.hdc_ctx) && !wd2797_get_drq(&state.fdc_ctx)) {
					uint32_t const done = i + cycles_run;
					if (done < slot_cycles)
						clock_cycles += slot_cycles - done;
					break;
				}
			}
		}
		// Is it time to run the 60Hz periodic interrupt yet?
		if (clock_cycles > CLOCKS_PER_60HZ * turbo) {
			// Refresh the screen if VRAM has been changed. Draw the whole
			// window (screen + LEDs) and present it only when something
			// changed: presenting a frame that was not drawn whole makes
			// the window flicker on macOS.
			bool vram_changed = state.vram_updated;
			if (vram_changed){
				refreshScreen(screen, renderer, fbTexture);
			}
			if (vram_changed || last_leds != state.leds || window_dirty){
				SDL_RenderClear(renderer);
				SDL_RenderCopy(renderer, fbTexture, NULL, NULL);
				refreshStatusBar(renderer, lightbarTexture);
				SDL_RenderPresent(renderer);
				last_leds = state.leds;
				window_dirty = false;
			}
			state.vram_updated = false;

			// Latch the 60Hz interrupt. If CLRSINT- is being held low the
			// clear input is asserted, so the latch can't set and this tick
			// is lost -- same as the real hardware.
			if (state.timer_clrsint) {
				state.timer_int_latch = true;
			}
			// scan the keyboard
			keyboard_scan(&state.kbd);
			// scan the serial pty for new data
			i8274_scan_incoming(&state.serial_ctx, CHAN_A);
			// decrement clock cycle counter, we've handled the intr.
			clock_cycles -= CLOCKS_PER_60HZ * turbo;
		}

		// handle SDL events -- returns true if we need to exit
		if (HandleSDLEvents(window))
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

	// Close the disc images before exiting
	wd2797_unload(&state.fdc_ctx);

	if (state.fdc_disc != NULL) {
		fclose(state.fdc_disc);
	}

	dialer_done();

	// Clean up SDL
	SDL_DestroyTexture(lightbarTexture);
	SDL_FreeSurface(screen);
	SDL_DestroyTexture(fbTexture);
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);

    	// clean up all hardware state
	state_done();

	return 0;
}
