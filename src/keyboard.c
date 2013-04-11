#include <stdbool.h>
#include "SDL.h"
#include "utils.h"
#include "keyboard.h"

// Enable/disable KBC debugging
#define kbc_debug false

/**
 * Key map -- a mapping from SDLK_xxx constants to scancodes and vice versa.
 */
struct {
	SDLKey			key;			///< SDLK_xxx key code constant
	int				extended;		///< 1 if this is an extended keycode
	unsigned char	scancode;		///< Keyboard scan code
} keymap[] = {
	{ SDLK_UP,				0,	0x01 },	// ROLL/Up		[UpArrow]
	{ SDLK_KP2,				0,	0x01 },	// ROLL/Up		[Keypad 2]
//	{ SDLK_,				1,	0x02 },	// Clear Line
//	{ SDLK_,				1,	0x03 },	// Rstrt / Ref
//	{ SDLK_,				1,	0x04 },	// Exit
	{ SDLK_KP1,				0,	0x05 },	// PREV			[Keypad 1]
//	{ SDLK_,				1,	0x06 },	// Msg
//	{ SDLK_,				1,	0x07 },	// Cancl
	{ SDLK_BACKSPACE,		0,	0x08 },	// Backspace
	{ SDLK_TAB,				0,	0x09 },	// Tab
//	{ SDLK_RETURN,			1,	0x0a },	// ENTER
	{ SDLK_DOWN,			0,	0x0b },	// ROLL/Down	[DownArrow]
	{ SDLK_KP0,				0,	0x0b },	// ROLL/Down	[Keypad 0]
	{ SDLK_KP3,				0,	0x0c },	// NEXT			[Keypad 3]
	{ SDLK_RETURN,			0,	0x0d },	// RETURN		[Return]
	{ SDLK_LEFT,			0,	0x0e },	// <--			[LeftArrow]
	{ SDLK_KP_MINUS,		0,	0x0e },	// <--			[Keypad -]
	{ SDLK_RIGHT,			0,	0x0f },	// -->			[RightArrow]
	{ SDLK_KP_PERIOD,		0,	0x0f },	// -->			[Keypad .]
//	{ SDLK_,				1,	0x10 },	// Creat
//	{ SDLK_,				1,	0x11 },	// Save
//	{ SDLK_,				1,	0x12 },	// Move
//	{ SDLK_,				1,	0x13 },	// Ops
//	{ SDLK_,				1,	0x14 },	// Copy
	{ SDLK_F1,				0,	0x15 },	// F1
	{ SDLK_F2,				0,	0x16 },	// F2
	{ SDLK_F3,				0,	0x17 },	// F3
	{ SDLK_F4,				0,	0x18 },	// F4
	{ SDLK_F5,				0,	0x19 },	// F5
	{ SDLK_F6,				0,	0x1a },	// F6
	{ SDLK_ESCAPE,			0,	0x1b },	// ESC/DEL		[Escape]
	{ SDLK_F7,				0,	0x1c },	// F7
	{ SDLK_F8,				0,	0x1d },	// F8
//	{ SDLK_,				1,	0x1e },	// Suspd
//	{ SDLK_,				1,	0x1f },	// Rsume
	{ SDLK_SPACE,			0,	0x20 },	// SPACE		[Spacebar]
//	{ SDLK_,				1,	0x21 },	// Undo
//	{ SDLK_,				1,	0x22 },	// Redo
//	{ SDLK_,				1,	0x23 },	// FIND
//	{ SDLK_,				1,	0x24 },	// RPLAC
	{ SDLK_BREAK,			0,	0x25 },	// RESET/BREAK	[Pause/Break]
//	{ SDLK_,				1,	0x26 },	// DleteChar
	{ SDLK_QUOTE,			0,	0x27 },	// ' (single-quote)
//	{ SDLK_,				1,	0x28 },	// SLCT/MARK
//	{ SDLK_,				1,	0x29 },	// INPUT/MODE
//	{ SDLK_,				1,	0x2a },	// HELP
//	Keycode 2B not used
	{ SDLK_COMMA,			0,	0x2c },	// ,			[Comma]
	{ SDLK_MINUS,			0,	0x2d },	// -			[Dash]
	{ SDLK_PERIOD,			0,	0x2e },	// .			[Period]
	{ SDLK_SLASH,			0,	0x2f },	// /			[Forward-slash]
	{ SDLK_0,				0,	0x30 },	// 0
	{ SDLK_1,				0,	0x31 },	// 1
	{ SDLK_2,				0,	0x32 },	// 2
	{ SDLK_3,				0,	0x33 },	// 3
	{ SDLK_4,				0,	0x34 },	// 4
	{ SDLK_5,				0,	0x35 },	// 5
	{ SDLK_6,				0,	0x36 },	// 6
	{ SDLK_7,				0,	0x37 },	// 7
	{ SDLK_8,				0,	0x38 },	// 8
	{ SDLK_9,				0,	0x39 },	// 9
// Keycode 3A not used
	{ SDLK_SEMICOLON,		0,	0x3b },	// ;			[Semicolon]
// Keycode 3C not used
	{ SDLK_EQUALS,			0,	0x3d },	// =			[Equals]
// Keycodes 3E, 3F, 40 not used
//	{ SDLK_,				1,	0x41 },	// CMD
//	{ SDLK_,				1,	0x42 },	// CLOSE/OPEN
	{ SDLK_KP7,				0,	0x43 },	// PRINT
	{ SDLK_KP8,				0,	0x44 },	// CLEAR/RFRSH
	{ SDLK_CAPSLOCK,		0,	0x45 },	// Caps Lock
	{ SDLK_KP9,				0,	0x46 },	// PAGE
	{ SDLK_KP4,				0,	0x47 },	// BEG
	{ SDLK_LSHIFT,			0,	0x48 },	// Left Shift
	{ SDLK_RSHIFT,			0,	0x49 },	// Right Shift
	{ SDLK_HOME,			0,	0x4a },	// Home
	{ SDLK_KP5,				0,	0x4a },	// Home			[Keypad 5]
	{ SDLK_END,				0,	0x4b },	// End
	{ SDLK_KP6,				0,	0x4b },	// End			[Keypad 6]
	{ SDLK_LCTRL,			0,	0x4c },	// Left Ctrl?	\___ not sure which is left and which is right...
	{ SDLK_RCTRL,			0,	0x4d },	// Right Ctrl?	/
// Keycodes 4E thru 5A not used
	{ SDLK_LEFTBRACKET,		0,	0x5b },	// [
	{ SDLK_BACKSLASH,		0,	0x5c },	// \ (backslash)
	{ SDLK_RIGHTBRACKET,	0,	0x5d },	// ]
// Keycodes 5E, 5F not used
	{ SDLK_BACKQUOTE,		0,	0x60 },	// `
	{ SDLK_a,				0,	0x61 },	// A
	{ SDLK_b,				0,	0x62 },	// B
	{ SDLK_c,				0,	0x63 },	// C
	{ SDLK_d,				0,	0x64 },	// D
	{ SDLK_e,				0,	0x65 },	// E
	{ SDLK_f,				0,	0x66 },	// F
	{ SDLK_g,				0,	0x67 },	// G
	{ SDLK_h,				0,	0x68 },	// H
	{ SDLK_i,				0,	0x69 },	// I
	{ SDLK_j,				0,	0x6a },	// J
	{ SDLK_k,				0,	0x6b },	// K
	{ SDLK_l,				0,	0x6c },	// L
	{ SDLK_m,				0,	0x6d },	// M
	{ SDLK_n,				0,	0x6e },	// N
	{ SDLK_o,				0,	0x6f },	// O
	{ SDLK_p,				0,	0x70 },	// P
	{ SDLK_q,				0,	0x71 },	// Q
	{ SDLK_r,				0,	0x72 },	// R
	{ SDLK_s,				0,	0x73 },	// S
	{ SDLK_t,				0,	0x74 },	// T
	{ SDLK_u,				0,	0x75 },	// U
	{ SDLK_v,				0,	0x76 },	// V
	{ SDLK_w,				0,	0x77 },	// W
	{ SDLK_x,				0,	0x78 },	// X
	{ SDLK_y,				0,	0x79 },	// Y
	{ SDLK_z,				0,	0x7a },	// Z
// Keycodes 7B, 7C, 7D not used
	{ SDLK_NUMLOCK,			0,	0x7e },	// Numlock
	{ SDLK_DELETE,			0,	0x7f }	// Dlete
};

