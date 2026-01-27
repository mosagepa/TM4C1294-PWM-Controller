#include "psu_tachmap.h"

#include <stdbool.h>
#include <stdint.h>

#include "pwmin.h"
#include "timebase.h"
#include "tsyn.h"
#include "tach_default.h"

/* Standard 4-wire fan tach is typically 2 pulses per revolution. */
#define PSU_TACHMAP_PULSES_PER_REV 2U

/* Duty threshold where we treat it as “high” (used for Phase2 clamp). */
#define PSU_TACHMAP_HIGH_DUTY_X10 450U /* 45.0% */

/* Default Phase2 minimum RPM based on observed IMM2 behavior. */
#define PSU_TACHMAP_PHASE2_MIN_RPM_DEFAULT 11000U

/* Default RPM ramp limit (0 = disabled). */
#define PSU_TACHMAP_RAMP_RPM_PER_S_DEFAULT 1500U

typedef struct {
    uint16_t duty_x10; /* 0.1% units */
    uint16_t rpm;
} psu_tachmap_point_t;

/*
 * Phase1 LUT extracted/approximated from REGIMELOGS/ENCENDIDO_02.TXT.
 * This is not meant to be perfect; it just gives a plausible monotonic ramp
 * into the stable ~6.3k RPM regime at ~47-49% duty.
 */
static const psu_tachmap_point_t g_phase1_points[] = {
    {   0,     0 },
    {  80,     0 },  /* ~8%: still stopped */
    { 150,  1500 },
    { 250,  2000 },
    { 350,  3200 },
    { 385,  3800 },
    { 415,  4020 },
    { 445,  4920 },
    { 461,  5520 },
    { 474,  6120 },
    { 488,  6360 },
    { 520,  6500 },
};

static volatile bool g_enabled = false;
static volatile psu_tachmap_phase_t g_phase = PSU_TACHMAP_PHASE1;

static volatile uint32_t g_phase2_min_rpm = PSU_TACHMAP_PHASE2_MIN_RPM_DEFAULT;
static volatile uint32_t g_ramp_rpm_per_s = PSU_TACHMAP_RAMP_RPM_PER_S_DEFAULT;

static uint32_t g_last_task_ms = 0;
static uint32_t g_curr_rpm = 0;
static uint32_t g_last_freq_hz = 0;

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static uint32_t interp_phase1_rpm(uint32_t duty_x10)
{
    const uint32_t count = (uint32_t)(sizeof(g_phase1_points) / sizeof(g_phase1_points[0]));
    if (count == 0) return 0;

    if (duty_x10 <= g_phase1_points[0].duty_x10) {
        return g_phase1_points[0].rpm;
    }
    if (duty_x10 >= g_phase1_points[count - 1U].duty_x10) {
        return g_phase1_points[count - 1U].rpm;
    }

    for (uint32_t i = 0; i + 1U < count; i++) {
        const uint32_t x0 = g_phase1_points[i].duty_x10;
        const uint32_t x1 = g_phase1_points[i + 1U].duty_x10;
        if (duty_x10 < x0 || duty_x10 > x1) {
            continue;
        }

        const uint32_t y0 = g_phase1_points[i].rpm;
        const uint32_t y1 = g_phase1_points[i + 1U].rpm;
        const uint32_t dx = x1 - x0;
        const uint32_t xn = duty_x10 - x0;

        if (dx == 0U) {
            return y0;
        }

        /* Linear interpolation with rounding. */
        const int32_t dy = (int32_t)y1 - (int32_t)y0;
        int32_t y = (int32_t)y0 + (int32_t)(((int64_t)dy * (int64_t)xn + (int64_t)(dx / 2U)) / (int64_t)dx);
        if (y < 0) y = 0;
        return (uint32_t)y;
    }

    return g_phase1_points[0].rpm;
}

static uint32_t rpm_to_tach_hz(uint32_t rpm)
{
    if (rpm == 0U) return 0U;

    /* freq_hz = rpm * PPR / 60; rounding to nearest. */
    uint64_t num = (uint64_t)rpm * (uint64_t)PSU_TACHMAP_PULSES_PER_REV;
    uint32_t hz = (uint32_t)((num + 30ULL) / 60ULL);
    if (hz == 0U) hz = 1U;
    return hz;
}

