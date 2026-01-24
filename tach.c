#include "tach.h"

#include <stdbool.h>
#include <stdint.h>

#include "inc/hw_ints.h"

#include "driverlib/gpio.h"
#include "driverlib/interrupt.h"
#include "driverlib/rom.h"
#include "driverlib/sysctl.h"

#include "timebase.h"
#include "pwmin.h"

/* Reject edges closer than this (microseconds). Helps ignore ~24.9kHz PWM coupling. */
#ifndef TACH_MIN_EDGE_US
#define TACH_MIN_EDGE_US 200U
#endif

/* Count of detected TACH pulses (falling edges). Updated in ISR. */
static volatile uint32_t g_tach_pulses = 0;
static volatile uint32_t g_tach_rejects = 0;
static volatile uint32_t g_last_edge_cycles = 0;

static volatile bool g_tach_capture_enabled = true;

static volatile bool g_copy_to_pm3 = false;

static volatile bool g_tach_reporting = false;
static uint32_t g_next_report_ms = 0;
static uint32_t g_last_report_ms = 0;
static uint32_t g_print_suppress = 0;
static volatile bool g_tach_print_enabled = true;

static volatile uint32_t g_last_rpm = 0;
static volatile bool g_have_last_rpm = false;

static volatile uint32_t g_loopback_expected_hz = 0;

/*
 * GPIO Port ISR (vector must point here).
 * Counts falling edges from open-collector TACH.
 */
void GPIOFIntHandler(void)
{
    uint32_t status = GPIOIntStatus(TACH_GPIO_BASE, true);
    GPIOIntClear(TACH_GPIO_BASE, status);

    if (status & TACH_GPIO_PIN) {
        /* Optional COPY mode: mirror input level onto PM3 edge-for-edge. */
        if (g_copy_to_pm3) {
            uint8_t level = GPIOPinRead(TACH_GPIO_BASE, TACH_GPIO_PIN) ? 1U : 0U;
            GPIOPinWrite(GPIO_PORTM_BASE, GPIO_PIN_3, level ? GPIO_PIN_3 : 0);
            /* For reporting, count falling edges only (level==0 after interrupt). */
            if (level == 0U) {
                g_tach_pulses++;
            }
            return;
        }

        /* Glitch reject: ignore unrealistically fast edges. */
        uint32_t now = timebase_cycles32();
        uint32_t delta = now - g_last_edge_cycles;

        uint32_t sysclk = timebase_sysclk_hz();
        uint32_t min_cycles = (sysclk / 1000000U) * TACH_MIN_EDGE_US;
        if (min_cycles == 0) {
            min_cycles = 1;
        }

        if (delta < min_cycles) {
            g_tach_rejects++;
            return;
        }

        g_last_edge_cycles = now;
        g_tach_pulses++;
    }

    /* Optional: PWMIN sensing on PF3 (enabled only when PWMIN is active). */
    if (status & GPIO_PIN_3) {
        pwmin_gpiof_isr(status);
    }
}

void tach_set_copy_to_pm3(bool enabled)
{
    g_copy_to_pm3 = enabled;

    if (enabled) {
        /* Ensure PM3 is configured as a push-pull GPIO output. */
        SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOM);
        while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOM)) { }
        GPIOPinTypeGPIOOutput(GPIO_PORTM_BASE, GPIO_PIN_3);
        GPIOPadConfigSet(GPIO_PORTM_BASE, GPIO_PIN_3, GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD);
        GPIOPinWrite(GPIO_PORTM_BASE, GPIO_PIN_3, 0);

        /* Switch PF1 interrupt to BOTH edges so we can mirror high and low. */
        GPIOIntDisable(TACH_GPIO_BASE, TACH_GPIO_PIN);
        GPIOIntClear(TACH_GPIO_BASE, TACH_GPIO_PIN);
        GPIOIntTypeSet(TACH_GPIO_BASE, TACH_GPIO_PIN, GPIO_BOTH_EDGES);
        GPIOIntEnable(TACH_GPIO_BASE, TACH_GPIO_PIN);
        IntEnable(TACH_GPIO_INT);

        return;
    }

    /* Restore normal falling-edge capture. */
    GPIOIntDisable(TACH_GPIO_BASE, TACH_GPIO_PIN);
    GPIOIntClear(TACH_GPIO_BASE, TACH_GPIO_PIN);
    GPIOIntTypeSet(TACH_GPIO_BASE, TACH_GPIO_PIN, GPIO_FALLING_EDGE);
    GPIOIntEnable(TACH_GPIO_BASE, TACH_GPIO_PIN);
    IntEnable(TACH_GPIO_INT);
}

