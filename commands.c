#include "commands.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ctype_helpers.h"
#include "strtok_compat.h"
#include "ui_uart3.h"

#include "tach.h"
#include "pwmin.h"
#include "bothin.h"
#include "tsyn.h"
#include "tach_default.h"

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
    ui_uart3_puts("  PSYN ON     Enable PWM on PF2\r\n");
    ui_uart3_puts("  PSYN OFF    Disable PWM and force PF2 low\r\n");
    ui_uart3_puts("  PHASE1      PWM 24.9kHz@46% + TACHSYN 168Hz@50% on PM3\r\n");
    ui_uart3_puts("  PHASE2      PWM 24.9kHz@54% + TACHSYN 235Hz@50% on PM3\r\n");
    ui_uart3_puts("  PHASE1L     PWM 24.9kHz@15% + TACHSYN 168Hz@50% on PM3\r\n");
    ui_uart3_puts("  PHASE2L     PWM 24.9kHz@21% + TACHSYN 235Hz@50% on PM3\r\n");
    ui_uart3_puts("  PHASE 1/2/1L/2L  (aliases)\r\n");
    ui_uart3_puts("  TSYN BOOT BEGIN  Start boot-profile TACHSYN on PM3\r\n");
    ui_uart3_puts("  TSYN BOOT END    Stop boot-profile\r\n");
    ui_uart3_puts("  TSYN COPY BEGIN  Mirror TACHIN -> TACHOUT (PF1 -> PM3)\r\n");
    ui_uart3_puts("  TSYN COPY END    Stop mirroring and restore TACH DEFAULT\r\n");
    ui_uart3_puts("  TSYN STATUS      Show tach generator status\r\n");
    ui_uart3_puts("  TACHIN ON   Start printing RPM on UART0 every 0.5s\r\n");
    ui_uart3_puts("  TACHIN OFF  Stop printing RPM on UART0\r\n");
    ui_uart3_puts("  PWMIN ON    Start printing PWM-in on UART0 every 1s (f + duty only)\r\n");
    ui_uart3_puts("  PWMIN OFF   Stop printing PWM-in on UART0\r\n");
    ui_uart3_puts("  BOTHIN ON   Start printing combined duty+rpm on UART0 every 1s\r\n");
    ui_uart3_puts("  BOTHIN OFF  Stop printing combined duty+rpm\r\n");
    ui_uart3_puts("  PWMINDBG ON     Enable PWMIN verbose diagnostics on UART0\r\n");
    ui_uart3_puts("  PWMINDBG OFF    Disable PWMIN verbose diagnostics\r\n");
    ui_uart3_puts("  PWMINDBG DUMP   Print one-time PWMIN diagnostic dump\r\n");
    ui_uart3_puts("  TACH LOOPBACK BEGIN  Self-test (jumper PM3->PF1)\r\n");
    ui_uart3_puts("  TACH LOOPBACK END    Stop self-test and restore TACH DEFAULT\r\n");
    ui_uart3_puts("  TACH DEFAULT <1|2|1L|2L|BOOT|COPY>  Persist boot default\r\n");
    ui_uart3_puts("  TACH DEFAULT CURRENT               Show persisted default\r\n");
    ui_uart3_puts("  CLEAR       Clear the terminal screen (ANSI)\r\n");
    ui_uart3_puts("  HELP        This help\r\n");
    ui_uart3_puts("  EXIT        Close UART3 session\r\n");
    ui_uart3_puts("  DEBUG ON    Enable UART0 diagnostics\r\n");
    ui_uart3_puts("  DEBUG OFF   Disable UART0 diagnostics (default)\r\n");
    ui_uart3_puts("  LSAMPLES    Show suppressed (outlier) sample counts\r\n");
    ui_uart3_prompt_once();
}

static void cmd_clear(const char *arg)
{
    if (arg && *arg != '\0') {
        ui_uart3_puts("\r\nERROR: CLEAR takes no arguments\r\n");
        ui_uart3_prompt_once();
        return;
    }

    /* ANSI: clear screen + cursor home. */
    ui_uart3_puts("\x1b[2J\x1b[H");
    ui_uart3_prompt_once();
}

