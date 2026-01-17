#include "tsyn.h"

#include <stdbool.h>
#include <stdint.h>

#include "inc/hw_ints.h"
#include "inc/hw_memmap.h"

#include "driverlib/gpio.h"
#include "driverlib/interrupt.h"
#include "driverlib/pin_map.h"
#include "driverlib/sysctl.h"
#include "driverlib/timer.h"

#include "commands.h" /* pwm_get_percent_requested() */
#include "timebase.h"
#include "tach.h"

/* Base carrier frequency for legacy burst TSYN output. */
#define TSYN_BASE_FREQ_HZ 24900U

/* PM3 supports T3CCP1 and T5CCP1; we use Timer3B -> T3CCP1. */
#define TSYN_GPIO_PERIPH SYSCTL_PERIPH_GPIOM
#define TSYN_GPIO_BASE   GPIO_PORTM_BASE
#define TSYN_GPIO_PIN    GPIO_PIN_3

#define TSYN_PWM_TIMER_PERIPH SYSCTL_PERIPH_TIMER3
#define TSYN_PWM_TIMER_BASE   TIMER3_BASE

#define TSYN_SCHED_TIMER_PERIPH SYSCTL_PERIPH_TIMER4
#define TSYN_SCHED_TIMER_BASE   TIMER4_BASE
#define TSYN_SCHED_INT          INT_TIMER4A

typedef struct {
    uint8_t psyn_n;
    uint16_t pulses_per_burst;
    uint16_t tail_us_mid;
} tsyn_point_t;

/*
 * Table extracted from the lab notes.
 * Tail midpoints are computed from the observed ranges when provided.
 * For n>=62, tail was not measured yet in the notes; we currently hold the last
 * known midpoint (n=50) until refined data arrives.
 */
static const tsyn_point_t g_points[] = {
    {  6, 98,  37 },  /* 34-40us -> 37 */
    { 15, 50,  93 },  /* 69-116us -> 92.5 */
    { 25, 36,  92 },  /* 72-112us -> 92 */
    { 40, 29, 103 },  /* 90-115us -> 102.5 */
    { 50, 28, 102 },  /* 87-117us -> 102 */
    { 62, 23, 102 },  /* tail TBD */
    { 80, 19, 102 },  /* tail TBD */
};

typedef enum {
    TSYN_STATE_OFF = 0,
    TSYN_STATE_PULSES,
    TSYN_STATE_TAIL,
} tsyn_state_t;

static volatile bool g_tsyn_enabled = false;
static volatile tsyn_state_t g_state = TSYN_STATE_OFF;

/* Continuous TACHSYN mode (PM3 square wave) state. */
static volatile bool g_tachsyn_running = false;
static volatile uint32_t g_tachsyn_freq_hz = 0;
static volatile uint32_t g_tachsyn_duty_percent = 50;
static volatile tachsyn_drive_mode_t g_tachsyn_drive_mode = TACHSYN_DRIVE_OPENDRAIN;

static uint32_t g_sysclk_hz = 0;
static uint32_t g_pwm_period_cycles = 0;

static uint32_t g_curr_pulses = 0;
static uint32_t g_curr_tail_us = 0;

/* TSYN BOOT (time-based TACHSYN profile) state. */
static volatile bool g_tachsyn_boot_running = false;
static uint32_t g_tachsyn_boot_start_ms = 0;
static uint32_t g_tachsyn_boot_last_freq_hz = 0;

static volatile bool g_tach_loopback_running = false;
static volatile uint32_t g_tach_loopback_expected_hz = 0;

static volatile bool g_tachsyn_copy_running = false;

