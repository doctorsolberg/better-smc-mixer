/*
 * Host tests for the BLE MIDI queue and its merge (firmware/midi/bleq.c). Run: make -C firmware test
 *
 * Units are written as text: messages "B0 07 10" separated by " | ", each followed by its mark: '.'
 * plain, a letter a merge group (the same letter in a row: one group).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bleq.h"
#include "check.h"

int check_failed;

static struct bleq Q;

/* Pushes a unit written as text; returns what bleq_push_unit() returned. */
static bool push(const char *spec)
{
    uint8_t msg[64][3], mark[64];
    unsigned n = 0;
    const char *p = spec;
    while (*p && n < 64) {
        unsigned a, b, c;
        char m;
        int used = 0;
        memset(msg[n], 0, 3);
        if (sscanf(p, "%x %x %x %c%n", &a, &b, &c, &m, &used) == 4 && m != '|') {
            msg[n][0] = (uint8_t)a;
            msg[n][1] = (uint8_t)b;
            msg[n][2] = (uint8_t)c;
        } else if (sscanf(p, "%x %x %c%n", &a, &b, &m, &used) == 3) {
            msg[n][0] = (uint8_t)a;
            msg[n][1] = (uint8_t)b;
        } else {
            fprintf(stderr, "bad unit: %s\n", spec);
            exit(2);
        }
        mark[n] = m == '.' ? 0 : (uint8_t)(m - 'a' + 1);
        n++;
        p += used;
        while (*p == ' ' || *p == '|')
            p++;
    }
    return bleq_push_unit(&Q, (const uint8_t(*)[3])msg, mark, n, 1000u, true);
}

/* Takes every message the consumer would send, as text. */
static char got[4096];
static const char *drain(void)
{
    const struct bleq_msg *e;
    size_t n = 0;
    got[0] = 0;
    while ((e = bleq_peek(&Q)) != NULL) {
        if (n)
            n += (size_t)snprintf(got + n, sizeof got - n, " | ");
        if (bleq_msg_len(e->m[0]) == 2)
            n += (size_t)snprintf(got + n, sizeof got - n, "%02X %02X", e->m[0], e->m[1]);
        else
            n += (size_t)snprintf(got + n, sizeof got - n, "%02X %02X %02X", e->m[0], e->m[1], e->m[2]);
        bleq_pop(&Q);
    }
    return got;
}

#define EXPECT(want) expect(__LINE__, want)
static void expect(int line, const char *want)
{
    const char *g = drain();
    if (strcmp(g, want) != 0) {
        check_failed++;
        fprintf(stderr, "test_bleq.c:%d:\n  got:  \"%s\"\n  want: \"%s\"\n", line, g, want);
    }
}

static void test_basics(void)
{
    bleq_init(&Q);
    CHECK_EQ(bleq_msg_len(0xC3), 2);
    CHECK_EQ(bleq_msg_len(0xD0), 2);
    CHECK_EQ(bleq_msg_len(0xB0), 3);
    CHECK_EQ(bleq_msg_len(0xE5), 3);
    CHECK(bleq_peek(&Q) == NULL);
    CHECK(push("90 3C 7F . | C0 05 . | B0 07 10 a"));
    CHECK_EQ(bleq_depth(&Q), 3);
    const struct bleq_msg *e = bleq_peek(&Q);
    CHECK(e && e->timed && e->t_us == 1000u && e->len == 2 && e->pos == 0 && !e->merge);
    CHECK(bleq_at_group_start(&Q));
    bleq_pop(&Q);
    CHECK(!bleq_at_group_start(&Q));
    EXPECT("C0 05 | B0 07 10");
    CHECK(bleq_at_group_start(&Q));
    CHECK_EQ(Q.pushed, 3);
    CHECK_EQ(Q.taken, 3);

    /* all or none */
    for (unsigned i = 0; i < BLEQ_SIZE - 2u; i++)
        CHECK(push("90 3C 7F ."));
    CHECK_EQ(bleq_room(&Q), 1);
    CHECK(!push("90 3D 7F . | 80 3D 00 ."));
    CHECK_EQ(Q.refused, 2);
    CHECK_EQ(bleq_depth(&Q), BLEQ_SIZE - 2u);
    CHECK_EQ(bleq_take_window_max(&Q), BLEQ_SIZE - 2u);

    /* backpressure, as midiq's */
    bleq_want_room(&Q, 8);
    CHECK(!bleq_room_due(&Q));
    CHECK_EQ(bleq_drop_all(&Q), BLEQ_SIZE - 2u);
    CHECK(bleq_room_due(&Q));
    CHECK(!bleq_room_due(&Q));
    CHECK_EQ(Q.dropped, BLEQ_SIZE - 2u);
    CHECK_EQ(Q.depth_max, BLEQ_SIZE - 2u);
}

