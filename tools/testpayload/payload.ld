OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)

MEMORY
{
    FCRAM (rwx) : ORIGIN = 0x22000000, LENGTH = 0x00100000
}

SECTIONS
{
    . = 0x22000000;
    .start :
    {
        *(.start)
        *(.text*)
        *(.rodata*)
        *(.data*)
        . = ALIGN(4);
    } > FCRAM

    /DISCARD/ :
    {
        *(.bss*)
        *(COMMON)
        *(.ARM.exidx*)
        *(.ARM.attributes*)
        *(.comment*)
        *(.note*)
    }
}
