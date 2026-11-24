/*
 * USB descriptors of the key: one CDC ACM interface pair (log out, commands in).
 *
 * 1209:0001 is the pid.codes test ID, as in the bootloader's development builds (SPEC Q-14): the
 * key is a development tool and is never released as a product.
 */
#include <stddef.h>
#include <stdint.h>

#include "tusb.h"

enum { STR_LANG, STR_VENDOR, STR_PRODUCT, STR_SERIAL, STR_CDC };
enum { ITF_CDC, ITF_CDC_DATA, ITF_COUNT };

#define EP_NOTIFY 0x81
#define EP_OUT    0x02
#define EP_IN     0x82

static const tusb_desc_device_t device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    /* A composite-style device class, as CDC ACM devices with an IAD use. */
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x1209,
    .idProduct          = 0x0001,
    .bcdDevice          = 0x0100,
    .iManufacturer      = STR_VENDOR,
    .iProduct           = STR_PRODUCT,
    .iSerialNumber      = STR_SERIAL,
    .bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&device;
}

#define CONFIG_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static const uint8_t configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_CDC, STR_CDC, EP_NOTIFY, 8, EP_OUT, EP_IN, 64),
};

_Static_assert(sizeof(configuration) == CONFIG_LEN, "configuration descriptor length");

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return configuration;
}

static const char *const strings[] = {
    [STR_VENDOR]  = "better-smc-mixer",
    [STR_PRODUCT] = "JieLi USB key (RP2040)",
    [STR_SERIAL]  = "1",
    [STR_CDC]     = "usbkey log",
};

static uint16_t string_buf[1 + 32];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    size_t n;
    if (index == STR_LANG) {
        string_buf[1] = 0x0409; /* English (US) */
        n = 1;
    } else if (index < sizeof(strings) / sizeof(strings[0]) && strings[index]) {
        const char *s = strings[index];
        for (n = 0; s[n] && n < 32; n++)
            string_buf[1 + n] = (uint8_t)s[n];
    } else {
        return NULL;
    }
    string_buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return string_buf;
}
