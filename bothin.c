#include "bothin.h"

#include <stdbool.h>
#include <stdint.h>

#include "inc/hw_memmap.h"

#include "driverlib/rom.h"

#include "timebase.h"
#include "pwmin.h"
#include "tach.h"
#include "psu_tachmap.h"

static volatile bool g_bothin_enabled = false;
static uint32_t g_next_report_ms = 0;
static uint32_t g_suppress_reports = 0;

static bool g_prev_pwmin_reporting = false;
static bool g_prev_pwmin_printing = false;
static bool g_prev_tach_reporting = false;
static bool g_prev_tach_printing = false;

static void uart0_puts(const char *s)
{
    if (!s) return;
    while (*s) {
        ROM_UARTCharPut(UART0_BASE, *s++);
    }
}

static void uart0_put_u32(uint32_t v)
{
    char buf[11];
    uint32_t n = v;
    uint32_t i = 0;

    do {
        buf[i++] = (char)('0' + (n % 10U));
        n /= 10U;
    } while (n != 0U && i < sizeof(buf));

    while (i > 0) {
        ROM_UARTCharPut(UART0_BASE, buf[--i]);
    }
}

static void uart0_put_u32_1dp(uint32_t value_x10)
{
    uart0_put_u32(value_x10 / 10U);
    uart0_puts(".");
    uart0_put_u32(value_x10 % 10U);
}

static void uart0_put_u32_zpad5(uint32_t v)
{
    if (v > 99999U) {
        uart0_put_u32(v);
        return;
    }

    uint32_t div = 10000U;
    while (div > 0U) {
        uint32_t digit = (v / div) % 10U;
        ROM_UARTCharPut(UART0_BASE, (char)('0' + digit));
        div /= 10U;
    }
}

void bothin_set_enabled(bool enabled)
{
    if (enabled) {
        if (g_bothin_enabled) return;

        g_prev_pwmin_reporting = pwmin_is_reporting();
        g_prev_pwmin_printing = pwmin_is_printing();
        g_prev_tach_reporting = tach_is_reporting();
        g_prev_tach_printing = tach_is_printing();

        /* Ensure both measurement engines are running, but suppress their
           individual prints so we only emit the combined line. */
        pwmin_set_reporting_ex(true, false);
        tach_set_reporting_ex(true, false);

        g_next_report_ms = timebase_millis() + 1000U;
        g_suppress_reports = 2U;
        g_bothin_enabled = true;
        return;
    }

    if (!g_bothin_enabled) return;

    g_bothin_enabled = false;

    pwmin_set_reporting_ex(g_prev_pwmin_reporting, g_prev_pwmin_printing);
    tach_set_reporting_ex(g_prev_tach_reporting, g_prev_tach_printing);
}

bool bothin_is_enabled(void)
{
    return g_bothin_enabled;
}

void bothin_task(void)
{
    if (!g_bothin_enabled) return;

    uint32_t now_ms = timebase_millis();
    if ((int32_t)(now_ms - g_next_report_ms) < 0) {
        return;
    }
    g_next_report_ms += 1000U;

    if (g_suppress_reports > 0U) {
        g_suppress_reports--;
        return;
    }

    uint32_t duty_x10 = 0U;
    bool have_duty = pwmin_get_last_duty_x10(&duty_x10);

    uint32_t rpm = 0U;
    bool have_rpm = tach_get_last_rpm(&rpm);

    uart0_puts("BOTHIN");
    const char *regime_tag = psu_tachmap_get_regime_tag();
    if (psu_tachmap_is_enabled() && regime_tag) {
        uart0_puts(" [");
        uart0_puts(regime_tag);
        uart0_puts("]");
    }
    uart0_puts(": duty=");
    if (have_duty) {
        uart0_put_u32_1dp(duty_x10);
        uart0_puts("%");
    } else {
        uart0_puts("0.0%");
    }

    uart0_puts(" rpm=");
    if (have_rpm) {
        uart0_put_u32_zpad5(rpm);
    } else {
        uart0_puts("00000");
    }
    uart0_puts("\r\n");
}
