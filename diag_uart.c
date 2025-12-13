
/*
 * diag_uart.c - consolidated diagnostic UART helpers and memory/allocator diagnostics
 *
 * Replace the existing diag_uart.c with this file. It merges the earlier pieces
 * into one consistent, self-contained implementation.
 *
 * Usage:
 * - Compile and link this file into your firmware (add to SRCS/OBJS).
 * - Call diag_print_memory_layout(), diag_sbrk_probe(), diag_test_malloc_*(),
 *   diag_print_full_mem_state(), diag_print_variables_summary(), or diag_print_variable()
 *   from main/context code (not from ISRs) to send diagnostics to ICDI UART0.
 *
 * Notes:
 * - This file writes diagnostic output using UART0 (ICDI) via UARTCharPut - blocking.
 * - Ensure linker defines the symbols: _end_bss, _heap_start, _heap_end, _stack_top,
 *   and that _sbrk(ptrdiff_t) is present (syscalls).
 * - If you want to inspect application globals in diag_print_variables_summary(),
 *   ensure those variables have external linkage (not static).
 */

#include "diag_uart.h"

#include "cmdline.h"

#include "inc/hw_memmap.h"

#include "driverlib/uart.h"
#include "driverlib/gpio.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/sysctl.h"

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h> /* ptrdiff_t, size_t, NULL */
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#include <string.h>
#include <inttypes.h>
#include <stdio.h>

/* Fallback if main doesn't define this */
#ifndef UART_RX_BUF_SIZE
#define UART_RX_BUF_SIZE 64
#endif

/* Linker-provided section symbols (must be in your linker script) */
extern char _end_bss;
extern char _heap_start;
extern char _heap_end;
extern char _stack_top;

/* _sbrk syscall implemented in syscalls.c / newlib syscalls */
extern void * _sbrk(ptrdiff_t incr);

/* Optional debug counter maintained in your _sbrk implementation */
extern volatile unsigned int sbrk_calls;


/* --- We'll be using these heap-based formatting helpers --- */
/* Make sure to replace stack snprintf calls in main.c and other files :

   Search for any char msgbuf[....] + snprintf usage and
   replace with diag_snprintf_heap_send or diag_vasprintf_heap + UARTSend
    so no large stack frames remain.
*/

char *diag_vasprintf_heap(const char *fmt, va_list ap_in)
{
    va_list ap;
    va_copy(ap, ap_in);

    /* determine required size (C99: vsnprintf(NULL,0,...) returns needed length) */
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (needed < 0) return NULL;

    /* safety cap */
    if ((size_t)needed + 1 > DIAG_FMT_MAX_ALLOC) return NULL;

    size_t bufsize = (size_t)needed + 1;
    char *buf = (char *)malloc(bufsize);
    if (!buf) return NULL;

    va_list ap2;
    va_copy(ap2, ap_in);
    int r = vsnprintf(buf, bufsize, fmt, ap2);
    va_end(ap2);

    if (r < 0) { free(buf); return NULL; }

    return buf; /* caller must free() */
}

char *diag_asprintf_heap(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *p = diag_vasprintf_heap(fmt, ap);
    va_end(ap);
    return p;
}

/* Format into heap and send via ICDI (UART0). Frees buffer. */
int diag_snprintf_heap_send(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *buf = diag_vasprintf_heap(fmt, ap);
    va_end(ap);

    if (!buf) return -1;

    int len = (int)strlen(buf);
    /* Send directly to UART0 (ICDI) using ROM functions */
    for (int i = 0; i < len; i++) {
        ROM_UARTCharPut(UART0_BASE, buf[i]);
    }
    free(buf);
    return len;
}    
/* --- end formatting helpers --- */

