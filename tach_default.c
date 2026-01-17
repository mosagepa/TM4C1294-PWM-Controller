#include "tach_default.h"

#include <stdbool.h>
#include <stdint.h>

#include "driverlib/eeprom.h"
#include "driverlib/sysctl.h"

#include "commands.h" /* pwm_set_percent/pwm_set_enabled */
#include "tsyn.h"
#include "tach.h"

#define TACH_DEFAULT_EE_MAGIC 0x54444346u /* 'TDCF' */
#define TACH_DEFAULT_EE_OFFSET 0x100u

typedef struct {
    uint32_t magic;
    uint32_t mode;
} tach_default_ee_t;

static volatile bool g_ee_ready = false;
static volatile tach_default_mode_t g_mode_cached = TACH_DEFAULT_PHASE1L;

static bool tach_default_mode_valid(uint32_t mode)
{
    return mode <= (uint32_t)TACH_DEFAULT_COPY;
}

static tach_default_mode_t tach_default_fallback(void)
{
    return TACH_DEFAULT_PHASE1L;
}

const char *tach_default_mode_to_str(tach_default_mode_t mode)
{
    switch (mode) {
    case TACH_DEFAULT_PHASE1: return "1";
    case TACH_DEFAULT_PHASE2: return "2";
    case TACH_DEFAULT_PHASE1L: return "1L";
    case TACH_DEFAULT_PHASE2L: return "2L";
    case TACH_DEFAULT_BOOT: return "BOOT";
    case TACH_DEFAULT_COPY: return "COPY";
    default: return "?";
    }
}

void tach_default_init(void)
{
    /* Enable and init EEPROM (TM4C has EEPROM0). */
    SysCtlPeripheralEnable(SYSCTL_PERIPH_EEPROM0);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_EEPROM0)) { }

    if (EEPROMInit() != 0) {
        g_ee_ready = false;
        g_mode_cached = tach_default_fallback();
        return;
    }

    g_ee_ready = true;

    tach_default_ee_t ee;
    ee.magic = 0;
    ee.mode = 0;

    EEPROMRead((uint32_t *)&ee, TACH_DEFAULT_EE_OFFSET, sizeof(ee));

    if (ee.magic != TACH_DEFAULT_EE_MAGIC || !tach_default_mode_valid(ee.mode)) {
        g_mode_cached = tach_default_fallback();
        return;
    }

    g_mode_cached = (tach_default_mode_t)ee.mode;
}

tach_default_mode_t tach_default_get(void)
{
    if (!tach_default_mode_valid((uint32_t)g_mode_cached)) {
        return tach_default_fallback();
    }
    return g_mode_cached;
}

bool tach_default_set(tach_default_mode_t mode)
{
    if (!tach_default_mode_valid((uint32_t)mode)) {
        return false;
    }

    g_mode_cached = mode;

    if (!g_ee_ready) {
        /* Cache only if EEPROM isn't initialized. */
        return false;
    }

    tach_default_ee_t ee;
    ee.magic = TACH_DEFAULT_EE_MAGIC;
    ee.mode = (uint32_t)mode;

    EEPROMProgram((uint32_t *)&ee, TACH_DEFAULT_EE_OFFSET, sizeof(ee));
    return true;
}

static void apply_phase(uint32_t pwm_percent, uint32_t tachsyn_hz)
{
    /* Stop legacy burst TSYN. */
    if (tsyn_is_enabled()) {
        tsyn_set_enabled(false);
    }

    /* Stop BOOT if active. */
    if (tachsyn_boot_is_running()) {
        tachsyn_boot_stop();
    }

    /* Ensure PWM out is enabled. */
    pwm_set_percent(pwm_percent);
    if (!pwm_is_enabled()) {
        pwm_set_enabled(true);
    }

    tachsyn_set_drive_mode(TACHSYN_DRIVE_PUSHPULL);
    tachsyn_set_continuous(tachsyn_hz, 50U);
}

void tach_default_apply(void)
{
    tach_default_mode_t mode = tach_default_get();

    switch (mode) {
    case TACH_DEFAULT_PHASE1:
        apply_phase(46U, 168U);
        break;
    case TACH_DEFAULT_PHASE2:
        apply_phase(54U, 235U);
        break;
    case TACH_DEFAULT_PHASE1L:
        apply_phase(15U, 168U);
        break;
    case TACH_DEFAULT_PHASE2L:
        apply_phase(21U, 235U);
        break;
    case TACH_DEFAULT_BOOT:
        tachsyn_boot_start();
        break;
    case TACH_DEFAULT_COPY:
        /* COPY default: keep PWM in a safe low-noise state, but mirror PF1 edges to PM3. */
        pwm_set_percent(15U);
        if (!pwm_is_enabled()) {
            pwm_set_enabled(true);
        }
        tachsyn_copy_begin();
        break;
    default:
        apply_phase(15U, 168U);
        break;
    }

    /* Default behavior: keep tach input capture enabled. */
    tach_set_capture_enabled(true);
}
