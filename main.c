
/*
  main_V24.c - FINAL: Your working PWM + simple UART from uart_echo.c pattern

  Based on your working pwm.c:
  - PWM setup and update logic EXACTLY as in pwm.c (no disable/enable on updates)
  - Simple ISR-based UART echo + line accumulation (like uart_echo.c)
  - DTR session detection on PQ1
  - PSYN command parsing

  NO complex line editor, NO FIFO, NO diagnostics overhead.
  Just clean, working code.

  Build: requires TivaWare DriverLib.
*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
//#include <ctype.h>

#include <stddef.h>   /* for ptrdiff_t */

/* prototype for newlib/syscalls _sbrk */
extern void * _sbrk(ptrdiff_t incr);


/* Custom functions for command line processing via UART
   and re-implementing typical newlib-style printf's and
   sprintf's via our own lightweight UART output helpers... */
#include "cmdline.h"

// Other libc-related custom helpers and callbacks
#include "ctype_helpers.h"
#include "strtok_compat.h"

// Custom debug helpers to help
// diagnose memory allocations and all that
#include "diag_uart.h"

#include "inc/hw_ints.h"
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "inc/hw_nvic.h"

#include "driverlib/debug.h"
#include "driverlib/gpio.h"
#include "driverlib/interrupt.h"
#include "driverlib/pin_map.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/sysctl.h"
#include "driverlib/uart.h"
#include "driverlib/pwm.h"
#include "drivers/pinout.h"
#include "utils/uartstdio.h"

#include "utils/ustdlib.h"


uint32_t g_ui32SysClock;


/* Configs at MAIN level (avoid declaring UART-comm constants here,
   those will be better place within 'cmdline.h' ... */ 
#define TARGET_PWM_FREQ_HZ 21500U
#define TARGET_DUTY_PERCENT_INIT 30U
#define PSYN_MIN 5
#define PSYN_MAX 96

/* DTR detection pin (PQ1) */
#define DTR_PORT GPIO_PORTQ_BASE
#define DTR_PIN  GPIO_PIN_1

/* PWM globals - as in your working pwm.c */
static uint32_t g_pwmPeriod = 0;
static uint32_t g_pwmPulse  = 0;

/* Forward declarations */
static void setup_system_clock(void);
static void setup_pwm_pf2(void);
void set_pwm_percent(uint32_t percent);
static void setup_uarts(void);


/* ICDI UART0 ISR - echo only */
void ICDIUARTIntHandler(void)
{
    uint32_t ui32Status = ROM_UARTIntStatus(UART0_BASE, true);

    ROM_UARTIntClear(UART0_BASE, ui32Status);

    while (ROM_UARTCharsAvail(UART0_BASE)) {

        ROM_UARTCharPutNonBlocking(UART0_BASE, ROM_UARTCharGetNonBlocking(UART0_BASE));
        GPIOPinWrite(GPIO_PORTN_BASE, GPIO_PIN_0, GPIO_PIN_0);
        SysCtlDelay(g_ui32SysClock / (1000 * 3));
        GPIOPinWrite(GPIO_PORTN_BASE, GPIO_PIN_0, 0);

    }

}


/* USER UART3 ISR - echo + accumulate, exactly as in your pwm.c */
void USERUARTIntHandler(void)
{
    uint32_t ui32Status = ROM_UARTIntStatus(UART3_BASE, true);

    ROM_UARTIntClear(UART3_BASE, ui32Status);

    /* UART3 is handled by the cmdline (polling) module in the main loop.
       Keep the ISR as a safe no-op in case UART3 interrupts are enabled by mistake. */
    while (ROM_UARTCharsAvail(UART3_BASE)) {
        (void)ROM_UARTCharGetNonBlocking(UART3_BASE);
    }

}


/* PWM update - EXACTLY as in your working pwm.c (NO disable/enable!) */
void set_pwm_percent(uint32_t percent)
{
    if (percent > 100) percent = 100;
    uint32_t pulse = (uint32_t)(((uint64_t)g_pwmPeriod * percent) / 100U);
    if (pulse >= g_pwmPeriod) pulse = g_pwmPeriod - 1;
    if (pulse == 0) pulse = 1;

    /* ONLY set pulse width - no disable/enable */
    PWMPulseWidthSet(PWM0_BASE, PWM_OUT_2, pulse);
    g_pwmPulse = pulse;
}


