/*
 * fbconfig - freebee configuration routines.
 */

#ifndef FBCONNFIG_H
#define FBCONNFIG_H 1

extern const char *fbc_get_string(const char *heading, const char *item);
extern double fbc_get_double(const char *heading, const char *item);
extern bool fbc_get_bool(const char *heading, const char *item);
extern int fbc_get_int(const char *heading, const char *item);
#endif /* FBCONNFIG_H */
