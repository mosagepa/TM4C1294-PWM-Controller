/*
 * TM4C1294_startup.c
 *
 * Startup + vector table for TM4C1294 (based on the vector list you provided).
 * The NVIC vector table is copied literally from your provided file; the rest of
 * the startup code (reset handler, default handlers) is implemented here to
 * cooperate with the heartbeat code (Timer0A_Handler and UART0_Handler).
 *
 * This file expects the linker to provide the symbols:
 *   _stack_top, _start_text, _end_text, _start_data, _end_data, _start_bss, _end_bss
 *
 * After copy/zero of data segments, rst_handler calls main() (user code).
 */

#include <stdint.h>

/* User entry point (heartbeat/main) */
extern void main(void);

/* Handlers used by your heartbeat code */
//void Timer0A_Handler(void);      /* timer ISR used for 1 Hz heartbeat */
void ICDIUARTIntHandler(void);
void USERUARTIntHandler(void);


/* Standard handlers (prototypes) */
void rst_handler(void);
void nmi_handler(void);
void hardfault_handler(void);
void empty_def_handler(void);

/* Linker symbols (provided by the linker script) */
extern unsigned long _stack_top;
extern unsigned long _start_text;
extern unsigned long _end_text;
extern unsigned long _start_data;
extern unsigned long _end_data;
extern unsigned long _start_bss;
extern unsigned long _end_bss;

