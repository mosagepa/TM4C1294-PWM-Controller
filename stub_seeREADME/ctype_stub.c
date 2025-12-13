/* Minimal __ctype_ptr__ stub to satisfy ctype macros that expect it.
 * ASCII-only. Add to your project and link it in.
 *
 * NOTE: This may be library-specific; some systems use __ctype_b_loc or
 * other symbol names. If your toolchain still complains about a different
 * symbol, inspect the error and adapt accordingly.
 */

#include <stdint.h>

/* Table values: bitflags are not used here, we provide simple 0/1 in high bytes
   so macros that index table by character works for basic checks.
   To be conservative we provide an array of unsigned short similar to
   what some ctype implementations expect.
*/

/* Basic bitset flags (not all macros will use these exact flags) */
#define CTYPE_SPACE  0x0001
#define CTYPE_DIGIT  0x0002
#define CTYPE_UPPER  0x0004
#define CTYPE_LOWER  0x0008
#define CTYPE_ALPHA  (CTYPE_UPPER | CTYPE_LOWER)
#define CTYPE_ALNUM  (CTYPE_ALPHA | CTYPE_DIGIT)
#define CTYPE_HEX    0x0010

/* Create a table for 256 entries */
static const unsigned short __simple_ctype_table[256] = {
    /* 0..31 control characters: none are spaces except 9..13 (HT LF VT FF CR) */
    /* We'll mark 9..13 as whitespace; rest zero */
    0,0,0,0,0,0,0,0,               /* 0..7 */
    0, CTYPE_SPACE, CTYPE_SPACE, CTYPE_SPACE, CTYPE_SPACE, CTYPE_SPACE, 0,0, /* 8..15 (9..13 are space) */
    0,0,0,0,0,0,0,0,               /* 16..23 */
    0,0,0,0,0,0,0,0,               /* 24..31 */
    /* 32 ' ' space */
    CTYPE_SPACE,                    /* 32 ' ' */
    /* 33..47 punctuation */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* 33..48 */
    /* 48..57 digits '0'..'9' */
    CTYPE_DIGIT | CTYPE_ALNUM, CTYPE_DIGIT | CTYPE_ALNUM, CTYPE_DIGIT | CTYPE_ALNUM, CTYPE_DIGIT | CTYPE_ALNUM,
    CTYPE_DIGIT | CTYPE_ALNUM, CTYPE_DIGIT | CTYPE_ALNUM, CTYPE_DIGIT | CTYPE_ALNUM, CTYPE_DIGIT | CTYPE_ALNUM,
    CTYPE_DIGIT | CTYPE_ALNUM, CTYPE_DIGIT | CTYPE_ALNUM, /* '0'..'9' */
    /* remaining punctuation 58..64 */
    0,0,0,0,0,0,0,
    /* 65..90 'A'..'Z' uppercase */
    CTYPE_UPPER | CTYPE_ALPHA | CTYPE_ALNUM, CTYPE_UPPER | CTYPE_ALPHA | CTYPE_ALNUM,
    CTYPE_UPPER | CTYPE_ALPHA | CTYPE_ALNUM, CTYPE_UPPER | CTYPE_ALPHA | CTYPE_ALNUM,
    CTYPE_UPPER | CTYPE_ALPHA | CTYPE_ALNUM, CTYPE_UPPER | CTYPE_ALPHA | CTYPE_ALNUM,
    CTYPE_UPPER | CTYPE_ALPHA | CTYPE_ALNUM, CTYPE_UPPER | CTYPE_ALPHA | CTYPE_ALNUM,
    CTYPE_UPPER | CTYPE_ALPHA | CTYPE_ALNUM, CTYPE_UPPER | CTYPE_ALPHA | CTYPE_ALNUM,
    CTYPE_UPPER | CTYPE_ALPHA | CTYPE_ALNUM, CTYPE_UPPER | CTYPE_ALPHA | CTYPE_ALNUM,
    CTYPE_UPPER | CTYPE_ALPHA | CTYPE_ALNUM, CTYPE_UPPER | CTYPE_ALPHA | CTYPE_ALNUM,
    CTYPE_UPPER | CTYPE_ALPHA | CTYPE_ALNUM, CTYPE_UPPER | CTYPE_ALPHA | CTYPE_ALNUM,
    CTYPE_UPPER | CTYPE_ALPHA | CTYPE_ALNUM, CTYPE_UPPER | CTYPE_ALPHA | CTYPE_ALNUM,
    CTYPE_UPPER | CTYPE_ALPHA | CTYPE_ALNUM, CTYPE_UPPER | CTYPE_ALPHA | CTYPE_ALNUM,
    CTYPE_UPPER | CTYPE_ALPHA | CTYPE_ALNUM, CTYPE_UPPER | CTYPE_ALPHA | CTYPE_ALNUM,
    CTYPE_UPPER | CTYPE_ALPHA | CTYPE_ALNUM, CTYPE_UPPER | CTYPE_ALPHA | CTYPE_ALNUM,
    CTYPE_UPPER | CTYPE_ALPHA | CTYPE_ALNUM, CTYPE_UPPER | CTYPE_ALPHA | CTYPE_ALNUM,
    /* 91..96 punctuation */
    0,0,0,0,0,0,
    /* 97..122 'a'..'z' lowercase */
    CTYPE_LOWER | CTYPE_ALPHA | CTYPE_ALNUM, CTYPE_LOWER | CTYPE_ALPHA | CTYPE_ALNUM,
    CTYPE_LOWER | CTYPE_ALPHA | CTYPE_ALNUM, CTYPE_LOWER | CTYPE_ALPHA | CTYPE_ALNUM,
    CTYPE_LOWER | CTYPE_ALPHA | CTYPE_ALNUM, CTYPE_LOWER | CTYPE_ALPHA | CTYPE_ALNUM,
    CTYPE_LOWER | CTYPE_ALPHA | CTYPE_ALNUM, CTYPE_LOWER | CTYPE_ALPHA | CTYPE_ALNUM,
    CTYPE_LOWER | CTYPE_ALPHA | CTYPE_ALNUM, CTYPE_LOWER | CTYPE_ALPHA | CTYPE_ALNUM,
    CTYPE_LOWER | CTYPE_ALPHA | CTYPE_ALNUM, CTYPE_LOWER | CTYPE_ALPHA | CTYPE_ALNUM,
    CTYPE_LOWER | CTYPE_ALPHA | CTYPE_ALNUM, CTYPE_LOWER | CTYPE_ALPHA | CTYPE_ALNUM,
    CTYPE_LOWER | CTYPE_ALPHA | CTYPE_ALNUM, CTYPE_LOWER | CTYPE_ALPHA | CTYPE_ALNUM,
    CTYPE_LOWER | CTYPE_ALPHA | CTYPE_ALNUM, CTYPE_LOWER | CTYPE_ALPHA | CTYPE_ALNUM,
    CTYPE_LOWER | CTYPE_ALPHA | CTYPE_ALNUM, CTYPE_LOWER | CTYPE_ALPHA | CTYPE_ALNUM,
    CTYPE_LOWER | CTYPE_ALPHA | CTYPE_ALNUM, CTYPE_LOWER | CTYPE_ALPHA | CTYPE_ALNUM,
    CTYPE_LOWER | CTYPE_ALPHA | CTYPE_ALNUM, CTYPE_LOWER | CTYPE_ALPHA | CTYPE_ALNUM,
    CTYPE_LOWER | CTYPE_ALPHA | CTYPE_ALNUM, CTYPE_LOWER | CTYPE_ALPHA | CTYPE_ALNUM,
    /* 123..255: zeros (non-ASCII) */
    0 /* repeat zeros for rest */ 
};

/* Fill the rest of entries with zeros (compiler will do this for static initializers,
   but ensure array size is 256). We'll use a static initializer above and pad with zeros */
static const unsigned short __ctype_table_full[256] = {
    /* copy first elements from __simple_ctype_table explicitly then zeros for remaining */
    /* For brevity we reference the previous table pointer at link time */
};

 /* Export a pointer that some ctype macros expect */
 const unsigned short * const __ctype_ptr__ = __simple_ctype_table;