/* PWM setup - EXACTLY as in your working pwm.c */
static void setup_pwm_pf2(void)
{
    SysCtlPeripheralEnable(SYSCTL_PERIPH_PWM0);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOF);

    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_PWM0)) { }
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOF)) { }

    GPIOPinConfigure(GPIO_PF2_M0PWM2);
    GPIOPinTypePWM(GPIO_PORTF_BASE, GPIO_PIN_2);

    PWMClockSet(PWM0_BASE, PWM_SYSCLK_DIV_1);

    uint32_t pwmClock = g_ui32SysClock;
    uint32_t period = (pwmClock + (TARGET_PWM_FREQ_HZ / 2U)) / TARGET_PWM_FREQ_HZ;
    if (period == 0) period = 1;
    if (period > 0xFFFF) period = 0xFFFF;

    g_pwmPeriod = period;

    uint32_t init_pulse = (uint32_t)(((uint64_t)period * TARGET_DUTY_PERCENT_INIT) / 100U);
    if (init_pulse >= period) init_pulse = period - 1;
    if (init_pulse == 0) init_pulse = 1;
    g_pwmPulse = init_pulse;

    PWMGenConfigure(PWM0_BASE, PWM_GEN_1, PWM_GEN_MODE_DOWN | PWM_GEN_MODE_NO_SYNC);
    PWMGenPeriodSet(PWM0_BASE, PWM_GEN_1, period);
    PWMPulseWidthSet(PWM0_BASE, PWM_OUT_2, init_pulse);
    PWMOutputState(PWM0_BASE, PWM_OUT_2_BIT, true);
    PWMGenEnable(PWM0_BASE, PWM_GEN_1);
}


static void setup_system_clock(void)
{
    g_ui32SysClock = MAP_SysCtlClockFreqSet((SYSCTL_XTAL_25MHZ |
                                             SYSCTL_OSC_MAIN |
                                             SYSCTL_USE_PLL |
                                             SYSCTL_CFG_VCO_480), 120000000);
}


static void setup_uarts(void)
{
    MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_UART0);
    MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_UART3);
    MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOA);
    MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOJ);
    MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPION);
    MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOQ);
    MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOF);

    GPIOPinConfigure(GPIO_PA0_U0RX);
    GPIOPinConfigure(GPIO_PA1_U0TX);
    MAP_GPIOPinTypeUART(GPIO_PORTA_BASE, GPIO_PIN_0 | GPIO_PIN_1);

    GPIOPinConfigure(GPIO_PJ0_U3RX);
    GPIOPinConfigure(GPIO_PJ1_U3TX);
    MAP_GPIOPinTypeUART(GPIO_PORTJ_BASE, GPIO_PIN_0 | GPIO_PIN_1);

    /* PF4 LED for RX activity */
    ROM_GPIOPinTypeGPIOOutput(GPIO_PORTF_BASE, GPIO_PIN_4);
    ROM_GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_4, 0);

    /* PQ1 DTR detection */
    GPIOPadConfigSet(GPIO_PORTQ_BASE, GPIO_PIN_1, GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD_WPU);
    ROM_GPIOPinTypeGPIOInput(GPIO_PORTQ_BASE, GPIO_PIN_1);

    ROM_UARTConfigSetExpClk(UART0_BASE, g_ui32SysClock, 9600,
                            (UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE | UART_CONFIG_PAR_NONE));
    ROM_UARTConfigSetExpClk(UART3_BASE, g_ui32SysClock, 115200,
                            (UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE | UART_CONFIG_PAR_NONE));

    MAP_IntMasterEnable();
    ROM_IntEnable(INT_UART0);
    ROM_UARTIntEnable(UART0_BASE, UART_INT_RX | UART_INT_RT);
    /* UART3 is handled by cmdline (polling). Leave UART3 interrupts disabled. */
}


// ==== AUXILIARY STRUCTURES AND FUNCTIONS FOR UART0 (ICDI) DEBUG

//static char hexShowBuf[13] = "CHAR: 0x--\r\n\0";


// Tiny sprintf that ONLY handles "%02X" format
/*
static int tiny_sprintf_hex(char *buf, const char *fmt, uint8_t value)
{
    const char hex[] = "0123456789ABCDEF";

    // Simple pattern match for "%02X"
    if (fmt[0] == '%' && fmt[1] == '0' && fmt[2] == '2' && fmt[3] == 'X') {
        buf[0] = hex[(value >> 4) & 0x0F];
        buf[1] = hex[value & 0x0F];
        buf[2] = '\0';
        return 2;
    }
    return 0;
}
*/


