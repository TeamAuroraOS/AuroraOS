/* ARM11 core, run in place at AUDIO_CORE_ADDR (0x23000000). */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_audio11_start)

MEMORY
{
    FCRAM (rwx) : ORIGIN = 0x23000000, LENGTH = 0x00200000
}

SECTIONS
{
    . = 0x23000000;

    .text :
    {
        *(.start)        /* _audio11_start must be first */
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FCRAM

    .data :
    {
        *(.data*)
        . = ALIGN(4);
    } > FCRAM

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _abss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _abss_end = .;
    } > FCRAM

    _audio11_stack_top = 0x23200000;

    /DISCARD/ :
    {
        *(.ARM.exidx*)
        *(.ARM.attributes*)
        *(.comment*)
        *(.note*)
    }
}
