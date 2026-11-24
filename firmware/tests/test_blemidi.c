/*
 * Host tests for BLE-MIDI packets (firmware/midi/blemidi.c). Run: make -C firmware test
 */
#include <stdio.h>
#include <string.h>

#include "blemidi.h"
#include "check.h"

int check_failed;

/* --- a sink that records what the decoder passes on --- */

#define REC_MAX 512
struct rec {
    unsigned n;
    struct {
        uint16_t ts;
        uint8_t m[3];
        uint8_t len;
    } msg[REC_MAX];
    uint8_t sysex[4096];
    unsigned sysex_len, sysex_calls;
};

static void rec_msg(void *ctx, uint16_t ts, const uint8_t *m, unsigned len)
{
    struct rec *r = ctx;
    if (r->n < REC_MAX) {
        r->msg[r->n].ts = ts;
        r->msg[r->n].len = (uint8_t)len;
        memcpy(r->msg[r->n].m, m, len);
        r->n++;
    }
}

static void rec_sysex(void *ctx, const uint8_t *b, unsigned len)
{
    struct rec *r = ctx;
    if (r->sysex_len + len <= sizeof r->sysex) {
        memcpy(r->sysex + r->sysex_len, b, len);
        r->sysex_len += len;
    }
    r->sysex_calls++;
}

static struct rec rec;
static const struct blemidi_sink sink = {rec_msg, rec_sysex, &rec};

static unsigned decode(struct blemidi_rx *rx, const uint8_t *pkt, unsigned len)
{
    return blemidi_rx_packet(rx, pkt, len, &sink);
}

/* --- tests --- */

static void test_msg_len(void)
{
    CHECK_EQ(blemidi_msg_len(0x90), 3);
    CHECK_EQ(blemidi_msg_len(0xB0), 3);
    CHECK_EQ(blemidi_msg_len(0xC5), 2);
    CHECK_EQ(blemidi_msg_len(0xDF), 2);
    CHECK_EQ(blemidi_msg_len(0xE0), 3);
    CHECK_EQ(blemidi_msg_len(0xF1), 2);
    CHECK_EQ(blemidi_msg_len(0xF2), 3);
    CHECK_EQ(blemidi_msg_len(0xF3), 2);
    CHECK_EQ(blemidi_msg_len(0xF6), 1);
    CHECK_EQ(blemidi_msg_len(0xF8), 1);
    CHECK_EQ(blemidi_msg_len(0xFF), 1);
    CHECK_EQ(blemidi_msg_len(0xF0), 0);
    CHECK_EQ(blemidi_msg_len(0xF7), 0);
    CHECK_EQ(blemidi_msg_len(0xF4), 0);
    CHECK_EQ(blemidi_msg_len(0xF5), 0);
    CHECK_EQ(blemidi_msg_len(0x40), 0);
}

