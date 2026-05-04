#include "psu_tachmap.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "inc/hw_memmap.h"
#include "driverlib/rom.h"
#include "driverlib/uart.h"

#include "pwmin.h"
#include "timebase.h"
#include "tsyn.h"
#include "tach_default.h"

/* Platform callback — implemented in main.c. */
extern void pwm_set_percent(uint32_t percent);

#define PSU_TACHMAP_PULSES_PER_REV 2U

/* --------------------------------------------------------------------------
 * Regime table  (sorted ascending by entry_duty_x10 — MUST stay sorted)
 * --------------------------------------------------------------------------
 * synth_rpm = what IBM "expects to see" at this operating point.
 * Add/remove rows here only; algorithm adapts automatically.
 */
static const psu_regime_t g_regimes[] = {
    /* tag          entry_duty_x10   synth_rpm (midpoint of observed real-fan range)  */
    { "COLD BOOT",  430,              9000 },  /* IBM ~45%  real fan 7500–10500  mid=9000  */
    { "ESXi ON",    480,             12000 },  /* IBM ~51%  real fan 10500–13500 mid=12000 */
    { "VCSA ON",    530,             14000 },  /* IBM ~55%  real fan 13500–14500 mid=14000 */
    { ">ONE VM",    560,             15600 },  /* IBM >57%  real fan 14500–16700 mid=15600 */
};
#define REGIME_COUNT ((uint32_t)(sizeof(g_regimes) / sizeof(g_regimes[0])))

static volatile bool     g_enabled         = false;
static volatile int32_t  g_curr_regime_idx = -1;
static volatile uint32_t g_hyst_x10        = PSU_TACHMAP_HYST_X10_DEFAULT;
static volatile uint32_t g_fan_fixed_duty  = PSU_TACHMAP_FAN_FIXED_DUTY_DEFAULT;
static uint32_t          g_last_freq_hz    = 0;

/* ---- UART0 helpers (non-blocking; same pattern as bothin.c) ------------- */

static void u0_putc(char c)
{
    (void)ROM_UARTCharPutNonBlocking(UART0_BASE, (uint8_t)c);
}

static void u0_puts(const char *s)
{
    if (!s) return;
    while (*s) { u0_putc(*s++); }
}

static void u0_put_u32(uint32_t v)
{
    char buf[11];
    uint32_t n = v, i = 0;
    do { buf[i++] = (char)('0' + (n % 10U)); n /= 10U; } while (n && i < sizeof(buf));
    while (i > 0) { u0_putc(buf[--i]); }
}

static void u0_put_duty_1dp(uint32_t duty_x10)
{
    u0_put_u32(duty_x10 / 10U);
    u0_putc('.');
    u0_putc((char)('0' + (duty_x10 % 10U)));
    u0_putc('%');
}

/* ---- internal helpers ---------------------------------------------------- */

static uint32_t rpm_to_hz(uint32_t rpm)
{
    if (rpm == 0U) return 0U;
    uint64_t num = (uint64_t)rpm * PSU_TACHMAP_PULSES_PER_REV;
    uint32_t hz  = (uint32_t)((num + 30ULL) / 60ULL);
    return hz ? hz : 1U;
}

/* Highest regime index whose entry_duty_x10 <= duty_x10; -1 if below all. */
static int32_t regime_for_duty(uint32_t duty_x10)
{
    int32_t result = -1;
    for (uint32_t i = 0; i < REGIME_COUNT; i++) {
        if (duty_x10 >= (uint32_t)g_regimes[i].entry_duty_x10) {
            result = (int32_t)i;
        }
    }
    return result;
}

static void emit_regime_change(int32_t from_idx, int32_t to_idx, uint32_t duty_x10)
{
    const char *from_tag = (from_idx >= 0 && (uint32_t)from_idx < REGIME_COUNT)
                           ? g_regimes[from_idx].tag : "NONE";
    const char *to_tag   = (to_idx   >= 0 && (uint32_t)to_idx   < REGIME_COUNT)
                           ? g_regimes[to_idx].tag   : "NONE";
    u0_puts("PSUTACH: [");
    u0_puts(from_tag);
    u0_puts("] -> [");
    u0_puts(to_tag);
    u0_puts("] duty=");
    u0_put_duty_1dp(duty_x10);
    u0_puts("\r\n");
}

