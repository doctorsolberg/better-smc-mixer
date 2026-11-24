/*
 * Host tests for the application's USB descriptors (firmware/app/usb/usb_descriptors.c): the bytes
 * a host sees, walked as a host would walk them.
 */
#include <stdio.h>
#include <string.h>

#include "app_version.h"
#include "check.h"
#include "tusb.h"
#include "usb_app.h"

int check_failed;

static void test_device(void)
{
    const uint8_t *d = tud_descriptor_device_cb();
    CHECK_EQ(d[0], 18);
    CHECK_EQ(d[1], TUSB_DESC_DEVICE);
    CHECK_EQ(d[2] | d[3] << 8, 0x0200);        /* bcdUSB */
    CHECK_EQ(d[4], TUSB_CLASS_MISC);           /* the IAD triple: CDC has two interfaces */
    CHECK_EQ(d[5], MISC_SUBCLASS_COMMON);
    CHECK_EQ(d[6], MISC_PROTOCOL_IAD);
    CHECK_EQ(d[7], CFG_TUD_ENDPOINT0_SIZE);
    CHECK_EQ(d[8] | d[9] << 8, 0x1209);        /* pid.codes test ID (SPEC Q-14) */
    CHECK_EQ(d[10] | d[11] << 8, 0x0001);
    CHECK_EQ(d[12] | d[13] << 8, APP_VERSION_BCD); /* bcdDevice: the product's version */
    CHECK_EQ(d[17], 1);                        /* configurations */
}

/* What the configuration walk found. */
struct walk {
    unsigned interfaces, iads, eps, jacks;
    unsigned in_nums, out_nums;  /* endpoint numbers used per direction */
    uint8_t jack_string[2];      /* iJack of each cable's embedded IN jack */
    unsigned midi_ep_jacks[2];   /* embedded jacks listed by the MIDI OUT and IN endpoints */
};

static void walk_configuration(struct walk *w)
{
    const uint8_t *c = tud_descriptor_configuration_cb(0);
    unsigned total = c[2] | c[3] << 8, off = c[0];
    /* interface n: class, subclass, endpoints */
    static const uint8_t itf[4][3] = {
        {TUSB_CLASS_CDC, CDC_COMM_SUBCLASS_ABSTRACT_CONTROL_MODEL, 1},
        {TUSB_CLASS_CDC_DATA, 0, 2},
        {TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_CONTROL, 0},
        {TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_MIDI_STREAMING, 2},
    };

    memset(w, 0, sizeof *w);
    while (off < total) {
        const uint8_t *d = c + off;
        CHECK(d[0] >= 2 && off + d[0] <= total);
        if (d[0] < 2)
            break;
        if (d[1] == TUSB_DESC_INTERFACE_ASSOCIATION) {
            w->iads++;
            CHECK_EQ(d[2], 0);                 /* CDC: the first two interfaces */
            CHECK_EQ(d[3], 2);
            CHECK_EQ(d[4], TUSB_CLASS_CDC);
        } else if (d[1] == TUSB_DESC_INTERFACE) {
            CHECK_EQ(d[2], w->interfaces);     /* numbered in order */
            if (w->interfaces < 4) {
                CHECK_EQ(d[5], itf[w->interfaces][0]);
                CHECK_EQ(d[6], itf[w->interfaces][1]);
                CHECK_EQ(d[4], itf[w->interfaces][2]);
            }
            w->interfaces++;
        } else if (d[1] == TUSB_DESC_CS_INTERFACE && w->interfaces == 4 &&
                   (d[2] == MIDI_CS_INTERFACE_IN_JACK || d[2] == MIDI_CS_INTERFACE_OUT_JACK)) {
            w->jacks++;
            if (d[2] == MIDI_CS_INTERFACE_IN_JACK && d[3] == MIDI_JACK_EMBEDDED && d[4] / 4 < 2)
                w->jack_string[d[4] / 4] = d[5]; /* jack IDs 1 and 5: cables 1 and 2 */
        } else if (d[1] == TUSB_DESC_CS_ENDPOINT && w->interfaces == 4) {
            CHECK_EQ(d[2], MIDI_CS_ENDPOINT_GENERAL);
            w->midi_ep_jacks[w->eps == 4 ? 0 : 1] = d[3];
        } else if (d[1] == TUSB_DESC_ENDPOINT) {
            uint8_t addr = d[2], num = addr & 0x0F, type = d[3] & 3;
            unsigned size = d[4] | d[5] << 8;
            w->eps++;
            CHECK(num >= 1 && num < TUP_DCD_ENDPOINT_MAX); /* the controller has 1-4 */
            CHECK(size <= 64);
            CHECK(type == TUSB_XFER_BULK || type == TUSB_XFER_INTERRUPT); /* docs/usb.md, item 8 */
            if (addr & 0x80)
                w->in_nums |= 1u << num;
            else
                w->out_nums |= 1u << num;
        }
        off += d[0];
    }
    CHECK_EQ(off, total);
}