/* Plain CC, pitch bend, 14-bit CC: the newest value of a target goes last; others never merge. */
static void test_merge_plain(void)
{
    bleq_init(&Q);
    push("B0 07 10 a");
    push("90 3C 7F .");
    push("B0 07 20 a");
    EXPECT("90 3C 7F | B0 07 20");
    CHECK_EQ(Q.merged, 1);
    CHECK_EQ(Q.skipped, 1);

    push("B0 07 10 a");
    push("B0 07 11 a");
    push("B0 07 12 a");
    push("B1 07 13 a"); /* another channel */
    push("B0 08 14 a"); /* another number */
    push("E0 00 40 a");
    push("E0 10 41 a");
    push("B0 01 05 a | B0 21 06 a"); /* 14-bit */
    push("B0 01 07 a | B0 21 08 a");
    EXPECT("B0 07 12 | B1 07 13 | B0 08 14 | E0 10 41 | B0 01 07 | B0 21 08");

    /* plain groups: buttons and notes, never */
    push("B0 40 7F .");
    push("B0 40 00 .");
    push("90 3C 7F .");
    push("90 3C 00 .");
    EXPECT("B0 40 7F | B0 40 00 | 90 3C 7F | 90 3C 00");

    /* a merge value never replaces a plain message of the same target, nor the other way */
    push("B0 07 10 a");
    push("B0 07 7F .");
    push("B0 07 20 a");
    EXPECT("B0 07 7F | B0 07 20");
    push("B0 07 7F .");
    push("B0 07 10 a");
    push("B0 07 7F .");
    EXPECT("B0 07 7F | B0 07 10 | B0 07 7F");

    /* a unit with a value and a plain tail (a null on another channel) */
    push("B0 07 01 a");
    push("B0 07 02 a | B1 65 7F . | B1 64 7F .");
    EXPECT("B0 07 02 | B1 65 7F | B1 64 7F");
}

/* NRPN: the shape includes the parameter's number; the selection the receiver ends with is the
 * core's. */
static void test_merge_nrpn(void)
{
    bleq_init(&Q);
    /* other parameters do not merge; the same one does, past full selections of others */
    push("B0 63 00 a | B0 62 05 a | B0 06 10 a | B0 26 00 a");
    push("B0 63 00 a | B0 62 06 a | B0 06 11 a | B0 26 00 a");
    push("B0 63 00 a | B0 62 05 a | B0 06 12 a | B0 26 00 a");
    EXPECT("B0 63 00 | B0 62 06 | B0 06 11 | B0 26 00 | B0 63 00 | B0 62 05 | B0 06 12 | B0 26 00");

    /* data entry alone replaces data entry alone */
    push("B0 63 00 a | B0 62 05 a | B0 06 10 a | B0 26 00 a");
    push("B0 06 11 a | B0 26 00 a");
    push("B0 06 12 a | B0 26 00 a");
    push("B0 06 13 a | B0 26 00 a");
    EXPECT("B0 63 00 | B0 62 05 | B0 06 10 | B0 26 00 | B0 06 13 | B0 26 00");

    /* ... but not past a selection */
    push("B0 63 00 a | B0 62 05 a | B0 06 10 a | B0 26 00 a");
    push("B0 06 11 a | B0 26 00 a");
    push("B0 63 00 a | B0 62 06 a | B0 06 20 a | B0 26 00 a");
    push("B0 06 21 a | B0 26 00 a");
    EXPECT("B0 63 00 | B0 62 05 | B0 06 10 | B0 26 00 | B0 06 11 | B0 26 00 | B0 63 00 | B0 62 06 | B0 06 20 | B0 26 00 | B0 06 21 | B0 26 00");

    /* a full selection is kept while data entry after it relies on it */
    push("B0 63 00 a | B0 62 05 a | B0 06 10 a | B0 26 00 a");
    push("B0 06 11 a | B0 26 00 a");
    push("B0 63 00 a | B0 62 05 a | B0 06 12 a | B0 26 00 a");
    EXPECT("B0 63 00 | B0 62 05 | B0 06 10 | B0 26 00 | B0 06 11 | B0 26 00 | B0 63 00 | B0 62 05 | B0 06 12 | B0 26 00");
    /* ... a plain CC 6 (relies) too, and a half number */
    push("B0 63 00 a | B0 62 05 a | B0 06 10 a | B0 26 00 a");
    push("B0 63 07 .");
    push("B0 63 00 a | B0 62 05 a | B0 06 12 a | B0 26 00 a");
    EXPECT("B0 63 00 | B0 62 05 | B0 06 10 | B0 26 00 | B0 63 07 | B0 63 00 | B0 62 05 | B0 06 12 | B0 26 00");
    /* ... unless a full selection covers it */
    push("B0 63 00 a | B0 62 05 a | B0 06 10 a | B0 26 00 a");
    push("B0 63 00 a | B0 62 06 a | B0 06 20 a | B0 26 00 a");
    push("B0 06 21 a | B0 26 00 a");
    push("B0 63 00 a | B0 62 05 a | B0 06 12 a | B0 26 00 a");
    EXPECT("B0 63 00 | B0 62 06 | B0 06 20 | B0 26 00 | B0 06 21 | B0 26 00 | B0 63 00 | B0 62 05 | B0 06 12 | B0 26 00");
    /* other channels do not matter; RPN as NRPN */
    push("B0 65 00 a | B0 64 00 a | B0 06 02 a | B0 26 00 a");
    push("B1 06 11 . | B1 26 00 .");
    push("B0 65 00 a | B0 64 00 a | B0 06 03 a | B0 26 00 a");
    EXPECT("B1 06 11 | B1 26 00 | B0 65 00 | B0 64 00 | B0 06 03 | B0 26 00");
}

