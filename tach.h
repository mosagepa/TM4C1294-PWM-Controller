#ifndef TACH_H
#define TACH_H

#include <stdbool.h>
#include <stdint.h>

/* Needed for GPIO_PORT*_BASE, UART0_BASE, GPIO_PIN_*, SYSCTL_PERIPH_GPIO* */
#include "inc/hw_memmap.h"
#include "driverlib/gpio.h"
#include "driverlib/sysctl.h"
#include "inc/hw_ints.h"

/*
 * TACH input (open-collector) sensing.
 *
 * IMPORTANT: PM3 is reserved for synthesized tach output (TACHSYN).
 * TACH input is intentionally on a different pin so TSYN/PHASE cannot
 * accidentally reconfigure the same physical line as both input and output.
 */
#ifndef TACH_GPIO_PERIPH
#define TACH_GPIO_PERIPH SYSCTL_PERIPH_GPIOF
#endif
#ifndef TACH_GPIO_BASE
#define TACH_GPIO_BASE GPIO_PORTF_BASE
#endif
#ifndef TACH_GPIO_PIN
#define TACH_GPIO_PIN GPIO_PIN_1
#endif
#ifndef TACH_GPIO_INT
#define TACH_GPIO_INT INT_GPIOF
#endif

void tach_init(void);

/* Enable/disable GPIO interrupt capture on the tach input pin (PF1 by default). */
void tach_set_capture_enabled(bool enabled);
bool tach_is_capture_enabled(void);

/* Enable/disable periodic reporting to UART0 (ICDI). */
void tach_set_reporting(bool enabled);
bool tach_is_reporting(void);

/* Loopback self-test helper: when expected_hz is nonzero, tach_task prints an OK/FAIL hint. */
void tach_set_loopback_expected_hz(uint32_t expected_hz);
uint32_t tach_get_loopback_expected_hz(void);

/* Exact edge-for-edge mirroring of TACH input onto PM3 (tach out). */
void tach_set_copy_to_pm3(bool enabled);

/* Call periodically from the main loop. */
void tach_task(void);

#endif /* TACH_H */
