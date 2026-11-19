/* link.ld - CH32V203K8/C8/G8/F8 (D6: Flash 64KB / SRAM 20KB) */

ENTRY(_start)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 64K
    RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 20K
}

__stack_size = 2048;

SECTIONS
{
    .init :
    {
        KEEP(*(.init))
    } >FLASH

    .vector : ALIGN(4)
    {
        KEEP(*(.vector))
    } >FLASH

    .text : ALIGN(4)
    {
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
        . = ALIGN(4);
    } >FLASH

    .data : ALIGN(4)
    {
        _data_vma = .;
        *(.data .data.*)
        . = ALIGN(8);
        PROVIDE(__global_pointer$ = . + 0x800);
        *(.sdata .sdata.*)
        . = ALIGN(4);
        _edata = .;
    } >RAM AT>FLASH
    _data_lma = LOADADDR(.data);

    .bss (NOLOAD) : ALIGN(4)
    {
        _sbss = .;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } >RAM

    .stack ORIGIN(RAM) + LENGTH(RAM) - __stack_size (NOLOAD) :
    {
        . = ALIGN(4);
        . = . + __stack_size;
        _estack = .;
    } >RAM

    ASSERT(_ebss <= ORIGIN(RAM) + LENGTH(RAM) - __stack_size, "RAM overflow")

    /DISCARD/ : { *(.comment) }
}