static void cmd_phase_apply(uint32_t pwm_percent, uint32_t tachsyn_hz, const char *label)
{
    if (tachsyn_boot_is_running()) {
        ui_uart3_puts("\r\nWARNING: TSYN BOOT is in progress. PHASE testing commands are not allowed until this completes!\r\n");
        ui_uart3_prompt_once();
        return;
    }
    if (tach_loopback_is_running()) {
        ui_uart3_puts("\r\nWARNING: TACH LOOPBACK is in progress. Stop it before PHASE testing.\r\n");
        ui_uart3_prompt_once();
        return;
    }
    if (tachsyn_copy_is_running()) {
        ui_uart3_puts("\r\nWARNING: TSYN COPY is in progress. Stop it before PHASE testing.\r\n");
        ui_uart3_prompt_once();
        return;
    }

    /* Ensure legacy burst TSYN is off. */
    if (tsyn_is_enabled()) {
        tsyn_set_enabled(false);
    }

    pwm_set_percent(pwm_percent);
    if (!pwm_is_enabled()) {
        pwm_set_enabled(true);
    }

    tachsyn_set_drive_mode(TACHSYN_DRIVE_PUSHPULL);
    tachsyn_set_continuous(tachsyn_hz, 50U);

    ui_uart3_puts("\r\nOK: ");
    ui_uart3_puts(label ? label : "PHASE");
    ui_uart3_puts(" applied\r\n");
    ui_uart3_prompt_once();
}

static void cmd_tsyn(const char *arg1, const char *arg2)
{
    if (!arg1 || *arg1 == '\0') {
        ui_uart3_puts("\r\nERROR: missing value. Use: TSYN STATUS | TSYN BOOT BEGIN|END | TSYN COPY BEGIN|END\r\n");
        ui_uart3_prompt_once();
        return;
    }

    char a1[12];
    size_t i = 0;
    while (arg1[i] && i + 1 < sizeof(a1)) {
        a1[i] = (char)my_toupper((unsigned char)arg1[i]);
        i++;
    }
    a1[i] = '\0';

    char a2[12];
    a2[0] = '\0';
    if (arg2 && *arg2) {
        size_t j = 0;
        while (arg2[j] && j + 1 < sizeof(a2)) {
            a2[j] = (char)my_toupper((unsigned char)arg2[j]);
            j++;
        }
        a2[j] = '\0';
    }

    if (strcmp(a1, "STATUS") == 0) {
        ui_uart3_puts("\r\nSTATUS:\r\n");
        ui_uart3_puts("  TSYN BOOT: ");
        ui_uart3_puts(tachsyn_boot_is_running() ? "RUNNING\r\n" : "STOPPED\r\n");
        ui_uart3_puts("  TSYN COPY: ");
        ui_uart3_puts(tachsyn_copy_is_running() ? "RUNNING\r\n" : "STOPPED\r\n");
        ui_uart3_puts("  TACH LOOPBACK: ");
        ui_uart3_puts(tach_loopback_is_running() ? "RUNNING\r\n" : "STOPPED\r\n");
        ui_uart3_puts("  Legacy burst TSYN: ");
        ui_uart3_puts(tsyn_is_enabled() ? "ON\r\n" : "OFF\r\n");
        ui_uart3_puts("  TACHSYN: ");
        ui_uart3_puts(tachsyn_is_running() ? "RUNNING\r\n" : "STOPPED\r\n");
        ui_uart3_prompt_once();
        return;
    }

    if (strcmp(a1, "BOOT") == 0) {
        if (tach_loopback_is_running()) {
            ui_uart3_puts("\r\nERROR: TACH LOOPBACK is running; stop it before TSYN BOOT\r\n");
            ui_uart3_prompt_once();
            return;
        }
        if (tachsyn_copy_is_running()) {
            ui_uart3_puts("\r\nERROR: TSYN COPY is running; stop it before TSYN BOOT\r\n");
            ui_uart3_prompt_once();
            return;
        }

        if (a2[0] == '\0' || strcmp(a2, "BEGIN") == 0) {
            tachsyn_boot_start();
            ui_uart3_puts("\r\nOK: TSYN BOOT BEGIN\r\n");
            ui_uart3_prompt_once();
            return;
        }

        if (strcmp(a2, "END") == 0) {
            tachsyn_boot_stop();
            ui_uart3_puts("\r\nOK: TSYN BOOT END\r\n");
            ui_uart3_prompt_once();
            return;
        }

        ui_uart3_puts("\r\nERROR: Use TSYN BOOT BEGIN|END\r\n");
        ui_uart3_prompt_once();
        return;
    }

    if (strcmp(a1, "COPY") == 0) {
        if (tach_loopback_is_running()) {
            ui_uart3_puts("\r\nERROR: TACH LOOPBACK is running; stop it before TSYN COPY\r\n");
            ui_uart3_prompt_once();
            return;
        }
        if (tachsyn_boot_is_running()) {
            ui_uart3_puts("\r\nERROR: TSYN BOOT is running; stop it before TSYN COPY\r\n");
            ui_uart3_prompt_once();
            return;
        }

        if (a2[0] == '\0' || strcmp(a2, "BEGIN") == 0) {
            tachsyn_copy_begin();
            ui_uart3_puts("\r\nOK: TSYN COPY BEGIN\r\n");
            ui_uart3_puts("NOTE: PF1 interrupts switch to BOTH edges while COPY is active.\r\n");
            ui_uart3_prompt_once();
            return;
        }

        if (strcmp(a2, "END") == 0) {
            tachsyn_copy_end();
            tach_default_apply();
            ui_uart3_puts("\r\nOK: TSYN COPY END (restored TACH DEFAULT)\r\n");
            ui_uart3_prompt_once();
            return;
        }

        ui_uart3_puts("\r\nERROR: Use TSYN COPY BEGIN|END\r\n");
        ui_uart3_prompt_once();
        return;
    }

    /* Deprecated legacy aliases (keep for compatibility). */
    if (strcmp(a1, "ON") == 0) {
        tachsyn_boot_start();
        ui_uart3_puts("\r\nOK: TSYN ON is deprecated; starting TSYN BOOT BEGIN\r\n");
        ui_uart3_prompt_once();
        return;
    }

    if (strcmp(a1, "OFF") == 0) {
        tachsyn_boot_stop();
        ui_uart3_puts("\r\nOK: TSYN OFF is deprecated; stopping TSYN (BOOT END)\r\n");
        ui_uart3_prompt_once();
        return;
    }

    ui_uart3_puts("\r\nERROR: invalid value. Use: TSYN STATUS | TSYN BOOT BEGIN|END | TSYN COPY BEGIN|END\r\n");
    ui_uart3_prompt_once();
}

