/* Linker script for Auric apps.
 *
 * Mirrors AuroraOS's src/os/os.ld: the payload runs in place at the ARM9 load
 * address (0x22000000, FCRAM, clear of the loader), with the stack high in
 * FCRAM well above code/bss. _start (auric_start.s) must be first.
 */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)

MEMORY
{
    FCRAM (rwx) : ORIGIN = 0x22000000, LENGTH = 0x00E00000
}

SECTIONS
{
    . = 0x22000000;

    .text :
    {
        /* App icon header at fixed offset (32x32 icon, branch to _start). */
        KEEP(*(.aurhead))
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

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _auric_bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _auric_bss_end = .;
    } > FCRAM

    _auric_stack_top = 0x23000000;

    /DISCARD/ :
    {
        *(.ARM.exidx*)
        *(.ARM.attributes*)
        *(.comment*)
        *(.note*)
    }
}
