/*
 * Host tests for USB-MIDI packets (firmware/midi/usbmidi.c) and the packet queue
 * (firmware/midi/midiq.c). Run: make -C firmware test
 */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "midiq.h"
#include "usbmidi.h"

int check_failed;

static void test_pack(void)
{
    uint8_t pkt[4] = {0xEE, 0xEE, 0xEE, 0xEE};
    const uint8_t cc[3] = {0xB2, 20, 100}, pc[2] = {0xC0, 5}, at[2] = {0xD5, 64}, pb[3] = {0xEF, 0, 0x40};

    CHECK(usbmidi_pack(0, cc, 3, pkt));
    CHECK(memcmp(pkt, "\x0B\xB2\x14\x64", 4) == 0);
    CHECK(usbmidi_pack(1, pc, 2, pkt)); /* the config port; two bytes, the last one 0 */
    CHECK(memcmp(pkt, "\x1C\xC0\x05\x00", 4) == 0);
    CHECK(usbmidi_pack(0, at, 2, pkt));
    CHECK(memcmp(pkt, "\x0D\xD5\x40\x00", 4) == 0);
    CHECK(usbmidi_pack(0, pb, 3, pkt));
    CHECK(memcmp(pkt, "\x0E\xEF\x00\x40", 4) == 0);

    memset(pkt, 0xEE, 4);
    const uint8_t sysex[3] = {0xF0, 1, 2}, data[3] = {0x40, 1, 2}, bad_data[3] = {0x90, 0x80, 1};
    CHECK(!usbmidi_pack(0, cc, 2, pkt));     /* wrong length */
    CHECK(!usbmidi_pack(0, pc, 3, pkt));
    CHECK(!usbmidi_pack(0, sysex, 3, pkt));  /* not a channel message */
    CHECK(!usbmidi_pack(0, data, 3, pkt));
    CHECK(!usbmidi_pack(0, bad_data, 3, pkt));
    CHECK(!usbmidi_pack(16, cc, 3, pkt));
    CHECK(memcmp(pkt, "\xEE\xEE\xEE\xEE", 4) == 0); /* untouched */

    CHECK_EQ(usbmidi_channel_len(0x80), 3);
    CHECK_EQ(usbmidi_channel_len(0xCF), 2);
    CHECK_EQ(usbmidi_channel_len(0xF8), 0);
    CHECK_EQ(usbmidi_channel_len(0x7F), 0);
}

static void test_unpack(void)
{
    uint8_t msg[3];
    static const struct {
        uint8_t pkt[4];
        enum usbmidi_kind kind;
    } cases[] = {
        {{0x09, 0x90, 60, 100}, USBMIDI_CHANNEL},
        {{0x18, 0x81, 60, 0}, USBMIDI_CHANNEL}, /* cable 1 */
        {{0x0C, 0xC3, 7, 0}, USBMIDI_CHANNEL},
        {{0x09, 0x80, 60, 0}, USBMIDI_BAD},     /* the CIN does not match the status */
        {{0x09, 0x90, 0x80, 0}, USBMIDI_BAD},   /* a data byte with the top bit */
        {{0x0B, 0x40, 1, 2}, USBMIDI_BAD},      /* no status */
        {{0x04, 0xF0, 0x7D, 0x01}, USBMIDI_SYSEX},
        {{0x06, 0x02, 0xF7, 0x00}, USBMIDI_SYSEX},
        {{0x07, 0x01, 0x02, 0xF7}, USBMIDI_SYSEX},
        {{0x05, 0xF7, 0x00, 0x00}, USBMIDI_SYSEX},
        {{0x05, 0xF6, 0x00, 0x00}, USBMIDI_SYSTEM}, /* tune request */
        {{0x03, 0xF2, 0x10, 0x00}, USBMIDI_SYSTEM}, /* song position */
        {{0x02, 0xF3, 0x01, 0x00}, USBMIDI_SYSTEM},
        {{0x0F, 0xF8, 0x00, 0x00}, USBMIDI_REALTIME},
        {{0x0F, 0x40, 0x00, 0x00}, USBMIDI_BAD},
        {{0x00, 0x90, 60, 100}, USBMIDI_BAD},       /* reserved CINs */
        {{0x01, 0x90, 60, 100}, USBMIDI_BAD},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        memset(msg, 0xEE, sizeof msg);
        enum usbmidi_kind k = usbmidi_unpack(cases[i].pkt, msg);
        if (k != cases[i].kind)
            fprintf(stderr, "case %zu: kind %d, want %d\n", i, k, cases[i].kind);
        CHECK_EQ(k, cases[i].kind);
        if (k == USBMIDI_CHANNEL) {
            unsigned len = usbmidi_channel_len(cases[i].pkt[1]);
            CHECK(memcmp(msg, cases[i].pkt + 1, len) == 0);
            CHECK_EQ(msg[2], len == 3 ? cases[i].pkt[3] : 0);
        } else {
            CHECK_EQ(msg[0], 0xEE); /* untouched */
        }
    }
    CHECK_EQ(USBMIDI_CABLE(cases[1].pkt), 1);

    /* a round trip */
    uint8_t pkt[4];
    const uint8_t cc[3] = {0xB0, 102, 65};
    CHECK(usbmidi_pack(0, cc, 3, pkt));
    CHECK_EQ(usbmidi_unpack(pkt, msg), USBMIDI_CHANNEL);
    CHECK(memcmp(msg, cc, 3) == 0);
}

