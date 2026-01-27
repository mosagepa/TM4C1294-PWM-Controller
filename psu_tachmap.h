#ifndef PSU_TACHMAP_H
#define PSU_TACHMAP_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PSU_TACHMAP_PHASE1 = 1,
    PSU_TACHMAP_PHASE2 = 2,
} psu_tachmap_phase_t;

/*
 * PSU tach mapper:
 * - Reads sensed PSU PWM duty (PF3 via PWMIN engine)
 * - Drives synthetic tach feedback on PM3 using TACHSYN continuous mode
 *
 * Phase2 is intentionally user-triggered (or externally triggered) because
 * in the observed logs the PSU PWM duty stays ~constant while the fan RPM jumps.
 */

void psu_tachmap_init(void);

void psu_tachmap_set_enabled(bool enabled);
bool psu_tachmap_is_enabled(void);

void psu_tachmap_set_phase(psu_tachmap_phase_t phase);
psu_tachmap_phase_t psu_tachmap_get_phase(void);

/* Phase2 behavior: clamp the synthesized RPM to at least this value when duty is high. */
void psu_tachmap_set_phase2_min_rpm(uint32_t rpm);
uint32_t psu_tachmap_get_phase2_min_rpm(void);

/* Rate limit for synthesized RPM changes (ramp). 0 disables rate limiting. */
void psu_tachmap_set_ramp_rpm_per_s(uint32_t rpm_per_s);
uint32_t psu_tachmap_get_ramp_rpm_per_s(void);

/* Main-loop task. Safe to call often. */
void psu_tachmap_task(void);

#endif /* PSU_TACHMAP_H */