void psu_tachmap_init(void)
{
    g_enabled = false;
    g_phase = PSU_TACHMAP_PHASE1;

    g_phase2_min_rpm = PSU_TACHMAP_PHASE2_MIN_RPM_DEFAULT;
    g_ramp_rpm_per_s = PSU_TACHMAP_RAMP_RPM_PER_S_DEFAULT;

    g_last_task_ms = timebase_millis();
    g_curr_rpm = 0;
    g_last_freq_hz = 0;
}

void psu_tachmap_set_enabled(bool enabled)
{
    if (enabled) {
        if (g_enabled) return;

        /* Ensure PWMIN capture is running so we can observe the PSU duty. */
        if (!pwmin_is_enabled()) {
            pwmin_set_enabled(true);
        }

        /* Prefer push-pull to match PHASE behavior driving the external interface. */
        tachsyn_set_drive_mode(TACHSYN_DRIVE_PUSHPULL);

        g_last_task_ms = timebase_millis();
        g_curr_rpm = 0;
        g_last_freq_hz = 0;
        g_enabled = true;
        return;
    }

    if (!g_enabled) return;
    g_enabled = false;

    /* Stop our synthetic tach and restore the persisted default behavior. */
    tachsyn_stop();
    tach_default_apply();
}

bool psu_tachmap_is_enabled(void)
{
    return g_enabled;
}

void psu_tachmap_set_phase(psu_tachmap_phase_t phase)
{
    if (phase != PSU_TACHMAP_PHASE1 && phase != PSU_TACHMAP_PHASE2) {
        return;
    }
    g_phase = phase;
}

psu_tachmap_phase_t psu_tachmap_get_phase(void)
{
    return g_phase;
}

void psu_tachmap_set_phase2_min_rpm(uint32_t rpm)
{
    /* Reasonable bounds for safety. */
    g_phase2_min_rpm = clamp_u32(rpm, 0U, 30000U);
}

uint32_t psu_tachmap_get_phase2_min_rpm(void)
{
    return g_phase2_min_rpm;
}

void psu_tachmap_set_ramp_rpm_per_s(uint32_t rpm_per_s)
{
    /* 0 disables. Upper bound avoids overflow/insane ramps. */
    g_ramp_rpm_per_s = clamp_u32(rpm_per_s, 0U, 60000U);
}

uint32_t psu_tachmap_get_ramp_rpm_per_s(void)
{
    return g_ramp_rpm_per_s;
}

void psu_tachmap_task(void)
{
    if (!g_enabled) return;

    uint32_t duty_x10 = 0;
    if (!pwmin_get_last_duty_x10(&duty_x10)) {
        return;
    }

    uint32_t target_rpm = interp_phase1_rpm(duty_x10);

    if (g_phase == PSU_TACHMAP_PHASE2 && duty_x10 >= PSU_TACHMAP_HIGH_DUTY_X10) {
        if (target_rpm < g_phase2_min_rpm) {
            target_rpm = g_phase2_min_rpm;
        }
    }

    /* Rate limit changes for realism and to avoid huge jumps. */
    uint32_t now = timebase_millis();
    uint32_t elapsed_ms = now - g_last_task_ms;
    if (elapsed_ms > 5000U) {
        /* If the loop stalled, cap dt so we don’t do a massive jump. */
        elapsed_ms = 5000U;
    }
    g_last_task_ms = now;

    if (g_ramp_rpm_per_s == 0U) {
        g_curr_rpm = target_rpm;
    } else {
        uint32_t max_delta = (g_ramp_rpm_per_s * elapsed_ms) / 1000U;
        if (max_delta == 0U) max_delta = 1U;

        if (g_curr_rpm < target_rpm) {
            uint32_t delta = target_rpm - g_curr_rpm;
            if (delta > max_delta) delta = max_delta;
            g_curr_rpm += delta;
        } else if (g_curr_rpm > target_rpm) {
            uint32_t delta = g_curr_rpm - target_rpm;
            if (delta > max_delta) delta = max_delta;
            g_curr_rpm -= delta;
        }
    }

    uint32_t freq_hz = rpm_to_tach_hz(g_curr_rpm);

    if (freq_hz == 0U) {
        if (tachsyn_is_running()) {
            tachsyn_stop();
        }
        g_last_freq_hz = 0;
        return;
    }

    /* Avoid reprogramming Timer3B too often. */
    if (tachsyn_is_running() && freq_hz == g_last_freq_hz) {
        return;
    }

    g_last_freq_hz = freq_hz;
    tachsyn_set_continuous(freq_hz, 50U);
}