static void pkt_of(uint8_t pkt[4], unsigned n)
{
    pkt[0] = 0x0B;
    pkt[1] = 0xB0;
    pkt[2] = (uint8_t)(n & 0x7F);
    pkt[3] = (uint8_t)(n >> 7 & 0x7F);
}

static void test_queue(void)
{
    static struct midiq q;
    uint8_t pkt[4];
    midiq_init(&q);
    CHECK(midiq_peek(&q) == NULL);
    midiq_pop(&q); /* nothing to pop: harmless */
    CHECK_EQ(q.taken, 0);

    /* fill: one slot stays empty */
    for (unsigned i = 0; i < MIDIQ_SIZE - 1; i++) {
        pkt_of(pkt, i);
        CHECK(midiq_push(&q, pkt));
    }
    pkt_of(pkt, 999);
    CHECK(!midiq_push(&q, pkt));
    CHECK_EQ(q.refused, 1);
    CHECK_EQ(midiq_depth(&q), MIDIQ_SIZE - 1);
    CHECK_EQ(q.depth_max, MIDIQ_SIZE - 1);

    /* oldest first, across the wrap */
    for (unsigned i = 0; i < 10; i++) {
        const uint8_t *p = midiq_peek(&q);
        pkt_of(pkt, i);
        CHECK(p && memcmp(p, pkt, 4) == 0);
        midiq_pop(&q);
    }
    for (unsigned i = 0; i < 10; i++) {
        pkt_of(pkt, 100 + i);
        CHECK(midiq_push(&q, pkt));
    }
    for (unsigned i = 10; i < MIDIQ_SIZE - 1; i++) {
        pkt_of(pkt, i);
        CHECK(memcmp(midiq_peek(&q), pkt, 4) == 0);
        midiq_pop(&q);
    }
    pkt_of(pkt, 100);
    CHECK(memcmp(midiq_peek(&q), pkt, 4) == 0);
    CHECK_EQ(q.taken, MIDIQ_SIZE - 1);

    /* no host: everything thrown away, counted */
    CHECK_EQ(midiq_drop_all(&q), 10);
    CHECK_EQ(q.dropped, 10);
    CHECK(midiq_peek(&q) == NULL);
    CHECK_EQ(midiq_drop_all(&q), 0);
    CHECK_EQ(q.pushed, MIDIQ_SIZE - 1 + 10);
}

/* LOG-06: a packet carries its input event's time, or none; the producer's window maximum. */
static void test_queue_times(void)
{
    static struct midiq q;
    const uint8_t a[4] = {0x09, 0x90, 0x48, 0x7F}, b[4] = {0x0B, 0xB0, 0x14, 0x64};
    uint32_t t = 0;
    midiq_init(&q);
    CHECK(!midiq_peek_time(&q, &t)); /* empty */
    CHECK(midiq_push_at(&q, a, 123456));
    CHECK(midiq_push(&q, b));
    CHECK(midiq_push_at(&q, a, 0xFFFFFFF0u));
    CHECK_EQ(midiq_take_window_max(&q), 3);
    CHECK_EQ(midiq_take_window_max(&q), 3); /* a new window starts at the depth now */
    CHECK(midiq_peek_time(&q, &t));
    CHECK_EQ(t, 123456);
    midiq_pop(&q);
    CHECK(!midiq_peek_time(&q, &t)); /* no event's time */
    CHECK(midiq_peek(&q)[0] == 0x0B);
    midiq_pop(&q);
    CHECK(midiq_peek_time(&q, &t));
    CHECK_EQ(t, 0xFFFFFFF0u);
    midiq_pop(&q);
    CHECK_EQ(midiq_take_window_max(&q), 3);
    CHECK_EQ(midiq_take_window_max(&q), 0);
    CHECK_EQ(q.depth_max, 3); /* since start: kept */
}

