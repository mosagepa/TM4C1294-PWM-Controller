#include "ui_uart3.h"

#include <stdint.h>
#include <string.h>

#include "cmdline.h" /* ANSI_* + PROMPT_SYMBOL + UARTSend/UARTDev */

/* Extra ANSI colors for the rainbow banner (ESP32 reference style). */
#define ANSI_RED          "\x1B[31m"
#define ANSI_YELLOW_BOLD  "\x1B[93m"
#define ANSI_GREEN        "\x1B[32m"
#define ANSI_CYAN         "\x1B[36m"
#define ANSI_MAGENTA      "\x1B[35m"
#define ANSI_BLUE         "\x1B[34m"
#define ANSI_WHITE        "\x1B[37m"
#define ANSI_BOLD_GREEN   "\x1B[1;32m"

static bool g_last_output_was_prompt = false;
static bool g_session_welcome_printed = false;

static void uart3_send_cstr(const char *s);

static void ui_uart3_print_rainbow_banner(void)
{
    /*
     * Keep this banner implementation extremely simple and deterministic.
     * Avoid libc-heavy helpers (strstr, variable-length pointer math), since
     * session-begin output must never stall the MCU.
     */
    static const char banner[] =
        ANSI_WHITE "=== "
        ANSI_BOLD_GREEN "IBM PS FAN CONTROL"
        ANSI_WHITE " (c) 2025 by Purposeful Designs, Inc. === "
        /* Rainbow-ish "--- booting ---" (spaces preserved) */
        ANSI_RED "-" ANSI_YELLOW_BOLD "-" ANSI_GREEN "-" ANSI_WHITE " "
        ANSI_CYAN "b" ANSI_MAGENTA "o" ANSI_BLUE "o" ANSI_RED "t" ANSI_YELLOW_BOLD "i" ANSI_GREEN "n" ANSI_CYAN "g"
        ANSI_WHITE " "
        ANSI_MAGENTA "-" ANSI_BLUE "-" ANSI_RED "-"
        ANSI_RESET "\r\n";

    UARTSend((const uint8_t *)banner, (uint32_t)(sizeof(banner) - 1U), UARTDEV_USER);
}

static void uart3_send_cstr(const char *s)
{
    if (!s) return;
    size_t n = strlen(s);
    if (n == 0) return;
    UARTSend((const uint8_t *)s, (uint32_t)n, UARTDEV_USER);
}

void ui_uart3_prompt_force_next(void)
{
    g_last_output_was_prompt = false;
}

void ui_uart3_puts(const char *s)
{
    g_last_output_was_prompt = false;
    uart3_send_cstr(s);
}

void ui_uart3_prompt_once(void)
{
    if (g_last_output_was_prompt) return;

    uart3_send_cstr(ANSI_PROMPT);
    uart3_send_cstr(PROMPT_SYMBOL);
    uart3_send_cstr(ANSI_RESET);

    g_last_output_was_prompt = true;
}

void ui_uart3_session_begin(void)
{
    /* Start-of-session always permits a welcome+prompt. */
    g_session_welcome_printed = false;
    g_last_output_was_prompt = false;

    if (!g_session_welcome_printed) {
        ui_uart3_print_rainbow_banner();
        ui_uart3_puts(ANSI_WELCOME);
        ui_uart3_puts("PWM Ready. Commands: PSYN n | HELP | EXIT\r\n");
        ui_uart3_puts(ANSI_RESET);
        ui_uart3_prompt_once();
        g_session_welcome_printed = true;
    }
}