/**
 * List of special key codes
 */
enum {
	KEY_ALL_UP				= 0x40,		///< All keys up
	KEY_LIST_END			= 0x80,		///< End of key code list
	KEY_BEGIN_MOUSE			= 0xCF,		///< Mouse data follows
	KEY_BEGIN_KEYBOARD		= 0xDF,		///< Keyboard data follows
};

/**
 * List of keyboard commands
 */
enum {
	KEY_CMD_RESET			= 0x92,		///< Reset keyboard
	KEY_CMD_CAPSLED_OFF		= 0xB1,		///< Caps Lock LED off--CHECK!
	KEY_CMD_CAPSLED_ON		= 0xB0,		///< Caps Lock LED on --CHECK!
	KEY_CMD_NUMLED_OFF		= 0xA1,		///< Num Lock LED off --CHECK!
	KEY_CMD_NUMLED_ON		= 0xA0,		///< Num Lock LED on  --CHECK!
	KEY_CMD_MOUSE_ENABLE	= 0xD0,		///< Enable mouse
	KEY_CMD_MOUSE_DISABLE	= 0xD1		///< Disable mouse
};

void keyboard_init(KEYBOARD_STATE *ks)
{
	// Set all key states to "not pressed"
	for (int i=0; i<(sizeof(ks->keystate)/sizeof(ks->keystate[0])); i++) {
		ks->keystate[i] = 0;
	}

	// Reset the R/W pointers and length
	ks->readp = ks->writep = ks->buflen = 0;

	// Clear the update flag
	ks->update_flag = false;
}