/* Units, all or none; backpressure: the producer asks for room, the consumer says when it has it. */
static void test_queue_units(void)
{
    static struct midiq q;
    uint8_t u[4][4];
    uint32_t t = 0;
    midiq_init(&q);
    for (unsigned i = 0; i < 4; i++)
        pkt_of(u[i], 200 + i);
    CHECK(midiq_push_unit(&q, (const uint8_t(*)[4])u, 0, 0, false)); /* nothing: fine */
    CHECK_EQ(q.pushed, 0);
    for (unsigned i = 0; i < MIDIQ_SIZE - 4; i++) { /* 3 left */
        uint8_t p[4];
        pkt_of(p, i);
        CHECK(midiq_push(&q, p));
    }
    CHECK_EQ(midiq_room(&q), 3);
    CHECK(!midiq_push_unit(&q, (const uint8_t(*)[4])u, 4, 77, true)); /* does not fit: none */
    CHECK_EQ(q.refused, 4);
    CHECK_EQ(midiq_depth(&q), MIDIQ_SIZE - 4);
    for (unsigned i = 0; i < MIDIQ_SIZE - 4; i++)
        midiq_pop(&q);
    CHECK(midiq_push_unit(&q, (const uint8_t(*)[4])u, 4, 77, true)); /* across the wrap */
    CHECK_EQ(midiq_depth(&q), 4);
    for (unsigned i = 0; i < 4; i++) {
        uint8_t p[4];
        pkt_of(p, 200 + i);
        CHECK(memcmp(midiq_peek(&q), p, 4) == 0);
        CHECK(midiq_peek_time(&q, &t));
        CHECK_EQ(t, 77); /* every packet of the unit carries its event's time */
        midiq_pop(&q);
    }

    /* backpressure */
    CHECK(!midiq_room_due(&q)); /* nobody asked */
    for (unsigned i = 0; i < 10; i++) {
        uint8_t p[4];
        pkt_of(p, i);
        CHECK(midiq_push(&q, p));
    }
    midiq_want_room(&q, 8);
    CHECK(!midiq_room_due(&q)); /* 10 wait */
    midiq_pop(&q);
    CHECK(!midiq_room_due(&q)); /* 9 */
    midiq_pop(&q);
    CHECK(midiq_room_due(&q)); /* 8: once */
    CHECK(!midiq_room_due(&q));
    midiq_want_room(&q, 8);
    CHECK_EQ(midiq_drop_all(&q), 8);
    CHECK(midiq_room_due(&q)); /* dropped (no host): room all the same */
}

static void test_sysex_packets(void)
{
    /* every length of a SysEx through usbmidi_sysex_pack() and back through usbmidi_packet_bytes() */
    for (unsigned n = 2; n < 40; n++) {
        uint8_t msg[40], back[48], pkt[4];
        unsigned pos = 0, got = 0, packets = 0;
        msg[0] = 0xF0;
        for (unsigned i = 1; i + 1 < n; i++)
            msg[i] = (uint8_t)(i * 13 & 0x7F);
        msg[n - 1] = 0xF7;
        while (pos < n) {
            unsigned k = usbmidi_sysex_pack(USBMIDI_CABLE_CONFIG, msg + pos, n - pos, pkt);
            CHECK(k >= 1 && k <= 3);
            if (!k)
                break;
            CHECK_EQ(USBMIDI_CABLE(pkt), USBMIDI_CABLE_CONFIG);
            unsigned cin = USBMIDI_CIN(pkt);
            CHECK_EQ(cin, pos + k == n ? 4u + k : 4u);
            uint8_t m[3];
            CHECK_EQ(usbmidi_unpack(pkt, m), USBMIDI_SYSEX);
            CHECK_EQ(usbmidi_packet_bytes(pkt, back + got), k);
            got += k;
            pos += k;
            packets++;
        }
        CHECK_EQ(got, n);
        CHECK(memcmp(back, msg, n) == 0);
        CHECK_EQ(packets, (n + 2) / 3);
    }
    /* the rest of a message without its end yet: three bytes or nothing */
    const uint8_t part[] = {0xF0, 0x7D, 0x53, 0x4D};
    uint8_t pkt[4];
    CHECK_EQ(usbmidi_sysex_pack(1, part, 2, pkt), 0);
    CHECK_EQ(usbmidi_sysex_pack(1, part, 0, pkt), 0);
    CHECK_EQ(usbmidi_sysex_pack(1, part, 4, pkt), 3);
    CHECK_EQ(pkt[0], 0x14);
    /* other packets' bytes */
    const uint8_t note[4] = {0x09, 0x90, 0x40, 0x7F}, pc[4] = {0x0C, 0xC1, 0x05, 0}, clk[4] = {0x0F, 0xF8, 0, 0};
    uint8_t b[3];
    CHECK(usbmidi_packet_bytes(note, b) == 3 && b[0] == 0x90 && b[2] == 0x7F);
    CHECK(usbmidi_packet_bytes(pc, b) == 2 && b[1] == 0x05);
    CHECK(usbmidi_packet_bytes(clk, b) == 1 && b[0] == 0xF8);
    const uint8_t rsv[4] = {0x01, 0x90, 0x40, 0x7F};
    CHECK_EQ(usbmidi_packet_bytes(rsv, b), 0);
}

int main(void)
{
    RUN(test_pack);
    RUN(test_unpack);
    RUN(test_sysex_packets);
    RUN(test_queue);
    RUN(test_queue_times);
    RUN(test_queue_units);
    printf("test_midi: %s\n", check_failed ? "FAILED" : "all ok");
    return check_failed ? 1 : 0;
}
