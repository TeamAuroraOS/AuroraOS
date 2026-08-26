

OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)

MEMORY
{
    
    ARM9_IMEM (rwx) : ORIGIN = 0x08006800, LENGTH = 0x0F9800
}

SECTIONS
{
    .text :
    {
        *(.vectors)     
        *(.text*)       
        *(.rodata*)     
        . = ALIGN(4);
    } > ARM9_IMEM

    .data :
    {
        _data_start = .;
        *(.data*)
        . = ALIGN(4);
        _data_end = .;
    } > ARM9_IMEM

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } > ARM9_IMEM

    
    _stack_top = ORIGIN(ARM9_IMEM) + LENGTH(ARM9_IMEM);

    
    /DISCARD/ :
    {
        *(.ARM.exidx*)
        *(.ARM.attributes*)
        *(.comment*)
        *(.note*)
    }
}
