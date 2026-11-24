/* Linker script for /Aurora/amenu C SDK plugins (AMNU).
 * Linked at AMENU_LINK_BASE (0x27000000) with --emit-relocs; aurcc.py packs
 * all R_ARM_ABS32 offsets so the OS relocates the plugin into any FCRAM slot. */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(amenu_plugin_entry)

MEMORY
{
    AMENU (rwx) : ORIGIN = 0x27000000, LENGTH = 0x0001E000
}

SECTIONS
{
    . = 0x27000000;

    .text :
    {
        KEEP(*(.amenu_entry))
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > AMENU

    .data :
    {
        *(.data .data.*)
        . = ALIGN(4);
    } > AMENU

    _amenu_payload_end = .;

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _amenu_bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        _amenu_bss_end = .;
    } > AMENU

    ASSERT(_amenu_bss_end - 0x27000000 <= 0x0001E000,
           "amenu plugin too large: code+data+bss must fit in 120 KB")

    /DISCARD/ :
    {
        *(.ARM.exidx*)
        *(.ARM.extab*)
        *(.ARM.attributes*)
        *(.comment*)
        *(.note*)
    }
}