static void cmd_pwmin(const char *arg)
{
    if (!arg || *arg == '\0') {
        pwmin_set_reporting(true);
        ui_uart3_puts("\r\nOK: PWMIN ON (printing PWM input on UART0)\r\n");
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
        pwmin_set_reporting(true);
        ui_uart3_puts("\r\nOK: PWMIN ON (printing PWM input on UART0)\r\n");
        ui_uart3_prompt_once();
        return;
    }

    if (strcmp(mode, "OFF") == 0) {
        pwmin_set_reporting(false);
        ui_uart3_puts("\r\nOK: PWMIN OFF\r\n");
        ui_uart3_prompt_once();
        return;
    }

    ui_uart3_puts("\r\nERROR: invalid value. Use: PWMIN ON | PWMIN OFF\r\n");
    ui_uart3_prompt_once();
}

static void cmd_pwmindbg(const char *arg)
{
    if (!arg || *arg == '\0') {
        /* One-time dump without changing persistent verbose state. */
        const bool prev = pwmin_is_verbose();
        pwmin_set_verbose(true);
        pwmin_debug_dump();
        pwmin_set_verbose(prev);
        ui_uart3_puts("\r\nOK: PWMINDBG DUMP printed on UART0\r\n");
        ui_uart3_prompt_once();
        return;
    }

    char mode[12];
    size_t i = 0;
    while (arg[i] && i + 1 < sizeof(mode)) {
        mode[i] = (char)my_toupper((unsigned char)arg[i]);
        i++;
    }
    mode[i] = '\0';

    if (strcmp(mode, "ON") == 0) {
        pwmin_set_verbose(true);
        pwmin_set_reporting(true);
        pwmin_debug_dump();
        ui_uart3_puts("\r\nOK: PWMINDBG ON (verbose enabled; diagnostics printed on UART0)\r\n");
        ui_uart3_prompt_once();
        return;
    }

    if (strcmp(mode, "OFF") == 0) {
        pwmin_set_verbose(false);
        ui_uart3_puts("\r\nOK: PWMINDBG OFF\r\n");
        ui_uart3_prompt_once();
        return;
    }

    if (strcmp(mode, "DUMP") == 0) {
        const bool prev = pwmin_is_verbose();
        pwmin_set_verbose(true);
        pwmin_debug_dump();
        pwmin_set_verbose(prev);
        ui_uart3_puts("\r\nOK: PWMINDBG DUMP printed on UART0\r\n");
        ui_uart3_prompt_once();
        return;
    }

    ui_uart3_puts("\r\nERROR: invalid value. Use: PWMINDBG ON | PWMINDBG OFF | PWMINDBG DUMP\r\n");
    ui_uart3_prompt_once();
}

