#ifndef COMMANDS_H
#define COMMANDS_H

#include <stdbool.h>
#include <stdint.h>

/* Must be provided by the platform (TM4C main.c). */
void pwm_set_percent(uint32_t percent);

/* Allows temporarily forcing PF2 low (PWM OFF) for scope/debug. */
void pwm_set_enabled(bool enabled);
bool pwm_is_enabled(void);

/* Optional platform-provided debug gating (UART0 diagnostics). */
void debug_set_enabled(bool enabled);
bool debug_is_enabled(void);

/* Platform-provided UART3 session control.
	Requests that the current UART3 DTR session be closed.
	Implemented in main.c; the existing UART0 disconnect diagnostics will fire automatically. */
void uart3_request_disconnect(void);

/* Process one complete command line (NUL-terminated). */
void commands_process_line(const char *line);

#endif /* COMMANDS_H */
