#ifndef TSYN_H
#define TSYN_H

#include <stdbool.h>
#include <stdint.h>

/*
 * TSYN: TACH signal synthesizer.
 *
 * Historically: generates a bursty tach-like waveform on PM3 based on the
 * currently applied PSYN 'n' value (requested PWM percent).
 *
 * Additionally: supports a continuous square-wave output on PM3 (TACHSYN)
 * for mimicking fan tach feedback to a PSU.
 */

void tsyn_init(uint32_t sysclk_hz);

void tsyn_set_enabled(bool enabled);
bool tsyn_is_enabled(void);

typedef enum {
	TACHSYN_DRIVE_OPENDRAIN = 0,
	TACHSYN_DRIVE_PUSHPULL  = 1,
} tachsyn_drive_mode_t;

/*
 * TACHSYN continuous mode (PM3 output, open-drain):
 * Generates a continuous PWM-like square wave at base_freq_hz and duty_percent.
 * Typical use for IBM PSU probing: duty_percent=50, base_freq_hz=168 or 235.
 */
void tachsyn_set_drive_mode(tachsyn_drive_mode_t mode);
tachsyn_drive_mode_t tachsyn_get_drive_mode(void);
void tachsyn_set_continuous(uint32_t base_freq_hz, uint32_t duty_percent);
void tachsyn_stop(void);
bool tachsyn_is_running(void);

/*
 * TSYN BOOT mode (Jan 2026):
 * Generates a time-based TACHSYN profile on PM3 intended to mimic the IBM PSU
 * “boot expectation” behavior discussed in lab notes.
 *
 * Driven from the main loop via tachsyn_boot_task() (no IRQs required).
 */
void tachsyn_boot_start(void);
void tachsyn_boot_stop(void);
bool tachsyn_boot_is_running(void);
void tachsyn_boot_task(void);

/* TSYN COPY mode:
 * Mirrors the tach input pin state onto PM3 (tach out) edge-for-edge.
 * Implemented via the tach input ISR (GPIOFIntHandler) switching to BOTH_EDGES.
 */
void tachsyn_copy_begin(void);
void tachsyn_copy_end(void);
bool tachsyn_copy_is_running(void);

/* TACH LOOPBACK test mode.
 * This is a firmware-side guard/flag used by the UART3 command layer.
 * Physical loopback is done externally by jumpering PM3 (TACH OUT) to PF1 (TACH IN).
 */
void tach_loopback_begin(uint32_t expected_hz);
void tach_loopback_end(void);
bool tach_loopback_is_running(void);
uint32_t tach_loopback_expected_hz(void);

#endif /* TSYN_H */
