/* The configuration's binary form (firmware/config/cfgbin.c; docs/config.md). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfgbin.h"
#include "check.h"

int check_failed;

static struct core_config a, b;
static uint8_t buf[CFGBIN_MAX], buf2[CFGBIN_MAX];

/* Encode, decode, compare: the same configuration and the same bytes again. */
static size_t round_trip(const struct core_config *cfg)
{
    struct cfgbin_error e;
    struct cfgbin_meta meta = {{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}, 42}, got;
    size_t n = cfgbin_encode(cfg, &meta, buf, sizeof buf, &e);
    CHECK(n > CFGBIN_HEADER);
    CHECK_EQ(e.code, CFGBIN_OK);
    CHECK_EQ(cfgbin_decode(buf, n, NULL, NULL, &e), CFGBIN_OK);
    memset(&b, 0x55, sizeof b);
    CHECK_EQ(cfgbin_decode(buf, n, &b, &got, &e), CFGBIN_OK);
    CHECK(!memcmp(cfg, &b, sizeof b));
    CHECK(!memcmp(&meta, &got, sizeof got));
    CHECK_EQ(cfgbin_encode(&b, &got, buf2, sizeof buf2, &e), n);
    CHECK(!memcmp(buf, buf2, n));
    return n;
}

static void test_crc(void)
{
    CHECK_EQ(cfgbin_crc32(0, (const uint8_t *)"123456789", 9), 0xCBF43926u);
    CHECK_EQ(cfgbin_crc32(cfgbin_crc32(0, (const uint8_t *)"1234", 4), (const uint8_t *)"56789", 5), 0xCBF43926u);
}

static void test_builtin(void)
{
    struct cfgbin_error e;
    core_config_factory(&a);
    CHECK_EQ(cfgbin_check(&a, &e), CFGBIN_OK);
    size_t n = round_trip(&a);
    printf("     factory configuration: %u bytes (the structure: %u)\n", (unsigned)n, (unsigned)sizeof a);
    CHECK(n < 8000);
    core_config_test(&a);
    CHECK_EQ(cfgbin_check(&a, &e), CFGBIN_OK);
    round_trip(&a);
    memset(&a, 0, sizeof a); /* not valid: the gesture time is 0 */
    CHECK_EQ(cfgbin_check(&a, &e), CFGBIN_E_VALUE);
    CHECK_EQ(cfgbin_encode(&a, NULL, buf, sizeof buf, &e), 0);
}

/* Every control with all its fields set and a Shift layer that differs in all of them. */
static void worst(struct core_config *cfg)
{
    core_config_factory(cfg);
    cfg->global.layer_mode = CORE_LAYER_IMMEDIATE;
    cfg->global.pickup = CORE_SCALE;
    cfg->global.gesture_ms = 1000;
    cfg->global.hybrid_ms = 1000;
    cfg->global.enc_accel = 200;
    cfg->global.fine14 = CORE_FINE14_LSB;
    for (unsigned p = 0; p < CORE_PRESETS; p++) {
        struct core_preset *pr = &cfg->preset[p];
        pr->channel = 2;
        pr->shift_mode = CORE_SHIFT_LATCH;
        pr->note_off = CORE_NOTE_OFF_8N;
        pr->param_null = CORE_PARAM_NULL_GESTURE;
        for (unsigned pg = 0; pg < CORE_PAGES; pg++)
            for (unsigned n = 0; n < CORE_STRIPS; n++) {
                struct core_strip *s = &pr->page[pg][n];
                struct core_assign f = {CORE_MSG_CC14, (uint8_t)n, 5, CORE_SCALE, 0, 0, 10, 16000, 0};
                struct core_assign g = {CORE_MSG_CC, (uint8_t)(n + 1), 6, CORE_JUMP, 0, 0, 1, 100, 0};
                s->fader = (struct core_control){f, g, CORE_SHIFT_OVERRIDE};
                struct core_assign en = {CORE_MSG_CC14, (uint8_t)n, 7, CORE_DIV, 4, 0, 5, 9000, 300};
                struct core_assign es = {CORE_MSG_PB, (uint8_t)(n + 2), 0, CORE_STEP, 3, 0, 6, 9001, 301};
                es.number = 0;
                s->encoder = (struct core_control){en, es, CORE_SHIFT_OVERRIDE};
                for (unsigned i = 0; i < 4; i++) {
                    struct core_assign bn = {CORE_MSG_CC, (uint8_t)(n + i), (uint8_t)(i + 1), CORE_TOGGLE, 0,
                                             CORE_F_NO_RELEASE | CORE_F_LED_ON, 3, 120, 0};
                    struct core_assign bs = {CORE_MSG_NOTE, (uint8_t)(n + i + 1), (uint8_t)(i + 2), CORE_TRIGGER, 0,
                                             CORE_F_LED_OFF, 4, 121, 0};
                    s->button[i] = (struct core_control){bn, bs, CORE_SHIFT_OVERRIDE};
                }
            }
        for (unsigned i = 0; i < CORE_ROW_BUTTONS; i++) {
            struct core_assign bn = {CORE_MSG_CC, (uint8_t)i, (uint8_t)i, CORE_TOGGLE, 0, CORE_F_NO_RELEASE, 3, 120, 0};
            struct core_assign bs = {CORE_MSG_NOTE, (uint8_t)(i + 1), (uint8_t)(i + 1), CORE_TRIGGER, 0, CORE_F_LED_OFF,
                                     4, 121, 0};
            bool nav = i >= 3;
            pr->row[i] = (struct core_control){bn, nav ? (struct core_assign){0} : bs,
                                               nav ? CORE_SHIFT_INHERIT : CORE_SHIFT_OVERRIDE};
        }
    }
}

