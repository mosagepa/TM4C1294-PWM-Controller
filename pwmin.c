#include "pwmin.h"

#include <stdbool.h>
#include <stdint.h>

#include "inc/hw_ints.h"
#include "inc/hw_memmap.h"
#include "inc/hw_nvic.h"
#include "inc/hw_types.h"
#include "inc/hw_gpio.h"

#include "driverlib/gpio.h"
#include "driverlib/interrupt.h"
#include "driverlib/pin_map.h"
#include "driverlib/sysctl.h"
#include "driverlib/timer.h"
#include "driverlib/rom.h"

#include "timebase.h"
#include "tach.h"

/* PWMIN capture pinning:
 *   PF3 / T1CCP1 -> Timer1B capture
 */

/* PF3 / T1CCP1 -> Timer1B capture. */
#define PWMIN_GPIO_PERIPH SYSCTL_PERIPH_GPIOF
#define PWMIN_GPIO_BASE   GPIO_PORTF_BASE
#define PWMIN_GPIO_PIN    GPIO_PIN_3

#define PWMIN_TIMER_PERIPH SYSCTL_PERIPH_TIMER1
#define PWMIN_TIMER_BASE   TIMER1_BASE
#define PWMIN_TIMER        TIMER_B
#define PWMIN_INT          INT_TIMER1B
#define PWMIN_INT_FLAG     TIMER_CAPB_EVENT

#define PWMIN_PINNAME_STR  "PF3/T1CCP1 (Timer1B CAP_TIME_UP)"

/* Some environments in this workspace have incomplete/mismatched pin-map
    visibility. To avoid relying on GPIO_PF3_T1CCP1, we can auto-detect the
    correct PF3 PCTL nibble by scanning for a value that produces Timer1B
    capture events while PF3 is toggling.
    Default guess is 0x7 (commonly used for T1CCP1 on TM4C1294). */
static uint8_t g_pwmin_pf3_pctl_nibble = 0x7U;

static volatile bool g_pwmin_enabled = false;
static volatile bool g_pwmin_reporting = false;

static volatile bool g_pwmin_print_enabled = true;

static volatile bool g_pwmin_verbose = false;

static uint32_t g_sysclk_hz = 0;

/* Print/acceptance plausibility bounds (helps suppress one-off glitch samples). */
#ifndef PWMIN_VALID_MIN_HZ
#define PWMIN_VALID_MIN_HZ 24800U
#endif
#ifndef PWMIN_VALID_MAX_HZ
#define PWMIN_VALID_MAX_HZ 25200U
#endif

/* ISR-updated capture state (16-bit timer deltas are sufficient at 24.9kHz). */
static volatile uint32_t g_last_rise = 0;
static volatile bool g_have_rise = false;
static volatile uint32_t g_last_period = 0;
static volatile bool g_have_period = false;
static volatile uint32_t g_last_high = 0;
static volatile bool g_have_high = false;

/* Edge classification: do not rely on GPIOPinRead() while the pin is in
    alternate (timer) function mode. Instead, toggle the timer's capture event
    between rising and falling edges.
    true  => next capture expected on rising edge
    false => next capture expected on falling edge
*/
static volatile bool g_expect_rising = true;

static volatile uint32_t g_edge_count = 0;

/* Fallback: use GPIOF interrupts on PF3 when timer capture cannot be made to work. */
static volatile bool g_pwmin_gpio_capture = false;
static volatile uint32_t g_last_rise_cycles32 = 0;
static volatile bool g_have_rise32 = false;
static volatile uint32_t g_last_period_cycles32 = 0;
static volatile bool g_have_period32 = false;
static volatile uint32_t g_last_high_cycles32 = 0;
static volatile bool g_have_high32 = false;

/* Last valid (plausible) measurement snapshot for consumers (e.g. BOTHIN). */
static volatile uint32_t g_last_valid_freq_hz = 0;
static volatile uint32_t g_last_valid_duty_x10 = 0;
static volatile bool g_have_last_valid = false;

/* Count of detected outlier samples that were suppressed. */
static volatile uint32_t g_pwmin_suppressed_samples = 0;

static uint32_t g_next_report_ms = 0;

/* PWMINDBG session + regime-change tracking.
        A "regime change" is a >15% relative change in PWM duty and/or tach RPM.

        IMPORTANT GATING RULE:
        - Regime-change WARNING messages are emitted ONLY when TACHIN reporting is
            active AND we have a valid RPM baseline.
        - While TACHIN is inactive, we still update internal PWM baselines but we
            never emit regime-change warnings (and we ignore the implied/zero RPM).
*/
static bool g_dbg_session_active = false;
static uint32_t g_dbg_last_regime_sec = 0;

