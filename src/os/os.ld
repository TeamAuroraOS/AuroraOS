/* Links the AuroraOS payload to run in place at 0x22000000 (FCRAM).
   Stack lives higher up in FCRAM, well clear of code/bss. */
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
        *(.start)
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FCRAM

    .data :
    {
        *(.data*)
        . = ALIGN(4);
    } > FCRAM

    /* End of the loadable image (text+rodata+data). The Home Menu snapshots
       [_os_start .. _os_image_end) so it can restore itself when an app exits
       via HOME. .bss is NOLOAD, so it is not part of the snapshot -- the OS
       re-clears it on restart. */
    _os_image_end = .;

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _os_bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _os_bss_end = .;
    } > FCRAM

    _os_stack_top = 0x23000000;

    /DISCARD/ :
    {
        *(.ARM.exidx*)
        *(.ARM.attributes*)
        *(.comment*)
        *(.note*)
    }
}
