/*
 * Bootloader startup (no SDK): stacks, .bss, .data, then boot_main().
 *
 * The sequence follows what the SDK's startup does on BR25 (seen in the disassembly of an SDK
 * image, docs/build.md): both stack pointers (sp, ssp) are set to the same address, .bss is zeroed
 * and .data copied word by word (boot.ld keeps both 4-byte aligned and sized), bits 8-9 of icfg
 * are cleared before C code runs. What those icfg bits mean is not known yet (stage 2); the SDK's
 * other early calls (boot info, maskrom_init) are not needed by the bootloader.
 */
    .section .boot_start, "ax"
    .global _start
_start:
    sp = __stack_top
    ssp = __stack_top

    r3 = __bss_start
    r2 = __bss_end
    r1 = 0
1:
    if (r3 >= r2) goto 2f
    [r3++=4] = r1
    goto 1b
2:
    r4 = __data_start
    r3 = __data_end
    r1 = __data_load
3:
    if (r4 >= r3) goto 4f
    r2 = [r1++=4]
    [r4++=4] = r2
    goto 3b
4:
    r2 = icfg
    r2 = r2 & 0xFFFFFCFF
    icfg = r2

    call boot_main
5:
    goto 5b
