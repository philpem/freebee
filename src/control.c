/*
 * control - a command socket for scripting FreeBee.
 *
 * Turn it on with
 *	[control]
 *		socket = "freebee.sock"
 * in .freebee.toml, or with the environment variable FREEBEE_CONTROL=path
 * (the variable wins). Connect with e.g. "nc -U freebee.sock". One command
 * per line; every command answers one line, "ok ..." or "err ...".
 *
 *   type TEXT          type TEXT on the 3B1 keyboard. Escapes: \n or \r
 *                      RETURN, \t TAB, \e ESC, \b BACKSPACE, \\ \, \{ {.
 *                      {NAME} is a special key, {CTRL-D}, {SHIFT-F1} and
 *                      {CTRL-SHIFT-X} add modifiers. Names: see key_names.
 *   key NAME [NAME..]  press keys together, e.g. "key CTRL d", "key EXIT"
 *   floppy PATH        insert a floppy image (the old one comes out first)
 *   eject              take the floppy out
 *   hd2 PATH           change the second hard disk's image (the guest must
 *                      not be using it: nothing mounted, no cpio running)
 *   screenshot PATH    save the screen as a 720x348 1-bit PNG
 *   mouse DX DY [B]    move the mouse, B = buttons (4 left, 2 mid, 1 right)
 *   speed max|real     run as fast as the host can, or at 10 MHz
 *   status             floppy, speed, keys still queued, LEDs
 *   quit               leave FreeBee (like Alt-F12)
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>

#include "state.h"
#include "fbconfig.h"
#include "keyboard.h"
#include "control.h"

bool control_speed_max = false;
bool control_quit = false;

/* ---- key queue ---- */

#define MAXCHORD 6
struct chord {
	uint8_t code[MAXCHORD];
	int n;
};

#define QSIZE 65536
static struct chord queue[QSIZE];
static int q_head, q_tail;		// pop at head, push at tail
static int q_count;

static struct chord cur;
static int phase;			// 0 idle, 3 modifiers down, 1 keys down, 2 keys up (gap)
static int counter;

// SHIFT and CTRL go down one tick before the other keys: the keyboard sends
// the keys that are down in scan code order, so a letter or digit (lower
// code) would reach the kernel before its SHIFT (0x48) and come out plain.
static bool is_modifier(uint8_t code)
{
	return code == 0x48 || code == 0x49 || code == 0x4c || code == 0x4d;
}

static bool keys_set_mods(const struct chord *c)
{
	bool any = false;
	for (int i = 0; i < c->n; i++) {
		if (is_modifier(c->code[i])) {
			state.kbd.keystate[c->code[i]] = 1;
			any = true;
		}
	}
	if (any)
		state.kbd.update_flag = true;
	return any;
}

static bool q_push(const struct chord *c)
{
	if (q_count >= QSIZE)
		return false;
	queue[q_tail] = *c;
	q_tail = (q_tail + 1) % QSIZE;
	q_count++;
	return true;
}

static void keys_set(const struct chord *c, int down)
{
	for (int i = 0; i < c->n; i++)
		state.kbd.keystate[c->code[i] & 0x7f] = down;
	state.kbd.update_flag = true;
}

static void key_machine(void)
{
	if (phase == 3) {
		if (--counter <= 0) {
			keys_set(&cur, 1);
			phase = 1;
			counter = 2;
		}
		return;
	}
	if (phase == 1) {
		if (--counter <= 0) {
			keys_set(&cur, 0);
			phase = 2;
			counter = 1;
		}
		return;
	}
	if (phase == 2) {
		if (--counter > 0)
			return;
		phase = 0;
	}
	if (phase == 0 && q_count > 0) {
		cur = queue[q_head];
		q_head = (q_head + 1) % QSIZE;
		q_count--;
		if (keys_set_mods(&cur)) {
			phase = 3;
			counter = 1;
		} else {
			keys_set(&cur, 1);
			phase = 1;
			counter = 2;
		}
	}
}

/* ---- key names and ASCII ---- */

enum { K_SHIFT = 0x48, K_CTRL = 0x4c, K_RETURN = 0x0d };