static bool g_dbg_prev_tach_active = false;

static bool g_dbg_have_baseline_duty = false;
static uint32_t g_dbg_last_duty_pct = 0;

static bool g_dbg_have_baseline_rpm = false;
static uint32_t g_dbg_last_rpm = 0;

static bool g_dbg_warning_pending = false;
static uint32_t g_dbg_pending_new_regime_sec = 0;
static uint32_t g_dbg_prev_duty_pct = 0;
static uint32_t g_dbg_new_duty_pct = 0;
static uint32_t g_dbg_prev_rpm = 0;
static uint32_t g_dbg_new_rpm = 0;

static void uart0_putc_nb(char c);
static void uart0_puts(const char *s);
static void uart0_put_u32(uint32_t v);

static void uart0_putc_nb(char c)
{
    /* Non-blocking UART0 output: if TX FIFO is full (e.g. slow 9600 baud),
       drop characters instead of stalling the main loop and UART3 CLI. */
    (void)ROM_UARTCharPutNonBlocking(UART0_BASE, (uint8_t)c);
}

static void uart0_put_2d(uint32_t v)
{
    v %= 100U;
    uart0_putc_nb((char)('0' + (v / 10U)));
    uart0_putc_nb((char)('0' + (v % 10U)));
}

static void uart0_put_hhmmss(uint32_t uptime_sec)
{
    uint32_t hh = (uptime_sec / 3600U) % 100U;
    uint32_t mm = (uptime_sec / 60U) % 60U;
    uint32_t ss = uptime_sec % 60U;
    uart0_put_2d(hh);
    uart0_puts(":");
    uart0_put_2d(mm);
    uart0_puts(":");
    uart0_put_2d(ss);
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
        uart0_putc_nb((char)('0' + digit));
        div /= 10U;
    }
}

static bool rel_change_gt_15pct(uint32_t prev, uint32_t cur)
{
    if (prev == 0U) {
        return (cur != 0U);
    }
    uint32_t delta = (cur > prev) ? (cur - prev) : (prev - cur);
    return (delta * 100U) > (prev * 15U);
}

static void uart0_puts(const char *s)
{
    if (!s) return;
    while (*s) {
        uart0_putc_nb(*s++);
    }
}

static void uart0_put_u32_1dp(uint32_t value_x10)
{
    uart0_put_u32(value_x10 / 10U);
    uart0_puts(".");
    uart0_put_u32(value_x10 % 10U);
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
        uart0_putc_nb(buf[--i]);
    }
}

static void pwmin_quick_gpio_probe(uint32_t probe_us)
{
    if (!g_pwmin_verbose) return;
    if (g_sysclk_hz == 0) return;

    /* Temporarily sample PF3 as a plain GPIO input to confirm the pin is
       physically toggling (helps diagnose wiring vs capture config).
       This runs only once when PWMIN reporting is enabled. */
    GPIOPinTypeGPIOInput(PWMIN_GPIO_BASE, PWMIN_GPIO_PIN);
    GPIOPadConfigSet(PWMIN_GPIO_BASE, PWMIN_GPIO_PIN, GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD);

    const uint32_t start = timebase_cycles32();
    const uint32_t cycles_target = (g_sysclk_hz / 1000000U) * (probe_us ? probe_us : 1U);

    uint32_t transitions = 0;
    bool any_high = false;
    uint32_t prev = (GPIOPinRead(PWMIN_GPIO_BASE, PWMIN_GPIO_PIN) != 0) ? 1U : 0U;

    while ((uint32_t)(timebase_cycles32() - start) < cycles_target) {
        uint32_t cur = (GPIOPinRead(PWMIN_GPIO_BASE, PWMIN_GPIO_PIN) != 0) ? 1U : 0U;
        if (cur != prev) {
            transitions++;
            prev = cur;
        }
        if (cur) any_high = true;
    }

    uart0_puts("PWMIN PROBE: transitions=");
    uart0_put_u32(transitions);
    uart0_puts(" high_seen=");
    uart0_puts(any_high ? "1" : "0");
    uart0_puts(" (PF3 GPIO for ");
    uart0_put_u32(probe_us);
    uart0_puts("us)\r\n");
}

static void pwmin_pf3_set_pctl_nibble(uint8_t nibble)
{
    const uint32_t shift = 3U * 4U;
    uint32_t pctl = HWREG(PWMIN_GPIO_BASE + GPIO_O_PCTL);
    pctl &= ~(0xFU << shift);
    pctl |= ((uint32_t)(nibble & 0xFU) << shift);
    HWREG(PWMIN_GPIO_BASE + GPIO_O_PCTL) = pctl;
}

