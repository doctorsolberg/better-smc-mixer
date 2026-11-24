/*
 * The application variant (firmware/app/Makefile, VARIANT): what app_main() starts once the log
 * runs, and what it offers over USB.
 *
 *   app      controller.c, the controller: the scan, the core and USB MIDI (docs/core.md)
 *   hwtest   hwtest/hwtest.c, the hardware test (docs/hwtest.md)
 */
#ifndef VARIANT_H
#define VARIANT_H

#include "link.h"
#include "usb_app.h"

struct variant {
    const char *usb_product;     /* the USB product string */
    const char *usb_serial;      /* the USB serial string: one per variant, so a host keeps each
                                    variant as a device of its own, with its own name (usb.md) */
    const struct link_ops *link; /* the commands it serves beyond the log (docs/link.md) */
    struct usb_app_midi midi;    /* what it does with USB MIDI */
};

/* Starts the variant's work; app_main() then starts USB with what it returns. */
const struct variant *variant_start(void);

#endif /* VARIANT_H */