static void tsyn_interpolate_from_psyn(uint32_t psyn_n, uint32_t *pulses_out, uint32_t *tail_us_out)
{
    if (!pulses_out || !tail_us_out) return;

    const uint32_t count = (uint32_t)(sizeof(g_points) / sizeof(g_points[0]));

    if (psyn_n <= g_points[0].psyn_n) {
        *pulses_out = g_points[0].pulses_per_burst;
        *tail_us_out = g_points[0].tail_us_mid;
        return;
    }

    if (psyn_n >= g_points[count - 1].psyn_n) {
        *pulses_out = g_points[count - 1].pulses_per_burst;
        *tail_us_out = g_points[count - 1].tail_us_mid;
        return;
    }

    for (uint32_t i = 0; i + 1 < count; i++) {
        const uint32_t x0 = g_points[i].psyn_n;
        const uint32_t x1 = g_points[i + 1].psyn_n;
        if (psyn_n < x0 || psyn_n > x1) continue;

        const int32_t y0p = (int32_t)g_points[i].pulses_per_burst;
        const int32_t y1p = (int32_t)g_points[i + 1].pulses_per_burst;
        const int32_t y0t = (int32_t)g_points[i].tail_us_mid;
        const int32_t y1t = (int32_t)g_points[i + 1].tail_us_mid;

        const uint32_t dx = (x1 - x0);
        const uint32_t xn = (psyn_n - x0);

        /* Round-to-nearest integer for both interpolations. */
        int32_t pulses = y0p + (int32_t)(((int64_t)(y1p - y0p) * (int64_t)xn + (int64_t)(dx / 2U)) / (int64_t)dx);
        int32_t tail_us = y0t + (int32_t)(((int64_t)(y1t - y0t) * (int64_t)xn + (int64_t)(dx / 2U)) / (int64_t)dx);

        if (pulses < 1) pulses = 1;
        if (tail_us < 0) tail_us = 0;

        *pulses_out = (uint32_t)pulses;
        *tail_us_out = (uint32_t)tail_us;
        return;
    }

    /* Fallback: should not happen due to earlier clamps. */
    *pulses_out = g_points[0].pulses_per_burst;
    *tail_us_out = g_points[0].tail_us_mid;
}

static void pm3_set_gpio_low_pad(uint32_t pin_type)
{
    GPIOPinTypeGPIOOutput(TSYN_GPIO_BASE, TSYN_GPIO_PIN);
    GPIOPadConfigSet(TSYN_GPIO_BASE, TSYN_GPIO_PIN, GPIO_STRENGTH_2MA, pin_type);
    GPIOPinWrite(TSYN_GPIO_BASE, TSYN_GPIO_PIN, 0);
}

static void pm3_set_gpio_low(void)
{
    /* Default “safe” idle: open-drain low. */
    pm3_set_gpio_low_pad(GPIO_PIN_TYPE_OD);
}

static void pm3_set_timer_pwm_pad(uint32_t pin_type)
{
    GPIOPinConfigure(GPIO_PM3_T3CCP1);
    GPIOPinTypeTimer(TSYN_GPIO_BASE, TSYN_GPIO_PIN);
    GPIOPadConfigSet(TSYN_GPIO_BASE, TSYN_GPIO_PIN, GPIO_STRENGTH_2MA, pin_type);
}

static void pm3_set_timer_pwm(void)
{
    /* Legacy behavior: fan-like open-collector output. */
    pm3_set_timer_pwm_pad(GPIO_PIN_TYPE_OD);
}

static void tsyn_pwm_enable(void)
{
    TimerEnable(TSYN_PWM_TIMER_BASE, TIMER_B);
}

static void tsyn_pwm_disable(void)
{
    TimerDisable(TSYN_PWM_TIMER_BASE, TIMER_B);
}

static void tachsyn_configure_timer3b(uint32_t base_freq_hz, uint32_t duty_percent)
{
    if (base_freq_hz == 0) base_freq_hz = 1;
    if (duty_percent > 99U) duty_percent = 99U;

    uint32_t period_cycles = g_sysclk_hz / base_freq_hz;
    if (period_cycles < 2U) {
        period_cycles = 2U;
    }

    /* Timer3B is a 16-bit timer in split mode; use the 8-bit prescaler to extend to 24-bit. */
    uint32_t load_total = period_cycles - 1U;
    uint32_t prescale_load = (load_total >> 16) & 0xFFU;
    uint32_t load = load_total & 0xFFFFU;

    uint32_t match_total = (period_cycles * (100U - duty_percent)) / 100U;
    if (match_total == 0U) match_total = 1U;
    if (match_total >= period_cycles) match_total = period_cycles - 1U;

    uint32_t prescale_match = (match_total >> 16) & 0xFFU;
    uint32_t match = match_total & 0xFFFFU;

    TimerDisable(TSYN_PWM_TIMER_BASE, TIMER_B);
    TimerConfigure(TSYN_PWM_TIMER_BASE, TIMER_CFG_SPLIT_PAIR | TIMER_CFG_B_PWM);
    TimerPrescaleSet(TSYN_PWM_TIMER_BASE, TIMER_B, prescale_load);
    TimerLoadSet(TSYN_PWM_TIMER_BASE, TIMER_B, load);
    TimerPrescaleMatchSet(TSYN_PWM_TIMER_BASE, TIMER_B, prescale_match);
    TimerMatchSet(TSYN_PWM_TIMER_BASE, TIMER_B, match);
}

