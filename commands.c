#include "commands.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ctype_helpers.h"
#include "strtok_compat.h"
#include "ui_uart3.h"

#ifndef PSYN_MIN
#define PSYN_MIN 5
#endif
#ifndef PSYN_MAX
#define PSYN_MAX 96
#endif

static void u32_to_dec(char *out, size_t out_sz, uint32_t value)
{
    if (!out || out_sz == 0) return;

    char tmp[10];
    uint32_t n = value;
    size_t i = 0;
    do {
        tmp[i++] = (char)('0' + (n % 10U));
        n /= 10U;
    } while (n != 0U && i < sizeof(tmp));

    size_t pos = 0;
    while (i > 0 && (pos + 1) < out_sz) {
        out[pos++] = tmp[--i];
    }
    out[pos] = '\0';
}

static void cmd_help(void)
{
    ui_uart3_puts("\r\nAvailable commands:\r\n");
    ui_uart3_puts("  PSYN n      Set PWM duty (n=5..96)\r\n");
    ui_uart3_puts("  HELP        This help\r\n");
    ui_uart3_puts("  DEBUG ON    Enable UART0 diagnostics\r\n");
    ui_uart3_puts("  DEBUG OFF   Disable UART0 diagnostics (default)\r\n");
    ui_uart3_prompt_once();
}

static void cmd_debug(const char *arg)
{
    if (!arg || *arg == '\0') {
        ui_uart3_puts("\r\nERROR: missing value. Use: DEBUG ON | DEBUG OFF\r\n");
        ui_uart3_prompt_once();
        return;
    }

    char mode[8];
    size_t i = 0;
    while (arg[i] && i + 1 < sizeof(mode)) {
        mode[i] = (char)my_toupper((unsigned char)arg[i]);
        i++;
    }
    mode[i] = '\0';

    if (strcmp(mode, "ON") == 0) {
        debug_set_enabled(true);
        ui_uart3_puts("\r\nOK: DEBUG ON\r\n");
        ui_uart3_prompt_once();
        return;
    }

    if (strcmp(mode, "OFF") == 0) {
        debug_set_enabled(false);
        ui_uart3_puts("\r\nOK: DEBUG OFF\r\n");
        ui_uart3_prompt_once();
        return;
    }

    ui_uart3_puts("\r\nERROR: invalid value. Use: DEBUG ON | DEBUG OFF\r\n");
    ui_uart3_prompt_once();
}

static void cmd_psyn(const char *arg)
{
    if (!arg || *arg == '\0') {
        ui_uart3_puts("\r\nERROR: missing value. Use: PSYN n  (n=5..96)\r\n");
        ui_uart3_prompt_once();
        return;
    }

    char *endptr = NULL;
    long val = strtol(arg, &endptr, 10);
    if (!endptr || *endptr != '\0') {
        ui_uart3_puts("\r\nERROR: invalid number. Use: PSYN n\r\n");
        ui_uart3_prompt_once();
        return;
    }

    if (val < PSYN_MIN || val > PSYN_MAX) {
        ui_uart3_puts("\r\nERROR: value out of range (5..96)\r\n");
        ui_uart3_prompt_once();
        return;
    }

    pwm_set_percent((uint32_t)val);

    /* Avoid snprintf (newlib stalls were previously observed). */
    char num[11];
    u32_to_dec(num, sizeof(num), (uint32_t)val);
    ui_uart3_puts("\r\nOK: duty set to ");
    ui_uart3_puts(num);
    ui_uart3_puts("%\r\n");
    ui_uart3_prompt_once();
}

void commands_process_line(const char *line)
{
    if (!line) {
        ui_uart3_prompt_once();
        return;
    }

    while (*line && my_isspace((unsigned char)*line)) line++;
    if (*line == '\0') {
        ui_uart3_prompt_once();
        return;
    }

    char buf[128];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr = NULL;
    char *tok = strtok_r(buf, " \t", &saveptr);
    if (!tok) {
        ui_uart3_prompt_once();
        return;
    }

    for (char *p = tok; *p; ++p) *p = (char)my_toupper((unsigned char)*p);

    if (strcmp(tok, "PSYN") == 0) {
        cmd_psyn(strtok_r(NULL, " \t", &saveptr));
        return;
    }

    if (strcmp(tok, "HELP") == 0) {
        cmd_help();
        return;
    }

    if (strcmp(tok, "DEBUG") == 0) {
        cmd_debug(strtok_r(NULL, " \t", &saveptr));
        return;
    }

    ui_uart3_puts("\r\nERROR: unknown command. Type HELP\r\n");
    ui_uart3_prompt_once();
}
