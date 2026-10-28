/*
 * Describe the QEMU MPS2 reference code/RAM map and retain vectors and C++ initialization tables.
 * Reserve the initial exception stack separately from the heap used for dynamically created task stacks.
 */
ENTRY(Reset_Handler)
MEMORY {
 FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 4M
 RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 4M
}
_estack = ORIGIN(RAM) + LENGTH(RAM);
_heap_end = _estack - 16384;
SECTIONS {
 .text : { KEEP(*(.isr_vector)) *(.text*) *(.rodata*) KEEP(*(.init)) KEEP(*(.fini)) } > FLASH
 .ARM.extab : { *(.ARM.extab*) } > FLASH
 .ARM.exidx : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } > FLASH
 .preinit_array : { PROVIDE_HIDDEN(__preinit_array_start = .); KEEP(*(.preinit_array*)) PROVIDE_HIDDEN(__preinit_array_end = .); } > FLASH
 .init_array : { PROVIDE_HIDDEN(__init_array_start = .); KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array)) PROVIDE_HIDDEN(__init_array_end = .); } > FLASH
 .fini_array : { PROVIDE_HIDDEN(__fini_array_start = .); KEEP(*(SORT(.fini_array.*))) KEEP(*(.fini_array)) PROVIDE_HIDDEN(__fini_array_end = .); } > FLASH
 .data : { . = ALIGN(8); _sdata = .; *(.data*) . = ALIGN(8); _edata = .; } > RAM AT> FLASH
 _sidata = LOADADDR(.data);
 .bss (NOLOAD) : { . = ALIGN(8); _sbss = .; *(.bss*) *(COMMON) . = ALIGN(8); _ebss = .; } > RAM
 . = ALIGN(8);
 _end = .;
 end = .;
 ASSERT(_end < _heap_end, "RAM cannot fit runtime and boot stack")
}