void example_dynamic_cmd_copy_and_process(const volatile char *user_rx_buf, uint32_t len)
{

    /* allocate len+1 bytes */
    char *cmd_local = (char *)malloc((size_t)len + 1);
    if (!cmd_local) {
        diag_puts("ERROR: malloc for cmd_local failed\r\n");
        return;
    }

    /* copy and NUL-terminate */
    if (len > 0) {
        memcpy(cmd_local, (const void *)user_rx_buf, len);
    }
    cmd_local[len] = '\0';

    /* Print the pointer and length for diagnostics (ICDI UART) */
    diag_puts("cmd_local ptr = ");
    diag_put_ptr((void *)cmd_local);
    diag_puts(" ; len = ");
    diag_put_hex32(len);
    diag_puts("\n");

    /* simple decimal print helper assumed available; otherwise print small number */
     /* NOTE: UART3 command handling is now owned by cmdline.c.
         This legacy helper is retained for allocator/memory diagnostics only. */

    /* spew out some diagnostic summaries... */
    diag_print_variable("g_pwmPeriod", (const void *)&g_pwmPeriod, sizeof(g_pwmPeriod), DIAG_PREVIEW_LIMIT);
    diag_print_variable("g_pwmPulse", (const void *)&g_pwmPulse, sizeof(g_pwmPulse), DIAG_PREVIEW_LIMIT);

    /* Temporary: Test specific parts to find the exact problem */
    diag_puts("DEBUG: After PWM variables, testing malloc...\r\n");
    
    /* Test 1: strlen */
    const char *lit = "DYN_TEST: Hello from dynamic buffer!";
    size_t lit_len = strlen(lit);
    diag_puts("DEBUG: strlen completed, lit_len=");
    diag_put_u32_dec((uint32_t)lit_len);
    diag_puts("\r\n");
    
    /* Test 2: malloc */
    char* dyn = (char *)malloc(lit_len + 1);
    if (!dyn) {
        diag_puts("ERROR: malloc failed\r\n");
    } else {
        diag_puts("DEBUG: malloc succeeded, ptr=");
        diag_put_ptr((void *)dyn);
        diag_puts("\r\n");
        
        /* Test 3: memcpy */
        memcpy(dyn, lit, lit_len + 1);
        diag_puts("DEBUG: memcpy completed\r\n");
        
        /* Test 4: diag_print_variable with NOLIMIT (WORKS!) */
        diag_puts("DEBUG: About to call diag_print_variable with NOLIMIT...\r\n");
        diag_print_variable("dyn_str", (const void *)dyn, (size_t)(lit_len + 1), DIAG_PREVIEW_NOLIMIT);
        diag_puts("DEBUG: diag_print_variable completed\r\n");
        
        /* Memory integrity check before stack-heavy operations */
        diag_check_memory_integrity("pre-sprintf-test");
        
        /* Test 5: Stack usage test - declare msgbuf array (WORKS!) */
        diag_puts("DEBUG: About to declare msgbuf[320]...\r\n");
        diag_check_stack_usage("before-msgbuf-declaration");
        char msgbuf[320];
        diag_check_stack_usage("after-msgbuf-declaration");
        diag_puts("DEBUG: msgbuf declared, testing our sprintf replacement...\r\n");
        
        /* Test 6: Use our custom sprintf replacement */
        int n = sprintf(msgbuf, "SPRINTF: dyn@%p len=%u contents='%s'\r\n",
            (void *)dyn, (unsigned)lit_len, dyn);
        if (n > 0) {
            diag_puts("DEBUG: sprintf replacement succeeded, n=");
            diag_put_u32_dec((uint32_t)n);
            diag_puts("\r\n");
            // Send the formatted string to ICDI (UART0) using UARTSend
            UARTSend((const uint8_t *)msgbuf, (uint32_t)n, UARTDEV_ICDI);
            diag_puts("DEBUG: UARTSend completed\r\n");
        } else {
            diag_puts("ERROR: sprintf replacement failed\r\n");
        }
        
        /* Free immediately */
        free(dyn);
        diag_puts("DEBUG: free completed\r\n");
    }

    /*
    // Commented out dynamic allocation test to isolate stall issue
    /* --- Dynamic string allocation test (inserted test) ---
    const char *lit = "DYN_TEST: Hello from dynamic buffer!";
    size_t lit_len = strlen(lit);

    /* allocate exact space (len + NUL)
    char* dyn = NULL;
    
    //dyn = (char *)malloc(lit_len + 1);

    //if (!dyn) {
    if (false) {  // FORCE THE ERROR PATH FOR TESTING

        diag_puts("ERROR: malloc for dyn string FAILED\r\n");

    } else {
        
        /* copy literal into allocated area (this makes the bytes come from heap memory)
        memcpy(dyn, lit, lit_len + 1);

        /* Print address and length using diag helpers (ICDI UART)
        diag_puts("DYN ALLOC -> addr=");
        diag_put_ptr((void *)dyn);
        diag_puts(" len=");
        diag_put_u32_dec((uint32_t)lit_len);
        diag_puts("\r\n");

        /* Use diag_print_variable to print region and a hex-preview of the allocated block
        diag_print_variable("dyn_str", (const void *)dyn, (size_t)(lit_len + 1), DIAG_PREVIEW_NOLIMIT);

        /* Demonstrate snprintf usage: format a human line and send it via ICDI UART
        //char msgbuf[320];  ---> TRY TO USE THIS "THE PROPER WAY" LATER ON !!!

        void *sp = NULL;
        __asm__ volatile ("mov %0, sp" : "=r" (sp));

        diag_puts("SP before snprintf = ");
        diag_put_ptr(sp); diag_puts("\r\n");
        diag_puts("sbrk(0) before = ");
        diag_put_ptr(_sbrk(0)); diag_puts("\r\n");

        /*
        int n = snprintf(msgbuf, sizeof(msgbuf),
            "SNPRINTF: dyn@%p len=%u contents='%s'\r\n",
            (void *)dyn, (unsigned)lit_len, dyn);
        if (n > 0) {
            // Send the formatted string to ICDI (UART0) using your project wrapper
                UARTSend((const uint8_t *)msgbuf, (uint32_t)(n > (int)sizeof(msgbuf) ? sizeof(msgbuf) : n), UARTDEV_ICDI);
        }
        

        /* --- replace the old stack snprintf block with this: ---
        /*
        {
            int n = diag_snprintf_heap_send("SNPRINTF: dyn@%p len=%u contents='%s'\r\n",
                                    (void *)dyn, (unsigned)lit_len, dyn);
            if (n < 0) {
                diag_puts("ERROR: diag_snprintf_heap_send failed\r\n");
            }
        }
        

        /* Free the block and show a short note
        free(dyn);
        diag_puts("DYN ALLOC freed\r\n");

    }        /* --- end dynamic test ---
    */

    void *cur_brk = _sbrk(0);
    diag_print_variable("sbrk(0)", cur_brk, 16, DIAG_PREVIEW_LIMIT); /* preview 16 bytes at current brk */

    diag_print_variables_summary();

    /* free when done */
    free(cmd_local);

}


