/*
  Integrated PWM + UART (reconnect welcome & single-prompt fixes)

  Changes in this revision (per your request):
  - When the host reconnects (i.e. the session was marked inactive and we
    receive a character), the firmware prints exactly one Welcome+prompt.
    It will not repeatedly resend the welcome while the host is idle.
  - Restored strict "single > prompt" behavior: pressing Enter on an empty
    line will reprint the prompt only if it is not already the last thing
    printed. No forced prompt-printing that causes duplicates.
  - Kept previous features:
      * uppercase-as-you-type (echoed and stored),
      * PSYN range enforcement (5..96) with red ERROR messages,
      * green OK responses,
      * backspace/delete editing that cannot erase the prompt prefix,
      * prompt deduplication via last_output_was_prompt flag,
      * no added large buffers or layout changes.

  Notes:
  - Reconnection welcome is printed exactly once at the moment we detect the
    session transitioning from inactive -> active (i.e. on first received byte,
    before normal input processing proceeds).
  - We do NOT periodically resend the welcome while idle anymore (that previously
    caused repeated welcome messages when the user opened the terminal later).
*/

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "inc/hw_memmap.h"
#include "inc/hw_types.h"

#include "driverlib/rom.h"
#include "driverlib/rom_map.h"

#include "driverlib/sysctl.h"
#include "driverlib/gpio.h"
#include "driverlib/pin_map.h"
#include "driverlib/pwm.h"
#include "driverlib/uart.h"

/* --- Configuration --- */
#define TARGET_PWM_FREQ_HZ       21500U
#define INITIAL_DUTY_PERCENT     30U

#define UART_BAUD                115200U
#define LINEBUF_SIZE             128

/* Timing */
#define DISCONNECT_MS            5000U   /* session considered inactive after 5s idle */
#define IDLE_SLEEP_MS            10U     /* coarse ms step for polling */

/* ANSI color sequences */
#define ANSI_RESET      "\x1B[0m"
#define ANSI_WELCOME    "\x1B[1;36m"   /* bright cyan */
#define ANSI_PROMPT     "\x1B[1;33m"   /* bright yellow (used for prompt) */
#define ANSI_RESPONSE   "\x1B[0;32m"   /* green */
#define ANSI_ERROR      "\x1B[1;31m"   /* bright red */

/* Prompt symbol */
#define PROMPT_SYMBOL   "> "

/* PSYN limits (updated) */
#define PSYN_MIN 5
#define PSYN_MAX 96

/* --- Globals (PWM) --- */
static uint32_t g_sysClock = 0;
static uint32_t g_pwmPeriod = 0;
static uint32_t g_pwmPulse  = 0;

/* --- UART line buffer and state --- */
static char linebuf[LINEBUF_SIZE];
static uint32_t linepos = 0;

/* coarse millisecond counter advanced in main loop */
static volatile uint32_t g_coarse_ms = 0;

/* last receive timestamp (ms coarse). Used to detect inactivity */
static volatile uint32_t g_last_rx_ms = 0;

/* session state flags */
static volatile bool g_session_active = true;          /* true while host considered connected/active */
static volatile bool g_sent_initial_welcome = false;  /* true after boot welcome printed */

/* Track whether the last thing printed to the host was the prompt symbol.
   This prevents duplicate prompt printing when code paths would otherwise
   produce two prompts in a row. */
static bool last_output_was_prompt = false;

/* Forward declarations */
static void SetupClock(void);
static void SetupPWM_PB6(void);
static void SetupUART0(void);
static void uart_char_put_blocking(char c);
static void uart_send_raw(const char *s);
static void output_puts(const char *s);
static void output_putc(char c);
static void prompt_print_once(void);
static void set_pwm_percent(uint32_t percent);
static void handle_line_and_respond(const char *line);
static void send_welcome_and_prompt_once(void);
static void uart_poll_loop_with_editing(void);
static void uart_line_notify_current(const char *cur_line, uint32_t len);

/* --- Low-level helpers --- */

static void uart_char_put_blocking(char c)
{
    while (!ROM_UARTCharPutNonBlocking(UART0_BASE, c)) {
        ROM_SysCtlDelay(10);
    }
}

static void uart_send_raw(const char *s)
{
    while (*s) {
        uart_char_put_blocking(*s++);
    }
}

/* output helpers clear the prompt flag so a subsequent prompt will print */
static void output_puts(const char *s)
{
    last_output_was_prompt = false;
    uart_send_raw(s);
}

static void output_putc(char c)
{
    last_output_was_prompt = false;
    uart_char_put_blocking(c);
}

