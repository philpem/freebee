/*
 * json - a small JSON reader and a string builder (see json.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "json.h"

/* ---- builder ---- */

void sb_init(sbuf *b)
{
	b->s = NULL;
	b->len = b->cap = 0;
}

void sb_free(sbuf *b)
{
	free(b->s);
	sb_init(b);
}

static void sb_room(sbuf *b, size_t n)
{
	if (b->len + n + 1 <= b->cap)
		return;
	size_t cap = b->cap ? b->cap : 256;
	while (cap < b->len + n + 1)
		cap *= 2;
	char *s = realloc(b->s, cap);
	if (!s) {
		fprintf(stderr, "mcp: out of memory\n");
		abort();
	}
	b->s = s;
	b->cap = cap;
}

char *sb_reserve(sbuf *b, size_t n)
{
	sb_room(b, n);
	return b->s + b->len;
}

char *sb_take(sbuf *b)
{
	sb_room(b, 0);
	b->s[b->len] = 0;
	char *s = b->s;
	sb_init(b);
	return s;
}

void sb_add(sbuf *b, const void *data, size_t n)
{
	sb_room(b, n);
	memcpy(b->s + b->len, data, n);
	b->len += n;
	b->s[b->len] = 0;
}

void sb_puts(sbuf *b, const char *s)
{
	sb_add(b, s, strlen(s));
}

void sb_putc(sbuf *b, char c)
{
	sb_add(b, &c, 1);
}

void sb_vprintf(sbuf *b, const char *fmt, va_list ap)
{
	va_list ap2;
	va_copy(ap2, ap);
	int n = vsnprintf(NULL, 0, fmt, ap2);
	va_end(ap2);
	if (n < 0)
		return;
	sb_room(b, n);
	vsnprintf(b->s + b->len, n + 1, fmt, ap);
	b->len += n;
}

void sb_printf(sbuf *b, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	sb_vprintf(b, fmt, ap);
	va_end(ap);
}

void sb_jstrn(sbuf *b, const char *s, size_t n)
{
	static const char hex[] = "0123456789abcdef";
	sb_room(b, n + 2);
	sb_putc(b, '"');
	for (size_t i = 0; i < n; i++) {
		unsigned char c = s[i];
		switch (c) {
		case '"': sb_puts(b, "\\\""); break;
		case '\\': sb_puts(b, "\\\\"); break;
		case '\n': sb_puts(b, "\\n"); break;
		case '\r': sb_puts(b, "\\r"); break;
		case '\t': sb_puts(b, "\\t"); break;
		case '\b': sb_puts(b, "\\b"); break;
		case '\f': sb_puts(b, "\\f"); break;
		default:
			if (c < 0x20 || c == 0x7f) {
				char u[7] = { '\\', 'u', '0', '0', hex[c >> 4], hex[c & 15], 0 };
				sb_puts(b, u);
			} else if (c >= 0x80) {
				// pass valid UTF-8 through; a stray byte becomes U+FFFD
				int extra = c >= 0xf8 ? -1 : c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc2 ? 1 : -1;
				bool ok = extra > 0 && i + extra < n;
				for (int k = 1; ok && k <= extra; k++)
					ok = ((unsigned char)s[i + k] & 0xc0) == 0x80;
				if (ok) {
					sb_add(b, s + i, extra + 1);
					i += extra;
				} else {
					sb_puts(b, "\\ufffd");
				}
			} else {
				sb_putc(b, c);
			}
		}
	}
	sb_putc(b, '"');
}

void sb_jstr(sbuf *b, const char *s)
{
	sb_jstrn(b, s ? s : "", s ? strlen(s) : 0);
}