static void test_largest(void)
{
    struct cfgbin_error e;
    worst(&a);
    CHECK_EQ(cfgbin_check(&a, &e), CFGBIN_OK);
    size_t n = round_trip(&a);
    printf("     a configuration with everything set: %u bytes (at most %u)\n", (unsigned)n, (unsigned)CFGBIN_MAX);
    CHECK(n <= CFGBIN_MAX);
    CHECK_EQ(cfgbin_encode(&a, NULL, buf, n - 1, &e), 0);
    CHECK_EQ(e.code, CFGBIN_E_SPACE);
    CHECK_EQ(cfgbin_encode(&a, NULL, buf, 10, &e), 0);
}

/* A header around records, with the CRC right. */
static size_t blob(uint8_t *out, const uint8_t *records, size_t n)
{
    memset(out, 0, CFGBIN_HEADER);
    memcpy(out, "SMCF", 4);
    out[4] = 1;
    out[5] = CFGBIN_HEADER;
    out[8] = (uint8_t)n;
    out[9] = (uint8_t)(n >> 8);
    memcpy(out + CFGBIN_HEADER, records, n);
    uint32_t crc = cfgbin_crc32(0, out + 16, CFGBIN_HEADER - 16 + n);
    for (unsigned i = 0; i < 4; i++)
        out[12 + i] = (uint8_t)(crc >> (8 * i));
    return CFGBIN_HEADER + n;
}

static int decode_records(const uint8_t *records, size_t n, struct cfgbin_error *e)
{
    size_t len = blob(buf, records, n);
    return cfgbin_decode(buf, len, &a, NULL, e);
}

#define R(...) ((const uint8_t[]){__VA_ARGS__}), sizeof((const uint8_t[]){__VA_ARGS__})