static uint8_t pwmin_detect_pf3_pctl_nibble(void)
{
    /* PF3 should already be physically toggling (validated by GPIO probe). */

    SysCtlPeripheralEnable(PWMIN_GPIO_PERIPH);
    while (!SysCtlPeripheralReady(PWMIN_GPIO_PERIPH)) { }
    SysCtlPeripheralEnable(PWMIN_TIMER_PERIPH);
    while (!SysCtlPeripheralReady(PWMIN_TIMER_PERIPH)) { }

    /* Prepare PF3 for alternate function + digital input. */
    HWREG(PWMIN_GPIO_BASE + GPIO_O_AFSEL) |= PWMIN_GPIO_PIN;
    HWREG(PWMIN_GPIO_BASE + GPIO_O_DEN) |= PWMIN_GPIO_PIN;
    HWREG(PWMIN_GPIO_BASE + GPIO_O_AMSEL) &= ~PWMIN_GPIO_PIN;
    HWREG(PWMIN_GPIO_BASE + GPIO_O_DIR) &= ~PWMIN_GPIO_PIN;

    /* Configure Timer1B capture (no NVIC interrupts needed for detection). */
    TimerDisable(PWMIN_TIMER_BASE, PWMIN_TIMER);
    TimerConfigure(PWMIN_TIMER_BASE, TIMER_CFG_SPLIT_PAIR | TIMER_CFG_B_CAP_TIME_UP);
    TimerControlEvent(PWMIN_TIMER_BASE, PWMIN_TIMER, TIMER_EVENT_POS_EDGE);
    TimerLoadSet(PWMIN_TIMER_BASE, PWMIN_TIMER, 0xFFFFU);

    /* Scan mux values 1..15 and look for CAPB events in raw status. */
    for (uint8_t nib = 1U; nib <= 15U; nib++) {
        pwmin_pf3_set_pctl_nibble(nib);

        TimerIntClear(PWMIN_TIMER_BASE, PWMIN_INT_FLAG);
        TimerEnable(PWMIN_TIMER_BASE, PWMIN_TIMER);

        /* Wait ~0.5ms (should include many edges at ~25kHz). */
        if (g_sysclk_hz) {
            const uint32_t loops = (g_sysclk_hz / 3000000U) * 500U;
            SysCtlDelay(loops ? loops : 1U);
        } else {
            SysCtlDelay(20000U);
        }

        const uint32_t raw = TimerIntStatus(PWMIN_TIMER_BASE, true);
        TimerDisable(PWMIN_TIMER_BASE, PWMIN_TIMER);

        if (raw & PWMIN_INT_FLAG) {
            return nib;
        }
    }

    return 0x7U;
}

static void pwmin_quick_capture_probe(uint32_t wait_us)
{
    if (!g_pwmin_verbose) return;
    if (g_sysclk_hz == 0) return;

    /* Wait a short time for capture interrupts to arrive. */
    uint32_t loops = (g_sysclk_hz / 3000000U) * (wait_us ? wait_us : 1U);
    if (loops == 0) loops = 1;
    SysCtlDelay(loops);

    uint32_t edges;
    IntMasterDisable();
    edges = g_edge_count;
    IntMasterEnable();

    /* TivaWare INT_* values match the vector-table index.
       NVIC enable bits use IRQ numbers (vector-16). */
    const int32_t intnum = (int32_t)PWMIN_INT;
    const int32_t irq = intnum - 16;
    uint32_t nvic_enabled = 0;
    if (irq >= 0) {
        const uint32_t reg_index = ((uint32_t)irq) >> 5;
        const uint32_t bit = 1UL << (((uint32_t)irq) & 31U);
        const uint32_t en_reg = HWREG(NVIC_EN0 + (reg_index * 4U));
        nvic_enabled = (en_reg & bit) ? 1U : 0U;
    }

    const uint32_t tis_raw = TimerIntStatus(PWMIN_TIMER_BASE, true);
    const uint32_t tis_masked = TimerIntStatus(PWMIN_TIMER_BASE, false);

    uart0_puts("PWMIN CAPTURE: nvic_en=");
    uart0_put_u32(nvic_enabled);
    uart0_puts(" int=");
    uart0_put_u32((uint32_t)intnum);
    uart0_puts(" irq=");
    uart0_put_u32((irq >= 0) ? (uint32_t)irq : 0U);
    uart0_puts(" raw=");
    uart0_put_u32(tis_raw);
    uart0_puts(" masked=");
    uart0_put_u32(tis_masked);
    uart0_puts(" edges_seen=");
    uart0_put_u32(edges);
    uart0_puts(" (after ");
    uart0_put_u32(wait_us);
    uart0_puts("us)\r\n");
}