static void cmd_tach(const char *arg1, const char *arg2)
{
    if (!arg1 || *arg1 == '\0') {
        ui_uart3_puts("\r\nERROR: missing value. Use: TACH LOOPBACK BEGIN|END | TACH DEFAULT ...\r\n");
        ui_uart3_prompt_once();
        return;
    }

    char a1[12];
    size_t i = 0;
    while (arg1[i] && i + 1 < sizeof(a1)) {
        a1[i] = (char)my_toupper((unsigned char)arg1[i]);
        i++;
    }
    a1[i] = '\0';

    char a2[12];
    a2[0] = '\0';
    if (arg2 && *arg2) {
        size_t j = 0;
        while (arg2[j] && j + 1 < sizeof(a2)) {
            a2[j] = (char)my_toupper((unsigned char)arg2[j]);
            j++;
        }
        a2[j] = '\0';
    }

    if (strcmp(a1, "DEFAULT") == 0) {
        if (a2[0] == '\0' || strcmp(a2, "CURRENT") == 0) {
            tach_default_mode_t cur = tach_default_get();
            ui_uart3_puts("\r\nTACH DEFAULT CURRENT: ");
            ui_uart3_puts(tach_default_mode_to_str(cur));
            ui_uart3_puts("\r\n");
            ui_uart3_prompt_once();
            return;
        }

        tach_default_mode_t mode;
        if (strcmp(a2, "1") == 0) mode = TACH_DEFAULT_PHASE1;
        else if (strcmp(a2, "2") == 0) mode = TACH_DEFAULT_PHASE2;
        else if (strcmp(a2, "1L") == 0) mode = TACH_DEFAULT_PHASE1L;
        else if (strcmp(a2, "2L") == 0) mode = TACH_DEFAULT_PHASE2L;
        else if (strcmp(a2, "BOOT") == 0) mode = TACH_DEFAULT_BOOT;
        else if (strcmp(a2, "COPY") == 0) mode = TACH_DEFAULT_COPY;
        else {
            ui_uart3_puts("\r\nERROR: invalid default. Use: TACH DEFAULT <1|2|1L|2L|BOOT|COPY>\r\n");
            ui_uart3_prompt_once();
            return;
        }

        (void)tach_default_set(mode);
        ui_uart3_puts("\r\nOK: TACH DEFAULT set to ");
        ui_uart3_puts(tach_default_mode_to_str(mode));
        ui_uart3_puts("\r\n");
        ui_uart3_prompt_once();
        return;
    }

    if (strcmp(a1, "LOOPBACK") != 0) {
        ui_uart3_puts("\r\nERROR: invalid value. Use: TACH LOOPBACK BEGIN|END | TACH DEFAULT ...\r\n");
        ui_uart3_prompt_once();
        return;
    }

    if (strcmp(a2, "BEGIN") == 0) {
        if (tachsyn_boot_is_running()) {
            ui_uart3_puts("\r\nERROR: TSYN BOOT is running; stop it before LOOPBACK\r\n");
            ui_uart3_prompt_once();
            return;
        }
        if (tachsyn_copy_is_running()) {
            ui_uart3_puts("\r\nERROR: TSYN COPY is running; stop it before LOOPBACK\r\n");
            ui_uart3_prompt_once();
            return;
        }

        /* Use a known safe generator so production techs always get a reference. */
        if (tsyn_is_enabled()) {
            tsyn_set_enabled(false);
        }
        pwm_set_percent(15U);
        if (!pwm_is_enabled()) {
            pwm_set_enabled(true);
        }
        tachsyn_set_drive_mode(TACHSYN_DRIVE_PUSHPULL);
        tachsyn_set_continuous(168U, 50U);

        tach_set_reporting(true);
        tach_loopback_begin(168U);
        tach_set_loopback_expected_hz(168U);

        ui_uart3_puts("\r\nOK: TACH LOOPBACK BEGIN\r\n");
        ui_uart3_puts("NOTE: Install a jumper PM3 (TACH OUT) -> PF1 (TACH IN).\r\n");
        ui_uart3_puts("Watch UART0: pulses~84 per 0.5s and 'OK'.\r\n");
        ui_uart3_prompt_once();
        return;
    }

    if (strcmp(a2, "END") == 0) {
        tach_loopback_end();
        tach_set_loopback_expected_hz(0U);
        tach_set_reporting(false);
        tach_default_apply();
        ui_uart3_puts("\r\nOK: TACH LOOPBACK END (restored TACH DEFAULT)\r\n");
        ui_uart3_prompt_once();
        return;
    }

    ui_uart3_puts("\r\nERROR: Use: TACH LOOPBACK BEGIN|END\r\n");
    ui_uart3_prompt_once();
}