static void tsyn_schedule_cycles(uint32_t cycles)
{
    if (cycles == 0) cycles = 1;
    TimerDisable(TSYN_SCHED_TIMER_BASE, TIMER_A);
    TimerLoadSet(TSYN_SCHED_TIMER_BASE, TIMER_A, cycles - 1U);
    TimerEnable(TSYN_SCHED_TIMER_BASE, TIMER_A);
}

static void tsyn_start_pulse_burst(void)
{
    uint32_t psyn_n = pwm_get_percent_requested();
    tsyn_interpolate_from_psyn(psyn_n, &g_curr_pulses, &g_curr_tail_us);

    /* Switch PM3 to timer output and enable the 24.9kHz carrier. */
    pm3_set_timer_pwm();
    tsyn_pwm_enable();

    /* Schedule stop after N pulses (each pulse is one full carrier period). */
    uint64_t cycles64 = (uint64_t)g_curr_pulses * (uint64_t)g_pwm_period_cycles;
    if (cycles64 == 0) cycles64 = 1;
    if (cycles64 > 0xFFFFFFFFu) cycles64 = 0xFFFFFFFFu;

    g_state = TSYN_STATE_PULSES;
    tsyn_schedule_cycles((uint32_t)cycles64);
}

static void tsyn_start_tail(void)
{
    /* Stop carrier and hold low for the tail duration. */
    tsyn_pwm_disable();
    pm3_set_gpio_low();

    uint64_t cycles64 = ((uint64_t)g_sysclk_hz * (uint64_t)g_curr_tail_us) / 1000000ULL;
    if (cycles64 == 0) cycles64 = 1;
    if (cycles64 > 0xFFFFFFFFu) cycles64 = 0xFFFFFFFFu;

    g_state = TSYN_STATE_TAIL;
    tsyn_schedule_cycles((uint32_t)cycles64);
}

void Timer4AIntHandler(void)
{
    TimerIntClear(TSYN_SCHED_TIMER_BASE, TIMER_TIMA_TIMEOUT);

    if (!g_tsyn_enabled) {
        return;
    }

    if (g_state == TSYN_STATE_PULSES) {
        tsyn_start_tail();
        return;
    }

    /* Tail (or first kick) -> start the next pulse burst. */
    tsyn_start_pulse_burst();
}

void tsyn_init(uint32_t sysclk_hz)
{
    g_sysclk_hz = sysclk_hz;

    SysCtlPeripheralEnable(TSYN_GPIO_PERIPH);
    while (!SysCtlPeripheralReady(TSYN_GPIO_PERIPH)) { }

    SysCtlPeripheralEnable(TSYN_PWM_TIMER_PERIPH);
    while (!SysCtlPeripheralReady(TSYN_PWM_TIMER_PERIPH)) { }

    SysCtlPeripheralEnable(TSYN_SCHED_TIMER_PERIPH);
    while (!SysCtlPeripheralReady(TSYN_SCHED_TIMER_PERIPH)) { }

    /* Configure Timer3B as PWM carrier near 24.9kHz, ~50% duty. */
    g_pwm_period_cycles = g_sysclk_hz / TSYN_BASE_FREQ_HZ;
    if (g_pwm_period_cycles < 10) {
        g_pwm_period_cycles = 10;
    }

    TimerDisable(TSYN_PWM_TIMER_BASE, TIMER_B);
    TimerConfigure(TSYN_PWM_TIMER_BASE, TIMER_CFG_SPLIT_PAIR | TIMER_CFG_B_PWM);
    TimerLoadSet(TSYN_PWM_TIMER_BASE, TIMER_B, g_pwm_period_cycles - 1U);
    TimerMatchSet(TSYN_PWM_TIMER_BASE, TIMER_B, (g_pwm_period_cycles / 2U));

    /* Scheduler timer (one-shot) - very low interrupt rate (2 IRQs per burst). */
    TimerDisable(TSYN_SCHED_TIMER_BASE, TIMER_A);
    TimerConfigure(TSYN_SCHED_TIMER_BASE, TIMER_CFG_ONE_SHOT);
    TimerIntDisable(TSYN_SCHED_TIMER_BASE, TIMER_TIMA_TIMEOUT);
    TimerIntClear(TSYN_SCHED_TIMER_BASE, TIMER_TIMA_TIMEOUT);
    TimerIntEnable(TSYN_SCHED_TIMER_BASE, TIMER_TIMA_TIMEOUT);

    /* Keep IRQ disabled until TSYN is explicitly enabled. */
    IntDisable(TSYN_SCHED_INT);

    /* Default idle state: PM3 low (open-drain). */
    pm3_set_gpio_low();

    g_tsyn_enabled = false;
    g_state = TSYN_STATE_OFF;
}

