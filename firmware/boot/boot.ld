/*
 * Bootloader linker script. Stage 1: the image is built and checked, never written to the device.
 *
 * Flash: where the bootloader lives and how the boot chain reaches it are stage-2 decisions
 * (SPEC §3.4, C-2). Until then it is linked where the SDK links applications (0x1E00120, flash
 * seen through the cache), only so that code addresses are realistic. The size budget is ours.
 *
 * RAM: the bootloader runs alone, before the application, and takes the bottom 16 KB from 0x10000.
 * Never above 0x31AE0: the ROM and the SDK keep the update/boot area there (0x80 bytes), then the
 * mask ROM's RAM (0x31B60) and the interrupt vectors (0x31F00). A region the application leaves
 * crash records in (SPEC §3.4) must also be kept out of this one once it is placed (stage 2).
 */
MEMORY
{
    flash (rx) : ORIGIN = 0x1E00120, LENGTH = 32K - 0x120
    ram (rwx)  : ORIGIN = 0x10000, LENGTH = 16K
}

ENTRY(_start)

SECTIONS
{
    .text :
    {
        KEEP(*(.boot_start))
        . = ALIGN(4);
        /* namesz, descsz, type, "GNU\0", then the ID (smclog_port_build_id) */
        __build_id_note = .;
        KEEP(*(.note.gnu.build-id))
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > flash

    /* crt0.S copies and zeroes whole words */
    .data : ALIGN(4)
    {
        __data_start = .;
        *(.data .data.*)
        . = ALIGN(4);
        __data_end = .;
    } > ram AT > flash
    __data_load = LOADADDR(.data);

    .bss (NOLOAD) : ALIGN(4)
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > ram

    __stack_top = ORIGIN(ram) + LENGTH(ram);
    ASSERT(__stack_top - __bss_end >= 2K, "boot.ld: less than 2 KB left for the stack")

    INCLUDE smclog.ld
}
