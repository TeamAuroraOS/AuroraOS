/* Native apps run in place at the ARM9 load address, like the OS they
 * replace. The generated app header must come first, then _start. The heap
 * is not here: it is 0x24000000..0x25000000 (app_syscalls.c). */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)

MEMORY
{
    APP (rwx) : ORIGIN = 0x22000000, LENGTH = 0x01000000
}

SECTIONS
{
    . = 0x22000000;

    .text :
    {
        KEEP(*(.aurhead))
        KEEP(*(.start))
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > APP

    .init_array :
    {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN(__init_array_end = .);
    } > APP

    .fini_array :
    {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } > APP

    .data :
    {
        *(.data .data.*)
        . = ALIGN(4);
    } > APP

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _app_bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        _app_bss_end = .;
    } > APP

    /* The stack grows down from the end of the OS's region; 1 MB is kept
     * for it. */
    _app_stack_top = 0x23000000;
    ASSERT(_app_bss_end <= 0x22F00000, "app too large: code, data and .bss must stay under 15 MB; use malloc() for big buffers")

    /DISCARD/ :
    {
        *(.ARM.exidx*)
        *(.ARM.extab*)
        *(.ARM.attributes*)
        *(.comment*)
        *(.note*)
    }
}
