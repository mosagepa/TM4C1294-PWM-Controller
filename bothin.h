#ifndef BOTHIN_H
#define BOTHIN_H

#include <stdbool.h>
#include <stdint.h>

/* Combined PWMIN + TACHIN reporting to UART0 (ICDI).
 *
 * Output is intentionally minimal: duty (0.1%) + RPM (integer) only.
 */

void bothin_set_enabled(bool enabled);
bool bothin_is_enabled(void);

/* Call periodically from the main loop. */
void bothin_task(void);

#endif /* BOTHIN_H */
