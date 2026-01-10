#include "timebase.h"

#include <stdbool.h>
#include <stdint.h>

#include "driverlib/interrupt.h"
#include "driverlib/systick.h"

static volatile uint32_t g_ms_ticks = 0;
static uint32_t g_sysclk_hz = 0;
static uint32_t g_systick_reload = 0;

void SysTickIntHandler(void)
{
    g_ms_ticks++;
}

void timebase_init(uint32_t sysClockHz)
{
    g_ms_ticks = 0;
    g_sysclk_hz = sysClockHz;

    /* 1ms tick */
    g_systick_reload = sysClockHz / 1000U;
    if (g_systick_reload == 0) {
        g_systick_reload = 1;
    }

    SysTickPeriodSet(g_systick_reload);
    SysTickIntEnable();
    SysTickEnable();
}

uint32_t timebase_millis(void)
{
    uint32_t t;
    IntMasterDisable();
    t = g_ms_ticks;
    IntMasterEnable();
    return t;
}

uint32_t timebase_sysclk_hz(void)
{
    return g_sysclk_hz;
}

uint32_t timebase_cycles32(void)
{
    /*
     * SysTick counts DOWN from reload to 0 each millisecond.
     * We compute a monotonic-ish cycle count:
     *   cycles = ms * reload + (reload - current_val)
     *
     * To avoid race around the millisecond boundary, sample ms twice.
     */
    uint32_t ms1, ms2, val;
    uint32_t reload = g_systick_reload;

    if (reload == 0) {
        return 0;
    }

    do {
        ms1 = g_ms_ticks;
        val = SysTickValueGet();
        ms2 = g_ms_ticks;
    } while (ms1 != ms2);

    uint32_t elapsed = reload - val;
    return (ms1 * reload) + elapsed;
}