/* Simple sprintf implementation without vsnprintf (avoids runtime stall) */
static int diag_simple_sprintf(char *str, int max_size, const char *format, va_list ap)
{
    char *dest = str;
    const char *src = format;
    int written = 0;
    
    while (*src && written < max_size - 1) {
        if (*src != '%') {
            *dest++ = *src++;
            written++;
        } else {
            src++; /* skip '%' */
            if (*src == 's') {
                /* %s - string */
                char *s = va_arg(ap, char*);
                if (s) {
                    while (*s && written < max_size - 1) {
                        *dest++ = *s++;
                        written++;
                    }
                }
                src++;
            } else if (*src == 'd') {
                /* %d - integer */
                int val = va_arg(ap, int);
                char num_buf[12];
                int num_len = 0;
                
                /* Handle negative numbers */
                if (val < 0) {
                    if (written < max_size - 1) {
                        *dest++ = '-';
                        written++;
                    }
                    val = -val;
                }
                
                /* Convert to string (reverse order) */
                do {
                    num_buf[num_len++] = '0' + (val % 10);
                    val /= 10;
                } while (val > 0 && num_len < 11);
                
                /* Copy digits in correct order */
                for (int i = num_len - 1; i >= 0 && written < max_size - 1; i--) {
                    *dest++ = num_buf[i];
                    written++;
                }
                src++;
            } else if (*src == 'p') {
                /* %p - pointer */
                void *ptr = va_arg(ap, void*);
                uintptr_t addr = (uintptr_t)ptr;
                char hex_buf[20];
                int hex_len = 0;
                
                /* Add "0x" prefix */
                if (written < max_size - 2) {
                    *dest++ = '0';
                    *dest++ = 'x';
                    written += 2;
                }
                
                /* Convert to hex (reverse order) */
                do {
                    int digit = addr & 0xF;
                    hex_buf[hex_len++] = (digit < 10) ? ('0' + digit) : ('A' + digit - 10);
                    addr >>= 4;
                } while (addr > 0 && hex_len < 16);
                
                /* Copy hex digits in correct order */
                for (int i = hex_len - 1; i >= 0 && written < max_size - 1; i--) {
                    *dest++ = hex_buf[i];
                    written++;
                }
                src++;
            } else if (*src == '%') {
                /* %% - literal % */
                if (written < max_size - 1) {
                    *dest++ = '%';
                    written++;
                }
                src++;
            } else {
                /* Unknown format - just copy the % and char */
                if (written < max_size - 1) {
                    *dest++ = '%';
                    written++;
                }
                if (*src && written < max_size - 1) {
                    *dest++ = *src++;
                    written++;
                }
            }
        }
    }
    
    *dest = '\0';
    return written;
}

/* Standard library replacements using our proven UART mechanics */

/* Simple sprintf replacement: basic formatting without vsnprintf */
int sprintf(char *str, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int result = diag_simple_sprintf(str, 320, format, ap);
    va_end(ap);
    return result;
}

/* snprintf replacement: format into provided buffer with size limit */
int snprintf(char *str, size_t size, const char *format, ...)
{
    if (size == 0) return 0;
    
    va_list ap;
    va_start(ap, format);
    int result = diag_simple_sprintf(str, (int)size, format, ap);
    va_end(ap);
    return result;
}

/* printf replacement: format and send directly to UART */
int printf(const char *format, ...)
{
    char buffer[320];
    va_list ap;
    va_start(ap, format);
    int len = diag_simple_sprintf(buffer, sizeof(buffer), format, ap);
    va_end(ap);
    
    if (len > 0) {
        UARTSend((uint8_t*)buffer, len, UARTDEV_ICDI);
    }
    return len;
}


/* ------------------ Memory Protection Diagnostics ------------------ */

/* External symbols from linker script */
extern char _heap_start, _heap_end, _stack_bottom, _stack_top;
extern char _end_bss;

/* Get current stack pointer */
static uint32_t get_stack_pointer(void)
{
    uint32_t sp;
    __asm volatile ("mov %0, sp" : "=r" (sp));
    return sp;
}

