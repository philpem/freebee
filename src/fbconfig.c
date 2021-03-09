/*
 * fbconfig - freebee configuration routines.
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "fbconfig.h"
#include "toml.h"

static bool inited = false;
static bool have_toml = false;
static toml_table_t *table_config = NULL;

/* initialize --- initialize TOML files */

static void
initialize(void)
{
	FILE* fp;
	char errbuf[200];
	char toml_file[BUFSIZ];

	strcpy(toml_file, ".freebee.toml");
	fp = fopen(toml_file, "r");
	if (fp == NULL) {
		char *home = getenv("HOME");

		if (home != NULL) {
			sprintf(toml_file, "%s/.freebee.toml", home);
			fp = fopen(toml_file, "r");
		}
	}

	inited = true;
	if (fp == NULL)
		return;

	table_config = toml_parse_file(fp, errbuf, sizeof(errbuf));
	fclose(fp);

	if (table_config == NULL) {
		fprintf(stderr, "freebee: %s: cannot parse: %s",
			toml_file, errbuf);
		return;
	}

	have_toml = true;
}


/* get_default_string --- get a string value from the configuration */

static const char *
get_default_string(const char *heading, const char *item)
{
	static struct default_strings {
		const char *heading;
		const char *item;
		const char *value;
	} defaults[] = {
		{ "floppy", "disk", "floppy.img" },
		{ "hard_disk", "disk1", "hd.img" },
		{ "hard_disk", "disk2", "hd2.img" },
		{ "roms", "rom_14c", "roms/14c.bin" },
		{ "roms", "rom_15c", "roms/15c.bin" },
		{ "serial", "symlink", "serial-pty" },
		{ "display", "scale_quality", "nearest" },
		{ NULL, NULL, NULL }
	};

	int i;
	for (i = 0; defaults[i].heading != NULL; i++) {
		if (strcmp(defaults[i].heading, heading) == 0 && strcmp(defaults[i].item, item) == 0) {
			return defaults[i].value;
		}
	}

	return NULL;
}

/* get_default_double --- get a double value from the configuration */

static double
get_default_double(const char *heading, const char *item)
{
	static struct default_doubles {
		const char *heading;
		const char *item;
		double value;
	} defaults[] = {
		{ "display", "x_scale", 1.0 },
		{ "display", "y_scale", 1.0 },
		{ NULL, NULL, 0.0 }
	};

	int i;
	for (i = 0; defaults[i].heading != NULL; i++) {
		if (strcmp(defaults[i].heading, heading) == 0 && strcmp(defaults[i].item, item) == 0) {
			return defaults[i].value;
		}
	}

	return 0.0;
}

/* get_default_bool --- get a bool value from the configuration */

static bool
get_default_bool(const char *heading, const char *item)
{
	static struct default_doubles {
		const char *heading;
		const char *item;
		bool value;
	} defaults[] = {
		{ "vidpal", "installed", true },
		{ NULL, NULL, false }
	};

	int i;
	for (i = 0; defaults[i].heading != NULL; i++) {
		if (strcmp(defaults[i].heading, heading) == 0 && strcmp(defaults[i].item, item) == 0) {
			return defaults[i].value;
		}
	}

	return false;
}

/* get_default_int --- get an int value from the configuration */

static int
get_default_int(const char *heading, const char *item)
{
	static struct default_doubles {
		const char *heading;
		const char *item;
		int value;
	} defaults[] = {
		{ "display", "red", 0x00 },
		{ "display", "green", 0xFF },
		{ "display", "blue", 0x00 },
		{ "hard_disk", "heads", 8 },
		{ "hard_disk", "sectors_per_track", 17 },
		{ "memory", "base_memory", 2048 },
		{ "memory", "extended_memory", 2048 },
		{ NULL, NULL, 0 }
	};

	int i;
	for (i = 0; defaults[i].heading != NULL; i++) {
		if (strcmp(defaults[i].heading, heading) == 0 && strcmp(defaults[i].item, item) == 0) {
			return defaults[i].value;
		}
	}

	return 0;
}


/* fbc_get_string --- get a string value from the configuration */

const char *
fbc_get_string(const char *heading, const char *item)
{
	if (! inited)
		initialize();

	if (have_toml) {
		toml_table_t* category = toml_table_in(table_config, heading);
		if (category != NULL) {
			toml_datum_t value = toml_string_in(category, item);
			if (value.ok) {
				const char *ret = strdup(value.u.s);
				free(value.u.s);
				return ret;
			}
		}
	}

	return get_default_string(heading, item);
}

/* fbc_get_double --- get a double value from the configuration */

double
fbc_get_double(const char *heading, const char *item)
{
	if (! inited)
		initialize();

	if (have_toml) {
		toml_table_t* category = toml_table_in(table_config, heading);
		if (category != NULL) {
			toml_datum_t value = toml_double_in(category, item);
			if (value.ok) {
				return value.u.d;
			}
		}
	}

	return get_default_double(heading, item);
}

/* fbc_get_bool --- get a bool value from the configuration */

bool
fbc_get_bool(const char *heading, const char *item)
{
	if (! inited)
		initialize();

	if (have_toml) {
		toml_table_t* category = toml_table_in(table_config, heading);
		if (category != NULL) {
			toml_datum_t value = toml_bool_in(category, item);
			if (value.ok) {
				return value.u.b;
			}
		}
	}


	return get_default_bool(heading, item);
}

/* fbc_get_int --- get a int value from the configuration */

int
fbc_get_int(const char *heading, const char *item)
{
	if (! inited)
		initialize();

	if (have_toml) {
		toml_table_t* category = toml_table_in(table_config, heading);
		if (category != NULL) {
			toml_datum_t value = toml_int_in(category, item);
			if (value.ok) {
				return value.u.i;
			}
		}
	}


	return get_default_int(heading, item);
}