static void pwmin_gpio_capture_enable(void)
{
    /* Configure PF3 as GPIO input + both-edge interrupt. */
    SysCtlPeripheralEnable(PWMIN_GPIO_PERIPH);
    while (!SysCtlPeripheralReady(PWMIN_GPIO_PERIPH)) { }

    /* Ensure GPIO mode on PF3. */
    HWREG(PWMIN_GPIO_BASE + GPIO_O_AFSEL) &= ~PWMIN_GPIO_PIN;
    HWREG(PWMIN_GPIO_BASE + GPIO_O_PCTL) &= ~(0xFU << (3U * 4U));
    HWREG(PWMIN_GPIO_BASE + GPIO_O_DEN) |= PWMIN_GPIO_PIN;
    HWREG(PWMIN_GPIO_BASE + GPIO_O_AMSEL) &= ~PWMIN_GPIO_PIN;
    HWREG(PWMIN_GPIO_BASE + GPIO_O_DIR) &= ~PWMIN_GPIO_PIN;
    GPIOPadConfigSet(PWMIN_GPIO_BASE, PWMIN_GPIO_PIN, GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD);

    GPIOIntDisable(PWMIN_GPIO_BASE, PWMIN_GPIO_PIN);
    GPIOIntClear(PWMIN_GPIO_BASE, PWMIN_GPIO_PIN);
    GPIOIntTypeSet(PWMIN_GPIO_BASE, PWMIN_GPIO_PIN, GPIO_BOTH_EDGES);
    GPIOIntEnable(PWMIN_GPIO_BASE, PWMIN_GPIO_PIN);

    /* Ensure the port interrupt is enabled at NVIC. (tach_init already does this.) */
    IntEnable(INT_GPIOF);

    g_last_rise_cycles32 = 0;
    g_have_rise32 = false;
    g_last_period_cycles32 = 0;
    g_have_period32 = false;
    g_last_high_cycles32 = 0;
    g_have_high32 = false;

    g_pwmin_gpio_capture = true;
}

void pwmin_set_verbose(bool enabled)
{
    g_pwmin_verbose = enabled;
}

void pwmin_dbg_session_start(void)
{
    /* Seed the baseline timestamp from the moment PWMINDBG ON is issued.
       Baseline values are populated lazily on the first valid sample(s). */
    uint32_t now_sec = timebase_millis() / 1000U;
    g_dbg_session_active = true;
    g_dbg_last_regime_sec = now_sec;
    g_dbg_warning_pending = false;
    g_dbg_have_baseline_duty = false;
    g_dbg_have_baseline_rpm = false;
    g_dbg_last_duty_pct = 0U;
    g_dbg_last_rpm = 0U;
    g_dbg_prev_tach_active = false;
}

bool pwmin_is_verbose(void)
{
    return g_pwmin_verbose;
}

void pwmin_debug_dump(void)
{
    uart0_puts("PWMIN DBG: enabled=");
    uart0_puts(g_pwmin_enabled ? "1" : "0");
    uart0_puts(" reporting=");
    uart0_puts(g_pwmin_reporting ? "1" : "0");
    uart0_puts(" mode=");
    if (g_pwmin_enabled) {
        uart0_puts(g_pwmin_gpio_capture ? "GPIOF" : "TIMER1B");
    } else {
        uart0_puts("OFF");
    }
    uart0_puts(" pf3_pctl_nibble=");
    uart0_put_u32((uint32_t)g_pwmin_pf3_pctl_nibble);
    uart0_puts("\r\n");

    /* Avoid reconfiguring PF3 while capture is running.
       The probe/mux scan temporarily forces PF3 to GPIO/AF, so only do those
       when capture is currently disabled. */
    if (!g_pwmin_enabled) {
        SysCtlPeripheralEnable(PWMIN_GPIO_PERIPH);
        while (!SysCtlPeripheralReady(PWMIN_GPIO_PERIPH)) { }

        pwmin_quick_gpio_probe(2000U);
        g_pwmin_pf3_pctl_nibble = pwmin_detect_pf3_pctl_nibble();
        uart0_puts("PWMIN MUX: PF3 PCTL nibble=");
        uart0_put_u32((uint32_t)g_pwmin_pf3_pctl_nibble);
        uart0_puts("\r\n");
        return;
    }

    if (!g_pwmin_gpio_capture) {
        pwmin_quick_capture_probe(10000U);
    } else {
        uart0_puts("PWMIN NOTE: GPIO fallback active; timer-capture probe not applicable\r\n");
    }
}

