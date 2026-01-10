#ifndef TIMEBASE_H
#define TIMEBASE_H

#include <stdint.h>

/*
 * Minimal SysTick-based millisecond timebase.
 *
 * Note: Requires SysTick vector in TM4C1294XL_startup.c to point to
 * SysTickIntHandler() (provided by timebase.c).
 */
void timebase_init(uint32_t sysClockHz);
uint32_t timebase_millis(void);

/*
 * Returns a 32-bit cycle counter based on SysTick.
 * Wraps naturally; intended for short delta measurements.
 */
uint32_t timebase_cycles32(void);

/* Returns the system clock (Hz) passed to timebase_init(). */
uint32_t timebase_sysclk_hz(void);

#endif /* TIMEBASE_H */
