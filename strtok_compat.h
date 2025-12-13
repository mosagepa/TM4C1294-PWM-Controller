#ifndef STRTOK_COMPAT_H
#define STRTOK_COMPAT_H

/* Ensure POSIX declarations visible on toolchains that respect this macro.
   If you already have it in main.c, ensure it appears before any includes. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <string.h> /* declares strtok_r on POSIX-compliant toolchains */

/* Fallback prototype for strtok_r if the C library didn't declare it.
   snprintf-style: char *strtok_r(char *restrict s, const char *restrict delim,
                                  char **restrict saveptr);
*/
#if !defined(__STDC_LIB_EXT1__) && !defined(HAVE_STRTOK_R)
#ifdef __cplusplus
extern "C" {
#endif
char *strtok_r(char *s, const char *delim, char **saveptr);
#ifdef __cplusplus
}
#endif
#endif /* !HAVE_STRTOK_R */

#endif /* STRTOK_COMPAT_H */