/* Check for memory region overlaps and corruption */
void diag_check_memory_integrity(const char *context)
{
    uint32_t heap_start = (uint32_t)&_heap_start;
    uint32_t heap_end = (uint32_t)&_heap_end;
    uint32_t stack_bottom = (uint32_t)&_stack_bottom;
    uint32_t stack_top = (uint32_t)&_stack_top;
    uint32_t current_sp = get_stack_pointer();
    uint32_t bss_end = (uint32_t)&_end_bss;
    
    diag_puts("=== MEMORY INTEGRITY CHECK (");
    diag_puts(context);
    diag_puts(") ===\r\n");
    diag_puts("BSS End:       0x"); diag_put_hex32(bss_end); diag_puts("\r\n");
    diag_puts("Heap Start:    0x"); diag_put_hex32(heap_start); diag_puts("\r\n");
    diag_puts("Heap End:      0x"); diag_put_hex32(heap_end); diag_puts("\r\n");
    diag_puts("Stack Bottom:  0x"); diag_put_hex32(stack_bottom); diag_puts("\r\n");
    diag_puts("Stack Top:     0x"); diag_put_hex32(stack_top); diag_puts("\r\n");
    diag_puts("Current SP:    0x"); diag_put_hex32(current_sp); diag_puts("\r\n");
    
    /* Check for overlaps */
    int overlap_detected = 0;
    
    /* Check heap doesn't overlap stack */
    if (heap_end > stack_bottom) {
        diag_puts("*** CRITICAL: HEAP-STACK OVERLAP! ***\r\n");
        overlap_detected = 1;
    }
    
    /* Check stack hasn't grown into heap */
    if (current_sp < heap_end) {
        diag_puts("*** CRITICAL: STACK-HEAP COLLISION! ***\r\n");
        overlap_detected = 1;
    }
    
    /* Check stack overflow */
    if (current_sp < stack_bottom) {
        diag_puts("*** CRITICAL: STACK OVERFLOW! ***\r\n");
        overlap_detected = 1;
    }
    
    if (overlap_detected) {
        diag_puts("*** SYSTEM HALTED - MEMORY CORRUPTION DETECTED ***\r\n");
        /* Halt system - in real application you might want controlled shutdown */
        while (1) {
            /* Flash LED or other indication */
            ROM_SysCtlDelay(ROM_SysCtlClockGet() / 10); /* 100ms delay */
        }
    } else {
        diag_puts("Memory integrity: OK\r\n");
    }
    
    diag_puts("Stack usage: "); diag_put_u32_dec((uint32_t)diag_stack_bytes_used()); diag_puts(" bytes\r\n");
    diag_puts("Heap usage:  "); diag_put_u32_dec((uint32_t)diag_heap_bytes_used()); diag_puts(" bytes\r\n");
    diag_puts("================================\r\n");
}

/* Check stack usage and warn if getting close to limit */
void diag_check_stack_usage(const char *function_name)
{
    int stack_used = diag_stack_bytes_used();
    int stack_total = (uint32_t)&_stack_top - (uint32_t)&_stack_bottom;
    int stack_remaining = stack_total - stack_used;
    
    diag_puts("Stack check [");
    diag_puts(function_name);
    diag_puts("]: ");
    diag_put_u32_dec((uint32_t)stack_used);
    diag_puts("/");
    diag_put_u32_dec((uint32_t)stack_total);
    diag_puts(" bytes used (");
    diag_put_u32_dec((uint32_t)stack_remaining);
    diag_puts(" remaining)\r\n");
    
    /* Warn if stack usage > 75% */
    if (stack_used > (stack_total * 3) / 4) {
        diag_puts("*** WARNING: Stack usage > 75% in ");
        diag_puts(function_name);
        diag_puts(" ***\r\n");
    }
    
    /* Critical if stack usage > 90% */
    if (stack_used > (stack_total * 9) / 10) {
        diag_puts("*** CRITICAL: Stack usage > 90% in ");
        diag_puts(function_name);
        diag_puts(" ***\r\n");
        diag_check_memory_integrity(function_name);
    }
}

/* Get current stack usage in bytes */
int diag_stack_bytes_used(void)
{
    uint32_t current_sp = get_stack_pointer();
    uint32_t stack_top = (uint32_t)&_stack_top;
    return (int)(stack_top - current_sp);
}