static const struct { const char *name; uint8_t code; } key_names[] = {
	{ "UP", 0x01 }, { "CLEARLINE", 0x02 }, { "RSTRT", 0x03 }, { "REF", 0x03 },
	{ "EXIT", 0x04 }, { "PREV", 0x05 }, { "MSG", 0x06 },
	{ "CANCL", 0x07 }, { "CANCEL", 0x07 }, { "BACKSPACE", 0x08 }, { "BS", 0x08 },
	{ "TAB", 0x09 }, { "ENTER", 0x0a }, { "DOWN", 0x0b }, { "NEXT", 0x0c },
	{ "RETURN", 0x0d }, { "RET", 0x0d }, { "LEFT", 0x0e }, { "RIGHT", 0x0f },
	{ "CREAT", 0x10 }, { "CREATE", 0x10 }, { "SAVE", 0x11 }, { "MOVE", 0x12 },
	{ "OPS", 0x13 }, { "COPY", 0x14 },
	{ "F1", 0x15 }, { "F2", 0x16 }, { "F3", 0x17 }, { "F4", 0x18 },
	{ "F5", 0x19 }, { "F6", 0x1a }, { "ESC", 0x1b }, { "F7", 0x1c }, { "F8", 0x1d },
	{ "SUSPD", 0x1e }, { "SUSPEND", 0x1e }, { "RSUME", 0x1f }, { "RESUME", 0x1f },
	{ "SPACE", 0x20 }, { "UNDO", 0x21 }, { "REDO", 0x22 }, { "FIND", 0x23 },
	{ "RPLAC", 0x24 }, { "REPLACE", 0x24 }, { "BREAK", 0x25 }, { "RESET", 0x25 },
	{ "DLETECHAR", 0x26 }, { "DELCHAR", 0x26 }, { "SLCT", 0x28 }, { "MARK", 0x28 },
	{ "INPUT", 0x29 }, { "MODE", 0x29 }, { "HELP", 0x2a }, { "CMD", 0x41 },
	{ "CLOSE", 0x42 }, { "OPEN", 0x42 }, { "PRINT", 0x43 }, { "CLEAR", 0x44 },
	{ "RFRSH", 0x44 }, { "REFRESH", 0x44 }, { "CAPS", 0x45 }, { "PAGE", 0x46 },
	{ "BEG", 0x47 }, { "SHIFT", 0x48 }, { "LSHIFT", 0x48 }, { "RSHIFT", 0x49 },
	{ "HOME", 0x4a }, { "END", 0x4b }, { "CTRL", 0x4c }, { "LCTRL", 0x4c },
	{ "RCTRL", 0x4d }, { "NUMLOCK", 0x7e }, { "DEL", 0x7f }, { "DLETE", 0x7f },
	{ "DELETE", 0x7f },
	{ NULL, 0 }
};

// ASCII 0x20..0x7e -> scancode, bit 7 set = needs SHIFT
static uint8_t ascii_code(int c)
{
	static const char *shifted = "!@#$%^&*()_+{}|:\"<>?~";
	static const char *plain   = "1234567890-=[]\\;',./`";
	if (c >= 'a' && c <= 'z') return c;
	if (c >= 'A' && c <= 'Z') return 0x80 | (c - 'A' + 'a');
	if (c >= '0' && c <= '9') return c;
	if (c == ' ') return 0x20;
	const char *p = strchr(plain, c);
	if (p && c) return c;
	p = strchr(shifted, c);
	if (p && c) return 0x80 | plain[p - shifted];
	return 0;
}

// one name like "F1", "CTRL", "d", "A": adds to chord. false if unknown.
static bool chord_add_name(struct chord *ch, const char *name)
{
	if (ch->n >= MAXCHORD)
		return false;
	for (int i = 0; key_names[i].name; i++) {
		if (strcasecmp(key_names[i].name, name) == 0) {
			ch->code[ch->n++] = key_names[i].code;
			return true;
		}
	}
	if (strlen(name) == 1) {
		uint8_t c = ascii_code((unsigned char)name[0]);
		if (!c)
			return false;
		if ((c & 0x80) && ch->n < MAXCHORD)
			ch->code[ch->n++] = K_SHIFT;
		if (ch->n >= MAXCHORD)
			return false;
		ch->code[ch->n++] = c & 0x7f;
		return true;
	}
	return false;
}

