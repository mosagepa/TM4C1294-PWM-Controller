/*
 * cmdline.c - Tiva UART3 command-line module (fixed includes / externs)
 *
 * Drop this file into the project (replace previous cmdline.c).
 * After adding to the Makefile, rebuild.
 *
 * Notes:
 * - main.c must expose set_pwm_percent() with external linkage (remove 'static').
 * - If you keep the old USER UART ISR or old process_user_line(), remove them to avoid duplicate symbols.
 */

#include "cmdline.h"   

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

/* Use the project's strtok_compat if present */
#include "strtok_compat.h"

/* Tiva DriverLib */
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/uart.h"
#include "driverlib/gpio.h"
#include "driverlib/sysctl.h"

#include "inc/hw_memmap.h"


void UARTSend(const uint8_t *pui8Buffer, uint32_t ui32Count, UARTDev destUART)
{
    while (ui32Count--) {
        if (destUART == UARTDEV_USER) MAP_UARTCharPut(UART3_BASE, *pui8Buffer++);
        else MAP_UARTCharPut(UART0_BASE, *pui8Buffer++);
    }
}


/* set_pwm_percent must be visible (non-static) in main.c for linking */
extern void set_pwm_percent(uint32_t percent);

/* Session / prompt state */
static volatile bool g_session_active = false;
static volatile bool g_sent_welcome = false;
static bool last_output_was_prompt = false;

/* Local line buffer */
static char linebuf_local[UART_RX_BUF_SIZE];
static uint32_t linepos = 0;

/* Low-level blocking write helpers to UART3 (USER) */
static void uart3_putc_blocking(char c)
{
    /* blocking put */
    ROM_UARTCharPut(UART3_BASE, c);
}
static void uart3_puts_blocking(const char *s)
{
    while (*s) uart3_putc_blocking(*s++);
}

/* mark that we just printed non-prompt */
static void output_puts(const char *s)
{
    last_output_was_prompt = false;
    uart3_puts_blocking(s);
}
/*
static void output_putc(char c)
{
    last_output_was_prompt = false;
    uart3_putc_blocking(c);
}
*/

/* print prompt once */
static void prompt_print_once(void)
{
    if (!last_output_was_prompt) {
        uart3_puts_blocking(ANSI_PROMPT);
        uart3_puts_blocking(PROMPT_SYMBOL);
        uart3_puts_blocking(ANSI_RESET);
        last_output_was_prompt = true;
    }
}

/* optional hook to update an on-screen live preview (no-op here) */
static void uart_line_notify_current(const char *cur_line, uint32_t len)
{
    (void)cur_line;
    (void)len;
}

/* welcome and prompt */
static void send_welcome_and_prompt_once(void)
{
    output_puts(ANSI_WELCOME);
    output_puts("\r\nPWM Ready. Enter command: PSYN n  (n = 5..96)\r\n");
    last_output_was_prompt = false;
    prompt_print_once();
}

/* echo a backspace erase sequence */
static void uart_echo_bs(void)
{
    ROM_UARTCharPutNonBlocking(UART3_BASE, '\b');
    ROM_UARTCharPutNonBlocking(UART3_BASE, ' ');
    ROM_UARTCharPutNonBlocking(UART3_BASE, '\b');
}