/* Get current heap usage in bytes (simplified version) */
int diag_heap_bytes_used(void)
{
    /* Simple estimation - would need malloc_simple.c integration for accuracy */
    uint32_t heap_start = (uint32_t)&_heap_start;
    /* Remove unused variable to fix warning */
    
    /* This is a placeholder - real implementation would track malloc'd bytes */
    /* For now just show heap region size */
    return 0; /* TODO: integrate with malloc tracking */
}


/* ------------------ Low-level output helpers ------------------ */

/* Basic low-level blocking char put to UART0 (ICDI) */

/* PUTC NON-BLOCKING VERSION USING HEAP ::: */
/* Bounded non-blocking-ish put: wait for transmitter not-busy with timeout.
   Returns 0 on success, -1 on timeout. */
static int diag_putc_nb(char c)
{
    const uint32_t max_loops = 20000; /* tune: larger => longer wait before giving up */
    uint32_t loops = 0;

    /* Wait until UART isn't fully busy OR timeout */
    while (ROM_UARTBusy(UART0_BASE)) {
        /* small delay to allow USB/host to drain the FIFO */
        ROM_SysCtlDelay(ROM_SysCtlClockGet() / 200000U);
        if (++loops >= max_loops) return -1;
    }

    /* Now write the character (non-blocking put); ROM wrapper is used for portability */
    ROM_UARTCharPutNonBlocking(UART0_BASE, c);
    return 0;
}

/* diag_putc: write one character using simple blocking approach */
void diag_putc(char c)
{
    /* Use simple blocking put to avoid clock dependency issues */
    ROM_UARTCharPut(UART0_BASE, c);
}
/* --- end diag_putc implementations --- */


/* Output NUL-terminated string (uses diag_putc) */
void diag_puts(const char *s)
{
    while (s && *s) diag_putc(*s++);
}

/* Print a 32-bit hex value as 0xXXXXXXXX */
void diag_put_hex32(uint32_t v)
{
    const char hex[] = "0123456789ABCDEF";
    diag_puts("0x");
    for (int i = 7; i >= 0; --i) {
        uint8_t nib = (v >> (i * 4)) & 0xF;
        diag_putc(hex[nib]);
    }
}

/* Print pointer value using 32-bit hex (works for Cortex-M) */
void diag_put_ptr(const void *p)
{
    diag_put_hex32((uint32_t)(uintptr_t)p);
}

/* helper: print unsigned decimal (up to 32-bit) */
void diag_put_u32_dec(uint32_t v)
{
    char buf[12];
    int i = 0;
    if (v == 0) {
        diag_putc('0');
        return;
    }
    while (v) {
        buf[i++] = '0' + (v % 10);
        v /= 10;
    }
    while (i--) diag_putc(buf[i]);
}

/* ------------------ GPIO pulse helpers ------------------ */
static void gpio_pulse_enter(void)
{
    /* PN0 must be configured as output by main before using these probes */
    ROM_GPIOPinWrite(GPIO_PORTN_BASE, GPIO_PIN_0, GPIO_PIN_0);
}

static void gpio_pulse_exit(void)
{
    ROM_GPIOPinWrite(GPIO_PORTN_BASE, GPIO_PIN_0, 0);
}

/* ------------------ sbrk / malloc diagnostics ------------------ */

/* Probe sbrk by calling _sbrk(0) and a small _sbrk(64) request and printing results.
   Safe to call from main (not from ISR). */
void diag_sbrk_probe(void)
{
    diag_puts("\r\n--- SBRK PROBE ---\r\n");

    diag_puts("_heap_start= "); diag_put_ptr((void*)&_heap_start); diag_puts("\r\n");
    diag_puts("_heap_end  = "); diag_put_ptr((void*)&_heap_end); diag_puts("\r\n");

    void *cur = _sbrk(0);
    diag_puts("sbrk(0)   = "); diag_put_ptr(cur); diag_puts("\r\n");

    gpio_pulse_enter();

    void *p = _sbrk(64);
    if (p == (void*)-1) {
        diag_puts("sbrk(64) failed\r\n");
    } else {
        diag_puts("sbrk(64) -> "); diag_put_ptr(p); diag_puts("\r\n");
    }

    gpio_pulse_exit();

    void *cur2 = _sbrk(0);
    diag_puts("sbrk(0) after = "); diag_put_ptr(cur2); diag_puts("\r\n");
    diag_puts("--- SBRK PROBE END ---\r\n");
}

