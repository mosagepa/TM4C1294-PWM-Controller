#ifndef TSYN_H
#define TSYN_H

#include <stdbool.h>
#include <stdint.h>

/*
 * TSYN: TACH signal synthesizer.
 *
 * Generates a bursty tach-like waveform on PM3 based on the currently applied
 * PSYN 'n' value (requested PWM percent). Intended for lab diagnostics and for
 * feeding external PS electronics with a clean, controllable tach signature.
 */

void tsyn_init(uint32_t sysclk_hz);

void tsyn_set_enabled(bool enabled);
bool tsyn_is_enabled(void);

#endif /* TSYN_H */
