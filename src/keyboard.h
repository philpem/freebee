#ifndef _KEYBOARD_H
#define _KEYBOARD_H

#include "SDL.h"

/// Keyboard buffer size in bytes
#define KEYBOARD_BUFFER_SIZE 256

typedef struct {
	/// Key states
	int keystate[0x80];

	/// Keyboard buffer
	uint8_t buffer[KEYBOARD_BUFFER_SIZE];

	/// Read pointer
	size_t readp;

	/// Write pointer
	size_t writep;

	/// Number of bytes in keyboard buffer
	size_t buflen;

	/// Transmit Interrupt Enable
	bool txie;

	/// Receive Interrupt Enable
	bool rxie;

	/// "Keyboard State Changed" flag
	bool update_flag;
} KEYBOARD_STATE;

/**
 * Initialise a keyboard state block.
 *
 * Call this once when the keyboard is added to the emulation.
 */
void keyboard_init(KEYBOARD_STATE *ks);

/**
 * SDL_Event delegation routine.
 *
 * Call this when an SDL keyup or keydown event is received.
 */
void keyboard_event(KEYBOARD_STATE *ks, SDL_Event *ev);

/**
 * Keyboard scan routine.
 *
 * Call this periodically to scan the keyboard. 60 times/sec should be fine.
 */
void keyboard_scan(KEYBOARD_STATE *ks);

bool keyboard_get_irq(KEYBOARD_STATE *ks);
uint8_t keyboard_read(KEYBOARD_STATE *ks, uint8_t addr);
void keyboard_write(KEYBOARD_STATE *ks, uint8_t addr, uint8_t val);

#endif
