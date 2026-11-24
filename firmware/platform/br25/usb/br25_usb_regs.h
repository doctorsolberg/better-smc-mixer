/*
 * BR25 (AC696x) full-speed USB device controller: registers (docs/usb.md).
 *
 * Facts from the AC63 SDK: the register map in include_lib/driver/cpu/br25/asm/br25.h and usb.h,
 * the MUSB register numbers and bits in include_lib/driver/device/usb/usb_phy.h, and the access
 * sequences of its compiled driver (cpu/br25/liba/cpu.a: usb.c, usb_phy.c, gpio.c). Names here
 * are ours.
 *
 * The controller is a Mentor MUSB core behind two JieLi blocks:
 * - its byte-wide registers are reached one at a time through CON1 (BR25_USB_MUSB_*);
 * - packets move by DMA from/to RAM buffers whose addresses are in EP0_ADR and EPn_TADR/RADR;
 *   writing EPn_CNT starts a transmit.
 */
#ifndef BR25_USB_REGS_H
#define BR25_USB_REGS_H

#define BR25_USB_BASE          0x1E1800u
#define BR25_USB_CON0          (BR25_USB_BASE + 0x00u)
#define BR25_USB_CON1          (BR25_USB_BASE + 0x04u)
#define BR25_USB_EP_CNT(n)     (BR25_USB_BASE + 0x08u + 4u * (n))        /* n = 0..4 */
#define BR25_USB_EP0_ADR       (BR25_USB_BASE + 0x1Cu)
#define BR25_USB_EP_TADR(n)    (BR25_USB_BASE + 0x20u + 8u * ((n) - 1u)) /* n = 1..4 */
#define BR25_USB_EP_RADR(n)    (BR25_USB_BASE + 0x24u + 8u * ((n) - 1u)) /* n = 1..4 */
#define BR25_USB_IO_CON0       0x1E5100u /* D+/D- pads, in the GPIO block */

#define BR25_USB_EPS           5u        /* endpoints 0..4 */
#define BR25_USB_IRQ           9u        /* IRQ_USB_CTRL_IDX; 8 is the SOF interrupt */

/* CON0 */
#define BR25_CON0_PHY_ON       (1u << 0)
#define BR25_CON0_LOW_SPEED    (1u << 1)
#define BR25_CON0_NRST         (1u << 2)  /* MUSB out of reset: CON1 accesses need it */
#define BR25_CON0_TM1          (1u << 3)
#define BR25_CON0_CID          (1u << 4)
#define BR25_CON0_VBUS         (1u << 5)
#define BR25_CON0_SOFIE        (1u << 10)
#define BR25_CON0_SIEIE        (1u << 11) /* MUSB interrupt enable */
#define BR25_CON0_CLR_SOF_PND  (1u << 12)
#define BR25_CON0_SOF_PND      (1u << 13)
#define BR25_CON0_SIE_PND      (1u << 14)

/* CON1: one MUSB register access. Write: reg << 8 | data. Read: reg << 8 | RNW, then wait for
 * ACK and take the low byte. */
#define BR25_CON1_RNW          (1u << 14)
#define BR25_CON1_ACK          (1u << 15)

/* IO_CON0 */
#define BR25_IO_DPPD           (1u << 4)
#define BR25_IO_DMPD           (1u << 5)
#define BR25_IO_DPPU           (1u << 6)  /* the 1.5 kΩ on D+: attach */
#define BR25_IO_DMPU           (1u << 7)
#define BR25_IO_DPDIE          (1u << 9)  /* digital input on (gpio_set_die of the USB pins) */
#define BR25_IO_DMDIE          (1u << 10)
#define BR25_IO_IO_MODE        (1u << 11) /* set: the pads are GPIOs; clear: USB */
#define BR25_IO_SR             (1u << 12)

/* MUSB registers (CON1 numbers; JieLi's order, not the standard MUSB offsets). 0x10..0x17 are
 * indexed by INDEX. */
