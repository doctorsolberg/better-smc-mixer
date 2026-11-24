/*
 * Bootloader USB descriptors: one configuration, one DFU 1.1 interface with one alternate
 * setting (the application image).
 *
 * USB IDs: the firmware gets its own VID:PID, a pid.codes PID requested once the repository is
 * public (SPEC Q-14). Until then 1209:0001, the pid.codes test ID, for development builds only.
 * After the first release the bootloader and the application never change them (NFR-10).
 */
#include <stddef.h>
#include <stdint.h>

#include "tusb.h"

#define BOOT_USB_VID     0x1209
#define BOOT_USB_PID     0x0001
#define BOOT_USB_BCD     0x0100

/* Time the host may take between DFU_DETACH and the reset (ms); the bootloader is already in
 * DFU mode, so it is only reported. */
#define BOOT_DFU_DETACH_MS 1000

enum { STR_LANG, STR_VENDOR, STR_PRODUCT, STR_SERIAL, STR_ALT_APP };

static const tusb_desc_device_t device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = BOOT_USB_VID,
    .idProduct          = BOOT_USB_PID,
    .bcdDevice          = BOOT_USB_BCD,
    .iManufacturer      = STR_VENDOR,
    .iProduct           = STR_PRODUCT,
    .iSerialNumber      = STR_SERIAL,
    .bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&device;
}

#define CONFIG_LEN (TUD_CONFIG_DESC_LEN + TUD_DFU_DESC_LEN(1))

static const uint8_t configuration[] = {
    /* config number, interface count, string index, total length, attributes, power (mA) */
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_LEN, 0x00, 100),
    /* interface number, alternate count, first string index, attributes, detach timeout,
     * transfer size */
    TUD_DFU_DESCRIPTOR(0, 1, STR_ALT_APP, DFU_ATTR_CAN_DOWNLOAD | DFU_ATTR_MANIFESTATION_TOLERANT,
                       BOOT_DFU_DETACH_MS, CFG_TUD_DFU_XFER_BUFSIZE),
};

_Static_assert(sizeof(configuration) == CONFIG_LEN, "configuration descriptor length");

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return configuration;
}

static const char *const strings[] = {
    [STR_VENDOR]  = "better-smc-mixer",
    [STR_PRODUCT] = "SMC-Mixer DFU bootloader",
    /* The device serial number is not read yet (stage 2); a fixed one keeps hosts from seeing
     * a new device every time. */
    [STR_SERIAL]  = "0",
    [STR_ALT_APP] = "application",
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
