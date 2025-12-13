#ifndef CMDLINE_H
#define CMDLINE_H

#include <stdint.h>
#include <stdbool.h>


/* Config (keep in sync with main.c) */
#ifndef UART_RX_BUF_SIZE
#define UART_RX_BUF_SIZE 64
#endif

/* ANSI colors / prompt */
#define ANSI_RESET      "\x1B[0m"
#define ANSI_WELCOME    "\x1B[1;36m"
#define ANSI_PROMPT     "\x1B[1;33m"
#define ANSI_RESPONSE   "\x1B[0;32m"
#define ANSI_ERROR      "\x1B[1;31m"
#define PROMPT_SYMBOL   "> "


/* PSYN defaults (fall back if not defined in main) */
#ifndef PSYN_MIN
#define PSYN_MIN 5
#endif
#ifndef PSYN_MAX
#define PSYN_MAX 96
#endif

/* Re-declare UARTDev enum locally so we can call UARTSend with the right values */
typedef enum { UARTDEV_ICDI = 0, UARTDEV_USER } UARTDev;


/* Low-level UART send to any of the active UART channels */
void UARTSend(const uint8_t *pui8Buffer, uint32_t ui32Count, UARTDev destUART);


/* Initialize command-line module (call after setup_uarts() and before entering session) */
void cmdline_init(void);

/* Run the user-session loop until the DTR session is disconnected.
   This function returns when the session ends (so main can continue outer logic).
   It handles input on UART3 and outputs to UART3. */
void cmdline_run_until_disconnect(void);

#endif /* CMDLINE_H */

