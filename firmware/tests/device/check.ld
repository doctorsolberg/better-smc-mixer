/* Linker script for the device toolchain checks (images that are built but never run). */
MEMORY
{
    flash (rx) : ORIGIN = 0x1E00120, LENGTH = 480K
    ram (rwx)  : ORIGIN = 0x10000, LENGTH = 64K
}

SECTIONS
{
    .text : { *(.text*) *(.rodata*) } > flash
    .note.gnu.build-id : { __build_id_note = .; KEEP(*(.note.gnu.build-id)) } > flash
    .data : { *(.data*) } > ram AT > flash
    .bss (NOLOAD) : { *(.bss*) *(COMMON) } > ram
    INCLUDE smclog.ld
}
