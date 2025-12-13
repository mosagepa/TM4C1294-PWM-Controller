/* Minimal ASCII-only ctype helpers for embedded use.
 * Use these instead of isspace/toupper when newlib or the toolchain
 * does not provide full locale/ctype tables.
 */

#ifndef CTYPE_HELPERS_H
#define CTYPE_HELPERS_H

static inline int my_isspace(int c)
{
    /* Only ASCII whitespace handled; safe for chars passed as unsigned char */
    return (c == ' ')  || (c == '\f') || (c == '\n') ||
           (c == '\r') || (c == '\t') || (c == '\v');
}

static inline int my_toupper(int c)
{
    if ((unsigned)c >= 'a' && (unsigned)c <= 'z')
        return c - ('a' - 'A');
    return c;
}

#endif /* CTYPE_HELPERS_H */