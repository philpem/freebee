#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "SDL.h"

#include "dialer.h"
#include "fbconfig.h"
#include "utils.h"

#define SAMPLE_RATE	44100

/// Peak amplitude at volume 100. Two tones sum, so half of full scale each.
#define MAX_TONE_AMPLITUDE	(32767 / 2)

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
static int tone_amplitude = 0;		///< per-tone amplitude, from the volume setting

/// Decoded dialer state, for dialer_get_state().
static DIALER_STATE dialer;

/// Audio generator state. Written under the audio device lock, read by the SDL
/// audio callback.
static struct {
	bool	active;
	int		amplitude;
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
		buf[i] = (int16_t)(tone.amplitude * sin(tone.phase_lo))
		       + (int16_t)(tone.amplitude * sin(tone.phase_hi));
		tone.phase_lo += tone.step_lo;
		tone.phase_hi += tone.step_hi;
		if (tone.phase_lo >= 2.0*M_PI) tone.phase_lo -= 2.0*M_PI;
		if (tone.phase_hi >= 2.0*M_PI) tone.phase_hi -= 2.0*M_PI;
	}
}

void dialer_init(void)
{
	SDL_AudioSpec want, have;
	int volume;

	memset(&dialer, 0, sizeof(dialer));
	memset(&tone, 0, sizeof(tone));

	volume = fbc_get_int("beeper", "volume");
	if (volume < 0 || volume > 100) {
		fprintf(stderr, "beeper volume must be between 0 and 100 (got %d)\n", volume);
		exit(EXIT_FAILURE);
	}
	if (volume == 0) {
		// Muted -- don't bother bringing up the audio subsystem at all
		return;
	}
	tone_amplitude = (MAX_TONE_AMPLITUDE * volume) / 100;

	if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
		fprintf(stderr, "NOTE: no audio (%s); dialer tones and the system beep will be silent.\n",
				SDL_GetError());
		return;
	}

	memset(&want, 0, sizeof(want));
	want.freq     = SAMPLE_RATE;
	want.format   = AUDIO_S16SYS;
	want.channels = 1;
	want.samples  = 512;			// ~12ms, short enough to not smear a beep
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
	uint8_t tonebyte = ctrl & DIALER_TONE_MASK;
	DIALER_STATE prev = dialer;
	bool audible;

	dialer.active        = (tonebyte != 0);
	dialer.level         = tonebyte & DIALER_TONE_LEVEL;
	// TODO: dialer.h names soft/normal/loud levels for both touch tone and
	// ringing, but the beep uses 0x90, which is none of them -- so the level
	// field isn't fully understood and doesn't scale the output yet.
	dialer.f_low         = row_hz[tonebyte & 0x03];
	dialer.f_high        = col_hz[(tonebyte >> 2) & 0x03];
	dialer.to_speaker    = (ctrl & DIALER_SPEAKER)   != 0;
	dialer.to_line       = (ctrl & DIALER_TO_LINE)   != 0;
	dialer.to_handset    = (ctrl & DIALER_HANDSET)   != 0;
	dialer.call_progress = (ctrl & DIALER_CALL_PROG) != 0;
	dialer.open_circuit  = (ctrl & DIALER_OPEN_CCT)  != 0;

	// A tone routed to the line is a digit being dialled. Nothing emulates the
	// phone line yet, so just note it -- a future telephony or modem
	// implementation can pick this up from dialer_get_state().
	if (dialer.active && dialer.to_line && !(prev.active && prev.to_line)) {
		LOG("dialer: %d + %d Hz to line%s (control word %04X)",
				dialer.f_low, dialer.f_high,
				dialer.to_speaker ? " and speaker" : "", ctrl);
	}

	// Only the speaker path is audible to the user. The handset earpiece is a
	// separate output which we don't emulate.
	audible = dialer.active && dialer.to_speaker;

	if (audio_dev == 0)
		return;

	SDL_LockAudioDevice(audio_dev);
	if (audible) {
		if (!tone.active) {
			LOG("dialer tone on: %d + %d Hz (control word %04X)",
					dialer.f_low, dialer.f_high, ctrl);
		}
		tone.step_lo = 2.0*M_PI * dialer.f_low / SAMPLE_RATE;
		tone.step_hi = 2.0*M_PI * dialer.f_high / SAMPLE_RATE;
		tone.amplitude = tone_amplitude;
		tone.active = true;
	} else {
		if (tone.active) {
			LOG("dialer tone off (control word %04X)", ctrl);
		}
		tone.active = false;
	}
	SDL_UnlockAudioDevice(audio_dev);
}

const DIALER_STATE *dialer_get_state(void)
{
	return &dialer;
}
