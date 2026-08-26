

OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start11)

MEMORY
{
    AXI_WRAM (rwx) : ORIGIN = 0x1FF80000, LENGTH = 0x80000
}

SECTIONS
{
    .text :
    {
        *(.vectors)
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > AXI_WRAM

    .data :
    {
        *(.data*)
        . = ALIGN(4);
    } > AXI_WRAM

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
    } > AXI_WRAM

    
    _stack11_top = ORIGIN(AXI_WRAM) + LENGTH(AXI_WRAM);

    /DISCARD/ :
    {
        *(.ARM.exidx*)
        *(.ARM.attributes*)
        *(.comment*)
        *(.note*)
    }
}