static void test_encode(void)
{
    uint8_t buf[101];
    struct blemidi_tx tx;
    const uint8_t on[3] = {0x90, 0x3C, 0x64}, cc1[3] = {0xB0, 0x07, 0x64}, cc2[3] = {0xB0, 0x07, 0x65};
    const uint8_t clock[1] = {0xF8}, pc[2] = {0xC1, 0x05};

    blemidi_tx_init(&tx);
    blemidi_tx_begin(&tx, buf, sizeof buf);
    CHECK_EQ(blemidi_tx_len(&tx), 0);
    /* 0x0ABC: high 6 bits 0x15, low 7 bits 0x3C */
    CHECK(blemidi_tx_add(&tx, 0x0ABC, on, 3));
    CHECK_EQ(blemidi_tx_len(&tx), 5);
    CHECK(memcmp(buf, "\x95\xBC\x90\x3C\x64", 5) == 0);

    /* Running status: the same status and timestamp, the data only. */
    blemidi_tx_begin(&tx, buf, sizeof buf);
    CHECK(blemidi_tx_add(&tx, 0x0010, cc1, 3));
    CHECK(blemidi_tx_add(&tx, 0x0010, cc2, 3));
    CHECK_EQ(blemidi_tx_len(&tx), 7);
    CHECK(memcmp(buf, "\x80\x90\xB0\x07\x64\x07\x65", 7) == 0);
    /* A later timestamp: a timestamp and the status again. */
    CHECK(blemidi_tx_add(&tx, 0x0011, cc1, 3));
    CHECK(memcmp(buf + 7, "\x91\xB0\x07\x64", 4) == 0);
    /* Real-time, then a channel message with the same status and timestamp: written in full. */
    CHECK(blemidi_tx_add(&tx, 0x0011, clock, 1));
    CHECK(blemidi_tx_add(&tx, 0x0011, cc2, 3));
    CHECK_EQ(blemidi_tx_len(&tx), 17);
    CHECK(memcmp(buf + 11, "\x91\xF8\x91\xB0\x07\x65", 6) == 0);
    CHECK(blemidi_tx_add(&tx, 0x0011, pc, 2));
    CHECK(memcmp(buf + 17, "\x91\xC1\x05", 3) == 0);

    /* A new packet starts with the status even when the last one ended on it. */
    blemidi_tx_begin(&tx, buf, sizeof buf);
    CHECK(blemidi_tx_add(&tx, 0x0011, pc, 2));
    CHECK(memcmp(buf, "\x80\x91\xC1\x05", 4) == 0);

    /* Not messages: refused, the packet unchanged. */
    const uint8_t sx[3] = {0xF0, 1, 0xF7}, data[3] = {0x40, 1, 2}, high[3] = {0x90, 0x80, 1};
    const uint8_t undefined[1] = {0xF4};
    unsigned before = blemidi_tx_len(&tx);
    CHECK(!blemidi_tx_add(&tx, 0x11, sx, 3));
    CHECK(!blemidi_tx_add(&tx, 0x11, data, 3));
    CHECK(!blemidi_tx_add(&tx, 0x11, high, 3));
    CHECK(!blemidi_tx_add(&tx, 0x11, on, 2));
    CHECK(!blemidi_tx_add(&tx, 0x11, undefined, 1));
    CHECK(!blemidi_tx_add(&tx, 0x11, on, 0));
    CHECK_EQ(blemidi_tx_len(&tx), before);

    /* Nothing begun: refused. */
    struct blemidi_tx none;
    blemidi_tx_init(&none);
    CHECK(!blemidi_tx_add(&none, 0, on, 3));
}

static void test_fit(void)
{
    uint8_t buf[32];
    struct blemidi_tx tx;
    uint8_t m[3] = {0x90, 0x3C, 0x64};

    /* The default ATT MTU of 23: 20 bytes. Header and 4 messages of 1 + 3. */
    blemidi_tx_init(&tx);
    blemidi_tx_begin(&tx, buf, 20);
    memset(buf, 0xEE, sizeof buf);
    for (unsigned i = 0; i < 4; i++) {
        m[0] = (uint8_t)(0x90 + i); /* a new status each time: no running status */
        CHECK(blemidi_tx_add(&tx, 5, m, 3));
    }
    CHECK_EQ(blemidi_tx_len(&tx), 17);
    m[0] = 0x94;
    CHECK(!blemidi_tx_add(&tx, 5, m, 3)); /* 21 > 20 */
    CHECK_EQ(blemidi_tx_len(&tx), 17);
    CHECK_EQ(buf[17], 0xEE);
    m[0] = 0x93; /* running status: 2 bytes, fits */
    CHECK(blemidi_tx_add(&tx, 5, m, 3));
    CHECK_EQ(blemidi_tx_len(&tx), 19);
    const uint8_t clock[1] = {0xF8};
    CHECK(!blemidi_tx_add(&tx, 5, clock, 1)); /* 21 */
    /* A buffer too small for any message. */
    blemidi_tx_begin(&tx, buf, 4);
    CHECK(!blemidi_tx_add(&tx, 5, m, 3));
    CHECK_EQ(blemidi_tx_len(&tx), 0);
    CHECK(blemidi_tx_add(&tx, 5, clock, 1));
}