static void test_records(void)
{
    struct cfgbin_error e;
    /* nothing: every preset empty, the global defaults */
    CHECK_EQ(decode_records(NULL, 0, &e), CFGBIN_OK);
    CHECK_EQ(a.global.gesture_ms, 300);
    CHECK_EQ(a.global.pickup, CORE_PICKUP);
    CHECK_EQ(a.preset[3].channel, 0);
    /* preset 2 on channel 3: fader 1 of page 1 CC 7 on the preset's channel */
    CHECK_EQ(decode_records(R(2, 3, 1, 1, 2, 3, 7, 0, 0, 0x05, 0x00, 1, 7, 0), &e), CFGBIN_OK);
    const struct core_assign *f = &a.preset[1].page[0][0].fader.normal;
    CHECK_EQ(a.preset[1].channel, 2);
    CHECK_EQ(f->msg, CORE_MSG_CC);
    CHECK_EQ(f->channel, CORE_CHANNEL_PRESET);
    CHECK_EQ(core_channel(&a, 1, f), 2);
    CHECK_EQ(f->number, 7);
    CHECK_EQ(f->hi, 127);
    CHECK_EQ(a.preset[1].page[0][0].fader.shift_kind, CORE_SHIFT_INHERIT);
    /* an encoder gets fine mode as its Shift layer */
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 3, 5, 0, 8, 0x01, 0x00, 1), &e), CFGBIN_OK);
    const struct core_control *enc = &a.preset[0].page[0][0].encoder;
    CHECK_EQ(enc->normal.mode, CORE_ACC2);
    CHECK_EQ(enc->normal.start, 64); /* the middle of 0..127 */
    CHECK_EQ(enc->shift_kind, CORE_SHIFT_OVERRIDE);
    CHECK_EQ(enc->shift.mode, CORE_FINE);
    CHECK_EQ(enc->shift.msg, CORE_MSG_CC);
    CHECK_EQ(enc->shift.channel, CORE_CHANNEL_PRESET);
    CHECK_EQ(enc->shift.start, 64);
    /* the start between lo 10 and hi 20 */
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 3, 9, 0, 8, 0xC1, 0x00, 1, 10, 0, 20, 0), &e), CFGBIN_OK);
    CHECK_EQ(enc->normal.start, 15);
    /* a fixed channel: 16 on the device is the preset's, 17 nothing */
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 3, 6, 0, 0, 0x03, 0x00, 1, 15), &e), CFGBIN_OK);
    CHECK_EQ(a.preset[0].page[0][0].fader.normal.channel, 15);
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 3, 6, 0, 0, 0x03, 0x00, 1, 16), &e), CFGBIN_OK);
    CHECK_EQ(a.preset[0].page[0][0].fader.normal.channel, CORE_CHANNEL_PRESET);
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 3, 6, 0, 0, 0x03, 0x00, 1, 17), &e), CFGBIN_E_VALUE);
    CHECK_EQ(e.field, 1);
    CHECK_EQ(decode_records(R(2, 3, 0, 1, 16), &e), CFGBIN_E_VALUE); /* a preset on channel 17 */
    /* div and step need their n */
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 3, 6, 0, 8, 0x09, 0x00, 1, 4), &e), CFGBIN_E_VALUE);
    CHECK_EQ(e.field, 4);
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 3, 7, 0, 8, 0x19, 0x00, 1, 4, 3), &e), CFGBIN_OK);
    CHECK_EQ(enc->normal.mode, CORE_DIV);
    CHECK_EQ(enc->normal.param, 3);
    CHECK_EQ(enc->shift.mode, CORE_FINE);
    CHECK_EQ(enc->shift.param, 0);
    /* an unknown optional record is skipped, an unknown other one refused */
    CHECK_EQ(decode_records(R(0x85, 2, 9, 9), &e), CFGBIN_OK);
    CHECK_EQ(decode_records(R(0x05, 2, 9, 9), &e), CFGBIN_E_TAG);
    CHECK_EQ(e.offset, CFGBIN_HEADER);
    /* faults, each with its code */
    CHECK_EQ(decode_records(R(1, 2, 0x02, 0), &e), CFGBIN_E_VALUE); /* pickup "global" globally */
    CHECK_EQ(e.field, 1);
    CHECK_EQ(decode_records(R(1, 1, 0x80), &e), CFGBIN_E_FIELD); /* a bit past the global fields */
    CHECK_EQ(decode_records(R(1, 3, 0x01, 1, 0), &e), CFGBIN_E_RECORD); /* a byte too many */
    CHECK_EQ(decode_records(R(1, 1, 0, 1, 1, 0), &e), CFGBIN_E_TWICE);
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 2, 2, 0, 0), &e), CFGBIN_E_TWICE); /* preset 1 twice */
    CHECK_EQ(e.preset, 0);
    CHECK_EQ(decode_records(R(3, 4, 0, 0, 0x01, 0x00), &e), CFGBIN_E_ORDER); /* no preset record */
    CHECK_EQ(decode_records(R(2, 2, 4, 0), &e), CFGBIN_E_WHERE); /* preset 5 */
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 3, 4, 4, 0, 0x01, 0x00), &e), CFGBIN_E_WHERE); /* page 5 */
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 3, 4, 255, 11, 0x01, 0x00), &e), CFGBIN_E_WHERE); /* row button 12 */
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 3, 5, 0, 8, 0x01, 0x00, 4), &e), CFGBIN_E_TYPE); /* a note encoder */
    CHECK_EQ(e.page, 0);
    CHECK_EQ(e.control, 8);
    CHECK_EQ(e.field, 0);
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 3, 7, 0, 0, 0x05, 0x00, 2, 40, 0), &e), CFGBIN_E_VALUE); /* cc14 number 40 */
    CHECK_EQ(e.field, 2);
    /* NRPN and RPN: a 14-bit parameter number, u16 like every number */
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 3, 7, 0, 0, 0x05, 0x00, 10, 0x34, 0x12), &e), CFGBIN_OK);
    CHECK_EQ(a.preset[0].page[0][0].fader.normal.msg, CORE_MSG_NRPN);
    CHECK_EQ(a.preset[0].page[0][0].fader.normal.number, 0x1234);
    CHECK_EQ(a.preset[0].page[0][0].fader.normal.hi, 16383);
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 3, 7, 0, 0, 0x05, 0x00, 11, 0xFF, 0x3F), &e), CFGBIN_OK); /* rpn 16383 */
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 3, 7, 0, 0, 0x05, 0x00, 10, 0x00, 0x40), &e), CFGBIN_E_VALUE); /* 16384 */
    CHECK_EQ(e.field, 2);
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 3, 7, 0, 0, 0x05, 0x00, 1, 128, 0), &e), CFGBIN_E_VALUE); /* cc7 128 */
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 3, 7, 0, 16, 0x05, 0x00, 10, 1, 0), &e), CFGBIN_OK); /* an NRPN button */
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 3, 5, 0, 16, 0x01, 0x00, 11), &e), CFGBIN_E_TYPE); /* an RPN button */
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 3, 6, 0, 0, 0x11, 0x00, 1, 3), &e), CFGBIN_E_VALUE); /* n on a fader */
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 3, 6, 0, 0, 0x21, 0x00, 1, 6), &e), CFGBIN_E_VALUE); /* led on and off */
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 3, 6, 0, 16, 0x21, 0x00, 1, 6), &e), CFGBIN_E_VALUE);
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 3, 6, 0, 16, 0x21, 0x00, 1, 4), &e), CFGBIN_OK); /* a button: led on */
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 3, 4, 0, 0, 0x00, 0x02), &e), CFGBIN_E_FIELD);
    /* the Shift layer: a navigation button's, an override of nothing, apart from its Normal one */
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 4, 3, 255, 7, 2), &e), CFGBIN_E_SHIFT);
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 4, 3, 255, 0, 2), &e), CFGBIN_OK); /* play: block */
    CHECK_EQ(a.preset[0].row[0].shift_kind, CORE_SHIFT_BLOCK);
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 4, 5, 0, 0, 1, 0x00, 0x00), &e), CFGBIN_E_SHIFT);
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 3, 5, 0, 0, 0x01, 0x00, 1, 4, 3, 0, 1, 2), &e),
             CFGBIN_OK); /* fader 1, then a block of fader 2 */
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 3, 5, 0, 0, 0x01, 0x00, 1, 3, 5, 0, 1, 0x01, 0x00, 1, 4, 3, 0, 0, 2), &e),
             CFGBIN_E_ORDER);
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 4, 3, 0, 0, 2, 3, 5, 0, 0, 0x01, 0x00, 1), &e), CFGBIN_E_ORDER);
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 4, 3, 0, 0, 3), &e), CFGBIN_E_VALUE); /* Shift kind 3 */
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 4, 4, 0, 0, 2, 0), &e), CFGBIN_E_RECORD); /* a byte after block */
    /* an override over the Normal layer: CC 7 → the same with hi 64 */
    CHECK_EQ(decode_records(R(2, 2, 0, 0, 3, 7, 0, 0, 0x05, 0x00, 1, 7, 0, 4, 7, 0, 0, 1, 0x80, 0x00, 64, 0), &e),
             CFGBIN_OK);
    CHECK_EQ(a.preset[0].page[0][0].fader.shift.number, 7);
    CHECK_EQ(a.preset[0].page[0][0].fader.shift.hi, 64);
}