void sb_base64(sbuf *b, const unsigned char *d, size_t n)
{
	static const char t[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	sb_room(b, (n + 2) / 3 * 4);
	char *o = b->s + b->len;
	size_t i = 0;
	for (; i + 2 < n; i += 3) {
		uint32_t v = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
		*o++ = t[v >> 18]; *o++ = t[(v >> 12) & 63];
		*o++ = t[(v >> 6) & 63]; *o++ = t[v & 63];
	}
	if (i < n) {
		uint32_t v = d[i] << 16;
		if (i + 1 < n)
			v |= d[i + 1] << 8;
		*o++ = t[v >> 18]; *o++ = t[(v >> 12) & 63];
		*o++ = (i + 1 < n) ? t[(v >> 6) & 63] : '=';
		*o++ = '=';
	}
	b->len = o - b->s;
	b->s[b->len] = 0;
}

void json_dump(sbuf *b, const json *j)
{
	if (!j) {
		sb_puts(b, "null");
		return;
	}
	switch (j->type) {
	case JSON_NULL: sb_puts(b, "null"); break;
	case JSON_BOOL: sb_puts(b, j->b ? "true" : "false"); break;
	case JSON_NUMBER: sb_puts(b, j->str); break;
	case JSON_STRING: sb_jstrn(b, j->str, j->len); break;
	case JSON_ARRAY:
		sb_putc(b, '[');
		for (int i = 0; i < j->n; i++) {
			if (i)
				sb_putc(b, ',');
			json_dump(b, j->items[i]);
		}
		sb_putc(b, ']');
		break;
	case JSON_OBJECT:
		sb_putc(b, '{');
		for (int i = 0; i < j->n; i++) {
			if (i)
				sb_putc(b, ',');
			sb_jstr(b, j->keys[i]);
			sb_putc(b, ':');
			json_dump(b, j->items[i]);
		}
		sb_putc(b, '}');
		break;
	}
}

/* ---- reader ---- */

typedef struct {
	const char *p, *end;
	const char *err;
	int depth;
} reader;

static void skip_ws(reader *r)
{
	while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r'))
		r->p++;
}

static json *new_value(json_type t)
{
	json *j = calloc(1, sizeof(*j));
	if (j)
		j->type = t;
	return j;
}

void json_free(json *j)
{
	if (!j)
		return;
	for (int i = 0; i < j->n; i++) {
		json_free(j->items[i]);
		if (j->keys)
			free(j->keys[i]);
	}
	free(j->items);
	free(j->keys);
	free(j->str);
	free(j);
}

static bool add_item(json *j, char *key, json *v)
{
	if (j->n == j->cap) {
		int cap = j->cap ? j->cap * 2 : 8;
		json **items = realloc(j->items, cap * sizeof(*items));
		if (!items)
			return false;
		j->items = items;
		if (j->type == JSON_OBJECT) {
			char **keys = realloc(j->keys, cap * sizeof(*keys));
			if (!keys)
				return false;
			j->keys = keys;
		}
		j->cap = cap;
	}
	j->items[j->n] = v;
	if (j->type == JSON_OBJECT)
		j->keys[j->n] = key;
	j->n++;
	return true;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static void put_utf8(sbuf *b, uint32_t cp)
{
	char u[4];
	if (cp < 0x80) {
		u[0] = cp;
		sb_add(b, u, 1);
	} else if (cp < 0x800) {
		u[0] = 0xc0 | (cp >> 6); u[1] = 0x80 | (cp & 63);
		sb_add(b, u, 2);
	} else if (cp < 0x10000) {
		u[0] = 0xe0 | (cp >> 12); u[1] = 0x80 | ((cp >> 6) & 63); u[2] = 0x80 | (cp & 63);
		sb_add(b, u, 3);
	} else {
		u[0] = 0xf0 | (cp >> 18); u[1] = 0x80 | ((cp >> 12) & 63);
		u[2] = 0x80 | ((cp >> 6) & 63); u[3] = 0x80 | (cp & 63);
		sb_add(b, u, 4);
	}
}

static bool read_hex4(reader *r, uint32_t *v)
{
	if (r->end - r->p < 4)
		return false;
	*v = 0;
	for (int i = 0; i < 4; i++) {
		int h = hexval(r->p[i]);
		if (h < 0)
			return false;
		*v = (*v << 4) | h;
	}
	r->p += 4;
	return true;
}

// at the opening quote; returns a malloc'd string
static char *read_string(reader *r, size_t *lenp)
{
	sbuf b;
	sb_init(&b);
	r->p++;
	while (r->p < r->end && *r->p != '"') {
		unsigned char c = *r->p++;
		if (c < 0x20) {
			r->err = "control character in a string";
			goto fail;
		}
		if (c != '\\') {
			sb_putc(&b, c);
			continue;
		}
		if (r->p >= r->end)
			break;
		c = *r->p++;
		switch (c) {
		case '"': sb_putc(&b, '"'); break;
		case '\\': sb_putc(&b, '\\'); break;
		case '/': sb_putc(&b, '/'); break;
		case 'b': sb_putc(&b, '\b'); break;
		case 'f': sb_putc(&b, '\f'); break;
		case 'n': sb_putc(&b, '\n'); break;
		case 'r': sb_putc(&b, '\r'); break;
		case 't': sb_putc(&b, '\t'); break;
		case 'u': {
			uint32_t cp, lo;
			if (!read_hex4(r, &cp)) {
				r->err = "bad \\u escape";
				goto fail;
			}
			if (cp >= 0xd800 && cp < 0xdc00 && r->end - r->p >= 6 &&
			    r->p[0] == '\\' && r->p[1] == 'u') {
				r->p += 2;
				if (!read_hex4(r, &lo) || lo < 0xdc00 || lo > 0xdfff) {
					r->err = "bad surrogate pair";
					goto fail;
				}
				cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
			}
			put_utf8(&b, cp);
			break;
		}
		default:
			r->err = "bad escape in a string";
			goto fail;
		}
	}
	if (r->p >= r->end) {
		r->err = "unterminated string";
		goto fail;
	}
	r->p++;
	*lenp = b.len;
	if (!b.s)
		return calloc(1, 1);
	return sb_take(&b);
fail:
	sb_free(&b);
	return NULL;
}

static json *read_value(reader *r);

static json *read_container(reader *r, bool object)
{
	json *j = new_value(object ? JSON_OBJECT : JSON_ARRAY);
	char close = object ? '}' : ']';
	if (!j)
		return NULL;
	if (++r->depth > 200) {
		r->err = "nested too deep";
		goto fail;
	}
	r->p++;
	skip_ws(r);
	if (r->p < r->end && *r->p == close) {
		r->p++;
		r->depth--;
		return j;
	}
	for (;;) {
		char *key = NULL;
		skip_ws(r);
		if (object) {
			size_t kl;
			if (r->p >= r->end || *r->p != '"') {
				r->err = "expected a member name";
				goto fail;
			}
			key = read_string(r, &kl);
			if (!key)
				goto fail;
			skip_ws(r);
			if (r->p >= r->end || *r->p != ':') {
				free(key);
				r->err = "expected ':'";
				goto fail;
			}
			r->p++;
		}
		json *v = read_value(r);
		if (!v) {
			free(key);
			goto fail;
		}
		if (!add_item(j, key, v)) {
			free(key);
			json_free(v);
			r->err = "out of memory";
			goto fail;
		}
		skip_ws(r);
		if (r->p < r->end && *r->p == ',') {
			r->p++;
			continue;
		}
		if (r->p < r->end && *r->p == close) {
			r->p++;
			r->depth--;
			return j;
		}
		r->err = object ? "expected ',' or '}'" : "expected ',' or ']'";
		goto fail;
	}
fail:
	json_free(j);
	return NULL;
}

static json *read_value(reader *r)
{
	skip_ws(r);
	if (r->p >= r->end) {
		r->err = "unexpected end";
		return NULL;
	}
	char c = *r->p;
	if (c == '{' || c == '[')
		return read_container(r, c == '{');
	if (c == '"') {
		json *j = new_value(JSON_STRING);
		if (!j)
			return NULL;
		j->str = read_string(r, &j->len);
		if (!j->str) {
			free(j);
			return NULL;
		}
		return j;
	}
	if (c == '-' || (c >= '0' && c <= '9')) {
		const char *s = r->p;
		if (*r->p == '-')
			r->p++;
		while (r->p < r->end && ((*r->p >= '0' && *r->p <= '9') || *r->p == '.' ||
		       *r->p == 'e' || *r->p == 'E' || *r->p == '+' || *r->p == '-'))
			r->p++;
		json *j = new_value(JSON_NUMBER);
		if (!j)
			return NULL;
		j->str = strndup(s, r->p - s);
		char *end;
		j->num = strtod(j->str, &end);
		if (*end || end == j->str) {
			r->err = "bad number";
			json_free(j);
			return NULL;
		}
		return j;
	}
	static const struct { const char *word; json_type t; bool b; } words[] = {
		{ "true", JSON_BOOL, true }, { "false", JSON_BOOL, false }, { "null", JSON_NULL, false }
	};
	for (int i = 0; i < 3; i++) {
		size_t l = strlen(words[i].word);
		if ((size_t)(r->end - r->p) >= l && memcmp(r->p, words[i].word, l) == 0) {
			r->p += l;
			json *j = new_value(words[i].t);
			if (j)
				j->b = words[i].b;
			return j;
		}
	}
	r->err = "unexpected character";
	return NULL;
}

json *json_parse(const char *text, size_t len, const char **err)
{
	reader r = { text, text + len, NULL, 0 };
	json *j = read_value(&r);
	if (j) {
		skip_ws(&r);
		if (r.p != r.end) {
			json_free(j);
			j = NULL;
			r.err = "text after the value";
		}
	}
	if (!j && err)
		*err = r.err ? r.err : "out of memory";
	return j;
}

json *json_get(const json *obj, const char *key)
{
	if (!obj || obj->type != JSON_OBJECT)
		return NULL;
	for (int i = 0; i < obj->n; i++)
		if (strcmp(obj->keys[i], key) == 0)
			return obj->items[i];
	return NULL;
}

const char *json_get_str(const json *obj, const char *key)
{
	json *v = json_get(obj, key);
	return (v && v->type == JSON_STRING) ? v->str : NULL;
}

bool json_has(const json *obj, const char *key)
{
	json *v = json_get(obj, key);
	return v && v->type != JSON_NULL;
}
