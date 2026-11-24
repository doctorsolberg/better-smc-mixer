/*
 * USB in the application (docs/link.md, docs/usb.md): TinyUSB on our BR25 controller driver with a
 * CDC interface that carries the command link and the log stream, and a USB MIDI function with
 * two ports (performance, config).
 */
#ifndef USB_APP_H
#define USB_APP_H

#include <stdint.h>

#include "link.h"

struct midiq;
struct metrics_window;

/* What the variant does with USB MIDI. Both fields may be NULL. */
struct usb_app_midi {
    /* USB task: a channel message from the host on the performance port (3 bytes; a 2-byte
     * message has msg[2] = 0). */
    void (*in)(const uint8_t msg[3]);
    /* Packets for the host, filled by another task (midiq.h); it calls usb_app_wake() after. */
    struct midiq *out;
    /* USB task, or NULL: the producer of out asked for room (midiq_want_room()) and has it now. */
    void (*room)(void);
};

/* Starts the USB task. ops and midi (kept, not copied) are what the variant offers the link and
 * MIDI; product is the USB product string. Call once, from task context, after smclog_init(). */
void usb_app_start(const struct link_ops *ops, const char *product, const char *serial, const struct usb_app_midi *midi);

struct sxpipe;

/* Before usb_app_start(): the command link over BLE MIDI (sxpipe.h; kept). The USB task runs its
 * link and calls wake when a reply waits in the pipe. Without it (NULL, or never called): none. */
void usb_app_set_ble_link(struct sxpipe *p, void (*wake)(void));

/* Any task or interrupt: wakes the USB task (MIDI packets wait in midi->out). */
void usb_app_wake(void);

/* Logs the USB driver's, the command link's and USB MIDI's counters (METRICS_NOW, statistics). */
void usb_app_log_stats(void);

/* The heartbeat (LOG-06), one context at a time: the USB task's window into w (lat_usb,
 * busy_usb_us, midi_out, midi_in, midi_dropped), then a new window. */
void usb_app_take_window(struct metrics_window *w);

/* The USB product and serial strings (usb_descriptors.c); usb_app_start() sets them. */
extern const char *usb_app_product;
extern const char *usb_app_serial;

#endif /* USB_APP_H */
