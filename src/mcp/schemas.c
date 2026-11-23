/*
 * schemas - the tool list (tools.json) and the instructions
 * (instructions.txt), built into the program by embed.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "mcp.h"
#pragma GCC diagnostic ignored "-Woverlength-strings"
#include "schema_data.h"

// placeholders in tools.json, for schemas many tools share
static const char rect_schema[] =
	"{\"type\":\"object\",\"description\":\"a rectangle in screen pixels\","
	"\"properties\":{\"x\":{\"type\":\"integer\",\"description\":\"left edge\"},"
	"\"y\":{\"type\":\"integer\",\"description\":\"top edge\"},"
	"\"w\":{\"type\":\"integer\",\"description\":\"width\"},"
	"\"h\":{\"type\":\"integer\",\"description\":\"height\"}},"
	"\"required\":[\"x\",\"y\",\"w\",\"h\"]}";

#define SHOT(def) \
	"{\"description\":\"take a screenshot at the end (default: " def ")\"," \
	"\"anyOf\":[{\"type\":\"boolean\"},{\"type\":\"object\",\"properties\":{" \
	"\"region\":@RECT@,\"scale\":{\"type\":\"number\",\"description\":\"from 0.5 to 4 (default: 1)\"}," \
	"\"aspect\":{\"type\":\"boolean\",\"description\":\"stretch to 4:3 (default: false)\"}}}]}"

static const char then_schema[] =
	"{\"type\":\"object\",\"description\":\"wait and look after the input\",\"properties\":{"
	"\"wait\":{\"type\":\"string\",\"enum\":[\"none\",\"change\",\"stable\",\"change-then-stable\"],"
	"\"description\":\"what to wait for (default: none)\"},"
	"\"delay-ms\":{\"type\":\"integer\",\"description\":\"sleep this long after the input, before"
	" waiting (default: 0)\"},"
	"\"stable-ms\":{\"type\":\"integer\",\"description\":\"how long the screen must not change to be"
	" stable, in guest time (default: 1000)\"},"
	"\"timeout-ms\":{\"type\":\"integer\",\"description\":\"give up waiting after this long, at most"
	" 1500000, 25 minutes (default: 10000)\"},"
	"\"min-pixels\":{\"type\":\"integer\",\"description\":\"a change must touch at least this many"
	" pixels (default: 16)\"},"
	"\"screenshot\":" SHOT("false") "}}";

static char *tools_text;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static char *replace_all(const char *s, const char *from, const char *to)
{
	sbuf b;
	sb_init(&b);
	size_t fl = strlen(from);
	for (;;) {
		const char *p = strstr(s, from);
		if (!p) {
			sb_puts(&b, s);
			break;
		}
		sb_add(&b, s, p - s);
		sb_puts(&b, to);
		s = p + fl;
	}
	return sb_take(&b);
}

static void build(void)
{
	char *rect = strdup(rect_schema);
	char *then = replace_all(then_schema, "@RECT@", rect);
	char *shot = replace_all(SHOT("true"), "@RECT@", rect);
	char *a = replace_all(tools_src, "\"@THEN@\"", then);
	char *b = replace_all(a, "\"@SHOT_TRUE@\"", shot);
	tools_text = replace_all(b, "\"@RECT@\"", rect);
	free(a);
	free(b);
	free(rect);
	free(then);
	free(shot);
	const char *err = NULL;
	json *j = json_parse(tools_text, strlen(tools_text), &err);
	if (!j || j->type != JSON_ARRAY) {
		fprintf(stderr, "MCP: the tool list does not parse: %s\n", err ? err : "not an array");
		free(tools_text);
		tools_text = strdup("[]");
	}
	json_free(j);
}

const char *mcp_tools_json(void)
{
	pthread_once(&once, build);
	return tools_text;
}

const char *mcp_instructions(void)
{
	return instructions_src;
}

bool mcp_protocol_supported(const char *v)
{
	static const char *const versions[] = { "2025-11-25", "2025-06-18", NULL };
	for (int i = 0; v && versions[i]; i++)
		if (strcmp(v, versions[i]) == 0)
			return true;
	return false;
}