/* Diagnostic malloc/realloc loop with PN0 toggles to visualize allocation duration */
void diag_test_malloc_with_gpio(void)
{
    diag_puts("\r\n--- MALLOC+GPIO TEST ---\r\n");
    char *p = NULL;
    size_t size = 32;
    for (int i = 0; i < 12; ++i) {
        gpio_pulse_enter();
        char *q = (char *)realloc(p, size);
        gpio_pulse_exit();

        if (!q) {
            diag_puts("realloc failed size=");
            diag_put_u32_dec((uint32_t)size);
            diag_puts("\r\n");
            break;
        }
        p = q;
        diag_puts("realloc OK size=");
        diag_put_u32_dec((uint32_t)size);
        diag_puts(" ptr=");
        diag_put_ptr(p);
        diag_puts(" sbrk(0)=");
        diag_put_ptr(_sbrk(0));
        diag_puts(" sbrk_calls=");
        diag_put_u32_dec(sbrk_calls);
        diag_puts("\r\n");
        size *= 2;
    }

    if (p) free(p);
    diag_puts("--- MALLOC+GPIO TEST END ---\r\n");
}

/* ------------------ memory layout and simple diagnostics ------------------ */

/* Print memory layout: end_bss, heap_start, heap_end, stack_top, current SP, sbrk(0) */
void diag_print_memory_layout(void)
{
    diag_puts("\r\n--- MEMORY LAYOUT ---\r\n");
    diag_puts("_end_bss   = "); diag_put_ptr((void*)&_end_bss); diag_puts("\r\n");
    diag_puts("_heap_start= "); diag_put_ptr((void*)&_heap_start); diag_puts("\r\n");
    diag_puts("_heap_end  = "); diag_put_ptr((void*)&_heap_end); diag_puts("\r\n");
    diag_puts("_stack_top = "); diag_put_ptr((void*)&_stack_top); diag_puts("\r\n");

    void *cur = _sbrk(0);
    diag_puts("sbrk(0)    = "); diag_put_ptr(cur); diag_puts("\r\n");

    void *sp = NULL;
    __asm__ volatile ("mov %0, sp" : "=r" (sp));
    diag_puts("SP         = "); diag_put_ptr(sp); diag_puts("\r\n");

    diag_puts("sbrk_calls = "); diag_put_u32_dec(sbrk_calls); diag_puts("\r\n");
}

/* Print only sbrk info */
void diag_print_sbrk_info(void)
{
    void *cur = _sbrk(0);
    diag_puts("sbrk(0) = "); diag_put_ptr(cur); diag_puts("\r\n");
    diag_puts("sbrk_calls = "); diag_put_u32_dec(sbrk_calls); diag_puts("\r\n");
}

/* Simple malloc/realloc stress test that reports via UART (no GPIO) */
void diag_test_malloc_sequence(void)
{
    diag_puts("\r\n--- MALLOC TEST ---\r\n");
    char *p = NULL;
    size_t size = 32;
    for (int i = 0; i < 12; ++i) {
        char *q = (char *)realloc(p, size);
        if (!q) {
            diag_puts("realloc failed at size=");
            diag_put_u32_dec((uint32_t)size);
            diag_puts("\r\n");
            break;
        }
        p = q;
        diag_puts("allocated size=");
        diag_put_u32_dec((uint32_t)size);
        diag_puts(" ptr=");
        diag_put_ptr(p);
        diag_puts("\r\n");
        /* fill memory to exercise writes */
        for (size_t j = 0; j < size; ++j) p[j] = (char)(j & 0xFF);
        size *= 2;
    }
    if (p) free(p);
    diag_puts("--- MALLOC TEST END ---\r\n");
}

/* ------------------ full mem/state dump with previews ------------------ */

/* Small hex nibble helper */
static void diag_put_hex8(uint8_t v)
{
    const char hex[] = "0123456789ABCDEF";
    diag_putc(hex[(v >> 4) & 0xF]);
    diag_putc(hex[v & 0xF]);
}

