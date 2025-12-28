#ifndef COMMANDS_H
#define COMMANDS_H

#include <stdbool.h>
#include <stdint.h>

/* Must be provided by the platform (TM4C main.c). */
void pwm_set_percent(uint32_t percent);

/* Optional platform-provided debug gating (UART0 diagnostics). */
void debug_set_enabled(bool enabled);
bool debug_is_enabled(void);

/* Process one complete command line (NUL-terminated). */
void commands_process_line(const char *line);

#endif /* COMMANDS_H */
