/*
 * RP2040 second-stage boot: the first 256 bytes of flash. The boot ROM copies them to RAM, checks
 * the CRC-32 in the last 4 bytes (filled in by uf2.py) and runs them. This one sets the flash
 * interface to execute in place with the plain READ command (0x03), which every SPI flash knows,
 * then starts the program through its vector table at 0x10000100.
 *
 * Runs from wherever the ROM put it: only PC-relative loads.
 */
    .syntax unified
    .cpu cortex-m0plus
    .thumb

    .section .boot2, "ax"

    .equ SSI_BASE,      0x18000000
    .equ SSI_CTRLR0,    0x00
    .equ SSI_CTRLR1,    0x04
    .equ SSI_SSIENR,    0x08
    .equ SSI_BAUDR,     0x14
    .equ SSI_SPI_CTRLR0, 0xf4

    /* CTRLR0: standard SPI frame format, 32-bit frames, EEPROM read mode (TMOD 3) */
    .equ CTRLR0_XIP,    (0 << 21) | (31 << 16) | (3 << 8)
    /* SPI_CTRLR0: command 0x03, 24-bit address (6 nibbles), 8-bit instruction, command and
     * address on one line */
    .equ SPI_CTRLR0_XIP, (0x03 << 24) | (2 << 8) | (6 << 2) | (0 << 0)

    .global boot2_entry
    .type boot2_entry, %function
boot2_entry:
    push {lr}
    ldr r3, =SSI_BASE
    movs r1, #0
    str r1, [r3, #SSI_SSIENR]       /* disable the interface to configure it */
    movs r1, #4
    str r1, [r3, #SSI_BAUDR]        /* flash clock = system clock / 4 */
    ldr r1, =CTRLR0_XIP
    str r1, [r3, #SSI_CTRLR0]
    ldr r1, =SPI_CTRLR0_XIP
    ldr r0, =(SSI_BASE + SSI_SPI_CTRLR0)
    str r1, [r0]
    movs r1, #0
    str r1, [r3, #SSI_CTRLR1]       /* one 32-bit frame per read */
    movs r1, #1
    str r1, [r3, #SSI_SSIENR]
    /* Called from the ROM's flash boot (lr = 0): start the program; otherwise return. */
    pop {r0}
    cmp r0, #0
    beq 1f
    bx r0
1:  ldr r0, =0x10000100
    ldr r1, =0xe000ed08             /* VTOR */
    str r0, [r1]
    ldmia r0, {r0, r1}
    msr msp, r0
    bx r1

    .ltorg

    /* 252 bytes of code at most, then the CRC-32 */
    .org 252, 0
    .word 0