/* Bounded hex-dump (max 64 bytes) */
static void diag_hexdump(const void *addr, size_t len)
{
    if (!addr) {
        diag_puts("<NULL>\r\n");
        return;
    }
    const uint8_t *p = (const uint8_t *)addr;
    const size_t max = 64;
    if (len > max) len = max;

    for (size_t i = 0; i < len; ++i) {
        if ((i & 0x0F) == 0) {
            diag_puts("\r\n");
            diag_put_ptr((void *)(uintptr_t)(p + i));
            diag_puts(": ");
        }
        diag_put_hex8(p[i]);
        diag_puts(" ");
    }
    diag_puts("\r\n");
}

/* Full memory & runtime state dump (call from main after UART is ready) */
void diag_print_full_mem_state(void)
{
    diag_puts("\r\n=== FULL MEM STATE ===\r\n");

    /* Section & heap symbols */
    diag_puts("_end_bss    = "); diag_put_ptr((void*)&_end_bss); diag_puts("\r\n");
    diag_puts("_heap_start = "); diag_put_ptr((void*)&_heap_start); diag_puts("\r\n");
    diag_puts("_heap_end   = "); diag_put_ptr((void*)&_heap_end); diag_puts("\r\n");
    diag_puts("_stack_top  = "); diag_put_ptr((void*)&_stack_top); diag_puts("\r\n");

    /* current heap break */
    void *cur_brk = _sbrk(0);
    diag_puts("sbrk(0)     = "); diag_put_ptr(cur_brk); diag_puts("\r\n");
    diag_puts("sbrk_calls  = "); diag_put_u32_dec(sbrk_calls); diag_puts("\r\n");

    /* Current SP */
    void *sp = NULL;
    __asm__ volatile ("mov %0, sp" : "=r" (sp));
    diag_puts("SP (inst)   = "); diag_put_ptr(sp); diag_puts("\r\n");

    /* Free heap estimate */
    if ((uintptr_t)cur_brk <= (uintptr_t)&_heap_end) {
        uint32_t free_bytes = (uint32_t)((uintptr_t)&_heap_end - (uintptr_t)cur_brk);
        diag_puts("free heap   = "); diag_put_u32_dec(free_bytes); diag_puts(" bytes\r\n");
    } else {
        diag_puts("free heap   = <invalid: brk > heap_end>\r\n");
    }

    /* Memory previews */
    diag_puts("\r\n-- Memory previews --\r\n");
    diag_puts("heap_start preview:");
    diag_hexdump((void*)&_heap_start, 32);

    diag_puts("sbrk(0) preview:");
    diag_hexdump(cur_brk, 32);

    diag_puts("stack (near SP) preview:");
    diag_hexdump(sp, 32);

    diag_puts("=== END FULL MEM STATE ===\r\n");
}

/* ------------------ variable inspection helpers ------------------ */

/* Determine region for an address */
static const char * diag_addr_region(const void *addr)
{
    uintptr_t a = (uintptr_t)addr;
    uintptr_t heap_start = (uintptr_t)&_heap_start;
    uintptr_t heap_end   = (uintptr_t)&_heap_end;
    uintptr_t bss_start  = (uintptr_t)&_end_bss;
    uintptr_t stack_top  = (uintptr_t)&_stack_top;

    void *spv = NULL;
    __asm__ volatile ("mov %0, sp" : "=r" (spv));
    uintptr_t sp = (uintptr_t)spv;

    if (a >= heap_start && a < heap_end) return "heap";
    if (a >= bss_start && a < heap_start) return "bss/data";
    if (a <= stack_top && a >= (sp > 32768 ? sp - 32768 : 0)) return "stack";
    return "unknown";
}