/* Handle a complete line (NUL-terminated). Prints response and prompt. */
static void handle_line_and_respond(const char *line)
{
    if (!line) { prompt_print_once(); return; }

    /* Trim leading whitespace */
    while (*line && isspace((unsigned char)*line)) line++;

    if (*line == '\0') { prompt_print_once(); return; }

    char copy[UART_RX_BUF_SIZE];
    strncpy(copy, line, sizeof(copy)-1);
    copy[sizeof(copy)-1] = '\0';

    char *saveptr = NULL;
    char *tok = strtok_r(copy, " \t", &saveptr);
    if (!tok) { prompt_print_once(); return; }

    for (char *p = tok; *p; ++p) *p = (char)toupper((unsigned char)*p);

    if (strcmp(tok, "PSYN") == 0) {
        char *arg = strtok_r(NULL, " \t", &saveptr);
        if (!arg) {
            output_puts(ANSI_ERROR);
            output_puts("\r\nERROR: missing value. Use: PSYN n (5..96)\r\n");
            output_puts(ANSI_RESET);
            prompt_print_once();
            return;
        }
        char *endp = NULL;
        long val = strtol(arg, &endp, 10);
        if (*endp != '\0') {
            output_puts(ANSI_ERROR);
            output_puts("\r\nERROR: invalid number. Use: PSYN n\r\n");
            output_puts(ANSI_RESET);
            prompt_print_once();
            return;
        }
        if (val < PSYN_MIN || val > PSYN_MAX) {
            output_puts(ANSI_ERROR);
            output_puts("\r\nERROR: value out of range (5..96)\r\n");
            output_puts(ANSI_RESET);
            prompt_print_once();
            return;
        }

        /* Apply PWM change via external function */
        set_pwm_percent((uint32_t)val);

        /* Acknowledge using UARTSend so it goes through the project's wrapper */
        char ack[64];
        int n = snprintf(ack, sizeof(ack), "\r\nOK: duty set to %ld%%\r\n", val);
        if (n > 0) UARTSend((const uint8_t *)ack, (uint32_t)n, UARTDEV_USER);

        prompt_print_once();
        return;
    }

    /* Unknown command */
    output_puts(ANSI_ERROR);
    output_puts("\r\nERROR: unknown command. Use: PSYN n or HELP\r\n");
    output_puts(ANSI_RESET);
    prompt_print_once();
}

/* Public API: initialization */
void cmdline_init(void)
{
    g_session_active = true;
    g_sent_welcome = false;
    last_output_was_prompt = false;
    linepos = 0;
}

/* Run the session until DTR disconnect (returns when disconnect detected).
   This uses UART3 non-blocking reads and returns when GPIO PQ1 indicates session end. */
void cmdline_run_until_disconnect(void)
{
    if (!g_sent_welcome) {
        send_welcome_and_prompt_once();
        g_sent_welcome = true;
    }

    for (;;) {
        /* Exit if DTR indicates disconnect (same polarity used in main) */
        if (ROM_GPIOPinRead(GPIO_PORTQ_BASE, GPIO_PIN_1)) {
            /* session ended */
            return;
        }

        int ch = ROM_UARTCharGetNonBlocking(UART3_BASE);
        if (ch != -1) {
            char c = (char)ch;

            /* Toggle PF4 LED on receive (same behavior as original) */
            static uint8_t led = 0;
            led = !led;
            ROM_GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_4, led ? GPIO_PIN_4 : 0);

            /* Backspace / DEL */
            if (c == '\b' || c == 0x7F) {
                if (linepos > 0) {
                    linepos--;
                    uart_echo_bs();
                    uart_line_notify_current(linebuf_local, linepos);
                } else {
                    ROM_UARTCharPutNonBlocking(UART3_BASE, '\a');
                    last_output_was_prompt = true;
                }
                continue;
            }

            /* Ctrl-U: kill line */
            if (c == 0x15) {
                while (linepos > 0) {
                    linepos--;
                    uart_echo_bs();
                }
                continue;
            }

            /* CR / LF */
            if (c == '\r' || c == '\n') {
                if (linepos > 0) {
                    linebuf_local[linepos] = '\0';
                    ROM_UARTCharPutNonBlocking(UART3_BASE, '\r');
                    ROM_UARTCharPutNonBlocking(UART3_BASE, '\n');
                    handle_line_and_respond(linebuf_local);
                    linepos = 0;
                } else {
                    prompt_print_once();
                }
                continue;
            }

            /* Printable char: uppercase-as-you-type */
            if ((unsigned char)c >= 32) {
                char uc = (char)toupper((unsigned char)c);
                ROM_UARTCharPutNonBlocking(UART3_BASE, uc);
                if (linepos + 1 < UART_RX_BUF_SIZE) {
                    linebuf_local[linepos++] = uc;
                    uart_line_notify_current(linebuf_local, linepos);
                } else {
                    output_puts("\r\n");
                    output_puts(ANSI_ERROR);
                    output_puts("ERROR: line too long\r\n");
                    output_puts(ANSI_RESET);
                    prompt_print_once();
                    linepos = 0;
                }
            }
        } else {
            /* small sleep to avoid busy-waiting */
            ROM_SysCtlDelay(ROM_SysCtlClockGet() / 3000);
        }
    }
}