static void pwmin_gpio_capture_disable(void)
{
    GPIOIntDisable(PWMIN_GPIO_BASE, PWMIN_GPIO_PIN);
    GPIOIntClear(PWMIN_GPIO_BASE, PWMIN_GPIO_PIN);
    g_pwmin_gpio_capture = false;
}

void pwmin_gpiof_isr(uint32_t gpiof_status)
{
    (void)gpiof_status;
    if (!g_pwmin_enabled || !g_pwmin_gpio_capture) return;

    /* GPIOF interrupt is already cleared by the port ISR; just process state. */
    const uint32_t now = timebase_cycles32();
    const bool level_high = (GPIOPinRead(PWMIN_GPIO_BASE, PWMIN_GPIO_PIN) != 0);

    g_edge_count++;

    if (level_high) {
        if (g_have_rise32) {
            g_last_period_cycles32 = now - g_last_rise_cycles32;
            g_have_period32 = (g_last_period_cycles32 != 0U);
        }
        g_last_rise_cycles32 = now;
        g_have_rise32 = true;
        return;
    }

    if (g_have_rise32) {
        g_last_high_cycles32 = now - g_last_rise_cycles32;
        g_have_high32 = true;
    }
}

static void pwmin_isr(void)
{
    TimerIntClear(PWMIN_TIMER_BASE, PWMIN_INT_FLAG);

    const uint32_t now = TimerValueGet(PWMIN_TIMER_BASE, PWMIN_TIMER) & 0xFFFFU;

    g_edge_count++;

    if (g_expect_rising) {
        if (g_have_rise) {
            g_last_period = (now - g_last_rise) & 0xFFFFU;
            g_have_period = (g_last_period != 0U);
        }
        g_last_rise = now;
        g_have_rise = true;

        g_expect_rising = false;
        TimerControlEvent(PWMIN_TIMER_BASE, PWMIN_TIMER, TIMER_EVENT_NEG_EDGE);
        return;
    }

    /* Falling edge: high time since last rise. */
    if (g_have_rise) {
        g_last_high = (now - g_last_rise) & 0xFFFFU;
        g_have_high = true;
    }

    g_expect_rising = true;
    TimerControlEvent(PWMIN_TIMER_BASE, PWMIN_TIMER, TIMER_EVENT_POS_EDGE);
}

/* Provide both handlers so the startup vector table can safely point to
   either timer without link errors. Only the enabled timer will actually
   generate interrupts. */
void Timer1BIntHandler(void)
{
    pwmin_isr();
}

