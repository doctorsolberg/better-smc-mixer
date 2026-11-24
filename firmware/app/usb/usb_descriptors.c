/*
 * Application USB descriptors: one configuration with a CDC ACM function (two interfaces), the
 * command link (docs/link.md), and a USB MIDI function (two interfaces), the controller (SPEC
 * FR-IO-01).
 *
 * USB IDs: the pid.codes test ID 1209:0001 for development builds, like the bootloader
 * (boot/usb_descriptors.c, SPEC Q-14); never a release.
 *
 * Endpoints: the controller has 1-4. CDC keeps IN and OUT on different numbers (docs/usb.md, item
 * 5): notification 0x81, data OUT 0x02, data IN 0x83. MIDI's OUT and IN share endpoint 4 (0x04,
 * 0x84): five directions do not fit four numbers otherwise (docs/usb.md, "Endpoints in the
 * application"; ⚠️ to check on the device).
 *
 * MIDI has two ports (USB-MIDI cables, SPEC FR-IO-01) with stable names: "SMC-Mixer" for the
 * controller and "SMC-Mixer Config" for the command link over SysEx (docs/link.md, "Over SysEx";
 * usb_app.c).
 */
#include <stddef.h>
#include <stdint.h>

#include "tusb.h"
#include "app_version.h"
#include "usb_app.h"

#define APP_USB_VID 0x1209
#define APP_USB_PID 0x0001
#define APP_USB_BCD APP_VERSION_BCD /* the product's version (app_version.h) */

#define EP_CDC_NOTIFY 0x81
#define EP_CDC_OUT 0x02
#define EP_CDC_IN 0x83
#define CDC_NOTIFY_SIZE 8
#define EP_MIDI_OUT 0x04
#define EP_MIDI_IN 0x84
#define MIDI_EP_SIZE 64
#define MIDI_CABLES 2

enum { ITF_CDC, ITF_CDC_DATA, ITF_MIDI, ITF_MIDI_STREAMING, ITF_COUNT };
enum {
    STR_LANG,
    STR_VENDOR,
    STR_PRODUCT,
    STR_SERIAL,
    STR_CDC,
    STR_MIDI,
    STR_PORT_PERFORMANCE,
    STR_PORT_CONFIG,
    STR_COUNT
};

const char *usb_app_product = "SMC-Mixer";
/* One per variant (variant.h): the controller "C1", the hardware test "T1". With one serial for both
 * (it was "0") macOS kept the first name it saw for both (usb.md, "USB MIDI"). */
const char *usb_app_serial = "C1";

static const tusb_desc_device_t device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    /* an interface association (CDC) needs the IAD class triple */
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = APP_USB_VID,
    .idProduct = APP_USB_PID,
    .bcdDevice = APP_USB_BCD,
    .iManufacturer = STR_VENDOR,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&device;
}

#define MIDI_DESC_LEN \
    (TUD_MIDI_DESC_HEAD_LEN + MIDI_CABLES * TUD_MIDI_DESC_JACK_LEN + 2 * TUD_MIDI_DESC_EP_LEN(MIDI_CABLES))
#define CONFIG_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + MIDI_DESC_LEN)

static const uint8_t configuration[] = {
    /* config number, interface count, string index, total length, attributes, power (mA) */
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_LEN, 0x00, 100),
    /* interface number, string index, notification endpoint and size, data OUT, data IN, size */
    TUD_CDC_DESCRIPTOR(ITF_CDC, STR_CDC, EP_CDC_NOTIFY, CDC_NOTIFY_SIZE, EP_CDC_OUT, EP_CDC_IN,
                       CFG_TUD_CDC_EP_BUFSIZE),
    /* MIDI: the audio control and streaming interfaces, a pair of jacks per port (cable), then
     * the endpoints with the embedded jacks they carry, in cable order */
    TUD_MIDI_DESC_HEAD(ITF_MIDI, STR_MIDI, MIDI_CABLES),
    TUD_MIDI_DESC_JACK_DESC(1, STR_PORT_PERFORMANCE),
    TUD_MIDI_DESC_JACK_DESC(2, STR_PORT_CONFIG),
    TUD_MIDI_DESC_EP(EP_MIDI_OUT, MIDI_EP_SIZE, MIDI_CABLES),
    TUD_MIDI_JACKID_IN_EMB(1),
    TUD_MIDI_JACKID_IN_EMB(2),
    TUD_MIDI_DESC_EP(EP_MIDI_IN, MIDI_EP_SIZE, MIDI_CABLES),
    TUD_MIDI_JACKID_OUT_EMB(1),
    TUD_MIDI_JACKID_OUT_EMB(2),
};

_Static_assert(sizeof(configuration) == CONFIG_LEN, "configuration descriptor length");

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return configuration;
}

static uint16_t string_buf[1 + 32];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    const char *s = NULL;
    size_t n;

    switch (index) {
    case STR_LANG:
        string_buf[1] = 0x0409; /* English (US) */
        string_buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | 4);
        return string_buf;
    case STR_VENDOR:
        s = "better-smc-mixer";
        break;
    case STR_PRODUCT:
        s = usb_app_product;
        break;
    case STR_SERIAL:
        /* Fixed per variant, not per mixer (the chip's number is not read): a host keeps each
         * variant as one device of its own, with its own name. */
        s = usb_app_serial;
        break;
    case STR_CDC:
        s = "SMC-Mixer link";
        break;
    case STR_MIDI:
    case STR_PORT_PERFORMANCE:
        s = "SMC-Mixer";
        break;
    case STR_PORT_CONFIG:
        s = "SMC-Mixer Config";
        break;
    default:
        return NULL;
    }
    for (n = 0; s[n] && n < 32; n++)
        string_buf[1 + n] = (uint8_t)s[n];
    string_buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return string_buf;
}