void tsyn_set_enabled(bool enabled)
{
    if (enabled) {
        if (g_tsyn_enabled) return;

        /* If TACHSYN continuous mode is running, stop it before starting bursts. */
        if (g_tachsyn_running) {
            tachsyn_stop();
        }

        /* Avoid race with a pending Timer4A timeout. */
        IntDisable(TSYN_SCHED_INT);
        TimerDisable(TSYN_SCHED_TIMER_BASE, TIMER_A);
        TimerIntClear(TSYN_SCHED_TIMER_BASE, TIMER_TIMA_TIMEOUT);

        g_tsyn_enabled = true;

        /* Start in tail->pulses transition immediately. */
        g_state = TSYN_STATE_TAIL;
        g_curr_pulses = 0;
        g_curr_tail_us = 1;

        IntEnable(TSYN_SCHED_INT);
        tsyn_schedule_cycles(1);
        return;
    }

    if (!g_tsyn_enabled) return;

    /* Prevent Timer4A ISR from re-arming the burst while we tear down. */
    IntDisable(TSYN_SCHED_INT);
    TimerDisable(TSYN_SCHED_TIMER_BASE, TIMER_A);
    TimerIntClear(TSYN_SCHED_TIMER_BASE, TIMER_TIMA_TIMEOUT);

    g_tsyn_enabled = false;
    g_state = TSYN_STATE_OFF;

    tsyn_pwm_disable();

    /* Return PM3 to low. */
    pm3_set_gpio_low();
}

bool tsyn_is_enabled(void)
{
    return g_tsyn_enabled;
}

void tachsyn_set_continuous(uint32_t base_freq_hz, uint32_t duty_percent)
{
    /* Stop TSYN burst mode if it is active. */
    if (g_tsyn_enabled) {
        tsyn_set_enabled(false);
    }

    /* Ensure the scheduler IRQ cannot fire while we are in continuous mode. */
    IntDisable(TSYN_SCHED_INT);
    TimerDisable(TSYN_SCHED_TIMER_BASE, TIMER_A);
    TimerIntClear(TSYN_SCHED_TIMER_BASE, TIMER_TIMA_TIMEOUT);

    g_tachsyn_freq_hz = base_freq_hz;
    g_tachsyn_duty_percent = duty_percent;

    /* Configure PM3 as timer output and start continuous PWM. */
    if (g_tachsyn_drive_mode == TACHSYN_DRIVE_PUSHPULL) {
        pm3_set_timer_pwm_pad(GPIO_PIN_TYPE_STD);
    } else {
        pm3_set_timer_pwm_pad(GPIO_PIN_TYPE_OD);
    }
    tachsyn_configure_timer3b(base_freq_hz, duty_percent);
    tsyn_pwm_enable();

    g_tachsyn_running = true;
}

void tachsyn_stop(void)
{
    if (!g_tachsyn_running) return;

    /* Stop Timer3B PWM and return PM3 low. */
    tsyn_pwm_disable();
    if (g_tachsyn_drive_mode == TACHSYN_DRIVE_PUSHPULL) {
        pm3_set_gpio_low_pad(GPIO_PIN_TYPE_STD);
    } else {
        pm3_set_gpio_low_pad(GPIO_PIN_TYPE_OD);
    }
    g_tachsyn_running = false;
}

bool tachsyn_is_running(void)
{
    return g_tachsyn_running;
}