static void pwmin_hw_enable(void)
{
    SysCtlPeripheralEnable(PWMIN_GPIO_PERIPH);
    while (!SysCtlPeripheralReady(PWMIN_GPIO_PERIPH)) { }

    SysCtlPeripheralEnable(PWMIN_TIMER_PERIPH);
    while (!SysCtlPeripheralReady(PWMIN_TIMER_PERIPH)) { }

     /* Force PF3 -> (detected) timer capture function via GPIO mux registers.
         Also force PF3 direction to INPUT (capture) explicitly. */
     pwmin_pf3_set_pctl_nibble(g_pwmin_pf3_pctl_nibble);
     HWREG(PWMIN_GPIO_BASE + GPIO_O_AFSEL) |= PWMIN_GPIO_PIN;
     HWREG(PWMIN_GPIO_BASE + GPIO_O_DEN) |= PWMIN_GPIO_PIN;

     /* Ensure digital input path. */
     HWREG(PWMIN_GPIO_BASE + GPIO_O_AMSEL) &= ~PWMIN_GPIO_PIN;
     HWREG(PWMIN_GPIO_BASE + GPIO_O_DIR) &= ~PWMIN_GPIO_PIN;

    GPIOPadConfigSet(PWMIN_GPIO_BASE, PWMIN_GPIO_PIN, GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD);

    TimerDisable(PWMIN_TIMER_BASE, PWMIN_TIMER);
    TimerConfigure(PWMIN_TIMER_BASE, TIMER_CFG_SPLIT_PAIR | TIMER_CFG_B_CAP_TIME_UP);
    TimerControlEvent(PWMIN_TIMER_BASE, PWMIN_TIMER, TIMER_EVENT_POS_EDGE);

    /* 16-bit up-counter wraps at 0xFFFF; period/high deltas are far below that at ~25kHz. */
    TimerLoadSet(PWMIN_TIMER_BASE, PWMIN_TIMER, 0xFFFFU);

    TimerIntDisable(PWMIN_TIMER_BASE, PWMIN_INT_FLAG);
    TimerIntClear(PWMIN_TIMER_BASE, PWMIN_INT_FLAG);
    TimerIntEnable(PWMIN_TIMER_BASE, PWMIN_INT_FLAG);

    IntDisable(PWMIN_INT);
    IntEnable(PWMIN_INT);

    TimerEnable(PWMIN_TIMER_BASE, PWMIN_TIMER);

    g_last_rise = 0;
    g_have_rise = false;
    g_last_period = 0;
    g_have_period = false;
    g_last_high = 0;
    g_have_high = false;
    g_edge_count = 0;
    g_expect_rising = true;

    /* If timer capture isn't producing events, fall back to GPIO edge timing. */
    if ((TimerIntStatus(PWMIN_TIMER_BASE, true) & PWMIN_INT_FLAG) == 0U) {
        /* Give it a brief chance (~2ms). */
        SysCtlDelay((g_sysclk_hz / 3000000U) * 2000U);
    }
    if ((TimerIntStatus(PWMIN_TIMER_BASE, true) & PWMIN_INT_FLAG) == 0U) {
        TimerDisable(PWMIN_TIMER_BASE, PWMIN_TIMER);
        TimerIntDisable(PWMIN_TIMER_BASE, PWMIN_INT_FLAG);
        TimerIntClear(PWMIN_TIMER_BASE, PWMIN_INT_FLAG);
        IntDisable(PWMIN_INT);

        pwmin_gpio_capture_enable();
        if (g_pwmin_verbose) {
            uart0_puts("PWMIN NOTE: Timer1B capture not firing; using GPIOF PF3 BOTH_EDGES fallback\r\n");
        }
    } else {
        g_pwmin_gpio_capture = false;
    }

    g_pwmin_enabled = true;
}

static void pwmin_hw_disable(void)
{
    IntDisable(PWMIN_INT);
    TimerIntDisable(PWMIN_TIMER_BASE, PWMIN_INT_FLAG);
    TimerIntClear(PWMIN_TIMER_BASE, PWMIN_INT_FLAG);
    TimerDisable(PWMIN_TIMER_BASE, PWMIN_TIMER);

    pwmin_gpio_capture_disable();

    /* Return PF3 to GPIO input (Hi-Z). */
    GPIOPinTypeGPIOInput(PWMIN_GPIO_BASE, PWMIN_GPIO_PIN);
    GPIOPadConfigSet(PWMIN_GPIO_BASE, PWMIN_GPIO_PIN, GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD);

    g_pwmin_enabled = false;
}

void pwmin_init(uint32_t sysclk_hz)
{
    g_sysclk_hz = sysclk_hz;
    g_pwmin_enabled = false;
    g_pwmin_reporting = false;
    g_pwmin_print_enabled = true;
    g_next_report_ms = 0;
}

void pwmin_set_enabled(bool enabled)
{
    if (enabled) {
        if (g_pwmin_enabled) return;
        pwmin_hw_enable();
        return;
    }

    if (!g_pwmin_enabled) return;
    pwmin_hw_disable();
}

bool pwmin_is_enabled(void)
{
    return g_pwmin_enabled;
}

void pwmin_set_reporting_ex(bool enabled, bool print_enabled)
{
    g_pwmin_reporting = enabled;
    g_pwmin_print_enabled = print_enabled;
    g_next_report_ms = timebase_millis() + 1000U;

    if (enabled) {
        /* In normal PWMIN mode we stay quiet. Any probe/mux/capture diagnostics
           are requested explicitly via PWMINDBG (verbose). */

        pwmin_set_enabled(true);

        if (g_pwmin_verbose) {
            uart0_puts("PWMIN ON: ");
            uart0_puts(PWMIN_PINNAME_STR);
            uart0_puts(g_pwmin_gpio_capture ? " [MODE=GPIOF]" : " [MODE=TIMER1B]");
            uart0_puts("\r\n");
        }
        return;
    }

    if (g_pwmin_verbose) {
        uart0_puts("PWMIN OFF\r\n");
    }
}

void pwmin_set_reporting(bool enabled)
{
    pwmin_set_reporting_ex(enabled, true);
}

bool pwmin_is_reporting(void)
{
    return g_pwmin_reporting;
}

bool pwmin_is_printing(void)
{
    return g_pwmin_reporting && g_pwmin_print_enabled;
}

