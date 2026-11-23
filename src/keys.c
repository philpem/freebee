/*
 * keys - typing on the 3B1 keyboard from software (see keys.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#ifndef _WIN32
#include <pthread.h>
#endif

#include "state.h"
#include "keys.h"

#ifndef _WIN32
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()		pthread_mutex_lock(&lock)
#define UNLOCK()	pthread_mutex_unlock(&lock)
#else
#define LOCK()
#define UNLOCK()
#endif

#define QSIZE 65536
static key_chord queue[QSIZE];
static int q_head, q_tail, q_count;	// pop at head, push at tail
static uint64_t seq_pushed, seq_done;

static key_chord cur;
static int phase;	// 0 idle, 3 modifiers down, 1 keys down, 2 keys up (gap)
static int counter;

static bool is_modifier(uint8_t code)
{
	return code == 0x48 || code == 0x49 || code == 0x4c || code == 0x4d;
}

uint64_t keys_push(const key_chord *c)
{
	uint64_t s = 0;
	LOCK();
	if (q_count < QSIZE && c->n > 0) {
		queue[q_tail] = *c;
		q_tail = (q_tail + 1) % QSIZE;
		q_count++;
		s = ++seq_pushed;
	}
	UNLOCK();
	return s;
}

uint64_t keys_done(void)
{
	LOCK();
	uint64_t s = seq_done;
	UNLOCK();
	return s;
}

int keys_pending(void)
{
	LOCK();
	int n = q_count + (phase ? 1 : 0);
	UNLOCK();
	return n;
}

static void set_keys(const key_chord *c, int down, bool mods_only)
{
	for (int i = 0; i < c->n; i++) {
		if (mods_only && !is_modifier(c->code[i]))
			continue;
		state.kbd.keystate[c->code[i] & 0x7f] = down;
	}
	state.kbd.update_flag = true;
}

void keys_clear(void)
{
	LOCK();
	q_head = q_tail = q_count = 0;
	if (phase) {
		set_keys(&cur, 0, false);
		phase = 0;
	}
	seq_done = seq_pushed;
	UNLOCK();
}

void keys_tick(void)
{
	LOCK();
	if (phase == 3) {			// modifiers are down: now the keys
		if (--counter <= 0) {
			set_keys(&cur, 1, false);
			phase = 1;
			counter = cur.hold ? cur.hold : 2;
		}
	} else if (phase == 1) {	// keys are down: release them
		if (--counter <= 0) {
			set_keys(&cur, 0, false);
			phase = 2;
			counter = cur.gap ? cur.gap : 1;
		}
	} else if (phase == 2) {	// the gap after a chord
		if (--counter <= 0) {
			phase = 0;
			seq_done++;
		}
	}
	if (phase == 0 && q_count > 0) {
		cur = queue[q_head];
		q_head = (q_head + 1) % QSIZE;
		q_count--;
		bool mods = false;
		for (int i = 0; i < cur.n; i++)
			mods |= is_modifier(cur.code[i]);
		// SHIFT and CTRL go down one tick early: the keyboard sends the
		// keys that are down in scan code order, so a letter (lower code)
		// would reach the kernel before its SHIFT (0x48) and come out plain
		if (mods) {
			set_keys(&cur, 1, true);
			phase = 3;
			counter = 1;
		} else {
			set_keys(&cur, 1, false);
			phase = 1;
			counter = cur.hold ? cur.hold : 2;
		}
	}
	UNLOCK();
}

/* ---- names ---- */

static const struct { const char *name; uint8_t code; } key_names[] = {
	{ "up", 0x01 }, { "clearline", 0x02 }, { "rstrt", 0x03 }, { "ref", 0x03 },
	{ "exit", 0x04 }, { "prev", 0x05 }, { "msg", 0x06 },
	{ "cancl", 0x07 }, { "cancel", 0x07 },
	{ "backspace", 0x08 }, { "bksp", 0x08 }, { "bs", 0x08 },
	{ "tab", 0x09 }, { "enter", 0x0a }, { "down", 0x0b }, { "next", 0x0c },
	{ "return", 0x0d }, { "ret", 0x0d }, { "left", 0x0e }, { "right", 0x0f },
	{ "creat", 0x10 }, { "create", 0x10 }, { "save", 0x11 }, { "move", 0x12 },
	{ "ops", 0x13 }, { "copy", 0x14 },
	{ "f1", 0x15 }, { "f2", 0x16 }, { "f3", 0x17 }, { "f4", 0x18 },
	{ "f5", 0x19 }, { "f6", 0x1a }, { "esc", 0x1b }, { "escape", 0x1b },
	{ "f7", 0x1c }, { "f8", 0x1d },
	{ "suspd", 0x1e }, { "suspend", 0x1e }, { "rsume", 0x1f }, { "resume", 0x1f },
	{ "space", 0x20 }, { "spc", 0x20 }, { "undo", 0x21 }, { "redo", 0x22 },
	{ "find", 0x23 }, { "rplac", 0x24 }, { "replace", 0x24 },
	{ "break", 0x25 }, { "reset", 0x25 },
	{ "dletechar", 0x26 }, { "delchar", 0x26 },
	{ "apostrophe", 0x27 }, { "quote", 0x27 },
	{ "slct", 0x28 }, { "mark", 0x28 }, { "input", 0x29 }, { "mode", 0x29 },
	{ "help", 0x2a }, { "comma", 0x2c }, { "minus", 0x2d },
	{ "period", 0x2e }, { "dot", 0x2e }, { "slash", 0x2f },
	{ "semicolon", 0x3b }, { "equal", 0x3d }, { "equals", 0x3d },
	{ "cmd", 0x41 }, { "close", 0x42 }, { "open", 0x42 }, { "print", 0x43 },
	{ "clear", 0x44 }, { "rfrsh", 0x44 }, { "refresh", 0x44 },
	{ "caps", 0x45 }, { "capslock", 0x45 }, { "page", 0x46 }, { "beg", 0x47 },
	{ "shift", 0x48 }, { "lshift", 0x48 }, { "shift_l", 0x48 },
	{ "rshift", 0x49 }, { "shift_r", 0x49 },
	{ "home", 0x4a }, { "end", 0x4b },
	{ "ctrl", 0x4c }, { "lctrl", 0x4c }, { "ctrl_l", 0x4c },
	{ "rctrl", 0x4d }, { "ctrl_r", 0x4d },
	{ "bracketleft", 0x5b }, { "backslash", 0x5c }, { "bracketright", 0x5d },
	{ "grave", 0x60 }, { "backquote", 0x60 },
	{ "numlock", 0x7e }, { "del", 0x7f }, { "dlete", 0x7f }, { "delete", 0x7f },
	{ NULL, 0 }
};