void keyboard_event(KEYBOARD_STATE *ks, SDL_Event *ev)
{
	int v = 0;
	switch (ev->type) {
		case SDL_KEYDOWN:
			// Key down (pressed)
			v = 1;
			break;
		case SDL_KEYUP:
			// Key up (released)
			v = 0;
			break;
		default:
			// Not a keyboard event
			return;
	}

	// scan the keymap
	for (int i=0; i < sizeof(keymap)/sizeof(keymap[0]); i++) {
		if (keymap[i].key == ev->key.keysym.sym) {
			// Keycode match. Is this an Extended Map key?
			if (keymap[i].extended) {
				// Yes -- need ALT set when pressing the key for this to be a match
				if (ev->key.keysym.mod & KMOD_ALT) {
					ks->keystate[keymap[i].scancode] = v;
					ks->update_flag = true;
					break;
				}
			} else {
				// Standard Map key. ALT must NOT be pressed for this to be a match
				if (!(ev->key.keysym.mod & KMOD_ALT)) {
					ks->keystate[keymap[i].scancode] = v;
					ks->update_flag = true;
					break;
				}
			}
		}
	}
}
void keyboard_scan(KEYBOARD_STATE *ks)
{
	int nkeys = 0;

	// Skip doing the scan if the keyboard hasn't changed state
	if (!ks->update_flag) return;


	// if buffer empty, do a keyboard scan
	if (ks->buflen == 0) {
		// Keyboard Data Begins Here (BEGKBD)
		//ks->buffer[ks->writep] = KEY_BEGIN_KEYBOARD;
		//ks->writep = (ks->writep + 1) % KEYBOARD_BUFFER_SIZE;
		//if (ks->buflen < KEYBOARD_BUFFER_SIZE) ks->buflen++;

		for (int i=0; i<(sizeof(ks->keystate)/sizeof(ks->keystate[0])); i++) {
			if (ks->keystate[i]) {
				LOG_IF(kbc_debug, "KBC KEY DOWN: %d\n", i);
				ks->buffer[ks->writep] = i;
				ks->writep = (ks->writep + 1) % KEYBOARD_BUFFER_SIZE;
				if (ks->buflen < KEYBOARD_BUFFER_SIZE) ks->buflen++;
				nkeys++;
			}
		}
		if (nkeys) {
			ks->buffer[ks->writep - 1] |= 0x80;
		}else{
			// If no keys down, then send All Keys Up byte
			LOG_IFS(kbc_debug, "KBC ALL KEYS UP\n");
			ks->buffer[ks->writep] = KEY_ALL_UP;
			ks->writep = (ks->writep + 1) % KEYBOARD_BUFFER_SIZE;
			if (ks->buflen < KEYBOARD_BUFFER_SIZE) ks->buflen++;
		}

		// TODO: inject "mouse data follows" chunk header and mouse movement info

	}

	// Clear the update flag
	ks->update_flag = false;
}

