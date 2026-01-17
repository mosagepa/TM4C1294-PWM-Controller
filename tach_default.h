#ifndef TACH_DEFAULT_H
#define TACH_DEFAULT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TACH_DEFAULT_PHASE1 = 0,
    TACH_DEFAULT_PHASE2 = 1,
    TACH_DEFAULT_PHASE1L = 2,
    TACH_DEFAULT_PHASE2L = 3,
    TACH_DEFAULT_BOOT = 4,
    TACH_DEFAULT_COPY = 5,
} tach_default_mode_t;

/* Initializes EEPROM-backed default setting (safe if EEPROM not present/unused). */
void tach_default_init(void);

/* Returns current stored default (or PHASE1L if unset/invalid). */
tach_default_mode_t tach_default_get(void);

/* Persists a new default to EEPROM. Returns false on invalid input or EEPROM failure. */
bool tach_default_set(tach_default_mode_t mode);

/* Applies the current default mode (used on boot). */
void tach_default_apply(void);

/* Utility for UART display. */
const char *tach_default_mode_to_str(tach_default_mode_t mode);

#endif /* TACH_DEFAULT_H */
