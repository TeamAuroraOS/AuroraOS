/* The ARM11 job, run in place at JOB11_CODE (job11.h), entry first. Nothing
 * clears memory for it, so it may have no .bss. */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(job11_entry)

SECTIONS
{
    . = 0x25100000;

    .text :
    {
        KEEP(*(.text.entry))
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    }

    .data :
    {
        *(.data .data.*)
        . = ALIGN(4);
    }

    .bss :
    {
        *(.bss .bss.*)
        *(COMMON)
    }

    ASSERT(SIZEOF(.bss) == 0, "the ARM11 job must not use .bss: nothing clears it")
    ASSERT(. <= 0x25180000, "the ARM11 job runs into its parameter block")

    /DISCARD/ :
    {
        *(.ARM.exidx*)
        *(.ARM.extab*)
        *(.ARM.attributes*)
        *(.comment*)
        *(.note*)
    }
}
