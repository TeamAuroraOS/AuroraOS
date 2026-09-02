/* Links the AuroraOS ARM11 audio core to run in place at AUDIO_CORE_ADDR
   (0x23000000 in FCRAM). The ARM9 OS copies the raw binary there and wakes the
   ARM11 into it. Stack lives at 0x23200000; the command block (0x23300000) and
   PCM buffer (0x23400000) sit above and are referenced by absolute address. */
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

    /* Stack grows down from here (2 MB above the load address). */
    _audio11_stack_top = 0x23200000;

    /DISCARD/ :
    {
        *(.ARM.exidx*)
        *(.ARM.attributes*)
        *(.comment*)
        *(.note*)
    }
}
