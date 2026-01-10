#include "tach.h"

#include <stdbool.h>
#include <stdint.h>

#include "inc/hw_ints.h"

#include "driverlib/gpio.h"
#include "driverlib/interrupt.h"
#include "driverlib/rom.h"
#include "driverlib/sysctl.h"

#include "timebase.h"

/* Reject edges closer than this (microseconds). Helps ignore 21.5kHz PWM coupling. */
#ifndef TACH_MIN_EDGE_US
#define TACH_MIN_EDGE_US 200U
#endif

/* Count of detected TACH pulses (falling edges). Updated in ISR. */
static volatile uint32_t g_tach_pulses = 0;
static volatile uint32_t g_tach_rejects = 0;
static volatile uint32_t g_last_edge_cycles = 0;

static volatile bool g_tach_reporting = false;
static uint32_t g_next_report_ms = 0;

/*
 * GPIO Port K ISR (vector must point here).
 * Counts falling edges from open-collector TACH.
 */
void GPIOMIntHandler(void)
{
    uint32_t status = GPIOIntStatus(TACH_GPIO_BASE, true);
    GPIOIntClear(TACH_GPIO_BASE, status);

    if (status & TACH_GPIO_PIN) {
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

    g_tach_pulses = 0;
    g_tach_rejects = 0;
    g_last_edge_cycles = 0;
    g_tach_reporting = false;
    g_next_report_ms = 0;
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

    uart0_puts("TACH pulses=");
    uart0_put_u32(pulses);
    uart0_puts(" rejects=");
    uart0_put_u32(rejects);
    uart0_puts(" rpm=");
    uart0_put_u32(rpm);
    uart0_puts("\r\n");
}