static void test_timestamps(void)
{
    uint8_t buf[101];
    struct blemidi_tx tx;
    struct blemidi_rx rx;
    const uint8_t clock[1] = {0xF8};

    blemidi_tx_init(&tx);
    blemidi_tx_begin(&tx, buf, sizeof buf);
    CHECK(blemidi_tx_add(&tx, 0x007F, clock, 1));
    CHECK(!blemidi_tx_add(&tx, 0x007E, clock, 1)); /* earlier */
    CHECK(blemidi_tx_add(&tx, 0x0080, clock, 1));  /* the low bits wrap: high + 1 */
    CHECK(!blemidi_tx_add(&tx, 0x0100, clock, 1)); /* the same low bits again: ambiguous */
    CHECK(blemidi_tx_add(&tx, 0x00FF, clock, 1));
    CHECK(!blemidi_tx_add(&tx, 0x0200, clock, 1)); /* two wraps ahead */
    CHECK(blemidi_tx_add(&tx, 0x0105, clock, 1));
    unsigned n = blemidi_tx_len(&tx);

    memset(&rec, 0, sizeof rec);
    blemidi_rx_init(&rx);
    CHECK_EQ(decode(&rx, buf, n), 0);
    CHECK_EQ(rec.n, 4);
    CHECK_EQ(rec.msg[0].ts, 0x007F);
    CHECK_EQ(rec.msg[1].ts, 0x0080);
    CHECK_EQ(rec.msg[2].ts, 0x00FF);
    CHECK_EQ(rec.msg[3].ts, 0x0105);

    /* The 13-bit wrap: 0x1FFF then 0x0000. */
    blemidi_tx_begin(&tx, buf, sizeof buf);
    CHECK(blemidi_tx_add(&tx, 0x1FFF, clock, 1));
    CHECK(blemidi_tx_add(&tx, 0x0000, clock, 1));
    CHECK(buf[0] == 0xBF);
    memset(&rec, 0, sizeof rec);
    CHECK_EQ(decode(&rx, buf, blemidi_tx_len(&tx)), 0);
    CHECK_EQ(rec.n, 2);
    CHECK_EQ(rec.msg[0].ts, 0x1FFF);
    CHECK_EQ(rec.msg[1].ts, 0x0000);
}

/* Forms the specification allows that our encoder does not write. */
static void test_decode_forms(void)
{
    struct blemidi_rx rx;
    blemidi_rx_init(&rx);

    /* Running status with a new timestamp; then without one (the same timestamp). */
    static const uint8_t p1[] = {0x80, 0x81, 0x90, 0x3C, 0x64, 0x85, 0x3E, 0x64, 0x40, 0x64};
    memset(&rec, 0, sizeof rec);
    CHECK_EQ(decode(&rx, p1, sizeof p1), 0);
    CHECK_EQ(rec.n, 3);
    CHECK(rec.msg[0].ts == 1 && memcmp(rec.msg[0].m, "\x90\x3C\x64", 3) == 0);
    CHECK(rec.msg[1].ts == 5 && memcmp(rec.msg[1].m, "\x90\x3E\x64", 3) == 0);
    CHECK(rec.msg[2].ts == 5 && memcmp(rec.msg[2].m, "\x90\x40\x64", 3) == 0);

    /* Running status carries over into the next packet, after a timestamp. */
    static const uint8_t p2[] = {0x80, 0x86, 0x3C, 0x00};
    memset(&rec, 0, sizeof rec);
    CHECK_EQ(decode(&rx, p2, sizeof p2), 0);
    CHECK(rec.n == 1 && memcmp(rec.msg[0].m, "\x90\x3C\x00", 3) == 0 && rec.msg[0].ts == 6);

    /* Real-time between running-status messages keeps the status. */
    static const uint8_t p3[] = {0x80, 0x87, 0xB0, 0x07, 0x10, 0x87, 0xF8, 0x87, 0x07, 0x11};
    memset(&rec, 0, sizeof rec);
    CHECK_EQ(decode(&rx, p3, sizeof p3), 0);
    CHECK_EQ(rec.n, 3);
    CHECK(rec.msg[1].len == 1 && rec.msg[1].m[0] == 0xF8);
    CHECK(rec.msg[2].len == 3 && memcmp(rec.msg[2].m, "\xB0\x07\x11", 3) == 0);

    /* System Common cancels running status; a data byte after it is a fault. */
    static const uint8_t p4[] = {0x80, 0x88, 0xF3, 0x02, 0x88, 0x05, 0x06};
    memset(&rec, 0, sizeof rec);
    CHECK_EQ(decode(&rx, p4, sizeof p4), 2); /* 0x05 and 0x06: no status for them */
    CHECK(rec.n == 1 && rec.msg[0].len == 2 && memcmp(rec.msg[0].m, "\xF3\x02", 2) == 0);

    /* Program Change and Channel Aftertouch: two bytes; tune request: one. */
    static const uint8_t p5[] = {0x80, 0x89, 0xC2, 0x05, 0x06, 0x89, 0xD0, 0x40, 0x89, 0xF6};
    memset(&rec, 0, sizeof rec);
    CHECK_EQ(decode(&rx, p5, sizeof p5), 0);
    CHECK_EQ(rec.n, 4);
    CHECK(rec.msg[1].len == 2 && memcmp(rec.msg[1].m, "\xC2\x06", 2) == 0);
    CHECK(rec.msg[3].len == 1 && rec.msg[3].m[0] == 0xF6);
}

