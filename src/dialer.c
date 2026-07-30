#include <math.h>
#include <string.h>

#include "SDL.h"

#include "dialer.h"
#include "utils.h"

#define SAMPLE_RATE	44100
#define AMPLITUDE	9000		///< per tone; two sum, so keep clear of INT16_MAX

/**
 * DTMF frequency pairs, indexed by the low nibble of the control word.
 *
 * uts/kern/sys/dialer.h gives the digit codes rather than the frequencies:
 *		kDIGIT1 0x0  kDIGIT2 0x8  kDIGIT3 0x4  kDIGIT4 0x1  kDIGIT5 0x9
 *		kDIGIT6 0x5  kDIGIT7 0x2  kDIGIT8 0xA  kDIGIT9 0x6  kDIGIT0 0xB
 *		kDIGAST 0x3  kDIGPND 0x7
 * Lining those up against the standard DTMF keypad shows the nibble is just a
 * row/column pair: bits 1..0 select the low (row) frequency and bits 3..2 the
 * high (column) frequency.
 */
static const int row_hz[4] = { 697, 770, 852, 941 };
static const int col_hz[4] = { 1209, 1477, 1336, 1336 };	///< col 3 unused by the keypad

static SDL_AudioDeviceID audio_dev = 0;

/// Tone state. Written by the emulation thread under the audio device lock,
/// read by the SDL audio callback.
static struct {
	bool	active;
	double	phase_lo, phase_hi;		///< carried across buffers to avoid clicks
	double	step_lo, step_hi;		///< radians per sample
} tone;

static void dialer_callback(void *userdata, Uint8 *stream, int len)
{
	int16_t *buf = (int16_t *)stream;
	int samples = len / sizeof(int16_t);

	(void)userdata;

	if (!tone.active) {
		memset(stream, 0, len);
		// Reset the phase so the next tone starts cleanly from zero
		tone.phase_lo = tone.phase_hi = 0.0;
		return;
	}

	for (int i = 0; i < samples; i++) {
		buf[i] = (int16_t)(AMPLITUDE * sin(tone.phase_lo))
		       + (int16_t)(AMPLITUDE * sin(tone.phase_hi));
		tone.phase_lo += tone.step_lo;
		tone.phase_hi += tone.step_hi;
		if (tone.phase_lo >= 2.0*M_PI) tone.phase_lo -= 2.0*M_PI;
		if (tone.phase_hi >= 2.0*M_PI) tone.phase_hi -= 2.0*M_PI;
	}
}

void dialer_init(void)
{
	SDL_AudioSpec want, have;

	memset(&tone, 0, sizeof(tone));

	if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
		fprintf(stderr, "NOTE: no audio (%s); dialer tones and the system beep will be silent.\n",
				SDL_GetError());
		return;
	}

	memset(&want, 0, sizeof(want));
	want.freq     = SAMPLE_RATE;
	want.format   = AUDIO_S16SYS;
	want.channels = 1;
	want.samples  = 512;			///< ~12ms, short enough to not smear a beep
	want.callback = dialer_callback;

	audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (audio_dev == 0) {
		fprintf(stderr, "NOTE: could not open audio device (%s); dialer tones and the system beep will be silent.\n",
				SDL_GetError());
		return;
	}

	SDL_PauseAudioDevice(audio_dev, 0);
}

void dialer_done(void)
{
	if (audio_dev != 0) {
		SDL_CloseAudioDevice(audio_dev);
		audio_dev = 0;
	}
}

void dialer_write(uint16_t ctrl)
{
	// The low byte drives the tone generator: low nibble picks the DTMF pair,
	// upper nibble is volume/ringer mode. A zero low byte is silence -- that's
	// how the kernel stops a tone (beepoff() writes 0, and soundDTMF() writes
	// kCallProg, whose low byte is zero).
	uint8_t tonebyte = ctrl & 0xFF;
	bool on = (tonebyte != 0);

	// The high byte selects the output path (speaker, handset, line, call
	// progress). We don't model the routing, so any tone the chip generates is
	// audible -- slightly over-eager for tone-to-line-only, and correct for the
	// cases that matter: the system beep and DTMF with the speaker enabled.

	if (audio_dev == 0)
		return;

	SDL_LockAudioDevice(audio_dev);
	if (on) {
		int lo = row_hz[tonebyte & 0x03];
		int hi = col_hz[(tonebyte >> 2) & 0x03];
		if (!tone.active) {
			LOG("dialer tone on: %d + %d Hz (control word %04X)", lo, hi, ctrl);
		}
		tone.step_lo = 2.0*M_PI * lo / SAMPLE_RATE;
		tone.step_hi = 2.0*M_PI * hi / SAMPLE_RATE;
		tone.active = true;
	} else {
		if (tone.active) {
			LOG("dialer tone off (control word %04X)", ctrl);
		}
		tone.active = false;
	}
	SDL_UnlockAudioDevice(audio_dev);
}