/* Print the colored prompt once (no-op if it was last output) */
static void prompt_print_once(void)
{
    if (!last_output_was_prompt) {
        uart_send_raw(ANSI_PROMPT);
        uart_send_raw(PROMPT_SYMBOL);
        uart_send_raw(ANSI_RESET);
        last_output_was_prompt = true;
    }
}

/* Realtime notify stub */
static void uart_line_notify_current(const char *cur_line, uint32_t len)
{
    (void)cur_line;
    (void)len;
}

/* --- Peripheral setup and PWM logic --- */

static void SetupClock(void)
{
    ROM_SysCtlClockSet(SYSCTL_SYSDIV_2_5 | SYSCTL_USE_PLL |
                      SYSCTL_XTAL_16MHZ | SYSCTL_OSC_MAIN);
    g_sysClock = ROM_SysCtlClockGet();
}

static void SetupPWM_PB6(void)
{
    ROM_SysCtlPeripheralEnable(SYSCTL_PERIPH_PWM0);
    ROM_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOB);

    while (!ROM_SysCtlPeripheralReady(SYSCTL_PERIPH_PWM0) ||
           !ROM_SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOB)) { }

    ROM_GPIOPinConfigure(GPIO_PB6_M0PWM0);
    ROM_GPIOPinTypePWM(GPIO_PORTB_BASE, GPIO_PIN_6);

    MAP_PWMClockSet(PWM0_BASE, PWM_SYSCLK_DIV_1);

    uint32_t pwmClock = g_sysClock;
    uint32_t period = (pwmClock + (TARGET_PWM_FREQ_HZ/2U)) / TARGET_PWM_FREQ_HZ;
    if (period == 0) period = 1;
    if (period > 0xFFFF) period = 0xFFFF;
    g_pwmPeriod = period;

    uint32_t pulse = (uint32_t)(((uint64_t)period * INITIAL_DUTY_PERCENT) / 100U);
    if (pulse >= period) pulse = period - 1;
    if (pulse == 0) pulse = 1;
    g_pwmPulse = pulse;

    MAP_PWMGenConfigure(PWM0_BASE, PWM_GEN_0, PWM_GEN_MODE_DOWN | PWM_GEN_MODE_NO_SYNC);
    MAP_PWMGenPeriodSet(PWM0_BASE, PWM_GEN_0, period);
    MAP_PWMPulseWidthSet(PWM0_BASE, PWM_OUT_0, pulse);
    MAP_PWMOutputState(PWM0_BASE, PWM_OUT_0_BIT, true);
    MAP_PWMGenEnable(PWM0_BASE, PWM_GEN_0);
}

static void SetupUART0(void)
{
    ROM_SysCtlPeripheralEnable(SYSCTL_PERIPH_UART0);
    ROM_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOA);
    ROM_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOF);

    while (!ROM_SysCtlPeripheralReady(SYSCTL_PERIPH_UART0) ||
           !ROM_SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOA) ||
           !ROM_SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOF)) { }

    ROM_GPIOPinConfigure(GPIO_PA0_U0RX);
    ROM_GPIOPinConfigure(GPIO_PA1_U0TX);
    ROM_GPIOPinTypeUART(GPIO_PORTA_BASE, GPIO_PIN_0 | GPIO_PIN_1);

    ROM_GPIOPinTypeGPIOOutput(GPIO_PORTF_BASE, GPIO_PIN_2);
    ROM_GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_2, 0);

    ROM_UARTConfigSetExpClk(UART0_BASE, g_sysClock, UART_BAUD,
                            (UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE | UART_CONFIG_PAR_NONE));

    while (ROM_UARTCharsAvail(UART0_BASE)) {
        (void)ROM_UARTCharGetNonBlocking(UART0_BASE);
    }

    /* session considered active at start; boot welcome sent separately */
    g_session_active = true;
    g_sent_initial_welcome = false;
    g_last_rx_ms = 0;
}

/* Compute and program PWM pulse safely */
static void set_pwm_percent(uint32_t percent)
{
    if (g_pwmPeriod == 0) g_pwmPeriod = 1;
    uint32_t pulse = (uint32_t)(((uint64_t)g_pwmPeriod * (uint64_t)percent) / 100U);
    if (pulse >= g_pwmPeriod) pulse = g_pwmPeriod - 1;
    if (pulse == 0) pulse = 1;

    MAP_PWMPulseWidthSet(PWM0_BASE, PWM_OUT_0, pulse);
    g_pwmPulse = pulse;
}

/* --- Command handling --- */

