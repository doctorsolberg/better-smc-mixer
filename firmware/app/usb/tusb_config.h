/*
 * TinyUSB configuration for the application: device mode, a CDC interface (the command link,
 * docs/link.md) and a USB MIDI function (the controller, SPEC FR-IO-01), no OS (the USB task polls
 * the stack, firmware/app/usb/usb_app.c).
 */
#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

/* BR25 is not one of TinyUSB's MCUs: the controller driver is ours (dcd_*). */
#define CFG_TUSB_MCU               OPT_MCU_NONE
#define CFG_TUSB_OS                OPT_OS_NONE
#define CFG_TUSB_DEBUG             0
#define CFG_TUSB_MEM_ALIGN         __attribute__((aligned(4)))

/* Endpoints 0-4, all the controller has: CDC notification 0x81, data OUT 0x02, data IN 0x83; MIDI
 * OUT 0x04 and IN 0x84 share a number (docs/usb.md, "Endpoints in the application"). */
#define TUP_DCD_ENDPOINT_MAX       5
#define CFG_TUD_ENABLED            1
#define CFG_TUD_MAX_SPEED          OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE     64

#define CFG_TUD_CDC                1
/* Commands are small and answered one at a time; the log needs room while the host is slow. */
#define CFG_TUD_CDC_RX_BUFSIZE     256
#define CFG_TUD_CDC_TX_BUFSIZE     1024
#define CFG_TUD_CDC_EP_BUFSIZE     64
/* A full FIFO refuses: the link only writes whole frames that fit, and the log ring counts what
 * it cannot send (LOST records). Never overwrite. */
#define CFG_TUD_CDC_TX_OVERWRITABLE_IF_NOT_CONNECTED 0

#define CFG_TUD_MIDI               1
/* The USB task moves packets to and from its own queues (firmware/midi). In: 64 packets. Out: 16,
 * one USB packet's worth waiting behind the one on its way, so that the MIDI path stays short
 * (NFR-01; docs/core.md, "The MIDI queue under load"): the control task's backpressure watches
 * our queue only, and every packet in this FIFO is time on top. */
#define CFG_TUD_MIDI_RX_BUFSIZE    256
#define CFG_TUD_MIDI_TX_BUFSIZE    64
#define CFG_TUD_MIDI_EP_BUFSIZE    64

#endif /* TUSB_CONFIG_H */
