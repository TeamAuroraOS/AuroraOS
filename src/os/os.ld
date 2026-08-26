/* Coded By DisLoPik for the AuroraOS Project. */
/* Links the AuroraOS payload to run in place at AOS_ARM9_LOAD_ADDR (0x22000000).
   Stack lives higher up in FCRAM, well clear of the code/bss. */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_os_start)

MEMORY
{
    FCRAM (rwx) : ORIGIN = 0x22000000, LENGTH = 0x00E00000
}

SECTIONS
{
    . = 0x22000000;

    .text :
    {
        *(.start)       /* _os_start must be first */
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
        _os_bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _os_bss_end = .;
    } > FCRAM

    /* Stack grows down from here (16 MB above the load address). */
    _os_stack_top = 0x23000000;

    /DISCARD/ :
    {
        *(.ARM.exidx*)
        *(.ARM.attributes*)
        *(.comment*)
        *(.note*)
    }
}
