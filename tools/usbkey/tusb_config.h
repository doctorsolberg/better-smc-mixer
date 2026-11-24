/*
 * TinyUSB configuration for the USB key firmware: device mode, one CDC interface (the log and
 * the commands), no OS.
 */
#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

/* The controller driver is ours (dcd_rp2040.c): TinyUSB's own RP2040 port needs the Pico SDK. */
#define CFG_TUSB_MCU               OPT_MCU_NONE
#define CFG_TUSB_OS                OPT_OS_NONE
#define CFG_TUSB_DEBUG             0
#define CFG_TUSB_MEM_ALIGN         __attribute__((aligned(4)))

#define TUP_DCD_ENDPOINT_MAX       16
#define CFG_TUD_ENABLED            1
#define CFG_TUD_MAX_SPEED          OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE     64

#define CFG_TUD_CDC                1
#define CFG_TUD_CDC_RX_BUFSIZE     64
#define CFG_TUD_CDC_TX_BUFSIZE     1024
#define CFG_TUD_CDC_EP_BUFSIZE     64

#endif /* TUSB_CONFIG_H */