int main(void)
{
    setup_system_clock();

    // Check if we had a hard fault (bit 31 of SCB->HFSR)
    if (HWREG(0xE000ED2C) & 0x80000000) {
        // Hard fault occurred! Likely stack overflow
        while(1) {
            // Blink PN0 rapidly as a warning
            ROM_GPIOPinWrite(GPIO_PORTN_BASE, GPIO_PIN_0, GPIO_PIN_0);
            SysCtlDelay(g_ui32SysClock / 100);
            ROM_GPIOPinWrite(GPIO_PORTN_BASE, GPIO_PIN_0, 0);
            SysCtlDelay(g_ui32SysClock / 100);
        }
    }

    ROM_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPION);
    ROM_GPIOPinTypeGPIOOutput(GPIO_PORTN_BASE, GPIO_PIN_0);

    setup_pwm_pf2();
    setup_uarts();

    /* Initial basic probing of the _sbrk allocation callback/ helper */
    //diag_sbrk_probe();

    /* Immediately print the memory layout with direct UART writes */
    //diag_print_memory_layout();

    /* Optionally run the malloc stress test */
    //diag_test_malloc_sequence();

    /* Alternatively (if malloc_sequence stalls) use GPIO toggling
       in order to debug whether malloc() internally calls _sbrk
       adequately (we choose the LED on PN0 for this Launchpad)    */
    //diag_test_malloc_with_gpio();

    /* Print again the (updated) memory layout with direct UART writes */
    //diag_print_memory_layout();


    for (;;) {

        /* Wait for DTR session */
        UARTSend((const uint8_t *)"NO SESSION ACTIVE\r\n", 20, UARTDEV_ICDI);

        while (ROM_GPIOPinRead(DTR_PORT, DTR_PIN)) {
            SysCtlDelay(g_ui32SysClock / (1000 * 100));
        }

        UARTSend((const uint8_t *)"SESSION WAS INITIATED\r\n", 24, UARTDEV_ICDI);
        SysCtlDelay(g_ui32SysClock / (1000 * 12));

        /* UART3 interactive session handled by the slick cmdline module.
           It will return when DTR indicates disconnect. */
        cmdline_init();
        cmdline_run_until_disconnect();

        UARTSend((const uint8_t *)"SESSION WAS DISCONNECTED\r\n", 27, UARTDEV_ICDI);

    }

    return 0;
}