const char *keys_name_list(void)
{
	return "return (ret), enter (the 3B1 Enter key, not Return), tab, "
		"backspace (bksp), esc, del, space (spc), up, down, left, right, "
		"home, end, beg, page, next, prev, f1-f8, exit, cancl (cancel), "
		"cmd, help, msg, undo, redo, find, rplac, slct (mark), input, "
		"creat, save, move, ops, copy, close (open), print, clear "
		"(refresh), clearline, rstrt, suspd, rsume, break (reset), "
		"dletechar, caps, numlock, shift, rshift, ctrl, rctrl, a-z, 0-9 "
		"and punctuation (minus, equal, comma, period, slash, semicolon, "
		"apostrophe, grave, backslash, bracketleft, bracketright)";
}

// ASCII 0x20..0x7e -> scancode, bit 7 set = needs SHIFT
static uint8_t ascii_code(int c)
{
	static const char *shifted = "!@#$%^&*()_+{}|:\"<>?~";
	static const char *plain   = "1234567890-=[]\\;',./`";
	if (c >= 'a' && c <= 'z') return c;
	if (c >= 'A' && c <= 'Z') return 0x80 | (c - 'A' + 'a');
	if (c >= '0' && c <= '9') return c;
	if (c == ' ') return 0x20;
	if (c == 0)
		return 0;
	const char *p = strchr(plain, c);
	if (p) return c;
	p = strchr(shifted, c);
	if (p) return 0x80 | plain[p - shifted];
	return 0;
}

static bool add_code(key_chord *c, uint8_t code)
{
	for (int i = 0; i < c->n; i++)
		if (c->code[i] == code)
			return true;
	if (c->n >= KEYS_MAXCHORD)
		return false;
	c->code[c->n++] = code;
	return true;
}

bool keys_add_name(key_chord *c, const char *name)
{
	for (int i = 0; key_names[i].name; i++)
		if (strcasecmp(key_names[i].name, name) == 0)
			return add_code(c, key_names[i].code);
	if (strlen(name) == 1) {
		uint8_t k = ascii_code((unsigned char)name[0]);
		if (!k)
			return false;
		if ((k & 0x80) && !add_code(c, KEY_SHIFT))
			return false;
		return add_code(c, k & 0x7f);
	}
	return false;
}

bool keys_parse_combo(key_chord *c, const char *spec)
{
	char buf[64];
	size_t len = strlen(spec);
	if (len == 0 || len >= sizeof(buf))
		return false;
	memcpy(buf, spec, len + 1);
	// a lone "-" or "+" is that key
	if (len == 1)
		return keys_add_name(c, buf);
	char *p = buf;
	for (;;) {
		char *sep = strpbrk(p, "-+");
		// "ctrl--" and "ctrl-+": the last part is the key itself
		if (sep == p && sep[1] == 0)
			sep = NULL;
		if (sep)
			*sep = 0;
		if (!keys_add_name(c, p))
			return false;
		if (!sep)
			break;
		p = sep + 1;
		if (*p == 0)
			return false;
	}
	return true;
}

bool keys_from_char(key_chord *c, int ch)
{
	memset(c, 0, sizeof(*c));
	switch (ch) {
	case '\n': case '\r': return add_code(c, KEY_RETURN);
	case '\t': return add_code(c, 0x09);
	case 0x1b: return add_code(c, 0x1b);
	case '\b': return add_code(c, 0x08);
	case 0x7f: return add_code(c, 0x7f);
	}
	if (ch >= 1 && ch <= 26) {
		add_code(c, KEY_CTRL);
		return add_code(c, 'a' + ch - 1);
	}
	uint8_t k = ascii_code(ch);
	if (!k)
		return false;
	if (k & 0x80)
		add_code(c, KEY_SHIFT);
	return add_code(c, k & 0x7f);
}