/* Print a single variable name + address + region and small value preview */
void diag_print_variable(const char *name, const void *addr, size_t size, size_t preview_limit)
{
    diag_puts(name);
    diag_puts(" = ");
    diag_put_ptr(addr);
    diag_puts("  [");
    diag_puts(diag_addr_region(addr));
    diag_puts("]  size=");
    diag_put_u32_dec((uint32_t)size);
    diag_puts("  ");

    uint32_t v = 0;

    if (size == 4) {

        memcpy(&v, addr, sizeof(v));
        diag_puts("val=");
        diag_put_hex32(v);
        diag_puts(" (");
        diag_put_u32_dec(v);
        diag_puts(")");

    } else if (size == 2) {

        /* for 16-bit */
        diag_puts("val=0x");
        const char hex[] = "0123456789ABCDEF";
        uint16_t v16 = (uint16_t)v;
        diag_putc(hex[(v16 >> 12) & 0xF]);
        diag_putc(hex[(v16 >> 8) & 0xF]);
        diag_putc(hex[(v16 >> 4) & 0xF]);
        diag_putc(hex[v16 & 0xF]);

    } else if (size == 1) {

        /* for 8-bit */
        diag_puts("val=0x");
        const char hex[] = "0123456789ABCDEF";
        uint8_t v8 = (uint8_t)v;
        diag_putc(hex[(v8 >> 4) & 0xF]);
        diag_putc(hex[v8 & 0xF]);

    } else {

        /* Decide how many bytes to show:
           - If caller asked for DIAG_PREVIEW_NOLIMIT, print the full 'size' value
             (but we still enforce an absolute safety cap to prevent runaway).
           - If caller passed DIAG_PREVIEW_LIMIT or any other small value, use that as cap.
        */

        const size_t ABSOLUTE_MAX = 65536; /* safety cap: change if needed */
        size_t n;

        if (preview_limit == DIAG_PREVIEW_NOLIMIT) {

            /* full view requested */
            diag_puts("full view (printing entire region):");
            /* cap to ABSOLUTE_MAX for safety */
            n = (size > ABSOLUTE_MAX) ? ABSOLUTE_MAX : size;

        } else if (preview_limit == 0) {

            /* treat 0 as equivalent to DIAG_PREVIEW_LIMIT fallback */
            n = (size > DIAG_PREVIEW_LIMIT) ? DIAG_PREVIEW_LIMIT : size;

        } else {

            /* caller supplied explicit limit */
            n = (size > preview_limit) ? preview_limit : size;

        }

        /* Now print n bytes (diag_hexdump itself should already be rate-limited) */
        diag_puts("preview:");
        diag_hexdump(addr, n);

        /* If we truncated the region because of ABSOLUTE_MAX, notify the user */
        if (n < size) {
            diag_puts("[truncated]");
        }

        diag_puts("\r\n");

        return;

    }

    diag_puts("\r\n");

}


/* Replace diag_print_variables_summary with this (no app-specific externs) */
void diag_print_variables_summary(void)
{
    diag_puts("\r\n=== VARIABLES SUMMARY (generic) ===\r\n");

    /* basic memory layout */
    diag_puts("_end_bss    = "); diag_put_ptr((void*)&_end_bss); diag_puts("\r\n");
    diag_puts("_heap_start = "); diag_put_ptr((void*)&_heap_start); diag_puts("\r\n");
    diag_puts("_heap_end   = "); diag_put_ptr((void*)&_heap_end); diag_puts("\r\n");
    diag_puts("_stack_top  = "); diag_put_ptr((void*)&_stack_top); diag_puts("\r\n");

    /* current heap break and free estimate */
    void *cur = _sbrk(0);
    diag_puts("sbrk(0)     = "); diag_put_ptr(cur); diag_puts("\r\n");
    if ((uintptr_t)cur <= (uintptr_t)&_heap_end) {
        uint32_t freeb = (uint32_t)((uintptr_t)&_heap_end - (uintptr_t)cur);
        diag_puts("free heap   = "); diag_put_u32_dec(freeb); diag_puts(" bytes\r\n");
    } else {
        diag_puts("free heap   = <invalid>\r\n");
    }

    /* Also print instantaneous SP (stack pointer) */
    void *sp = NULL;
    __asm__ volatile ("mov %0, sp" : "=r" (sp));
    diag_puts("SP (inst)   = "); diag_put_ptr(sp); diag_puts("\r\n");

    diag_puts("=== END VARIABLES SUMMARY ===\r\n");
}


