
#ifndef DIAG_UART_H
#define DIAG_UART_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>

/* Preview control:
   - DIAG_PREVIEW_LIMIT : use default truncated preview (32 bytes)
   - DIAG_PREVIEW_NOLIMIT: ask for full dump of 'size' bytes (uses size param) */

#define DIAG_PREVIEW_LIMIT   ((size_t)32)
#define DIAG_PREVIEW_NOLIMIT ((size_t)-1)

/* Added for avoiding snprintf problems... */
#define DIAG_FMT_MAX_ALLOC (4096)    /* a conservative default (4KB) */


/* Call after UART0 is configured. These routines write directly to UART0 (ICDI). */
void diag_putc(char c);
void diag_puts(const char *s);
void diag_put_hex32(uint32_t v);
void diag_put_u32_dec(uint32_t v);
void diag_put_ptr(const void *p);

/* Prototypes for snprintf redirection to its lightweight, non-blocking
  UART implementation (we're not following default naive NewLib integrations) */
char* diag_vasprintf_heap(const char *fmt, va_list ap);
char* diag_asprintf_heap(const char *fmt, ...);
int diag_snprintf_heap_send(const char *fmt, ...);

/* Standard library replacements using our UART mechanics */
int sprintf(char *str, const char *format, ...);
int snprintf(char *str, size_t size, const char *format, ...);
int printf(const char *format, ...);

/* Diagnostic tests — implemented in diag_uart.c / diag_sbrk_probe.c */
void diag_test_malloc_with_gpio(void);
void diag_sbrk_probe(void);
void diag_test_malloc_sequence(void);

void diag_print_memory_layout(void);
void diag_print_sbrk_info(void);

void diag_print_variable(const char *name, const void *addr, size_t size, size_t preview_limit);
void diag_print_variables_summary(void);

/* backward compatible wrapper that uses the default preview limit */
static inline void diag_print_variable_default(const char *name, const void *addr, size_t size)
{
    /* Default to the limited preview (DIAG_PREVIEW_LIMIT) */
    diag_print_variable(name, addr, size, DIAG_PREVIEW_LIMIT);
}


#endif /* DIAG_UART_H */