static void test_header(void)
{
    struct cfgbin_error e;
    core_config_factory(&a);
    size_t n = cfgbin_encode(&a, NULL, buf, sizeof buf, &e);
    CHECK_EQ(cfgbin_decode(buf, CFGBIN_HEADER - 1, NULL, NULL, &e), CFGBIN_E_SHORT);
    CHECK_EQ(cfgbin_decode(buf, n - 1, NULL, NULL, &e), CFGBIN_E_LENGTH);
    buf[n - 1] ^= 1;
    CHECK_EQ(cfgbin_decode(buf, n, NULL, NULL, &e), CFGBIN_E_CRC);
    buf[n - 1] ^= 1;
    buf[4] = 2;
    CHECK_EQ(cfgbin_decode(buf, n, NULL, NULL, &e), CFGBIN_E_VERSION);
    buf[4] = 1;
    buf[0] = 'X';
    CHECK_EQ(cfgbin_decode(buf, n, NULL, NULL, &e), CFGBIN_E_MAGIC);
    buf[0] = 'S';
    CHECK_EQ(cfgbin_decode(buf, n, NULL, NULL, &e), CFGBIN_OK);
    /* A header that says it is longer than the data, with a body length that makes the sum the
     * data's length modulo 2^32 (the CRC right): refused, on a 32-bit device too. */
    uint8_t h[CFGBIN_HEADER + 4];
    memcpy(h, buf, CFGBIN_HEADER);
    h[5] = 200;
    uint32_t body = (uint32_t)sizeof h - 200u;
    for (unsigned i = 0; i < 4; i++)
        h[8 + i] = (uint8_t)(body >> 8 * i);
    uint32_t crc = cfgbin_crc32(0, h + 16, sizeof h - 16);
    for (unsigned i = 0; i < 4; i++)
        h[12 + i] = (uint8_t)(crc >> 8 * i);
    CHECK_EQ(cfgbin_decode(h, sizeof h, NULL, NULL, &e), CFGBIN_E_LENGTH);
    /* a longer header from a later version is read, its extra bytes skipped */
    memcpy(h, buf, CFGBIN_HEADER);
    h[5] = sizeof h;
    memset(h + 8, 0, 4);
    memset(h + CFGBIN_HEADER, 0xEE, 4);
    crc = cfgbin_crc32(0, h + 16, sizeof h - 16);
    for (unsigned i = 0; i < 4; i++)
        h[12 + i] = (uint8_t)(crc >> 8 * i);
    CHECK_EQ(cfgbin_decode(h, sizeof h, NULL, NULL, &e), CFGBIN_OK);
    CHECK(!strcmp(cfgbin_error_name(CFGBIN_E_SHIFT), "shift"));
    CHECK(!strcmp(cfgbin_error_name(99), "?"));
}