/* NVIC vector table placed at start of flash (copied verbatim from your file) */
__attribute__ ((section(".nvic_table")))
void(* myvectors[])(void) = {
    (void (*)) &_stack_top, /* initial stack pointer */
    rst_handler,            /* reset handler */
    nmi_handler,            /* NMI */
    hardfault_handler,      /* HardFault */

    /* Configurable priority interrupts handler start here. */
    empty_def_handler,      // Memory Management Fault    4
    empty_def_handler,      // Bus Fault                 5
    empty_def_handler,      // Usage Fault               6
    0,                      // Reserved                  7
    0,                      // Reserved                  8
    0,                      // Reserved                  9
    0,                      // Reserved                  10
    empty_def_handler,      // SV call                   11
    empty_def_handler,      // Debug monitor             12
    0,                      // Reserved                  13
    empty_def_handler,      // PendSV                    14
    empty_def_handler,      // SysTick                   15

    /* Peripheral interrupts start here. */
    empty_def_handler,      // GPIO Port A               16
    empty_def_handler,      // GPIO Port B               17
    empty_def_handler,      // GPIO Port C               18
    empty_def_handler,      // GPIO Port D               19
    empty_def_handler,      // GPIO Port E               20
    ICDIUARTIntHandler,	    // UART 0                    21
    empty_def_handler,      // UART 1                    22
    empty_def_handler,      // SSI 0                     23
    empty_def_handler,      // I2C 0                     24
    0,                      // PWM Fault                 25
    0,                      // PWM Gen 0                 26
    0,                      // PWM Gen 1                 27
    0,                      // PWM Gen 2                 28
    0,                      // Quadrature Encoder 0      29
    empty_def_handler,      // ADC 0 Seq 0               30
    empty_def_handler,      // ADC 0 Seq 1               31
    empty_def_handler,      // ADC 0 Seq 2               32
    empty_def_handler,      // ADC 0 Seq 3               33
    empty_def_handler,      // WDT 0 and 1               34
    empty_def_handler,      // 16/32 bit timer 0 A       35
    empty_def_handler,      // 16/32 bit timer 0 B       36
    empty_def_handler,      // 16/32 bit timer 1 A       37
    empty_def_handler,      // 16/32 bit timer 1 B       38
    empty_def_handler,      // 16/32 bit timer 2 A       39
    empty_def_handler,      // 16/32 bit timer 2 B       40
    empty_def_handler,      // Analog comparator 0       41
    empty_def_handler,      // Analog comparator 1       42
    empty_def_handler,      // Analog comparator 2       43
    empty_def_handler,      // System control (PLL, OSC, BO) 44
    empty_def_handler,      // Flash + EEPROM control    45
    empty_def_handler,      // GPIO Port F               46
    empty_def_handler,      // GPIO Port G               47
    empty_def_handler,      // GPIO Port H               48
    empty_def_handler,      // UART 2                    49
    empty_def_handler,      // SSI 1                     50
    empty_def_handler,      // 16/32 bit timer 3 A       51
    empty_def_handler,      // 16/32 bit timer 3 B       52
    empty_def_handler,      // I2C 1                     53
    empty_def_handler,      // CAN 0                     54
    empty_def_handler,      // CAN 1                     55
    empty_def_handler,      // Ethernet                  56
    empty_def_handler,      // Hibernation module        57
    empty_def_handler,      // USB                       58
    empty_def_handler,      // PWM Gen 3                 59
    empty_def_handler,      // UDMA SW                   60
    empty_def_handler,      // UDMA Error                61
    empty_def_handler,      // ADC 1 Seq 0               62
    empty_def_handler,      // ADC 1 Seq 1               63
    empty_def_handler,      // ADC 1 Seq 2               64
    empty_def_handler,      // ADC 1 Seq 3               65
    empty_def_handler,      // External Bus Interface 0  66
    empty_def_handler,      // GPIO Port J               67
    empty_def_handler,      // GPIO Port K               68
    empty_def_handler,      // GPIO Port L               69
    empty_def_handler,      // SSI 2                     70
    empty_def_handler,      // SSI 2                     71
    USERUARTIntHandler,     // UART 3                    72
    empty_def_handler,      // UART 4                    73
    empty_def_handler,      // UART 5                    74
    empty_def_handler,      // UART 6                    75
    empty_def_handler,      // UART 7                    76
    empty_def_handler,      // I2C 2 Master & Slave      77
    empty_def_handler,      // I2C 3 Master & Slave      76 (note: original list had duplicated numbers)
    empty_def_handler,      // 16/32 bit timer 4 A       77
    empty_def_handler,      // 16/32 bit timer 4 B       78
    empty_def_handler,      // 16/32 bit timer 5 A       79
    empty_def_handler,      // 16/32 bit timer 5 B       80
    empty_def_handler,      // FPU                       81
    0,                      // Reserved                  82
    0,                      // Reserved                  83
    empty_def_handler,      // I2C 4 Master & Slave      84
    empty_def_handler,      // I2C 5 Master & Slave      85
    empty_def_handler,      // GPIO Port M               86
    empty_def_handler,      // GPIO Port N               87
    0,                      // Reserved                  88
    empty_def_handler,      // Tamper                    89
    empty_def_handler,      // GPIO Port P (Summary or P0) 90
    empty_def_handler,      // GPIO Port P1              91
    empty_def_handler,      // GPIO Port P2              92
    empty_def_handler,      // GPIO Port P3              93
    empty_def_handler,      // GPIO Port P4              94
    empty_def_handler,      // GPIO Port P5              95
    empty_def_handler,      // GPIO Port P6              96
    empty_def_handler,      // GPIO Port P7              97
    empty_def_handler,      // GPIO Port Q (Summary or Q0) 98
    empty_def_handler,      // GPIO Port Q1              99
    empty_def_handler,      // GPIO Port Q2              100
    empty_def_handler,      // GPIO Port Q3              101
    empty_def_handler,      // GPIO Port Q4              102
    empty_def_handler,      // GPIO Port Q5              103
    empty_def_handler,      // GPIO Port Q6              104
    empty_def_handler,      // GPIO Port Q7              105
    empty_def_handler,      // GPIO Port R               106
    empty_def_handler,      // GPIO Port S               107
    empty_def_handler,      // SHA/ MD5 0                108
    empty_def_handler,      // AES 0                     109
    empty_def_handler,      // DES3DES 0                 110
    empty_def_handler,      // LCD Controller 0          111
    empty_def_handler,      // Timer 6 subtimer A        112
    empty_def_handler,      // Timer 6 subtimer B        113
    empty_def_handler,      // Timer 7 subtimer A        114
    empty_def_handler,      // Timer 7 subtimer B        115
    empty_def_handler,      // I2C 6 Master and Slave   116
    empty_def_handler,      // I2C 7 Master and Slave   117
    empty_def_handler,      // HIM Scan Matrix Keyboard 0 118
    empty_def_handler,      // One Wire 0               119
    empty_def_handler,      // HIM PS/2 0               120
    empty_def_handler,      // HIM LED Sequencer 0      121
    empty_def_handler,      // HIM Consumer IR 0        122
    empty_def_handler,      // I2C 8 Master & Slave     123
    empty_def_handler,      // I2C 9 Master & Slave     123 (duplicate in original)
    empty_def_handler       // GPIO Port T              124
};

/* Reset handler: copy .data from flash to RAM and zero .bss, then call main */
void rst_handler(void)
{
    unsigned long *src = &_end_text;    /* load address for .data (in flash) */
    unsigned long *dest = &_start_data; /* runtime address in RAM */

    /* Copy initialized data from flash to RAM */
    while (dest < &_end_data) {
        *dest++ = *src++;
    }

    /* Zero BSS */
    dest = &_start_bss;
    while (dest < &_end_bss) {
        *dest++ = 0;
    }

    /* Call the user entry (does not return under normal operation) */
    main();

    /* If main returns, loop forever */
    while (1) {}
}

/* Minimal handlers */
void nmi_handler(void) { while (1) {} }
void hardfault_handler(void) { while (1) {} }
void empty_def_handler(void) { while (1) {} }

/* Note:
 * - Timer0A_Handler and UART0_Handler are defined in your application file
 *   (main.c). They must have exactly these names to match the vector
 *   table entries above.
 * - The linker script must define the symbols declared at top of this file.
 */


