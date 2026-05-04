#ifndef PSU_TACHMAP_H
#define PSU_TACHMAP_H

#include <stdbool.h>
#include <stdint.h>

/*
 * PSU tach mapper (regime-based):
 * - Reads sensed PSU PWM duty (PF3 via PWMIN engine)
 * - Selects the active operating regime based on a threshold + hysteresis rule
 * - Drives synthetic tach feedback on PM3 via TACHSYN at the regime's upper RPM
 * - Applies a fixed low PWM duty to PF2 (real fan) while active,
 *   keeping the real fan quiet regardless of the IBM-commanded duty
 *
 * Regime transitions:
 *   Ascending  — switch up immediately when duty >= entry_duty_x10
 *   Descending — switch down only when duty < (entry_duty_x10 - hyst_x10)
 *
 * To add/remove set points: edit g_regimes[] in psu_tachmap.c only.
 * The table MUST remain sorted ascending by entry_duty_x10.
 */

typedef struct {
    const char    *tag;            /* human-readable label, e.g. "COLD BOOT"  */
    uint16_t       entry_duty_x10; /* rising threshold in 0.1% units           */
    uint16_t       synth_rpm;      /* TACHSYN RPM to synthesise in this regime */
} psu_regime_t;

/* Default hysteresis (0.1% units) — duty must drop by this much below the
   entry threshold before the regime steps down. */
#define PSU_TACHMAP_HYST_X10_DEFAULT      150U   /* 15.0% — wide band; real fan RPM
                                                    varies inside a regime, IBM only
                                                    needs a plausible midpoint tach */

/* Default fixed fan duty applied to PF2 while PSUTACH is active.
   Tune at runtime with: PSUTACH FANDUTY n */
#define PSU_TACHMAP_FAN_FIXED_DUTY_DEFAULT 30U   /* % */

void psu_tachmap_init(void);

void psu_tachmap_set_enabled(bool enabled);
bool psu_tachmap_is_enabled(void);

/* Hysteresis band (0.1% units). */
void     psu_tachmap_set_hyst_x10(uint32_t hyst_x10);
uint32_t psu_tachmap_get_hyst_x10(void);

/* Fixed PWM duty (5–96 %) applied to PF2 while PSUTACH is active. */
void     psu_tachmap_set_fan_fixed_duty(uint32_t pct);
uint32_t psu_tachmap_get_fan_fixed_duty(void);

/* Current active regime. Returns -1 / NULL when below all thresholds. */
int         psu_tachmap_get_regime_index(void);
const char *psu_tachmap_get_regime_tag(void);

/* Main-loop task. Safe to call often. */
void psu_tachmap_task(void);

#endif /* PSU_TACHMAP_H */
