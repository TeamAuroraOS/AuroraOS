/* AuroraOS payload, run in place at 0x22000000. */
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

    /* End of the loadable image. The Home Menu snapshots [_os_start,
     * _os_image_end) to restore itself after an app; .bss is NOLOAD and
     * re-cleared on restart. */
    _os_image_end = .;
    /* That snapshot (0x26000000) ends where the decoded image (0x26100000)
     * starts. */
    ASSERT(_os_image_end - 0x22000000 <= 0x100000,
           "the OS image is over 1 MB: its HOME snapshot would overrun")

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