/* Random damage to a valid form (the CRC made right): never a crash; what decodes is valid and
 * encodes to a form that decodes to the same. */
static void test_damage(void)
{
    struct cfgbin_error e;
    uint32_t seed = 12345;
    unsigned accepted = 0, rounds = 20000;
    worst(&a);
    size_t n0 = cfgbin_encode(&a, NULL, buf2, sizeof buf2, &e);
    static uint8_t d[CFGBIN_MAX + 16];
    for (unsigned r = 0; r < rounds; r++) {
        size_t n = n0;
        memcpy(d, buf2, n);
        unsigned hits = 1 + r % 4;
        for (unsigned h = 0; h < hits; h++) {
            seed = seed * 1103515245u + 12345u;
            size_t at = CFGBIN_HEADER + (seed >> 8) % (n - CFGBIN_HEADER);
            seed = seed * 1103515245u + 12345u;
            d[at] = (uint8_t)(seed >> 16);
        }
        if (r % 7 == 0) { /* cut the end off */
            seed = seed * 1103515245u + 12345u;
            n = CFGBIN_HEADER + (seed >> 8) % (n - CFGBIN_HEADER);
            d[8] = (uint8_t)(n - CFGBIN_HEADER);
            d[9] = (uint8_t)((n - CFGBIN_HEADER) >> 8);
        }
        uint32_t crc = cfgbin_crc32(0, d + 16, n - 16);
        for (unsigned i = 0; i < 4; i++)
            d[12 + i] = (uint8_t)(crc >> (8 * i));
        int check_only = cfgbin_decode(d, n, NULL, NULL, &e);
        int full = cfgbin_decode(d, n, &b, NULL, &e);
        CHECK_EQ(check_only, full);
        if (full)
            continue;
        accepted++;
        CHECK_EQ(cfgbin_check(&b, &e), CFGBIN_OK);
        size_t m = cfgbin_encode(&b, NULL, buf, sizeof buf, &e);
        CHECK(m > 0);
        static struct core_config c2;
        CHECK_EQ(cfgbin_decode(buf, m, &c2, NULL, &e), CFGBIN_OK);
        CHECK(!memcmp(&b, &c2, sizeof c2));
    }
    printf("     %u of %u damaged forms decoded, all valid\n", accepted, rounds);
    CHECK(accepted > 0);
}