/* ---- public API ---------------------------------------------------------- */

void psu_tachmap_init(void)
{
    g_enabled         = false;
    g_curr_regime_idx = -1;
    g_hyst_x10        = PSU_TACHMAP_HYST_X10_DEFAULT;
    g_fan_fixed_duty  = PSU_TACHMAP_FAN_FIXED_DUTY_DEFAULT;
    g_last_freq_hz    = 0;
}

void psu_tachmap_set_enabled(bool enabled)
{
    if (enabled) {
        if (g_enabled) return;
        if (!pwmin_is_enabled()) pwmin_set_enabled(true);
        tachsyn_set_drive_mode(TACHSYN_DRIVE_PUSHPULL);
        g_curr_regime_idx = -1;
        g_last_freq_hz    = 0;
        g_enabled         = true;
        pwm_set_percent(g_fan_fixed_duty);
        return;
    }
    if (!g_enabled) return;
    g_enabled         = false;
    g_curr_regime_idx = -1;
    tachsyn_stop();
    tach_default_apply();
}

bool psu_tachmap_is_enabled(void) { return g_enabled; }

void psu_tachmap_set_hyst_x10(uint32_t h)
{
    if (h > 200U) h = 200U;
    g_hyst_x10 = h;
}
uint32_t psu_tachmap_get_hyst_x10(void) { return g_hyst_x10; }

void psu_tachmap_set_fan_fixed_duty(uint32_t pct)
{
    if (pct < 5U)  pct = 5U;
    if (pct > 96U) pct = 96U;
    g_fan_fixed_duty = pct;
    if (g_enabled) pwm_set_percent(g_fan_fixed_duty);
}
uint32_t psu_tachmap_get_fan_fixed_duty(void) { return g_fan_fixed_duty; }

int psu_tachmap_get_regime_index(void) { return (int)g_curr_regime_idx; }

const char *psu_tachmap_get_regime_tag(void)
{
    if (g_curr_regime_idx < 0 || (uint32_t)g_curr_regime_idx >= REGIME_COUNT)
        return NULL;
    return g_regimes[g_curr_regime_idx].tag;
}

void psu_tachmap_task(void)
{
    if (!g_enabled) return;

    uint32_t duty_x10 = 0;
    if (!pwmin_get_last_duty_x10(&duty_x10)) return;

    /* ---- Regime selection with hysteresis -------------------------------- */
    int32_t target = regime_for_duty(duty_x10);
    int32_t prev   = g_curr_regime_idx;
    int32_t next   = prev;

    if (target > prev) {
        /* Ascending: immediate. */
        next = target;
    } else if (target < prev && prev >= 0) {
        /* Descending: only when duty drops below (entry – hyst). */
        uint32_t entry = (uint32_t)g_regimes[prev].entry_duty_x10;
        uint32_t exit_thresh = (entry > g_hyst_x10) ? (entry - g_hyst_x10) : 0U;
        if (duty_x10 < exit_thresh) next = target;
    } else if (prev < 0 && target >= 0) {
        next = target;
    }

    if (next != prev) {
        emit_regime_change(prev, next, duty_x10);
        g_curr_regime_idx = next;
        g_last_freq_hz    = 0;
    }

    /* ---- Drive TACHSYN --------------------------------------------------- */
    if (g_curr_regime_idx < 0) {
        if (tachsyn_is_running()) { tachsyn_stop(); g_last_freq_hz = 0; }
        return;
    }

    uint32_t freq_hz = rpm_to_hz((uint32_t)g_regimes[g_curr_regime_idx].synth_rpm);

    if (freq_hz == 0U) {
        if (tachsyn_is_running()) tachsyn_stop();
        g_last_freq_hz = 0;
        return;
    }

    if (tachsyn_is_running() && freq_hz == g_last_freq_hz) return;

    g_last_freq_hz = freq_hz;
    tachsyn_set_continuous(freq_hz, 50U);
}