static void test_decode_bad(void)
{
    struct blemidi_rx rx;
    blemidi_rx_init(&rx);
    memset(&rec, 0, sizeof rec);

    static const uint8_t no_header[] = {0x00, 0x80, 0x90, 0x3C, 0x64};
    CHECK_EQ(decode(&rx, no_header, sizeof no_header), 1);
    static const uint8_t c0_header[] = {0xC0, 0x80, 0xF8};
    CHECK_EQ(decode(&rx, c0_header, sizeof c0_header), 1);
    static const uint8_t short1[] = {0x80};
    CHECK_EQ(decode(&rx, short1, 1), 1);
    CHECK_EQ(decode(&rx, short1, 0), 1);
    CHECK_EQ(rec.n, 0);

    /* Data before the first timestamp; then a good message. */
    static const uint8_t p1[] = {0x80, 0x3C, 0x64, 0x81, 0x90, 0x3C, 0x64};
    CHECK_EQ(decode(&rx, p1, sizeof p1), 2);
    CHECK_EQ(rec.n, 1);
    /* A message cut short by a timestamp, and at the packet's end. */
    static const uint8_t p2[] = {0x80, 0x81, 0x90, 0x3C, 0x82, 0xB0, 0x07};
    CHECK_EQ(decode(&rx, p2, sizeof p2), 2);
    CHECK_EQ(rec.n, 1);
    /* The cut message does not complete in the next packet. */
    static const uint8_t p3[] = {0x80, 0x64};
    CHECK_EQ(decode(&rx, p3, sizeof p3), 1);
    CHECK_EQ(rec.n, 1);
    /* F7 outside a SysEx, an undefined status, a trailing timestamp. */
    static const uint8_t p4[] = {0x80, 0x81, 0xF7, 0x81, 0xF5, 0x81, 0xF8, 0x82};
    CHECK_EQ(decode(&rx, p4, sizeof p4), 3);
    CHECK_EQ(rec.n, 2); /* the clock */
    /* No running status after an F7 fault: the data byte is a fault too. */
    static const uint8_t p5[] = {0x80, 0x81, 0xF7, 0x81, 0x40};
    CHECK_EQ(decode(&rx, p5, sizeof p5), 2);
    CHECK_EQ(rx.bad, 1 + 1 + 1 + 1 + 2 + 2 + 1 + 3 + 2);
    CHECK_EQ(rx.packets, 9);

    /* A SysEx cut short by another status: passed on without F7, the status decoded. */
    blemidi_rx_init(&rx);
    memset(&rec, 0, sizeof rec);
    static const uint8_t p6[] = {0x80, 0x81, 0xF0, 0x01, 0x02, 0x82, 0x90, 0x3C, 0x64};
    CHECK_EQ(decode(&rx, p6, sizeof p6), 1);
    CHECK(rec.sysex_len == 3 && memcmp(rec.sysex, "\xF0\x01\x02", 3) == 0);
    CHECK(rec.n == 1 && memcmp(rec.msg[0].m, "\x90\x3C\x64", 3) == 0);

    /* A NULL sink, and NULL callbacks: counted all the same. */
    blemidi_rx_init(&rx);
    static const uint8_t p7[] = {0x80, 0x81, 0x90, 0x3C, 0x64, 0x81, 0xF0, 0x01, 0x81, 0xF7};
    CHECK_EQ(blemidi_rx_packet(&rx, p7, sizeof p7, NULL), 0);
    static const struct blemidi_sink empty = {NULL, NULL, NULL};
    CHECK_EQ(blemidi_rx_packet(&rx, p7, sizeof p7, &empty), 0);
    CHECK_EQ(rx.messages, 2);
    CHECK_EQ(rx.sysex_bytes, 6);
}