/* --- live edits (cfgbin_patch(), CFG_SET) ---------------------------------------------------- */

#define SETTINGS 254 /* records_of(): the preset's record */
#define GLOBAL 253   /* records_of(): the global record */

/* The records of a canonical form for one preset's control (page, id), its settings or the global
 * settings, appended to out; the bytes appended. */
static size_t records_of(const uint8_t *form, size_t n, unsigned preset, unsigned page, unsigned id, uint8_t *out)
{
    size_t k = 0;
    int cur = -1;
    for (size_t off = CFGBIN_HEADER; off < n; off += 2u + form[off + 1]) {
        unsigned tag = form[off];
        const uint8_t *p = form + off + 2;
        if (tag == 2)
            cur = p[0];
        bool take = page == GLOBAL ? tag == 1
                    : cur == (int)preset && (page == SETTINGS ? tag == 2 : (tag == 3 || tag == 4) && p[0] == page && p[1] == id);
        if (take) {
            memcpy(out + k, form + off, 2u + form[off + 1]);
            k += 2u + form[off + 1];
        }
    }
    return k;
}

static unsigned changing_calls, changing_settings;
static void count_changing(void *ctx, const struct core_control *old)
{
    (void)ctx;
    changing_calls++;
    if (!old)
        changing_settings++;
}

static int patch(unsigned preset, const uint8_t *rec, size_t n, struct core_config *cfg)
{
    struct cfgbin_error e;
    int r = cfgbin_patch(rec, n, preset, NULL, NULL, NULL, &e);
    if (r == CFGBIN_OK && cfg)
        CHECK_EQ(cfgbin_patch(rec, n, preset, cfg, count_changing, NULL, &e), CFGBIN_OK);
    return r;
}

/* Every control and setting of the factory configuration patched into the test one, one by one,
 * makes them equal: a patch means what a complete form means. */
