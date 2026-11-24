/*
 * The interrupt lock our code shares with the scan interrupt (docs/hwtest.md, "While flash is
 * written").
 *
 * The SDK's local_irq_disable() masks priorities 0-6 only: it sets IPMASK to 6 and turns the
 * global enable back on (from the image's disassembly), so the scan interrupt, at priority 7, runs
 * inside it. This lock clears the global enable (cli) and sets it again (sti) only if it was on.
 * "On" is the SDK's own test, both bits 0x300 of ICFG set (cpu_irq_disabled(), same source).
 * ⚠️ The bits' meaning comes from that function, not from a manual.
 *
 * Inline, so that code in RAM that takes it stays in RAM. Between the two: no SDK call (its
 * local_irq_enable() would turn interrupts back on) and nothing that waits.
 */
#ifndef IRQ_BR25_H
#define IRQ_BR25_H

#include <stdint.h>

#define IRQ_BR25_ON 0x300u

static inline __attribute__((always_inline)) uint32_t irq_br25_save(void)
{
    uint32_t icfg;
    __asm__ __volatile__("%0 = icfg" : "=r"(icfg));
    __asm__ __volatile__("cli" ::: "memory");
    return icfg;
}

static inline __attribute__((always_inline)) void irq_br25_restore(uint32_t icfg)
{
    if ((icfg & IRQ_BR25_ON) == IRQ_BR25_ON)
        __asm__ __volatile__("sti" ::: "memory");
}

/* An interrupt's enable bit, as the SDK's bit_set_ie()/bit_clr_ie() set it (bit 0 of the
 * interrupt's 4-bit field in IPCON at 0x10F100 + (idx / 8) * 4, request_irq()'s register), under
 * our lock: theirs goes through the ROM's patched interrupt lock, about 6.4 µs a pair
 * (docs/hwtest.md, "The CPU's time"), and TinyUSB masks the USB interrupt at every event. */
static inline __attribute__((always_inline)) void irq_br25_enable(unsigned idx, int on)
{
    volatile uint32_t *r = (volatile uint32_t *)(0x10F100u + (idx >> 3) * 4u);
    uint32_t bit = 1u << ((idx & 7u) * 4u);
    uint32_t s = irq_br25_save();
    if (on)
        *r |= bit;
    else
        *r &= ~bit;
    irq_br25_restore(s);
}

#endif /* IRQ_BR25_H */