static void handle_line_and_respond(const char *line)
{
    char buf[LINEBUF_SIZE];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr = NULL;
    char *tok = strtok_r(buf, " \t", &saveptr);
    if (!tok) {
        prompt_print_once();
        return;
    }

    if (strcmp(tok, "PSYN") != 0) {
        output_puts(ANSI_ERROR);
        output_puts("\r\nERROR: unknown command\r\n");
        output_puts(ANSI_RESET);
        //prompt_print_once();
        return;
    }

    char *numtok = strtok_r(NULL, " \t", &saveptr);
    if (!numtok) {
        output_puts(ANSI_ERROR);
        output_puts("\r\nERROR: missing parameter\r\n");
        output_puts(ANSI_RESET);
        //prompt_print_once();
        return;
    }

    char *endptr = NULL;
    long val = strtol(numtok, &endptr, 10);
    if (endptr == NULL || *endptr != '\0') {
        output_puts(ANSI_ERROR);
        output_puts("\r\nERROR: invalid number\r\n");
        output_puts(ANSI_RESET);
        //prompt_print_once();
        return;
    }

    if (val < PSYN_MIN || val > PSYN_MAX) {
        char err[64];
        int n = snprintf(err, sizeof(err), "\r\nERROR: value out of range (%d..%d)\r\n", PSYN_MIN, PSYN_MAX);
        (void)n;
        output_puts(ANSI_ERROR);
        output_puts(err);
        output_puts(ANSI_RESET);
        //prompt_print_once();
        return;
    }

    set_pwm_percent((uint32_t)val);

    char ack[64];
    int n = snprintf(ack, sizeof(ack), "\r\nOK: duty set to %ld%%\r\n", val);
    (void)n;
    output_puts(ANSI_RESPONSE);
    output_puts(ack);
    output_puts(ANSI_RESET);
    //prompt_print_once();
}

/* Print welcome and prompt once (used both at boot and on reconnect) */
static void send_welcome_and_prompt_once(void)
{
    output_puts(ANSI_WELCOME);
    output_puts("\r\nUART (ICDI) console ready. Type: PSYN <n> (5..96)\r\n");
    last_output_was_prompt = false;
    prompt_print_once();
}

/* --- Main UART polling loop --- */
static void uart_poll_loop_with_editing(void)
{
    /* Boot: print welcome once */
    if (!g_sent_initial_welcome) {
        send_welcome_and_prompt_once();
        g_sent_initial_welcome = true;
    }

    for (;;) {
        int ch = ROM_UARTCharGetNonBlocking(UART0_BASE);
        if (ch != -1) {
            char c = (char)ch;

            /* Update last RX time */
            g_last_rx_ms = g_coarse_ms;

            /* If session was inactive, treat this as reconnect: print welcome once */
            if (!g_session_active) {
                g_session_active = true;
                linepos = 0;
                send_welcome_and_prompt_once(); /* single welcome+prompt on reconnect */
            }

            /* Toggle PF2 on receive */
            uint32_t cur = ROM_GPIOPinRead(GPIO_PORTF_BASE, GPIO_PIN_2);
            ROM_GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_2, (cur ? 0 : GPIO_PIN_2));

            /* Backspace/delete */
            if (c == '\b' || c == 0x7F) {
                if (linepos > 0) {
                    linepos--;
                    output_puts("\b \b");
                    uart_line_notify_current(linebuf, linepos);
                } else {
                    output_puts("\a");
                    last_output_was_prompt = true;
                }
                continue;
            }

            /* Enter/CR/LF */
            if (c == '\r' || c == '\n') {
                /* Echo CRLF so terminal advances (keeps behavior consistent) */
                //output_puts("\r\n");
                if (linepos > 0) {
                    linebuf[linepos] = '\0';
                    if (g_session_active) handle_line_and_respond(linebuf);
                    linepos = 0;
                } else {
                    /* empty line: reprint prompt only if it wasn't the last output */
                    prompt_print_once();
                }
                continue;
            }

            /* Printable: uppercase-as-you-type, echo and store */
            if ((unsigned char)c >= 32) {
                char uc = (char)toupper((unsigned char)c);
                output_putc(uc);
                if (linepos + 1 < LINEBUF_SIZE) {
                    linebuf[linepos++] = uc;
                    uart_line_notify_current(linebuf, linepos);
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
            /* No char available: detect inactivity and advance coarse ms */
            if (g_session_active) {
                if (g_last_rx_ms != 0 && ((g_coarse_ms - g_last_rx_ms) >= DISCONNECT_MS)) {
                    g_session_active = false;
                }
            }
            ROM_SysCtlDelay((ROM_SysCtlClockGet() / 1000U) * IDLE_SLEEP_MS / 3U);
            g_coarse_ms += IDLE_SLEEP_MS;
        }
    }
}

/* --- Main --- */
int main(void)
{
    SetupClock();
    SetupPWM_PB6();
    SetupUART0();

    uart_poll_loop_with_editing();

    return 0;
}