bool keyboard_get_irq(KEYBOARD_STATE *ks)
{
	bool irq_status = false;

	// Conditions which may cause an IRQ :-
	//   Read Data Reg has data and RxIRQ enabled
	if (ks->rxie)
		if (ks->buflen > 0) irq_status = true;

	//   Transmit Data Reg empty and TxIRQ enabled
//	if (ks->txie)

	//   DCD set and RxIRQ enabled
//

	// returns interrupt status -- i.e. is there data in the buffer?
	return irq_status;
}

uint8_t keyboard_read(KEYBOARD_STATE *ks, uint8_t addr)
{
	if ((addr & 1) == 0) {
		// Status register -- RS=0, read
		uint8_t sr = 0;
		if (ks->buflen > 0) sr |= 1;			// SR0: a new character has been received
		sr |= 2;								// SR1: Transmitter Data Register Empty
//			0 +								// SR2: Data Carrier Detect
//			0 +								// SR3: Clear To Send
//			0 +								// SR4: Framing Error
//			0 +								// SR5: Receiver Overrun
//			0 +								// SR6: Parity Error
		if (keyboard_get_irq(ks)) sr |= 0x80;	// SR7: IRQ status
		//LOG_IF(kbc_debug, "KBC DBG: sr=%02X\n", sr);
		return sr;
	} else {
		// return data, pop off the fifo
		uint8_t x = ks->buffer[ks->readp];
		ks->readp = (ks->readp + 1) % KEYBOARD_BUFFER_SIZE;
		if (ks->buflen > 0) ks->buflen--;
		//LOG_IF(kbc_debug, "\tKBC DBG: rxd=%02X\n", x);
		return x;
	}
}

void keyboard_write(KEYBOARD_STATE *ks, uint8_t addr, uint8_t val)
{
	if ((addr & 1) == 0) {
		// write to control register
		// transmit intr enabled when CR6,5 = 01
		// receive intr enabled when CR7 = 1

		// CR0,1 = divider registers. When =11, do a software reset
		if ((val & 3) == 3) {
			ks->readp = ks->writep = ks->buflen = 0;
		}

		// Ignore CR2,3,4 (word length)...

		// CR5,6 = Transmit Mode
		ks->txie = (val & 0x60)==0x20;

		// CR7 = Receive Interrupt Enable
		ks->rxie = (val & 0x80)==0x80;
	} else {
		// Write command to KBC -- TODO!
		if (val == KEY_CMD_RESET) {
			LOG_IFS(kbc_debug, "KBC: KEYBOARD RESET!\n");
			ks->readp = ks->writep = ks->buflen = 0;
		} else {
			LOG("KBC TODO: write keyboard data 0x%02X\n", val);
		}
	}
}

