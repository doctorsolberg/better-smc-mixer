/*
 * Host tests of the key logic (key.c) against a model of the two lines and of a chip that shifts
 * in D- on every rising edge of D+.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -o build/usbkey/test_key tools/usbkey/test_key.c tools/usbkey/key.c
 */
#include <stdio.h>
#include <stdlib.h>

#include "key.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* The model */
static uint32_t now;
static int driven[2], level[2];
static uint16_t shifted;
static int rises;
static uint32_t chip_listens_at;        /* the chip looks for the key from this time on */
static int chip_answered;
static int (*external_dp)(uint32_t t);  /* D+ when nobody drives it: pull-ups on the bus */

static int no_pullup(uint32_t t) { (void)t; return 0; }

static int dp_level(void)
{
    if (driven[KEY_DP])
        return level[KEY_DP];
    return chip_answered || external_dp(now);
}

static void m_drive(enum key_line l, int v)
{
    if (l == KEY_DP && v && !dp_level()) {
        rises++;
        shifted = (uint16_t)(shifted << 1 | (driven[KEY_DM] ? level[KEY_DM] : 0));
        if (now >= chip_listens_at && shifted == KEY_VALUE)
            chip_answered = 1;
    }
    driven[l] = 1;
    level[l] = v;
}

static void m_release(enum key_line l) { driven[l] = 0; }
static int m_read(enum key_line l) { return l == KEY_DP ? dp_level() : (driven[l] ? level[l] : 0); }
static void m_delay(uint32_t us) { now += us; }
static uint32_t m_now(void) { return now; }

static const struct key_io io = { m_drive, m_release, m_read, m_delay, m_now };

static void reset_model(void)
{
    now = 1000;
    driven[0] = driven[1] = level[0] = level[1] = 0;
    shifted = 0;
    rises = 0;
    chip_listens_at = 0xFFFFFFFFu;
    chip_answered = 0;
    external_dp = no_pullup;
}

/* Polls until an event other than NONE, advancing the clock 50 us per poll as the main loop
 * would; gives up after max_us. */
static enum key_event run(struct key_attempt *a, uint32_t max_us)
{
    uint32_t end = now + max_us;
    while (now < end) {
        enum key_event e = key_poll(a, &io);
        if (e != KEY_EV_NONE)
            return e;
        now += 50;
    }
    return KEY_EV_NONE;
}

static void test_bit_order(void)
{
    reset_model();
    m_drive(KEY_DP, 0);
    m_drive(KEY_DM, 0);
    CHECK(!key_send_once(&io));
    CHECK(rises == 16);
    CHECK(shifted == KEY_VALUE);
}

static void test_answer_releases_lines(void)
{
    reset_model();
    struct key_attempt a;
    key_start(&a, &io);
    CHECK(run(&a, 20000) == KEY_EV_ARMED);
    CHECK(a.state == KEY_KEYING);
    chip_listens_at = now + 5000;  /* the chip starts listening in the middle of a key */
    CHECK(run(&a, 100000) == KEY_EV_ANSWERED);
    CHECK(!driven[KEY_DP] && !driven[KEY_DM]);
    CHECK(a.keys > 0);
}

static int device_until;
static int device_on_bus(uint32_t t) { return t < (uint32_t)device_until; }

static void test_waits_for_an_empty_bus(void)
{
    reset_model();
    external_dp = device_on_bus;
    device_until = (int)(now + 1000000);
    struct key_attempt a;
    key_start(&a, &io);
    CHECK(run(&a, 900000) == KEY_EV_NONE);
    CHECK(!driven[KEY_DP] && !driven[KEY_DM]);
    uint32_t gone = (uint32_t)device_until;
    CHECK(run(&a, 200000) == KEY_EV_ARMED);
    CHECK(now - gone >= KEY_ARM_LOW_US);
}

static void test_timeout(void)
{
    reset_model();
    struct key_attempt a;
    key_start(&a, &io);
    CHECK(run(&a, 20000) == KEY_EV_ARMED);
    CHECK(run(&a, KEY_TIMEOUT_US + 1000000) == KEY_EV_TIMEOUT);
    CHECK(a.state == KEY_IDLE);
    CHECK(!driven[KEY_DP] && !driven[KEY_DM]);
}

/* After the answer: the chip's pull-up stays, apart from what the script says. */
static uint32_t low_from, low_to;
static int scripted(uint32_t t) { return !(t >= low_from && t < low_to); }

static void watch(uint32_t low_start, uint32_t low_len, int *dropped, uint32_t *pulses)
{
    reset_model();
    struct key_attempt a;
    key_start(&a, &io);
    CHECK(run(&a, 20000) == KEY_EV_ARMED);
    chip_listens_at = 0;
    CHECK(run(&a, 10000) == KEY_EV_ANSWERED);
    chip_answered = 0;
    external_dp = scripted;
    low_from = now + low_start;
    low_to = low_from + low_len;
    CHECK(run(&a, KEY_WATCH_US + 100000) == KEY_EV_WATCHED);
    *dropped = key_dropped(&a);
    *pulses = a.low_pulses;
}

static void test_watch(void)
{
    int dropped;
    uint32_t pulses;
    watch(100000, 20000, &dropped, &pulses);   /* a 20 ms bus reset */
    CHECK(!dropped && pulses == 1);
    watch(500000, 10000000, &dropped, &pulses); /* the chip leaves the bus */
    CHECK(dropped);
    watch(0, 0, &dropped, &pulses);             /* stays */
    CHECK(!dropped && pulses == 0);
}

int main(void)
{
    test_bit_order();
    test_answer_releases_lines();
    test_waits_for_an_empty_bus();
    test_timeout();
    test_watch();
    printf("test_key: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
