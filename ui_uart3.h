#ifndef UI_UART3_H
#define UI_UART3_H

#include <stdbool.h>

/* Call at the start of each DTR session (once per session). */
void ui_uart3_session_begin(void);

/* Output helpers (USER UART3). */
void ui_uart3_puts(const char *s);

/* Prompt helpers. */
void ui_uart3_prompt_once(void);
void ui_uart3_prompt_force_next(void);

#endif /* UI_UART3_H */