static bool push_ascii(int c)
{
	struct chord ch = { {0}, 0 };
	switch (c) {
	case '\n': case '\r': ch.code[ch.n++] = K_RETURN; return q_push(&ch);
	case '\t': ch.code[ch.n++] = 0x09; return q_push(&ch);
	case 0x1b: ch.code[ch.n++] = 0x1b; return q_push(&ch);
	case '\b': ch.code[ch.n++] = 0x08; return q_push(&ch);
	case 0x7f: ch.code[ch.n++] = 0x7f; return q_push(&ch);
	}
	if (c >= 1 && c <= 26) {
		ch.code[ch.n++] = K_CTRL;
		ch.code[ch.n++] = 'a' + c - 1;
		return q_push(&ch);
	}
	uint8_t k = ascii_code(c);
	if (!k)
		return false;
	if (k & 0x80)
		ch.code[ch.n++] = K_SHIFT;
	ch.code[ch.n++] = k & 0x7f;
	return q_push(&ch);
}

// {NAME} or {CTRL-SHIFT-NAME}; s points after '{', len excludes '}'
static bool push_braced(const char *s, int len)
{
	char buf[64];
	struct chord ch = { {0}, 0 };
	if (len <= 0 || len >= (int)sizeof(buf))
		return false;
	memcpy(buf, s, len);
	buf[len] = 0;
	char *p = buf;
	for (;;) {
		char *dash = strchr(p, '-');
		// a lone "-" as the last part is the minus key
		if (dash && dash[1] == 0 && dash == p)
			dash = NULL;
		if (dash)
			*dash = 0;
		if (!chord_add_name(&ch, p))
			return false;
		if (!dash)
			break;
		p = dash + 1;
	}
	// CTRL with a shifted letter: CTRL-A means CTRL-a
	return q_push(&ch);
}

static const char *do_type(const char *s)
{
	while (*s) {
		int c = (unsigned char)*s++;
		if (c == '\\' && *s) {
			int e = (unsigned char)*s++;
			switch (e) {
			case 'n': c = '\n'; break;
			case 'r': c = '\r'; break;
			case 't': c = '\t'; break;
			case 'e': c = 0x1b; break;
			case 'b': c = '\b'; break;
			default: c = e; break;	// \\ \{ \} ...
			}
			if (!push_ascii(c))
				return "err cannot type character";
			continue;
		}
		if (c == '{') {
			const char *end = strchr(s, '}');
			if (!end)
				return "err missing }";
			if (!push_braced(s, end - s))
				return "err unknown key in {}";
			s = end + 1;
			continue;
		}
		if (!push_ascii(c))
			return "err cannot type character";
	}
	return "ok";
}

static const char *do_key(char *args)
{
	struct chord ch = { {0}, 0 };
	char *save = NULL;
	for (char *t = strtok_r(args, " \t", &save); t; t = strtok_r(NULL, " \t", &save)) {
		if (!chord_add_name(&ch, t))
			return "err unknown key";
	}
	if (ch.n == 0)
		return "err no key";
	return q_push(&ch) ? "ok" : "err queue full";
}

/* ---- PNG screenshot ---- */

static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t crc, const uint8_t *buf, size_t len)
{
	if (!crc_table[1]) {
		for (uint32_t n = 0; n < 256; n++) {
			uint32_t c = n;
			for (int k = 0; k < 8; k++)
				c = (c & 1) ? 0xedb88320U ^ (c >> 1) : c >> 1;
			crc_table[n] = c;
		}
	}
	crc ^= 0xffffffffU;
	for (size_t i = 0; i < len; i++)
		crc = crc_table[(crc ^ buf[i]) & 0xff] ^ (crc >> 8);
	return crc ^ 0xffffffffU;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static bool png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
	uint8_t hdr[8];
	put32(hdr, len);
	memcpy(hdr + 4, type, 4);
	uint32_t crc = crc32_update(0, hdr + 4, 4);
	crc = crc32_update(crc, data, len);
	uint8_t tail[4];
	put32(tail, crc);
	return fwrite(hdr, 8, 1, f) == 1 &&
		(len == 0 || fwrite(data, len, 1, f) == 1) &&
		fwrite(tail, 4, 1, f) == 1;
}

#define SCR_W 720
#define SCR_H 348
#define ROWBYTES (1 + SCR_W / 8)