static void cmd_tachin(const char *arg)
{
    if (!arg || *arg == '\0') {
        tach_set_reporting(true);
        ui_uart3_puts("\r\nOK: TACHIN ON (printing RPM on UART0)\r\n");
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
        tach_set_reporting(true);
        ui_uart3_puts("\r\nOK: TACHIN ON (printing RPM on UART0)\r\n");
        ui_uart3_prompt_once();
        return;
    }

    if (strcmp(mode, "OFF") == 0) {
        tach_set_reporting(false);
        ui_uart3_puts("\r\nOK: TACHIN OFF\r\n");
        ui_uart3_prompt_once();
        return;
    }

    ui_uart3_puts("\r\nERROR: invalid value. Use: TACHIN ON | TACHIN OFF\r\n");
    ui_uart3_prompt_once();
}

static void cmd_bothin(const char *arg)
{
    if (!arg || *arg == '\0') {
        bothin_set_enabled(true);
        ui_uart3_puts("\r\nOK: BOTHIN ON (printing duty+rpm on UART0)\r\n");
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
        bothin_set_enabled(true);
        ui_uart3_puts("\r\nOK: BOTHIN ON (printing duty+rpm on UART0)\r\n");
        ui_uart3_prompt_once();
        return;
    }

    if (strcmp(mode, "OFF") == 0) {
        bothin_set_enabled(false);
        ui_uart3_puts("\r\nOK: BOTHIN OFF\r\n");
        ui_uart3_prompt_once();
        return;
    }

    ui_uart3_puts("\r\nERROR: invalid value. Use: BOTHIN ON | BOTHIN OFF\r\n");
    ui_uart3_prompt_once();
}

