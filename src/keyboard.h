#ifndef _KEYBOARD_H
#define _KEYBOARD_H

#define KEYBOARD_BUFFER_SIZE 0x100

/**
 * Keyboard controller state
 */
typedef struct {
	unsigned char	keybuf[KEYBOARD_BUFFER_SIZE];		///< Keyboard data buffer
	size_t			readptr;							///< Keyboard buffer read pointer
	size_t			writeptr;							///< Keyboard buffer write pointer
	size_t			buflen;								///< Keyboard buffer fill level (buffer length)
	int				keystate[0x80];						///< List of key up/down states
} KEYBOARD_STATE;

/**
 * Initialise the keyboard.
 *
 * Initialises a keyboard state block in preparation for keyboard events.
 *
 * @param	state	Keyboard state block
 */
void keyboard_init(KEYBOARD_STATE *state);

/**
 * Issue a keyboard event.
 *
 * Call this function when SDL issues a keyboard event in the event loop.
 */
void keyboard_event(KEYBOARD_STATE *state, SDL_Event *ev);

#endif
