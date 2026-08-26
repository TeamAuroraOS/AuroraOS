/* Coded By DisLoPik for the AuroraOS Project. */
/* Links the Phase 7 test payload to run in place at the ARM9 load address
   (AOS_ARM9_LOAD_ADDR in include/loader.h). _start must be the very first byte. */
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
