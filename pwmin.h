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

/* Extended reporting control: allow capture/reporting to run while
	suppressing the normal 1 Hz UART0 print (useful for combined modes). */
void pwmin_set_reporting_ex(bool enabled, bool print_enabled);
bool pwmin_is_printing(void);

/* Enable/disable verbose diagnostic output on UART0.
 * This is intended to be toggled via a separate CLI command (not PWMIN).
 */
void pwmin_set_verbose(bool enabled);
bool pwmin_is_verbose(void);

/* Mark the start of a PWMINDBG verbose session.
 * Used to seed the regime-change timestamp baseline when PWMINDBG ON is issued.
 */
void pwmin_dbg_session_start(void);

/* Print a diagnostic dump to UART0.
 * Safe to call at any time; when PWMIN is actively capturing, this avoids
 * reconfiguring PF3.
 */
void pwmin_debug_dump(void);

/* Call periodically from the main loop. */
void pwmin_task(void);

/* Latest measured values (snapshotted from ISR-updated state). */
bool pwmin_get_last(uint32_t *freq_hz_out, uint32_t *duty_percent_out);

/* Latest measured duty in 0.1% units (e.g. 543 => 54.3%). */
bool pwmin_get_last_duty_x10(uint32_t *duty_x10_out);

/* Count of outlier samples suppressed (not printed / not accepted as last-valid), since boot. */
uint32_t pwmin_get_suppressed_samples(void);

/* Called from GPIOFIntHandler when PF3 interrupt is enabled for PWMIN.
 * (Internal hook; safe to call even when PWMIN is disabled.)
 */
void pwmin_gpiof_isr(uint32_t gpiof_status);

#endif /* PWMIN_H */
