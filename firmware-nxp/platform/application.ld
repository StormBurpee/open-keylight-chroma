ENTRY(Reset_Handler)
MEMORY {
    APP (rx) : ORIGIN = 0x2000, LENGTH = 0x7000
    RAM (rwx) : ORIGIN = 0x100000c0, LENGTH = 0x0f40
}
_stack_top = ORIGIN(RAM) + LENGTH(RAM);
SECTIONS {
    .vectors : { KEEP(*(.vectors)) } > APP
    .text : { *(.text*) *(.rodata*) } > APP
    .ARM.exidx : { *(.ARM.exidx*) } > APP
    .data : ALIGN(4) {
        _data_start = .; *(.data*) . = ALIGN(4); _data_end = .;
    } > RAM AT > APP
    _data_load = LOADADDR(.data);
    .bss (NOLOAD) : ALIGN(4) {
        _bss_start = .; *(.bss*) *(COMMON) . = ALIGN(4); _bss_end = .;
    } > RAM
    ASSERT(SIZEOF(.vectors) == 192, "resident bootloader requires 48 vectors")
    ASSERT(_bss_end + 1024 <= _stack_top, "reserve at least 1 KiB stack")
    ASSERT(LOADADDR(.data) + SIZEOF(.data) <= 0x9000, "application exceeds bank")
    /DISCARD/ : { *(.comment*) *(.note*) }
}
