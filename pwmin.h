#ifndef PWMIN_H
#define PWMIN_H

#include <stdbool.h>
#include <stdint.h>

/* PSU PWM sensing input (captures external PWM duty/frequency).
 *
 * Pinning:
 * - PWM input: PF3 / T1CCP1 (Timer1B capture)
 */

void pwmin_init(uint32_t sysclk_hz);

void pwmin_set_enabled(bool enabled);
bool pwmin_is_enabled(void);

/* Enable/disable periodic reporting to UART0 (ICDI).
 * When enabled, capture is also enabled.
 */
void pwmin_set_reporting(bool enabled);
bool pwmin_is_reporting(void);

/* Enable/disable verbose diagnostic output on UART0.
 * This is intended to be toggled via a separate CLI command (not PWMIN).
 */
void pwmin_set_verbose(bool enabled);
bool pwmin_is_verbose(void);

/* Print a diagnostic dump to UART0.
 * Safe to call at any time; when PWMIN is actively capturing, this avoids
 * reconfiguring PF3.
 */
void pwmin_debug_dump(void);

/* Call periodically from the main loop. */
void pwmin_task(void);

/* Latest measured values (snapshotted from ISR-updated state). */
bool pwmin_get_last(uint32_t *freq_hz_out, uint32_t *duty_percent_out);

/* Called from GPIOFIntHandler when PF3 interrupt is enabled for PWMIN.
 * (Internal hook; safe to call even when PWMIN is disabled.)
 */
void pwmin_gpiof_isr(uint32_t gpiof_status);

#endif /* PWMIN_H */
