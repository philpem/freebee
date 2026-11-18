/*
 * json - a small JSON reader and a string builder, for the MCP server.
 */
#ifndef _FB_JSON_H
#define _FB_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>

typedef enum {
	JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT
} json_type;

typedef struct json json;
struct json {
	json_type type;
	bool b;
	double num;
	char *str;		///< a string's value (NUL-terminated), or a number's text
	size_t len;		///< string length
	int n, cap;		///< array or object: number of items
	json **items;
	char **keys;	///< object keys
};

/// Parse one JSON value. On error returns NULL and sets *err.
json *json_parse(const char *text, size_t len, const char **err);
void json_free(json *j);

/// Object member, or NULL.
json *json_get(const json *obj, const char *key);
/// The string member, or NULL when it is missing or not a string.
const char *json_get_str(const json *obj, const char *key);
/// Whether the object has the member (not null).
bool json_has(const json *obj, const char *key);

/* ---- string builder ---- */

typedef struct {
	char *s;
	size_t len, cap;
} sbuf;

void sb_init(sbuf *b);
void sb_free(sbuf *b);
/// Take the string out of the builder; the builder is empty after.
char *sb_take(sbuf *b);
void sb_add(sbuf *b, const void *data, size_t n);
/// Make room for n more bytes; returns where they go (len is unchanged).
char *sb_reserve(sbuf *b, size_t n);
void sb_puts(sbuf *b, const char *s);
void sb_putc(sbuf *b, char c);
void sb_printf(sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_vprintf(sbuf *b, const char *fmt, va_list ap);
/// A JSON string literal, quoted and escaped.
void sb_jstr(sbuf *b, const char *s);
void sb_jstrn(sbuf *b, const char *s, size_t n);
/// Write a value back as JSON.
void json_dump(sbuf *b, const json *j);

/// Base64 of data, appended.
void sb_base64(sbuf *b, const unsigned char *data, size_t n);

#endif
