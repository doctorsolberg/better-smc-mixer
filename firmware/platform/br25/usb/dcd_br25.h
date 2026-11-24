/*
 * TinyUSB device controller driver for the BR25 (docs/usb.md). The driver implements TinyUSB's
 * dcd_* API (third_party/tinyusb/src/device/dcd.h); this header is what the platform adds.
 */
#ifndef DCD_BR25_H
#define DCD_BR25_H

#include <stdbool.h>
#include <stdint.h>

/* Provided by the platform: unmask (true) or mask (false) the controller's interrupt
 * (BR25_USB_IRQ). The SDK-based application does it through the interrupt controller; the
 * bootloader is polled and implements it empty. The handler itself is installed by the platform
 * and calls dcd_int_handler(0); it must be installed before tusb_init() (docs/usb.md). */
void br25_usb_irq_set(bool enabled);

/* Counters for diagnostics (the hardware-test firmware prints them). */
struct br25_usb_stats {
    uint32_t resets, suspends, resumes, setups;
    uint32_t stalls_sent, setup_ends;
    uint32_t tx_packets, rx_packets;
    uint32_t rx_waits;      /* packets whose count read 0 at first (docs/usb.md, item 2) */
    uint32_t rx_wait_max;   /* the longest wait, in count reads */
    uint32_t rx_wait_fails; /* packets whose count stayed 0: taken as zero-length */
    uint32_t rx_truncated;  /* bytes the host sent past the armed length */
    uint32_t reg_timeouts;  /* CON1 accesses that never got their ACK */
};

const struct br25_usb_stats *br25_usb_stats(void);

#endif /* DCD_BR25_H */