static void uart0_puts(const char *s)
{
    if (!s) return;
    const char *p = s;
    while (*p) {
        ROM_UARTCharPut(UART0_BASE, *p++);
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

static void uart0_put_u32_1dp_minwidth4(uint32_t value_x10)
{
    /* Minimal printf("%4.1f")-style formatting without floats.
       Pads a leading space for 0.0 .. 9.9 to improve scanability.
       For values >= 10.0, no padding is added.
    */
    if (value_x10 < 100U) {
        uart0_puts(" ");
    }
    uart0_put_u32(value_x10 / 10U);
    uart0_puts(".");
    uart0_put_u32(value_x10 % 10U);
}

static void uart0_put_hex32(uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int shift = 28; shift >= 0; shift -= 4) {
        ROM_UARTCharPut(UART0_BASE, hex[(v >> shift) & 0xFU]);
    }
}

void tach_init(void)
{
    SysCtlPeripheralEnable(TACH_GPIO_PERIPH);
    while (!SysCtlPeripheralReady(TACH_GPIO_PERIPH)) { }

    /* Input with weak pull-up (3.3V). */
    GPIOPadConfigSet(TACH_GPIO_BASE, TACH_GPIO_PIN, GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD_WPU);
    GPIOPinTypeGPIOInput(TACH_GPIO_BASE, TACH_GPIO_PIN);

    /* Interrupt on falling edge (typical for open-collector tach pulses). */
    GPIOIntDisable(TACH_GPIO_BASE, TACH_GPIO_PIN);
    GPIOIntClear(TACH_GPIO_BASE, TACH_GPIO_PIN);
    GPIOIntTypeSet(TACH_GPIO_BASE, TACH_GPIO_PIN, GPIO_FALLING_EDGE);
    GPIOIntEnable(TACH_GPIO_BASE, TACH_GPIO_PIN);

    IntEnable(TACH_GPIO_INT);

    g_tach_capture_enabled = true;

    g_tach_pulses = 0;
    g_tach_rejects = 0;
    g_last_edge_cycles = 0;
    g_tach_reporting = false;
    g_next_report_ms = 0;
}

void tach_set_capture_enabled(bool enabled)
{
    if (enabled) {
        if (g_tach_capture_enabled) return;

        /* Restore input + pull-up and enable falling-edge interrupt capture. */
        GPIOPadConfigSet(TACH_GPIO_BASE, TACH_GPIO_PIN, GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD_WPU);
        GPIOPinTypeGPIOInput(TACH_GPIO_BASE, TACH_GPIO_PIN);

        GPIOIntDisable(TACH_GPIO_BASE, TACH_GPIO_PIN);
        GPIOIntClear(TACH_GPIO_BASE, TACH_GPIO_PIN);
        GPIOIntTypeSet(TACH_GPIO_BASE, TACH_GPIO_PIN, GPIO_FALLING_EDGE);
        GPIOIntEnable(TACH_GPIO_BASE, TACH_GPIO_PIN);

        IntEnable(TACH_GPIO_INT);
        g_tach_capture_enabled = true;
        return;
    }

    if (!g_tach_capture_enabled) return;

    GPIOIntDisable(TACH_GPIO_BASE, TACH_GPIO_PIN);
    GPIOIntClear(TACH_GPIO_BASE, TACH_GPIO_PIN);
    IntDisable(TACH_GPIO_INT);
    g_tach_capture_enabled = false;
}

bool tach_is_capture_enabled(void)
{
    return g_tach_capture_enabled;
}

void tach_set_reporting(bool enabled)
{
    tach_set_reporting_ex(enabled, true);
}

void tach_set_reporting_ex(bool enabled, bool print_enabled)
{
    g_tach_reporting = enabled;
    g_tach_print_enabled = print_enabled;

    uint32_t now = timebase_millis();
    g_next_report_ms = now + 500U;
    g_last_report_ms = now;
    g_print_suppress = enabled ? 2U : 0U;

    if (enabled && print_enabled) {
        uart0_puts("TACHIN ON: gpio_base=0x");
        uart0_put_hex32((uint32_t)TACH_GPIO_BASE);
        uart0_puts(" pin_mask=0x");
        uart0_put_hex32((uint32_t)TACH_GPIO_PIN);
        uart0_puts(" edge=FALL pullup=WPU\r\n");
    }

    if (!enabled) {
        /* Reset counter when stopping to simplify the next start. */
        IntMasterDisable();
        g_tach_pulses = 0;
        g_tach_rejects = 0;
        g_last_edge_cycles = 0;
        IntMasterEnable();
    }
}

bool tach_is_reporting(void)
{
    return g_tach_reporting;
}

bool tach_is_printing(void)
{
    return g_tach_reporting && g_tach_print_enabled;
}

bool tach_get_last_rpm(uint32_t *rpm_out)
{
    if (!rpm_out) return false;
    IntMasterDisable();
    uint32_t rpm = g_last_rpm;
    bool have = g_have_last_rpm;
    IntMasterEnable();
    *rpm_out = have ? rpm : 0U;
    return have;
}

void tach_set_loopback_expected_hz(uint32_t expected_hz)
{
    g_loopback_expected_hz = expected_hz;
}

uint32_t tach_get_loopback_expected_hz(void)
{
    return g_loopback_expected_hz;
}

void tach_task(void)
{
    if (!g_tach_reporting) return;

    uint32_t now = timebase_millis();
    if ((int32_t)(now - g_next_report_ms) < 0) {
        return;
    }

    g_next_report_ms += 500U;

    uint32_t dt_ms = now - g_last_report_ms;
    g_last_report_ms = now;
    if (dt_ms == 0U) {
        dt_ms = 1U;
    }

    /* Atomically snapshot and clear pulse count. */
    uint32_t pulses;
    uint32_t rejects;
    IntMasterDisable();
    pulses = g_tach_pulses;
    g_tach_pulses = 0;
    rejects = g_tach_rejects;
    g_tach_rejects = 0;
    IntMasterEnable();

    /* Window is 0.5s. User's model: RPM = pulses_per_sec * 30.
       pulses_per_sec = pulses / 0.5 = 2*pulses => RPM = 60*pulses. */
    uint32_t rpm = pulses * 60U;

     /* Bare tach pulse frequency (falling edges per second), 0.1 Hz resolution.
         freq_x10 = round(pulses * 10000 / dt_ms)
     */
     uint32_t freq_x10 = (uint32_t)((((uint64_t)pulses * 10000ULL) + ((uint64_t)dt_ms / 2ULL)) / (uint64_t)dt_ms);

    /* Always update last computed values (even if printing is suppressed). */
    IntMasterDisable();
    g_last_rpm = rpm;
    g_have_last_rpm = true;
    IntMasterEnable();

    uint32_t expected_hz = g_loopback_expected_hz;
    uint32_t expected_pulses = 0;
    bool have_expect = (expected_hz != 0U);
    if (have_expect) {
        /* In this firmware, we count falling edges; for a square wave, pulses/sec ~= Hz.
           Window is 0.5s, so expected pulses ~= Hz/2. */
        expected_pulses = expected_hz / 2U;
    }

    if (!g_tach_print_enabled) {
        (void)freq_x10;
        return;
    }

    /* Suppress the first two prints after enabling to avoid confusing
       transient data during peripheral/pin switching.
    */
    if (g_print_suppress > 0U) {
        g_print_suppress--;
        return;
    }

    uart0_puts("TACH pulses=");
    uart0_put_u32(pulses);
    uart0_puts(" rejects=");
    uart0_put_u32(rejects);
    uart0_puts(" f=");
    uart0_put_u32_1dp_minwidth4(freq_x10);
    uart0_puts("Hz");

    if (have_expect) {
        /* Allow a small tolerance to avoid false negatives due to window alignment. */
        const uint32_t tol = 2U;
        uint32_t low = (expected_pulses > tol) ? (expected_pulses - tol) : 0U;
        uint32_t high = expected_pulses + tol;
        bool ok = (pulses >= low && pulses <= high);

        uart0_puts(" loopback_exp_hz=");
        uart0_put_u32(expected_hz);
        uart0_puts(" exp_pulses_0p5s=");
        uart0_put_u32(expected_pulses);
        uart0_puts(ok ? " OK" : " FAIL");
    }

    /* Keep RPM as the last datum in the line. */
    uart0_puts(" rpm=");
    uart0_put_u32(rpm);
    uart0_puts("\r\n");
}