static void test_configuration(void)
{
    const uint8_t *c = tud_descriptor_configuration_cb(0);
    struct walk w;
    CHECK_EQ(c[1], TUSB_DESC_CONFIGURATION);
    CHECK_EQ(c[4], 4);                         /* CDC control and data, MIDI control and streaming */
    walk_configuration(&w);
    CHECK_EQ(w.iads, 1u);
    CHECK_EQ(w.interfaces, 4u);
    CHECK_EQ(w.eps, 5u);
    CHECK_EQ(TUP_DCD_ENDPOINT_MAX, 5);         /* endpoints 0-4, all the controller has */
    /* docs/usb.md, item 5: CDC's on different numbers; only MIDI's pair shares endpoint 4 */
    CHECK_EQ(w.in_nums & w.out_nums, 1u << 4);
    CHECK_EQ(w.in_nums, (1u << 1) | (1u << 3) | (1u << 4)); /* 0x81, 0x83, 0x84 */
    CHECK_EQ(w.out_nums, (1u << 2) | (1u << 4));            /* 0x02, 0x04 */
    /* two MIDI ports (SPEC FR-IO-01): four jacks each, both carried by each endpoint */
    CHECK_EQ(w.jacks, 8u);
    CHECK_EQ(w.midi_ep_jacks[0], 2u);
    CHECK_EQ(w.midi_ep_jacks[1], 2u);
}

/* String descriptor `index` as ASCII; NULL when there is none. */
static const char *string(uint8_t index, char *buf, size_t size)
{
    const uint16_t *s = tud_descriptor_string_cb(index, 0x0409);
    if (!s)
        return NULL;
    unsigned len = s[0] & 0xFF, n = (len - 2) / 2;
    CHECK_EQ(s[0] >> 8, TUSB_DESC_STRING);
    for (unsigned i = 0; i < n && i + 1 < size; i++)
        buf[i] = (char)s[1 + i];
    buf[n < size ? n : size - 1] = 0;
    return buf;
}

static void test_strings(void)
{
    char buf[64];
    const uint16_t *lang = tud_descriptor_string_cb(0, 0);
    CHECK(lang && lang[0] == ((TUSB_DESC_STRING << 8) | 4) && lang[1] == 0x0409);
    const uint8_t *d = tud_descriptor_device_cb();
    const char *s = string(d[15], buf, sizeof buf);
    CHECK(s && strcmp(s, "SMC-Mixer") == 0);
    s = string(d[16], buf, sizeof buf); /* iSerialNumber: the controller's, not the hardware test's */
    CHECK(s && strcmp(s, "C1") == 0);
    usb_app_serial = "T1";
    s = string(d[16], buf, sizeof buf);
    CHECK(s && strcmp(s, "T1") == 0);
    usb_app_serial = "C1";
    usb_app_product = "SMC-Mixer hardware test"; /* usb_app_start() sets the variant's */
    s = string(d[15], buf, sizeof buf);
    CHECK(s && strcmp(s, "SMC-Mixer hardware test") == 0);
    usb_app_product = "a product name longer than thirty-two characters";
    s = string(d[15], buf, sizeof buf);
    CHECK(s && strlen(s) == 32);               /* cut, not overrun */
    const uint8_t *c = tud_descriptor_configuration_cb(0);
    s = string(c[9 + 8 + 8], buf, sizeof buf); /* after the IAD: the control interface's iInterface */
    CHECK(s && strcmp(s, "SMC-Mixer link") == 0);
    struct walk w;
    walk_configuration(&w);                    /* the MIDI port names: stable (SPEC FR-IO-01) */
    s = string(w.jack_string[0], buf, sizeof buf);
    CHECK(s && strcmp(s, "SMC-Mixer") == 0);
    s = string(w.jack_string[1], buf, sizeof buf);
    CHECK(s && strcmp(s, "SMC-Mixer Config") == 0);
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