static bool save_png(const char *path)
{
	static uint8_t raw[ROWBYTES * SCR_H];
	// 3B1 VRAM: 45 big-endian 16-bit words per line, leftmost pixel = bit 0
	uint32_t a = 0;
	for (int y = 0; y < SCR_H; y++) {
		uint8_t *row = raw + y * ROWBYTES;
		row[0] = 0;	// filter: none
		for (int x = 0; x < SCR_W; x += 16) {
			uint16_t w = (state.vram[a] << 8) | state.vram[a + 1];
			a += 2;
			uint8_t b0 = 0, b1 = 0;
			for (int i = 0; i < 8; i++) {
				if (w & (1 << i)) b0 |= 0x80 >> i;
				if (w & (1 << (i + 8))) b1 |= 0x80 >> i;
			}
			if (state.reverse_video) { b0 = ~b0; b1 = ~b1; }
			row[1 + x / 8] = b0;
			row[2 + x / 8] = b1;
		}
	}
	// zlib stream with stored (uncompressed) deflate blocks
	size_t rawlen = sizeof(raw);
	size_t nblocks = (rawlen + 65534) / 65535;
	size_t zlen = 2 + nblocks * 5 + rawlen + 4;
	uint8_t *z = malloc(zlen), *p = z;
	if (!z)
		return false;
	*p++ = 0x78; *p++ = 0x01;
	size_t left = rawlen;
	const uint8_t *src = raw;
	while (left) {
		uint16_t n = left > 65535 ? 65535 : left;
		*p++ = (left == n) ? 1 : 0;
		*p++ = n & 0xff; *p++ = n >> 8;
		*p++ = ~n & 0xff; *p++ = (~n >> 8) & 0xff;
		memcpy(p, src, n);
		p += n; src += n; left -= n;
	}
	uint32_t s1 = 1, s2 = 0;
	for (size_t i = 0; i < rawlen; i++) {
		s1 = (s1 + raw[i]) % 65521;
		s2 = (s2 + s1) % 65521;
	}
	put32(p, (s2 << 16) | s1);
	p += 4;

	FILE *f = fopen(path, "wb");
	if (!f) {
		free(z);
		return false;
	}
	static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
	uint8_t ihdr[13];
	put32(ihdr, SCR_W);
	put32(ihdr + 4, SCR_H);
	ihdr[8] = 1;	// bit depth
	ihdr[9] = 0;	// greyscale
	ihdr[10] = ihdr[11] = ihdr[12] = 0;
	bool ok = fwrite(sig, 8, 1, f) == 1 &&
		png_chunk(f, "IHDR", ihdr, 13) &&
		png_chunk(f, "IDAT", z, p - z) &&
		png_chunk(f, "IEND", NULL, 0);
	ok = (fclose(f) == 0) && ok;
	free(z);
	return ok;
}

/* ---- socket ---- */

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#define MAXCLIENTS 8
static int listen_fd = -1;
static char *sock_path;
static struct client {
	int fd;
	char buf[8192];
	int len;
} clients[MAXCLIENTS];

