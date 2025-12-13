/* TM4C1294.ld - Linker script for TM4C1294 projects
 *
 * Keeps .nvic_table at start of .text, reserves .heap (NOLOAD) between .bss and
 * stack; exports heap symbols as _heap_start/_heap_end/_heap_size to match your
 * provided script and startup code.
 *
 * Adjust _stack_size as needed for your project.
 */

ENTRY(rst_handler)

_stack_size = 8K;

/* Memory layout (same as your provided layout) */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 0x00040000
    SRAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 0x00008000
    STACK (rwx) : ORIGIN = 0x20007FFF, LENGTH = _stack_size
}

SECTIONS
{
    .text :
    {
        /* Place NVIC table at start of flash and keep it (don't garbage collect) */
        KEEP(*(.nvic_table))
        *(.text*)
        *(.rodata*)
        _end_text = .;
    } > FLASH

    .data :
    {
        _start_data = .;
        *(.data*)
        *(vtable)
        _end_data = .;
    } > SRAM AT > FLASH

    .bss :
    {
        _start_bss = .;
        *(.bss*)
        *(COMMON)
        _end_bss = .;
    } > SRAM

    /* Reserve heap area (NOLOAD) so the linker reports it in memory usage.
       The heap will be the region between the end of .bss and the start of the stack.
       Using NOLOAD causes the linker to reserve the address range but not place
       any initialized data in the output image. */
    _heap_bottom = .;
    .heap (NOLOAD) :
    {
        _heap_start = .;
        /* move location counter to the computed heap top (leave space for stack) */
        . = ORIGIN(SRAM) + LENGTH(SRAM) - _stack_size;
        _heap_end = .;
    } > SRAM

    /* Provide compatibility names and a computed symbol for heap size */
    PROVIDE(_heap_bottom = _heap_start);
    PROVIDE(_heap_top = _heap_end);
    PROVIDE(_heap_size = _heap_end - _heap_start);

    /* Define stack symbols */
    _stack_bottom = _heap_top;
    _stack_top = ORIGIN(SRAM) + LENGTH(SRAM);

    .stack :
    {
    } > STACK
}

/* End of linker script */