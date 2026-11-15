ENTRY(_aif)
SECTIONS
{
    . = 0x00010000;
    __image_base = .;
    .aifhdr : { KEEP(*(.aifhdr)) }
    .text   : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    . = ALIGN(4);
    __load_end = .;
    .bss (NOLOAD) : { *(.bss*) *(COMMON) }
    . = ALIGN(4);
    __end = .;
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.ARM.attributes) *(.comment) }
}
__ro_size = __load_end - __image_base;
__zi_size = __end - __load_end;