#define BR25_MUSB_FADDR        0x00u
#define BR25_MUSB_POWER        0x01u
#define BR25_MUSB_INTRTX1      0x02u
#define BR25_MUSB_INTRTX2      0x03u
#define BR25_MUSB_INTRRX1      0x04u
#define BR25_MUSB_INTRRX2      0x05u
#define BR25_MUSB_INTRUSB      0x06u
#define BR25_MUSB_INTRTX1E     0x07u
#define BR25_MUSB_INTRTX2E     0x08u
#define BR25_MUSB_INTRRX1E     0x09u
#define BR25_MUSB_INTRRX2E     0x0Au
#define BR25_MUSB_INTRUSBE     0x0Bu
#define BR25_MUSB_FRAME1       0x0Cu
#define BR25_MUSB_FRAME2       0x0Du
#define BR25_MUSB_INDEX        0x0Eu
#define BR25_MUSB_TXMAXP       0x10u
#define BR25_MUSB_CSR0         0x11u /* index 0 */
#define BR25_MUSB_TXCSR1       0x11u /* index n */
#define BR25_MUSB_TXCSR2       0x12u
#define BR25_MUSB_RXMAXP       0x13u
#define BR25_MUSB_RXCSR1       0x14u
#define BR25_MUSB_RXCSR2       0x15u
#define BR25_MUSB_COUNT0       0x16u /* index 0 */
#define BR25_MUSB_RXCOUNT1     0x16u /* index n */
#define BR25_MUSB_RXCOUNT2     0x17u

/* MaxP is written as 0xFF whatever the packet size, as the SDK does: with the real size the
 * controller double-buffers on its own (docs/usb.md). */
#define BR25_MUSB_MAXP_ANY     0xFFu

/* POWER */
#define BR25_POWER_RESUME      0x04u
#define BR25_POWER_SOFTCONN    0x40u

/* INTRUSB / INTRUSBE */
#define BR25_INTR_SUSPEND      0x01u
#define BR25_INTR_RESUME       0x02u
#define BR25_INTR_RESET        0x04u
#define BR25_INTR_SOF          0x08u

/* CSR0 (peripheral) */
#define BR25_CSR0_RXPKTRDY     0x01u
#define BR25_CSR0_TXPKTRDY     0x02u
#define BR25_CSR0_SENTSTALL    0x04u
#define BR25_CSR0_DATAEND      0x08u
#define BR25_CSR0_SETUPEND     0x10u
#define BR25_CSR0_SENDSTALL    0x20u
#define BR25_CSR0_SVDRXPKTRDY  0x40u
#define BR25_CSR0_SVDSETUPEND  0x80u

/* TXCSR (peripheral), low byte */
#define BR25_TXCSR_TXPKTRDY    0x01u
#define BR25_TXCSR_FIFONOTEMPTY 0x02u
#define BR25_TXCSR_UNDERRUN    0x04u
#define BR25_TXCSR_FLUSHFIFO   0x08u
#define BR25_TXCSR_SENDSTALL   0x10u
#define BR25_TXCSR_SENTSTALL   0x20u
#define BR25_TXCSR_CLRDATATOG  0x40u

/* RXCSR (peripheral), low byte */
#define BR25_RXCSR_RXPKTRDY    0x01u
#define BR25_RXCSR_FIFOFULL    0x02u
#define BR25_RXCSR_OVERRUN     0x04u
#define BR25_RXCSR_DATAERROR   0x08u
#define BR25_RXCSR_FLUSHFIFO   0x10u
#define BR25_RXCSR_SENDSTALL   0x20u
#define BR25_RXCSR_SENTSTALL   0x40u
#define BR25_RXCSR_CLRDATATOG  0x80u
/* What the SDK clears when it pops a packet (RxPktRdy, OverRun, FlushFIFO, the stall bits,
 * IncompRx), before setting FlushFIFO. */
#define BR25_RXCSR_POP_CLEAR   0x175u

#endif /* BR25_USB_REGS_H */