static void reply(struct client *c, const char *msg)
{
	size_t n = strlen(msg);
	char *out = malloc(n + 2);
	if (!out)
		return;
	memcpy(out, msg, n);
	out[n++] = '\n';
	size_t off = 0;
	while (off < n) {
		ssize_t w = write(c->fd, out + off, n - off);
		if (w > 0) {
			off += w;
			continue;
		}
		if (w < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		break;
	}
	free(out);
}

static void command(struct client *c, char *line)
{
	char msg[1100];
	size_t l = strlen(line);
	while (l && (line[l - 1] == '\r' || line[l - 1] == ' '))
		line[--l] = 0;
	char *args = strchr(line, ' ');
	if (args)
		*args++ = 0;
	else
		args = line + l;

	if (strcmp(line, "type") == 0) {
		reply(c, do_type(args));
	} else if (strcmp(line, "key") == 0) {
		reply(c, do_key(args));
	} else if (strcmp(line, "floppy") == 0) {
		if (!*args) {
			reply(c, "err floppy needs a path");
			return;
		}
		freebee_floppy_eject();
		if (freebee_floppy_insert(args))
			reply(c, "ok");
		else
			reply(c, "err cannot load floppy image");
	} else if (strcmp(line, "hd2") == 0) {
		if (!*args) {
			reply(c, "err hd2 needs a path");
			return;
		}
		if (freebee_hd2_insert(args))
			reply(c, "ok");
		else
			reply(c, "err cannot load disk image");
	} else if (strcmp(line, "eject") == 0) {
		freebee_floppy_eject();
		reply(c, "ok");
	} else if (strcmp(line, "screenshot") == 0) {
		if (!*args) {
			reply(c, "err screenshot needs a path");
			return;
		}
		if (save_png(args)) {
			snprintf(msg, sizeof(msg), "ok %s", args);
			reply(c, msg);
		} else {
			reply(c, "err cannot write the file");
		}
	} else if (strcmp(line, "mouse") == 0) {
		int dx = 0, dy = 0, b = 0;
		if (sscanf(args, "%d %d %d", &dx, &dy, &b) < 2) {
			reply(c, "err mouse DX DY [BUTTONS]");
			return;
		}
		mouse_event(&state.kbd, dx, dy, b);
		reply(c, "ok");
	} else if (strcmp(line, "speed") == 0) {
		if (strcmp(args, "max") == 0)
			control_speed_max = true;
		else if (strcmp(args, "real") == 0)
			control_speed_max = false;
		else if (*args) {
			reply(c, "err speed max|real");
			return;
		}
		snprintf(msg, sizeof(msg), "ok speed=%s", control_speed_max ? "max" : "real");
		reply(c, msg);
	} else if (strcmp(line, "status") == 0) {
		const char *fp = freebee_floppy_path();
		snprintf(msg, sizeof(msg), "ok floppy=%s speed=%s keys=%d leds=%d",
			fp ? fp : "none", control_speed_max ? "max" : "real",
			q_count + (phase ? 1 : 0), state.leds);
		reply(c, msg);
	} else if (strcmp(line, "quit") == 0) {
		control_quit = true;
		reply(c, "ok");
	} else if (*line == 0) {
		reply(c, "err empty command");
	} else {
		reply(c, "err unknown command (type key floppy eject hd2 screenshot mouse speed status quit)");
	}
}

bool control_init(void)
{
	const char *path = getenv("FREEBEE_CONTROL");
	if (!path || !*path)
		path = fbc_get_string("control", "socket");
	if (!path || !*path)
		return false;
	signal(SIGPIPE, SIG_IGN);
	struct sockaddr_un sa;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof(sa.sun_path)) {
		fprintf(stderr, "control: socket path too long: %s\n", path);
		return false;
	}
	strcpy(sa.sun_path, path);
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("control: socket");
		return false;
	}
	unlink(path);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(fd, 4) < 0) {
		fprintf(stderr, "control: cannot listen on %s: %s\n", path, strerror(errno));
		close(fd);
		return false;
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	listen_fd = fd;
	sock_path = strdup(path);
	for (int i = 0; i < MAXCLIENTS; i++)
		clients[i].fd = -1;
	printf("Control socket on %s\n", path);
	return true;
}

static void poll_socket(void)
{
	if (listen_fd < 0)
		return;
	for (;;) {
		int fd = accept(listen_fd, NULL, NULL);
		if (fd < 0)
			break;
		int i;
		for (i = 0; i < MAXCLIENTS; i++)
			if (clients[i].fd < 0)
				break;
		if (i == MAXCLIENTS) {
			close(fd);
			continue;
		}
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
		clients[i].fd = fd;
		clients[i].len = 0;
	}
	for (int i = 0; i < MAXCLIENTS; i++) {
		struct client *c = &clients[i];
		if (c->fd < 0)
			continue;
		for (;;) {
			ssize_t n = read(c->fd, c->buf + c->len, sizeof(c->buf) - 1 - c->len);
			if (n > 0) {
				c->len += n;
				c->buf[c->len] = 0;
				char *nl;
				while ((nl = memchr(c->buf, '\n', c->len)) != NULL) {
					*nl = 0;
					command(c, c->buf);
					int rest = c->len - (nl + 1 - c->buf);
					memmove(c->buf, nl + 1, rest);
					c->len = rest;
					c->buf[c->len] = 0;
				}
				if (c->len >= (int)sizeof(c->buf) - 1) {
					reply(c, "err line too long");
					c->len = 0;
				}
				continue;
			}
			if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
				close(c->fd);
				c->fd = -1;
			}
			break;
		}
	}
}

void control_done(void)
{
	if (listen_fd < 0)
		return;
	for (int i = 0; i < MAXCLIENTS; i++)
		if (clients[i].fd >= 0)
			close(clients[i].fd);
	close(listen_fd);
	listen_fd = -1;
	if (sock_path)
		unlink(sock_path);
}
#else
bool control_init(void) { return false; }
static void poll_socket(void) { }
void control_done(void) { }
#endif

void control_tick(void)
{
	poll_socket();
	key_machine();
}
