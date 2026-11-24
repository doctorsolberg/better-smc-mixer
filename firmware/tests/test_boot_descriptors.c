/*
 * Host tests for the bootloader's USB descriptors (firmware/boot/usb_descriptors.c): the bytes a
 * host sees, walked as a host would walk them.
 */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "tusb.h"

int check_failed;

static void test_device(void)
{
    const uint8_t *d = tud_descriptor_device_cb();
    CHECK_EQ(d[0], 18);
    CHECK_EQ(d[1], TUSB_DESC_DEVICE);
    CHECK_EQ(d[2] | d[3] << 8, 0x0200);         /* bcdUSB */
    CHECK_EQ(d[4], 0);                          /* class per interface */
    CHECK_EQ(d[7], CFG_TUD_ENDPOINT0_SIZE);
    CHECK_EQ(d[17], 1);                         /* configurations */
}

static void test_configuration(void)
{
    const uint8_t *c = tud_descriptor_configuration_cb(0);
    unsigned total = c[2] | c[3] << 8;
    CHECK_EQ(c[1], TUSB_DESC_CONFIGURATION);
    CHECK_EQ(c[4], 1);                          /* interfaces */
    CHECK_EQ(total, 9u + 9 + 9);

    /* Walk the descriptors: one DFU interface, then its functional descriptor. */
    unsigned off = c[0], interfaces = 0, functional = 0;
    while (off < total) {
        const uint8_t *d = c + off;
        CHECK(d[0] >= 2 && off + d[0] <= total);
        if (d[0] < 2)
            break;
        if (d[1] == TUSB_DESC_INTERFACE) {
            interfaces++;
            CHECK_EQ(d[4], 0);                  /* no endpoints besides 0 */
            CHECK_EQ(d[5], TUSB_CLASS_APPLICATION_SPECIFIC);
            CHECK_EQ(d[6], 1);                  /* DFU */
            CHECK_EQ(d[7], 2);                  /* DFU mode, not runtime */
        } else if (d[1] == DFU_DESC_FUNCTIONAL) {
            functional++;
            CHECK_EQ(d[0], 9);
            CHECK_EQ(d[2], DFU_ATTR_CAN_DOWNLOAD | DFU_ATTR_MANIFESTATION_TOLERANT);
            CHECK_EQ(d[5] | d[6] << 8, CFG_TUD_DFU_XFER_BUFSIZE);
            CHECK_EQ(d[7] | d[8] << 8, 0x0110);   /* DFU 1.1 */
        }
        off += d[0];
    }
    CHECK_EQ(off, total);
    CHECK_EQ(interfaces, 1u);
    CHECK_EQ(functional, 1u);
}

/* String descriptor `index` as ASCII; NULL when there is none. */
static const char *string(uint8_t index, char *buf, size_t size)
{
    const uint16_t *s = tud_descriptor_string_cb(index, 0x0409);
    if (!s)
        return NULL;
    unsigned len = s[0] & 0xFF, n = (len - 2) / 2;
    CHECK_EQ(s[0] >> 8, TUSB_DESC_STRING);
    CHECK_EQ(len % 2, 0u);
    for (unsigned i = 0; i < n && i + 1 < size; i++)
        buf[i] = (char)s[1 + i];
    buf[n < size ? n : size - 1] = 0;
    return buf;
}

static void test_strings(void)
{
    char buf[64];
    const uint16_t *lang = tud_descriptor_string_cb(0, 0);
    CHECK(lang != NULL);
    if (lang) {
        CHECK_EQ(lang[0], (TUSB_DESC_STRING << 8) | 4);
        CHECK_EQ(lang[1], 0x0409);
    }
    const uint8_t *d = tud_descriptor_device_cb();
    const char *product = string(d[15], buf, sizeof buf);
    CHECK(product && strcmp(product, "SMC-Mixer DFU bootloader") == 0);
    const uint8_t *c = tud_descriptor_configuration_cb(0);
    const char *alt = string(c[9 + 8], buf, sizeof buf);   /* the interface's iInterface */
    CHECK(alt && strcmp(alt, "application") == 0);
    CHECK(string(200, buf, sizeof buf) == NULL);
}

int main(void)
{
    RUN(test_device);
    RUN(test_configuration);
    RUN(test_strings);
    printf("%s\n", check_failed ? "FAILED" : "all passed");
    return check_failed != 0;
}