static void test_patch(void)
{
    static uint8_t form[CFGBIN_MAX];
    uint8_t rec[200];
    struct cfgbin_error e;
    size_t n, k;

    core_config_test(&a);
    core_config_factory(&b);
    b.global.save = CORE_SAVE_AUTO;
    b.global.enc_accel = 150;
    b.preset[2].page[1][3].fader.shift_kind = CORE_SHIFT_BLOCK;
    CHECK_EQ(cfgbin_check(&b, &e), CFGBIN_OK);
    n = cfgbin_encode(&b, NULL, form, sizeof form, &e);
    CHECK(n > CFGBIN_HEADER);
    changing_calls = changing_settings = 0;
    unsigned controls = 0;
    for (unsigned pr = 0; pr < CORE_PRESETS; pr++) {
        for (unsigned page = 0; page < CORE_PAGES + 1; page++) {
            unsigned pg = page < CORE_PAGES ? page : 255, count = page < CORE_PAGES ? 48 : CORE_ROW_BUTTONS;
            for (unsigned id = 0; id < count; id++) {
                k = records_of(form, n, pr, pg, id, rec);
                if (!k) { /* nothing written for it: a Normal layer of no fields, none */
                    const uint8_t none[] = {3, 4, (uint8_t)pg, (uint8_t)id, 0, 0};
                    memcpy(rec, none, k = sizeof none);
                }
                CHECK_EQ(patch(pr, rec, k, &a), CFGBIN_OK);
                controls++;
            }
        }
        k = records_of(form, n, pr, SETTINGS, 0, rec);
        if (!k) {
            const uint8_t none[] = {2, 2, (uint8_t)pr, 0};
            memcpy(rec, none, k = sizeof none);
        }
        CHECK_EQ(patch(pr, rec, k, &a), CFGBIN_OK);
    }
    k = records_of(form, n, 0, GLOBAL, 0, rec);
    CHECK(k > 0);
    CHECK_EQ(patch(3, rec, k, &a), CFGBIN_OK);
    CHECK(!memcmp(&a, &b, sizeof a));
    CHECK_EQ(changing_calls, controls + CORE_PRESETS); /* each control once, each preset's settings */
    CHECK_EQ(changing_settings, CORE_PRESETS);

    /* a control named is replaced whole: its Shift layer back to the default with a Normal layer
     * alone, its Normal layer none with a Shift layer alone */
    core_config_test(&a);
    CHECK(a.preset[0].page[0][0].encoder.normal.msg != CORE_MSG_NONE);
    const uint8_t enc_cc[] = {3, 5, 0, 8, 0x01, 0x00, 1};
    b = a;
    changing_calls = changing_settings = 0;
    CHECK_EQ(cfgbin_patch_visit(enc_cc, sizeof enc_cc, 0, &a, count_changing, NULL, &e), CFGBIN_OK);
    CHECK_EQ(changing_calls, 1);
    CHECK_EQ(changing_settings, 0);
    CHECK(!memcmp(&a, &b, sizeof a));
    CHECK_EQ(patch(0, enc_cc, sizeof enc_cc, &a), CFGBIN_OK);
    CHECK_EQ(a.preset[0].page[0][0].encoder.shift_kind, CORE_SHIFT_OVERRIDE); /* fine mode */
    CHECK_EQ(a.preset[0].page[0][0].encoder.shift.mode, CORE_FINE);
    const uint8_t block[] = {4, 3, 0, 8, 2};
    CHECK_EQ(patch(0, block, sizeof block, &a), CFGBIN_OK);
    CHECK_EQ(a.preset[0].page[0][0].encoder.normal.msg, CORE_MSG_NONE);
    CHECK_EQ(a.preset[0].page[0][0].encoder.shift_kind, CORE_SHIFT_BLOCK);
    /* the other presets and controls untouched */
    core_config_test(&b);
    b.preset[0].page[0][0].encoder = a.preset[0].page[0][0].encoder;
    CHECK(!memcmp(&a, &b, sizeof a));

    /* refused, and nothing applied when only checked */
    core_config_test(&a);
    CHECK_EQ(cfgbin_patch(enc_cc, sizeof enc_cc, 4, NULL, NULL, NULL, &e), CFGBIN_E_WHERE); /* preset 5 */
    const uint8_t other[] = {2, 2, 1, 0};
    CHECK_EQ(patch(0, other, sizeof other, NULL), CFGBIN_E_WHERE); /* another preset's settings */
    const uint8_t twice[] = {3, 5, 0, 8, 0x01, 0x00, 1, 3, 5, 0, 8, 0x01, 0x00, 1};
    CHECK_EQ(patch(0, twice, sizeof twice, NULL), CFGBIN_E_TWICE);
    const uint8_t order[] = {4, 3, 0, 8, 2, 3, 5, 0, 8, 0x01, 0x00, 1};
    CHECK_EQ(patch(0, order, sizeof order, NULL), CFGBIN_E_ORDER);
    const uint8_t cut[] = {3, 5, 0, 8, 0x01, 0x00};
    CHECK_EQ(patch(0, cut, sizeof cut, NULL), CFGBIN_E_RECORD);
    changing_calls = 0;
    CHECK_EQ(cfgbin_patch_visit(cut, sizeof cut, 0, &a, count_changing, NULL, &e), CFGBIN_E_RECORD);
    CHECK_EQ(changing_calls, 0);
    const uint8_t settings_rec[] = {2, 2, 0, 0};
    changing_settings = 0;
    CHECK_EQ(cfgbin_patch_visit(settings_rec, sizeof settings_rec, 0, &a, count_changing, NULL, &e), CFGBIN_OK);
    CHECK_EQ(changing_calls, 1);
    CHECK_EQ(changing_settings, 1);
    CHECK_EQ(cfgbin_patch(cut, sizeof cut, 0, NULL, NULL, NULL, &e), CFGBIN_E_RECORD);
    CHECK_EQ(e.offset, 0);
    const uint8_t later[] = {0x85, 2, 9, 9, 3, 5, 0, 8, 0x01, 0x00, 1}; /* optional data: skipped */
    CHECK_EQ(patch(0, later, sizeof later, NULL), CFGBIN_OK);
    core_config_test(&b);
    CHECK(!memcmp(&a, &b, sizeof a));
}

int main(void)
{
    RUN(test_crc);
    RUN(test_builtin);
    RUN(test_largest);
    RUN(test_records);
    RUN(test_header);
    RUN(test_damage);
    RUN(test_patch);
    if (check_failed) {
        printf("test_cfgbin: %d checks failed\n", check_failed);
        return 1;
    }
    printf("test_cfgbin: all ok\n");
    return 0;
}
