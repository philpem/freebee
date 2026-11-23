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
 *   pointer X Y [B]    as if the host pointer were at X,Y over the window,
 *                      with buttons B: the seamless mouse follows it
 *   wheel N            as if the host's mouse wheel turned N steps (+ up,
 *                      - down): it scrolls a window that has scroll arrows
 *   closeapp           like Alt-F9: close the program in the front window
 *                      (kill its process group), not the Office's
 *   closeapp result    what the last closeapp did
 *   speed max|real     run as fast as the host can, or at 10 MHz
 *   status             floppy, speed, keys still queued, LEDs, idle: how
 *                      much of the last second the idle skip took off the
 *                      CPU, and the mouse: off, or where the pointer is
 *                      (? = not known yet)
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
#include "keys.h"
#include "pointer.h"
#include "wheel.h"
#include "closeapp.h"

bool control_speed_max = false;
int control_turbo = 1;
int control_idle_pct = 0;
bool control_quit = false;

/* ---- typing (the key queue is in keys.c) ---- */

static bool push_ascii(int c)
{
	key_chord ch;
	return keys_from_char(&ch, c) && keys_push(&ch) != 0;
}

// {NAME} or {CTRL-SHIFT-NAME}; s points after '{', len excludes '}'
static bool push_braced(const char *s, int len)
{
	char buf[64];
	key_chord ch;
	memset(&ch, 0, sizeof(ch));
	if (len <= 0 || len >= (int)sizeof(buf))
		return false;
	memcpy(buf, s, len);
	buf[len] = 0;
	return keys_parse_combo(&ch, buf) && keys_push(&ch) != 0;
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
	key_chord ch;
	char *save = NULL;
	memset(&ch, 0, sizeof(ch));
	for (char *t = strtok_r(args, " \t", &save); t; t = strtok_r(NULL, " \t", &save)) {
		if (!keys_add_name(&ch, t))
			return "err unknown key";
	}
	if (ch.n == 0)
		return "err no key";
	return keys_push(&ch) ? "ok" : "err queue full";
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
// A client's replies wait in out until the socket takes them: the main
// loop never waits for a client. A client that lets OUT_MAX bytes of
// replies pile up is dropped.
#define OUT_MAX 65536
static struct client {
	int fd;
	char buf[8192];
	int len;
	char out[OUT_MAX];
	int out_len;
	bool drop;
} clients[MAXCLIENTS];

static void client_close(struct client *c)
{
	close(c->fd);
	c->fd = -1;
}

// send what the socket takes now; false if the client is gone
static bool flush_out(struct client *c)
{
	while (c->out_len > 0) {
		ssize_t w = write(c->fd, c->out, c->out_len);
		if (w > 0) {
			memmove(c->out, c->out + w, c->out_len - w);
			c->out_len -= w;
			continue;
		}
		if (w < 0 && errno == EINTR)
			continue;
		return w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
	}
	return true;
}

static void reply(struct client *c, const char *msg)
{
	size_t const n = strlen(msg);
	if (c->drop)
		return;
	if (c->out_len + n + 1 > sizeof(c->out)) {
		c->drop = true;		// it does not read its replies
		return;
	}
	memcpy(c->out + c->out_len, msg, n);
	c->out[c->out_len + n] = '\n';
	c->out_len += n + 1;
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
		if (!state.kbd.mouse_enabled)
			reply(c, "err the guest has not turned the mouse on");
		else if (!mouse_event(&state.kbd, dx, dy, b))
			reply(c, "err the keyboard buffer is full");
		else
			reply(c, "ok");
	} else if (strcmp(line, "pointer") == 0) {
		int x = 0, y = 0, b = -1;
		if (sscanf(args, "%d %d %d", &x, &y, &b) < 2) {
			reply(c, "err pointer X Y [BUTTONS]");
			return;
		}
		pointer_track(x, y);
		if (b >= 0)
			pointer_buttons(b & 7);
		reply(c, "ok");
	} else if (strcmp(line, "wheel") == 0) {
		float n;
		if (sscanf(args, "%f", &n) != 1) {
			reply(c, "err wheel STEPS (+ up, - down)");
			return;
		}
		wheel_scroll(n);
		reply(c, "ok");
	} else if (strcmp(line, "closeapp") == 0) {
		if (strcmp(args, "result") == 0) {
			snprintf(msg, sizeof(msg), "ok %s", closeapp_result());
			reply(c, msg);
		} else if (!*args) {
			closeapp_request();
			reply(c, "ok");
		} else {
			reply(c, "err closeapp [result]");
		}
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
	} else if (strcmp(line, "turbo") == 0) {
		int t;
		if (*args) {
			if (sscanf(args, "%d", &t) != 1 || t < 1 || t > TURBO_MAX) {
				reply(c, "err turbo 1-32");
				return;
			}
			control_turbo = t;
		}
		snprintf(msg, sizeof(msg), "ok turbo=%d", control_turbo);
		reply(c, msg);
	} else if (strcmp(line, "status") == 0) {
		const char *fp = freebee_floppy_path();
		char mouse[32] = "off";
		int px, py;
		if (state.kbd.mouse_enabled) {
			if (pointer_get(&px, &py))
				snprintf(mouse, sizeof(mouse), "%d,%d", px, py);
			else
				strcpy(mouse, "?");
		}
		snprintf(msg, sizeof(msg), "ok floppy=%s speed=%s turbo=%d keys=%d leds=%d idle=%d mouse=%s",
			fp ? fp : "none", control_speed_max ? "max" : "real", control_turbo,
			keys_pending(), state.leds, control_idle_pct, mouse);
		reply(c, msg);
	} else if (strcmp(line, "quit") == 0) {
		control_quit = true;
		reply(c, "ok");
	} else if (*line == 0) {
		reply(c, "err empty command");
	} else {
		reply(c, "err unknown command (type key floppy eject hd2 screenshot mouse pointer wheel closeapp speed status quit)");
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
		clients[i].out_len = 0;
		clients[i].drop = false;
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
				// the client is done: the replies it has not got yet go
				// now, as far as the socket takes them
				flush_out(c);
				client_close(c);
			}
			break;
		}
		if (c->fd >= 0 && (c->drop || !flush_out(c))) {
			if (c->drop)
				fprintf(stderr, "control: a client did not read its replies; it was dropped\n");
			client_close(c);
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
	keys_tick();
}

void control_poll(void)
{
	poll_socket();
}