bool pwmin_get_last(uint32_t *freq_hz_out, uint32_t *duty_percent_out)
{
    if (!freq_hz_out || !duty_percent_out) return false;

    IntMasterDisable();
    const uint32_t f = g_last_valid_freq_hz;
    const uint32_t duty_x10 = g_last_valid_duty_x10;
    const bool have = g_have_last_valid;
    IntMasterEnable();

    if (!have) {
        *freq_hz_out = 0U;
        *duty_percent_out = 0U;
        return false;
    }

    *freq_hz_out = f;
    *duty_percent_out = duty_x10 / 10U;
    return true;
}

bool pwmin_get_last_duty_x10(uint32_t *duty_x10_out)
{
    if (!duty_x10_out) return false;
    IntMasterDisable();
    const uint32_t duty_x10 = g_last_valid_duty_x10;
    const bool have = g_have_last_valid;
    IntMasterEnable();
    *duty_x10_out = have ? duty_x10 : 0U;
    return have;
}

uint32_t pwmin_get_suppressed_samples(void)
{
    IntMasterDisable();
    uint32_t v = g_pwmin_suppressed_samples;
    IntMasterEnable();
    return v;
}

void pwmin_task(void)
{
    if (!g_pwmin_reporting) return;

    uint32_t now_ms = timebase_millis();
    if ((int32_t)(now_ms - g_next_report_ms) < 0) {
        return;
    }
    g_next_report_ms += 1000U;

    uint32_t period;
    uint32_t high;
    uint32_t edges;
    bool have_period;
    bool have_high;

    IntMasterDisable();
    if (g_pwmin_gpio_capture) {
        period = g_last_period_cycles32;
        high = g_last_high_cycles32;
        have_period = g_have_period32;
        have_high = g_have_high32;
    } else {
        period = g_last_period;
        high = g_last_high;
        have_period = g_have_period;
        have_high = g_have_high;
    }
    edges = g_edge_count;
    g_edge_count = 0;
    IntMasterEnable();

    uint32_t freq = 0U;
    uint32_t duty_x10 = 0U;
    uint32_t freq_x10 = 0U;
    bool have_sample = false;
    if (have_period && have_high && period != 0U) {
        have_sample = true;
        freq = g_sysclk_hz / period;
        duty_x10 = (uint32_t)((((uint64_t)high * 1000ULL) + ((uint64_t)period / 2ULL)) / (uint64_t)period);
        freq_x10 = (uint32_t)((((uint64_t)g_sysclk_hz * 10ULL) + ((uint64_t)period / 2ULL)) / (uint64_t)period);
    }

    bool valid = false;
    if (have_sample) {
        /* Additional sanity checks: avoid single-sample spikes (e.g. 400kHz). */
        if (freq >= PWMIN_VALID_MIN_HZ && freq <= PWMIN_VALID_MAX_HZ && duty_x10 <= 1000U && high <= period) {
            valid = true;
        }
    }

    if (valid) {
        IntMasterDisable();
        g_last_valid_freq_hz = freq;
        g_last_valid_duty_x10 = duty_x10;
        g_have_last_valid = true;
        IntMasterEnable();
    } else if (have_sample) {
        /* We had a measurement, but it was implausible. Count it and suppress output/acceptance. */
        IntMasterDisable();
        g_pwmin_suppressed_samples++;
        IntMasterEnable();
        return;
    }

    if (g_pwmin_print_enabled) {
        if (valid) {
            uart0_puts("PWMIN: f=");
            uart0_put_u32(freq);
            uart0_puts("Hz duty=");
            uart0_put_u32_1dp(duty_x10);
            uart0_puts("%\r\n");
        } else {
            /* No measurement yet (or missing), keep prior behavior: show zeros. */
            uart0_puts("PWMIN: f=0Hz duty=0.0%\r\n");
        }

        /* Preserve the useful debug counters, but only in verbose mode. */
        if (g_pwmin_verbose && valid) {
            const uint32_t now_sec = now_ms / 1000U;
            const uint32_t duty_pct = (duty_x10 + 5U) / 10U;
            bool armed_warning_this_tick = false;

            /* Regime-change detection (armed by PWMINDBG ON). */
            if (g_dbg_session_active) {
                bool tach_active = tach_is_reporting();
                uint32_t rpm = 0U;
                bool have_rpm = false;
                if (tach_active != g_dbg_prev_tach_active) {
                    /* Edge-triggered handling so TACHIN ON captures a fresh baseline,
                       and TACHIN OFF prevents any pending warning from leaking out. */
                    if (!tach_active) {
                        g_dbg_warning_pending = false;
                        g_dbg_have_baseline_rpm = false;
                    } else {
                        g_dbg_have_baseline_rpm = false;
                        /* Reset duty baseline at the moment TACHIN becomes active so
                           we don't compare against a duty baseline captured while
                           TACHIN was inactive. */
                        g_dbg_last_duty_pct = duty_pct;
                        g_dbg_have_baseline_duty = true;
                    }
                    g_dbg_prev_tach_active = tach_active;
                }

                if (tach_active) {
                    have_rpm = tach_get_last_rpm(&rpm);
                    if (have_rpm && !g_dbg_have_baseline_rpm) {
                        /* First valid RPM after TACHIN becomes active seeds the baseline.
                           Do not emit a regime-change warning on this baseline capture. */
                        g_dbg_last_rpm = rpm;
                        g_dbg_have_baseline_rpm = true;
                    }
                }

                if (!g_dbg_have_baseline_duty) {
                    g_dbg_last_duty_pct = duty_pct;
                    g_dbg_have_baseline_duty = true;
                }

                bool regime_change = false;
                if (tach_active && have_rpm && g_dbg_have_baseline_rpm) {
                    /* Only arm warnings when TACHIN is active and RPM baseline exists.
                       While active, treat either PWM duty or RPM jumps as a regime change. */
                    const bool duty_jump = g_dbg_have_baseline_duty && rel_change_gt_15pct(g_dbg_last_duty_pct, duty_pct);
                    const bool rpm_jump = rel_change_gt_15pct(g_dbg_last_rpm, rpm);
                    regime_change = duty_jump || rpm_jump;
                }

                if (regime_change && !g_dbg_warning_pending) {
                    g_dbg_warning_pending = true;
                    g_dbg_pending_new_regime_sec = now_sec;
                    armed_warning_this_tick = true;

                    g_dbg_prev_duty_pct = g_dbg_last_duty_pct;
                    g_dbg_new_duty_pct = duty_pct;

                    g_dbg_prev_rpm = g_dbg_have_baseline_rpm ? g_dbg_last_rpm : 0U;
                    g_dbg_new_rpm = (tach_active && have_rpm) ? rpm : g_dbg_prev_rpm;
                }

                /* Update baselines for the next comparison window. */
                g_dbg_last_duty_pct = duty_pct;
                if (tach_active && have_rpm) {
                    g_dbg_last_rpm = rpm;
                    g_dbg_have_baseline_rpm = true;
                }
            }

            if (g_dbg_warning_pending) {
                /* Strictly gate the user-visible warning on TACHIN still being active.
                   If TACHIN was turned off, drop the pending warning silently. */
                if (!tach_is_reporting()) {
                    g_dbg_warning_pending = false;
                }
            }

            if (g_dbg_warning_pending && !armed_warning_this_tick) {
                /* 1) Timestamped warning line (printed on the next tick). */
                uart0_put_hhmmss(now_sec);
                uart0_puts(" *** WARNING - REGIME CHANGE ****\r\n");

                /* 2) Previous -> new regime timestamps. */
                uart0_puts("PREV. REGIME ");
                uart0_put_hhmmss(g_dbg_last_regime_sec);
                uart0_puts(" --> NEW REGIME: ");
                uart0_put_hhmmss(g_dbg_pending_new_regime_sec);
                uart0_puts("\r\n");

                /* 3) Numeric deltas (rounded to integer). */
                uart0_puts("PWM DUTY ---> ");
                uart0_put_u32(g_dbg_prev_duty_pct);
                uart0_puts(" %TO ");
                uart0_put_u32(g_dbg_new_duty_pct);
                uart0_puts(" % ;  TACH RPM ---> ");
                uart0_put_u32_zpad5(g_dbg_prev_rpm);
                uart0_puts(" to ");
                uart0_put_u32_zpad5(g_dbg_new_rpm);
                uart0_puts("\r\n");

                g_dbg_last_regime_sec = g_dbg_pending_new_regime_sec;
                g_dbg_warning_pending = false;
            }

            /* Timestamped per-second PWMINDBG line. */
            uart0_put_hhmmss(now_sec);
            uart0_puts(" ");
            uart0_puts("PWMIN DBG: f=");
            uart0_put_u32_1dp(freq_x10);
            uart0_puts("Hz duty=");
            uart0_put_u32_1dp(duty_x10);
            uart0_puts("% period_cycles=");
            uart0_put_u32(period);
            uart0_puts(" high_cycles=");
            uart0_put_u32(high);
            uart0_puts(" edges=");
            uart0_put_u32(edges);
            uart0_puts("\r\n");
        }
    }
}
