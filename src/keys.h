/*
 * keys - typing on the 3B1 keyboard from software (control socket, MCP).
 *
 * A chord is a set of 3B1 scan codes pressed together. Chords wait in a
 * queue; keys_tick(), called at the 60 Hz tick, presses and releases them.
 * SHIFT and CTRL go down one tick before the other keys. The queue may be
 * filled from any thread.
 */
#ifndef _KEYS_H
#define _KEYS_H

#include <stdint.h>
#include <stdbool.h>

#define KEYS_MAXCHORD 6

typedef struct {
	uint8_t code[KEYS_MAXCHORD];
	int n;
	uint8_t hold;	///< ticks the keys stay down (0: default 2)
	uint8_t gap;	///< ticks after the release (0: default 1)
} key_chord;

enum { KEY_SHIFT = 0x48, KEY_CTRL = 0x4c, KEY_RETURN = 0x0d, KEY_UP = 0x01, KEY_DOWN = 0x0b };

/// Push a chord. Returns its sequence number, or 0 when the queue is full.
uint64_t keys_push(const key_chord *c);
/// Sequence number of the last chord that was pressed and released.
uint64_t keys_done(void);
/// Chords queued or being pressed.
int keys_pending(void);
/// Drop the queue and release every key.
void keys_clear(void);
/// Called at the 60 Hz tick.
void keys_tick(void);

/// Add one key by name ("F1", "ctrl", "Return", "a", "A", "%") to a chord.
bool keys_add_name(key_chord *c, const char *name);
/// Parse "ctrl-d", "shift+F1", "CTRL-SHIFT-x" into a chord.
bool keys_parse_combo(key_chord *c, const char *spec);
/// The chord that types one character: printable ASCII, \n \r (Return),
/// \t, ESC, \b, DEL and control characters (CTRL + letter).
bool keys_from_char(key_chord *c, int ch);
/// A comma separated list of the key names, for messages.
const char *keys_name_list(void);

#endif