/* A SysEx of n data bytes, encoded into packets of cap bytes and decoded back. */
static unsigned sysex_round(unsigned n, unsigned cap, bool clock_inside)
{
    uint8_t sx[600], buf[128];
    struct blemidi_tx tx;
    struct blemidi_rx rx;
    unsigned packets = 0;

    sx[0] = 0xF0;
    for (unsigned i = 0; i < n; i++)
        sx[1 + i] = (uint8_t)(i * 7 & 0x7F);
    sx[n + 1] = 0xF7;
    unsigned total = n + 2, done = 0;

    blemidi_tx_init(&tx);
    blemidi_rx_init(&rx);
    memset(&rec, 0, sizeof rec);
    uint16_t ts = 100;
    while (done < total) {
        blemidi_tx_begin(&tx, buf, cap);
        if (clock_inside && done > 0) {
            const uint8_t clock[1] = {0xF8};
            CHECK(blemidi_tx_add(&tx, ts, clock, 1)); /* real-time inside the SysEx */
        }
        unsigned took = blemidi_tx_sysex(&tx, ts, sx + done, total - done);
        if (!took && !blemidi_tx_len(&tx)) {
            CHECK(!"no progress");
            break;
        }
        done += took;
        CHECK(blemidi_tx_len(&tx) <= cap);
        CHECK_EQ(decode(&rx, buf, blemidi_tx_len(&tx)), 0);
        packets++;
        ts++;
    }
    CHECK(!tx.sysex);
    CHECK_EQ(rec.sysex_len, total);
    CHECK(memcmp(rec.sysex, sx, total) == 0);
    CHECK(!rx.sysex);
    if (clock_inside)
        CHECK_EQ(rec.n, packets - 1);
    else
        CHECK_EQ(rec.n, 0);
    return packets;
}

static void test_sysex(void)
{
    /* Small: one packet, header + ts + F0 + 3 + ts + F7. */
    uint8_t buf[101];
    struct blemidi_tx tx;
    static const uint8_t sx[] = {0xF0, 0x7D, 0x01, 0x02, 0xF7};
    blemidi_tx_init(&tx);
    blemidi_tx_begin(&tx, buf, sizeof buf);
    CHECK_EQ(blemidi_tx_sysex(&tx, 3, sx, sizeof sx), sizeof sx);
    CHECK_EQ(blemidi_tx_len(&tx), 8);
    CHECK(memcmp(buf, "\x80\x83\xF0\x7D\x01\x02\x83\xF7", 8) == 0);
    /* After it, channel messages again. */
    const uint8_t on[3] = {0x90, 1, 2};
    CHECK(blemidi_tx_add(&tx, 3, on, 3));

    /* A message added while a SysEx is open is refused; real-time is not. */
    blemidi_tx_begin(&tx, buf, sizeof buf);
    CHECK_EQ(blemidi_tx_sysex(&tx, 3, sx, 3), 3);
    CHECK(tx.sysex);
    CHECK(!blemidi_tx_add(&tx, 3, on, 3));
    const uint8_t clock[1] = {0xF8};
    CHECK(blemidi_tx_add(&tx, 3, clock, 1));
    CHECK_EQ(blemidi_tx_sysex(&tx, 3, sx + 3, 2), 2);
    CHECK(!tx.sysex);

    /* Not SysEx: nothing taken. */
    blemidi_tx_begin(&tx, buf, sizeof buf);
    static const uint8_t no_f0[] = {0x01, 0xF7}, high[] = {0xF0, 0x01, 0x90, 0xF7}, after[] = {0xF0, 0xF7, 0x01};
    CHECK_EQ(blemidi_tx_sysex(&tx, 3, no_f0, sizeof no_f0), 0);
    CHECK_EQ(blemidi_tx_len(&tx), 0);
    CHECK_EQ(blemidi_tx_sysex(&tx, 3, high, sizeof high), 2); /* stops at 0x90 */
    blemidi_tx_init(&tx);
    blemidi_tx_begin(&tx, buf, sizeof buf);
    CHECK_EQ(blemidi_tx_sysex(&tx, 3, after, sizeof after), 1); /* F7 not the last byte */

    /* Across packets, at the default MTU and ours, with the F7 alone in the last packet. */
    CHECK_EQ(sysex_round(48, 20, false), 3); /* 17 data bytes, 19, then 12 and the F7 */
    CHECK_EQ(sysex_round(0, 20, false), 1);
    CHECK_EQ(sysex_round(500, 101, false), 6);
    CHECK_EQ(sysex_round(15, 20, false), 1);  /* 1 + 2 + 15 + 2 = 20 */
    CHECK_EQ(sysex_round(16, 20, false), 2);  /* 1 + 2 + 16 = 19: F7 next */
    CHECK_EQ(sysex_round(17, 20, false), 2);  /* 1 + 2 + 17 = 20: F7 next */
    sysex_round(200, 20, true);
    sysex_round(500, 101, true);
}