/* The consumer and the producer: a group begun goes out whole; one only looked at is skipped. */
static void test_consumer(void)
{
    bleq_init(&Q);
    push("B0 63 00 a | B0 62 05 a | B0 06 10 a | B0 26 00 a");
    const struct bleq_msg *e = bleq_peek(&Q);
    CHECK(e && e->m[1] == 0x63);
    bleq_pop(&Q); /* begun */
    push("B0 63 00 a | B0 62 05 a | B0 06 11 a | B0 26 00 a");
    CHECK_EQ(Q.merged, 0); /* it began before the tail */
    EXPECT("B0 62 05 | B0 06 10 | B0 26 00 | B0 63 00 | B0 62 05 | B0 06 11 | B0 26 00");

    push("B0 07 10 a");
    e = bleq_peek(&Q); /* looked at, a packet sent before it: not begun */
    CHECK(e && e->m[2] == 0x10);
    push("B0 07 11 a");
    EXPECT("B0 07 11");

    /* a group killed after its first message was taken still goes out whole */
    bleq_init(&Q);
    push("90 3C 7F .");
    push("B0 01 05 a | B0 21 06 a");
    e = bleq_peek(&Q);
    bleq_pop(&Q);
    e = bleq_peek(&Q);
    CHECK(e && e->m[1] == 0x01);
    Q.dead[1] = 1; /* as if the producer marked it between the look and the take */
    bleq_pop(&Q);
    EXPECT("B0 21 06");

    /* wrap around the ring, merging across the end */
    bleq_init(&Q);
    for (unsigned i = 0; i < 300; i++) {
        char u[64];
        snprintf(u, sizeof u, "B0 07 %02X a | B0 27 %02X a", i & 0x7Fu, (i * 3u) & 0x7Fu);
        CHECK(push(u));
        snprintf(u, sizeof u, "90 %02X 7F .", i & 0x7Fu);
        CHECK(push(u));
        if (i % 20 == 19) { /* dead groups hold their slots until the consumer passes them */
            CHECK(bleq_depth(&Q) <= 60u);
            drain();
        }
    }
    CHECK_EQ(Q.refused, 0);
    CHECK_EQ(Q.taken + Q.skipped, Q.pushed);
    CHECK_EQ(Q.merged, Q.skipped);
    CHECK_EQ(Q.taken, 300 + 15 * 2); /* the notes, and one value per drain */
}

int main(void)
{
    RUN(test_basics);
    RUN(test_merge_plain);
    RUN(test_merge_nrpn);
    RUN(test_consumer);
    printf("test_bleq: %s\n", check_failed ? "FAILED" : "all ok");
    return check_failed ? 1 : 0;
}
