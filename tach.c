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
    g_tach_reporting = enabled;
    g_next_report_ms = timebase_millis() + 500U;

    if (enabled) {
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

    uint32_t expected_hz = g_loopback_expected_hz;
    uint32_t expected_pulses = 0;
    bool have_expect = (expected_hz != 0U);
    if (have_expect) {
        /* In this firmware, we count falling edges; for a square wave, pulses/sec ~= Hz.
           Window is 0.5s, so expected pulses ~= Hz/2. */
        expected_pulses = expected_hz / 2U;
    }

    uart0_puts("TACH pulses=");
    uart0_put_u32(pulses);
    uart0_puts(" rejects=");
    uart0_put_u32(rejects);
    uart0_puts(" rpm=");
    uart0_put_u32(rpm);

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
    uart0_puts("\r\n");
}