/* A pseudo-random stream of messages with rising timestamps: encoded as a sender would, decoded,
 * compared. */
static uint32_t lcg = 12345;
static unsigned rnd(unsigned n)
{
    lcg = lcg * 1103515245u + 12345u;
    return (lcg >> 16) % n;
}

static void test_round_trip(void)
{
    static const unsigned caps[] = {20, 101, 5};
    for (unsigned c = 0; c < sizeof caps / sizeof caps[0]; c++) {
        struct {
            uint16_t ts;
            uint8_t m[3];
            unsigned len;
        } in[400];
        unsigned n = 0;
        uint16_t ts = (uint16_t)(0x1F00 + c); /* crosses the 13-bit wrap */
        while (n < 400) {
            ts = (uint16_t)((ts + rnd(3) * rnd(90)) & BLEMIDI_TS_MASK);
            unsigned k = rnd(10);
            uint8_t *m = in[n].m;
            if (k < 6) {
                static const uint8_t st[] = {0x90, 0x80, 0xB0, 0xB3, 0xE0, 0xA1};
                m[0] = st[rnd(k < 3 ? 2 : 6)];
                m[1] = (uint8_t)rnd(128);
                m[2] = (uint8_t)rnd(128);
                in[n].len = 3;
            } else if (k < 8) {
                m[0] = (uint8_t)(0xC0 + rnd(0x20)); /* Cn or Dn */
                m[1] = (uint8_t)rnd(128);
                in[n].len = 2;
            } else if (k < 9) {
                static const uint8_t rt[] = {0xF8, 0xFA, 0xFB, 0xFC, 0xFE};
                m[0] = rt[rnd(5)];
                in[n].len = 1;
            } else {
                m[0] = 0xF2;
                m[1] = (uint8_t)rnd(128);
                m[2] = (uint8_t)rnd(128);
                in[n].len = 3;
            }
            in[n++].ts = ts;
        }

        uint8_t buf[101];
        struct blemidi_tx tx;
        struct blemidi_rx rx;
        blemidi_tx_init(&tx);
        blemidi_rx_init(&rx);
        memset(&rec, 0, sizeof rec);
        unsigned i = 0, packets = 0;
        while (i < n) {
            blemidi_tx_begin(&tx, buf, caps[c]);
            while (i < n && blemidi_tx_add(&tx, in[i].ts, in[i].m, in[i].len))
                i++;
            if (!blemidi_tx_len(&tx)) {
                CHECK(!"no progress");
                break;
            }
            CHECK(blemidi_tx_len(&tx) <= caps[c]);
            CHECK_EQ(decode(&rx, buf, blemidi_tx_len(&tx)), 0);
            packets++;
        }
        CHECK_EQ(rec.n, n);
        unsigned diff = 0;
        for (unsigned j = 0; j < n && j < rec.n; j++)
            if (rec.msg[j].ts != in[j].ts || rec.msg[j].len != in[j].len ||
                memcmp(rec.msg[j].m, in[j].m, in[j].len) != 0) {
                if (!diff)
                    fprintf(stderr, "cap %u, message %u: ts %u len %u, want ts %u len %u\n", caps[c], j,
                            rec.msg[j].ts, rec.msg[j].len, in[j].ts, in[j].len);
                diff++;
            }
        CHECK_EQ(diff, 0);
        CHECK_EQ(rx.bad, 0);
        printf("  cap %3u: %u messages in %u packets\n", caps[c], n, packets);
    }
}

int main(void)
{
    RUN(test_msg_len);
    RUN(test_encode);
    RUN(test_fit);
    RUN(test_timestamps);
    RUN(test_decode_forms);
    RUN(test_decode_bad);
    RUN(test_sysex);
    RUN(test_round_trip);
    printf("test_blemidi: %s\n", check_failed ? "FAILED" : "all ok");
    return check_failed ? 1 : 0;
}
