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
 * Default wiring (can be overridden by defines at compile time):
 * - TACH signal -> GPIOM3 (PM3)
 * - Uses internal weak pull-up (3.3V). Do NOT pull up to +5V directly.
 */
#ifndef TACH_GPIO_PERIPH
#define TACH_GPIO_PERIPH SYSCTL_PERIPH_GPIOM
#endif
#ifndef TACH_GPIO_BASE
#define TACH_GPIO_BASE GPIO_PORTM_BASE
#endif
#ifndef TACH_GPIO_PIN
#define TACH_GPIO_PIN GPIO_PIN_3
#endif
#ifndef TACH_GPIO_INT
#define TACH_GPIO_INT INT_GPIOM
#endif

void tach_init(void);

/* Enable/disable GPIO interrupt capture on the tach pin (PM3 by default). */
void tach_set_capture_enabled(bool enabled);
bool tach_is_capture_enabled(void);

/* Enable/disable periodic reporting to UART0 (ICDI). */
void tach_set_reporting(bool enabled);
bool tach_is_reporting(void);

/* Call periodically from the main loop. */
void tach_task(void);

#endif /* TACH_H */