static void cmd_exit(const char *arg)
{
    if (arg && *arg != '\0') {
        ui_uart3_puts("\r\nERROR: EXIT takes no arguments\r\n");
        ui_uart3_prompt_once();
        return;
    }

    ui_uart3_puts("\r\nClosing session...\r\n");
    uart3_request_disconnect();
    /* No prompt here; session will close and UART0 will emit disconnect diagnostics. */
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

static void cmd_lsamples(const char *arg)
{
    if (arg && *arg != '\0') {
        ui_uart3_puts("\r\nERROR: LSAMPLES takes no arguments\r\n");
        ui_uart3_prompt_once();
        return;
    }

    const uint32_t pwmin_supp = pwmin_get_suppressed_samples();
    const uint32_t tach_supp = tach_get_suppressed_samples();

    char num[11];
    ui_uart3_puts("\r\nLSAMPLES (since boot):\r\n");

    ui_uart3_puts("  PWMIN suppressed=");
    u32_to_dec(num, sizeof(num), pwmin_supp);
    ui_uart3_puts(num);
    ui_uart3_puts("\r\n");

    ui_uart3_puts("  TACH  suppressed=");
    u32_to_dec(num, sizeof(num), tach_supp);
    ui_uart3_puts(num);
    ui_uart3_puts("\r\n");

    ui_uart3_prompt_once();
}

static void cmd_psyn(const char *arg)
{
    if (!arg || *arg == '\0') {
        ui_uart3_puts("\r\nERROR: missing value. Use: PSYN n | PSYN ON | PSYN OFF\r\n");
        ui_uart3_prompt_once();
        return;
    }

    /* Allow PSYN ON/OFF as a convenience when working on the scope. */
    char mode[8];
    size_t mi = 0;
    while (arg[mi] && mi + 1 < sizeof(mode)) {
        mode[mi] = (char)my_toupper((unsigned char)arg[mi]);
        mi++;
    }
    mode[mi] = '\0';

    if (strcmp(mode, "OFF") == 0) {
        pwm_set_enabled(false);
        ui_uart3_puts("\r\nOK: PWM OFF (PF2 forced low)\r\n");
        ui_uart3_prompt_once();
        return;
    }
    if (strcmp(mode, "ON") == 0) {
        pwm_set_enabled(true);
        ui_uart3_puts("\r\nOK: PWM ON\r\n");
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
    /* If PWM was previously disabled for scope/debug, numeric PSYN turns it back on. */
    if (!pwm_is_enabled()) {
        pwm_set_enabled(true);
    }

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

    if (strcmp(tok, "CLEAR") == 0) {
        cmd_clear(strtok_r(NULL, " \t", &saveptr));
        return;
    }

    if (strcmp(tok, "DEBUG") == 0) {
        cmd_debug(strtok_r(NULL, " \t", &saveptr));
        return;
    }

    if (strcmp(tok, "LSAMPLES") == 0) {
        cmd_lsamples(strtok_r(NULL, " \t", &saveptr));
        return;
    }

    if (strcmp(tok, "TACHIN") == 0) {
        cmd_tachin(strtok_r(NULL, " \t", &saveptr));
        return;
    }

    if (strcmp(tok, "BOTHIN") == 0) {
        cmd_bothin(strtok_r(NULL, " \t", &saveptr));
        return;
    }

    if (strcmp(tok, "TACH") == 0) {
        cmd_tach(strtok_r(NULL, " \t", &saveptr), strtok_r(NULL, " \t", &saveptr));
        return;
    }

    if (strcmp(tok, "PWMIN") == 0) {
        cmd_pwmin(strtok_r(NULL, " \t", &saveptr));
        return;
    }

    if (strcmp(tok, "PWMINDBG") == 0) {
        cmd_pwmindbg(strtok_r(NULL, " \t", &saveptr));
        return;
    }

    if (strcmp(tok, "PHASE") == 0) {
        char *arg = strtok_r(NULL, " \t", &saveptr);
        if (!arg || *arg == '\0') {
            ui_uart3_puts("\r\nERROR: PHASE requires 1 | 2 | 1L | 2L (or use PHASE1 etc)\r\n");
            ui_uart3_prompt_once();
            return;
        }

        for (char *p = arg; *p; ++p) *p = (char)my_toupper((unsigned char)*p);

        if (strcmp(arg, "1") == 0) { cmd_phase_apply(46U, 168U, "PHASE1"); return; }
        if (strcmp(arg, "2") == 0) { cmd_phase_apply(54U, 235U, "PHASE2"); return; }
        if (strcmp(arg, "1L") == 0) { cmd_phase_apply(15U, 168U, "PHASE1L"); return; }
        if (strcmp(arg, "2L") == 0) { cmd_phase_apply(21U, 235U, "PHASE2L"); return; }

        ui_uart3_puts("\r\nERROR: invalid PHASE. Use: PHASE 1 | PHASE 2 | PHASE 1L | PHASE 2L\r\n");
        ui_uart3_prompt_once();
        return;
    }

    if (strcmp(tok, "PHASE1") == 0) { cmd_phase_apply(46U, 168U, "PHASE1"); return; }
    if (strcmp(tok, "PHASE2") == 0) { cmd_phase_apply(54U, 235U, "PHASE2"); return; }
    if (strcmp(tok, "PHASE1L") == 0) { cmd_phase_apply(15U, 168U, "PHASE1L"); return; }
    if (strcmp(tok, "PHASE2L") == 0) { cmd_phase_apply(21U, 235U, "PHASE2L"); return; }

    if (strcmp(tok, "TSYN") == 0) {
        cmd_tsyn(strtok_r(NULL, " \t", &saveptr), strtok_r(NULL, " \t", &saveptr));
        return;
    }

    if (strcmp(tok, "TACH") == 0) {
        cmd_tach(strtok_r(NULL, " \t", &saveptr), strtok_r(NULL, " \t", &saveptr));
        return;
    }

    if (strcmp(tok, "EXIT") == 0) {
        cmd_exit(strtok_r(NULL, " \t", &saveptr));
        return;
    }

    ui_uart3_puts("\r\nERROR: unknown command. Type HELP\r\n");
    ui_uart3_prompt_once();
}