static uint32_t tachsyn_boot_compute_freq_hz(uint32_t elapsed_ms)
{
    /* Boot timeline (from lab notes / docs):
       - ~0s to ~4.5s: tach transitions from ~60Hz down to ~50Hz
       - ~4.5s to ~17s: hold ~50Hz
       - ~17s to ~28s: ramp ~50Hz up to ~168Hz
       - >= ~28s: hold ~168Hz
    */

    if (elapsed_ms < 4500U) {
        /* Linear ramp 60 -> 50 over 4.5s */
        const uint32_t span_ms = 4500U;
        const uint32_t f0 = 60U;
        const uint32_t f1 = 50U;
        const uint32_t df = f0 - f1; /* 10 */
        return f0 - ((df * elapsed_ms) / span_ms);
    }

    if (elapsed_ms < 17000U) {
        return 50U;
    }

    if (elapsed_ms < 28000U) {
        /* Linear ramp 50 -> 168 over 11s */
        const uint32_t span_ms = 28000U - 17000U;
        const uint32_t f0 = 50U;
        const uint32_t f1 = 168U;
        const uint32_t df = f1 - f0;
        return f0 + ((df * (elapsed_ms - 17000U)) / span_ms);
    }

    return 168U;
}

void tachsyn_boot_start(void)
{
    /* COPY has priority; do not start BOOT if COPY is active. */
    if (g_tachsyn_copy_running) {
        return;
    }
    /* Stop legacy burst TSYN if active. */
    if (g_tsyn_enabled) {
        tsyn_set_enabled(false);
    }

    /* Start (or re-start) boot mode. */
    g_tachsyn_boot_running = true;
    g_tachsyn_boot_start_ms = timebase_millis();
    g_tachsyn_boot_last_freq_hz = 0;

    /* Prefer push-pull for driving an interface transistor base (matches PHASE behavior). */
    g_tachsyn_drive_mode = TACHSYN_DRIVE_PUSHPULL;

    /* Force an immediate update. */
    tachsyn_boot_task();
}

void tachsyn_boot_stop(void)
{
    g_tachsyn_boot_running = false;
    g_tachsyn_boot_last_freq_hz = 0;
    tachsyn_stop();
}

bool tachsyn_boot_is_running(void)
{
    return g_tachsyn_boot_running;
}

void tach_loopback_begin(uint32_t expected_hz)
{
    /* Loopback is a “test guard”: it should not fight with TSYN BOOT. */
    if (g_tachsyn_boot_running) {
        tachsyn_boot_stop();
    }

    g_tach_loopback_running = true;
    g_tach_loopback_expected_hz = expected_hz;
}

void tach_loopback_end(void)
{
    g_tach_loopback_running = false;
    g_tach_loopback_expected_hz = 0;
}

bool tach_loopback_is_running(void)
{
    return g_tach_loopback_running;
}

uint32_t tach_loopback_expected_hz(void)
{
    return g_tach_loopback_expected_hz;
}

void tachsyn_boot_task(void)
{
    if (!g_tachsyn_boot_running) return;
    if (g_tachsyn_copy_running) return;

    uint32_t now = timebase_millis();
    uint32_t elapsed = now - g_tachsyn_boot_start_ms;

    uint32_t freq = tachsyn_boot_compute_freq_hz(elapsed);
    if (freq == 0U) freq = 1U;

    /* Avoid reprogramming Timer3B too often; only update when integer Hz changes. */
    if (freq == g_tachsyn_boot_last_freq_hz && g_tachsyn_running) {
        return;
    }

    g_tachsyn_boot_last_freq_hz = freq;
    tachsyn_set_continuous(freq, 50U);
}

void tachsyn_copy_begin(void)
{
    /* Stop other generators. */
    if (g_tsyn_enabled) {
        tsyn_set_enabled(false);
    }
    if (g_tachsyn_boot_running) {
        tachsyn_boot_stop();
    }
    if (g_tach_loopback_running) {
        tach_loopback_end();
    }

    tachsyn_stop();

    g_tachsyn_copy_running = true;
    tach_set_copy_to_pm3(true);
}

void tachsyn_copy_end(void)
{
    tach_set_copy_to_pm3(false);
    g_tachsyn_copy_running = false;
}

bool tachsyn_copy_is_running(void)
{
    return g_tachsyn_copy_running;
}

void tachsyn_set_drive_mode(tachsyn_drive_mode_t mode)
{
    g_tachsyn_drive_mode = mode;
}

tachsyn_drive_mode_t tachsyn_get_drive_mode(void)
{
    return g_tachsyn_drive_mode;
}
