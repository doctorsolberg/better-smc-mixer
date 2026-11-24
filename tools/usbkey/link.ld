/*
 * USB key firmware for the RP2040: runs in place from flash.
 * 0x10000000  second-stage boot (256 bytes, boot2.S)
 * 0x10000100  vector table, code, constants, then the initial values of .data
 * RAM: .data (with the code that must not run from flash), .bss, the stack at the top of the
 * striped 256 KB.
 */
MEMORY
{
    flash (rx)  : ORIGIN = 0x10000000, LENGTH = 2048K
    ram   (rwx) : ORIGIN = 0x20000000, LENGTH = 256K
}

ENTRY(reset_handler)

SECTIONS
{
    .boot2 :
    {
        KEEP(*(.boot2))
    } > flash
    ASSERT(SIZEOF(.boot2) == 256, "boot2 must be exactly 256 bytes")

    .text :
    {
        KEEP(*(.vectors))
        KEEP(*(.note.gnu.build-id))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > flash

    .data :
    {
        __data_start = .;
        *(.ram_func*)
        *(.data*)
        . = ALIGN(4);
        __data_end = .;
    } > ram AT > flash
    __data_load = LOADADDR(.data);

    .bss (NOLOAD) :
    {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > ram

    __stack_top = ORIGIN(ram) + LENGTH(ram);
    ASSERT(__stack_top - __bss_end >= 8K, "less than 8 KB left for the stack")

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) }
}
