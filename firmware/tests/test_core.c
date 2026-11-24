/*
 * Host unit tests for the control core (docs/core.md). Run: make -C firmware test
 *
 * Each test drives the core with events at a virtual time and checks the MIDI it sends (as a hex
 * string, messages separated by " | "), the LED states and the core's state.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "core.h"
#include "core_settings.h"
#include "smclog.h"
#include "smclog_host.h"

int check_failed;

/* --- harness --------------------------------------------------------------------------------- */

static struct core C;
static struct core_config CFG;
static uint32_t T; /* µs */
static char out[4096];
static unsigned led[CORE_LEDS], led_calls;
static uint16_t last_cause;
/* core.out_merge per message: '.' unmarked, 'a', 'b'... a value each, in order of appearance */
static char marks[256];
static uint8_t mark_of[26];

static void on_midi(void *ctx, const uint8_t *m, unsigned len, uint16_t cause)
{
    (void)ctx;
    size_t k = strlen(marks);
    if (k + 1 < sizeof marks) {
        char ch = '.';
        if (C.out_merge) {
            unsigned j = 0;
            while (j < 26 && mark_of[j] && mark_of[j] != C.out_merge)
                j++;
            if (j < 26) {
                mark_of[j] = C.out_merge;
                ch = (char)('a' + j);
            }
        }
        marks[k] = ch;
        marks[k + 1] = 0;
    }
    size_t n = strlen(out);
    if (n)
        n += (size_t)snprintf(out + n, sizeof out - n, " | ");
    for (unsigned i = 0; i < len; i++)
        n += (size_t)snprintf(out + n, sizeof out - n, i ? " %02X" : "%02X", m[i]);
    last_cause = cause;
}

static void on_led(void *ctx, unsigned i, unsigned state)
{
    (void)ctx;
    led[i] = state;
    led_calls++;
}

static unsigned bt_calls;
static int bt_last = -1;
static void on_bluetooth(void *ctx, unsigned on)
{
    (void)ctx;
    bt_last = (int)on;
    bt_calls++;
}

static const struct core_io IO = {on_midi, on_led, on_bluetooth};

static uint32_t clock_now(void) { return T; }

/* Starts the core on CFG (fill CFG first: setup_test() or by hand). */
static void start(void)
{
    smclog_host_set_clock(clock_now);
    smclog_reset();
    T = 1000;
    out[0] = 0;
    marks[0] = 0;
    memset(mark_of, 0, sizeof mark_of);
    memset(led, 0xFF, sizeof led);
    led_calls = 0;
    bt_calls = 0;
    bt_last = -1;
    core_init(&C, &CFG, &IO, NULL);
    core_set_bluetooth(&C, 1, false); /* as the controller with BLE, and the simulator */
}

static void setup_test(void)
{
    core_config_test(&CFG);
    start();
}

static void event(uint8_t type, unsigned index, int value)
{
    struct core_event ev;
    memset(&ev, 0, sizeof ev);
    ev.t_us = T;
    ev.type = type;
    ev.index = (uint8_t)index;
    ev.value = (int16_t)value;
    core_event(&C, &ev);
}

static void press(unsigned b) { event(CORE_EV_BUTTON, b, 1); }
static void release(unsigned b) { event(CORE_EV_BUTTON, b, 0); }
static void tap(unsigned b)
{
    press(b);
    release(b);
}
static void turn(unsigned e, int clicks) { event(CORE_EV_ENCODER, e, clicks); }
static void fader(unsigned f, int pos) { event(CORE_EV_FADER, f, pos); }
static void after_ms(unsigned ms) { T += ms * 1000u; }
static void wait_ms(unsigned ms)
{
    after_ms(ms);
    event(CORE_EV_TICK, 0, 0);
}

static void midi_in_from(unsigned transport, uint8_t s, uint8_t d1, uint8_t d2)
{
    struct core_event ev;
    memset(&ev, 0, sizeof ev);
    ev.t_us = T;
    ev.type = CORE_EV_MIDI_IN;
    ev.index = (uint8_t)transport;
    ev.midi[0] = s;
    ev.midi[1] = d1;
    ev.midi[2] = d2;
    core_event(&C, &ev);
}

static void midi_in(uint8_t s, uint8_t d1, uint8_t d2)
{
    struct core_event ev;
    memset(&ev, 0, sizeof ev);
    ev.t_us = T;
    ev.type = CORE_EV_MIDI_IN;
    ev.midi[0] = s;
    ev.midi[1] = d1;
    ev.midi[2] = d2;
    core_event(&C, &ev);
}

/* Fader position for a 7-bit value on a 0-127 range. */
static int P(int v) { return (v * CORE_FADER_MAX + 63) / 127; }

/* The MIDI sent since the last call must be exactly `want`. */
#define EXPECT(want) expect_out(__FILE__, __LINE__, want)
static void expect_out(const char *file, int line, const char *want)
{
    if (strcmp(out, want) != 0) {
        check_failed++;
        fprintf(stderr, "%s:%d: MIDI out\n  got:  \"%s\"\n  want: \"%s\"\n", file, line, out, want);
    }
    out[0] = 0;
}

/* The merge marks since the last call must be exactly `want` (and the MIDI is forgotten). */
#define MARKS(want) expect_marks(__FILE__, __LINE__, want)
static void expect_marks(const char *file, int line, const char *want)
{
    if (strcmp(marks, want) != 0) {
        check_failed++;
        fprintf(stderr, "%s:%d: merge marks\n  got:  \"%s\" (%s)\n  want: \"%s\"\n", file, line, marks,
                out, want);
    }
    marks[0] = 0;
    memset(mark_of, 0, sizeof mark_of);
    out[0] = 0;
}

static uint16_t target(unsigned msg, unsigned ch, unsigned num)
{
    struct core_assign a;
    memset(&a, 0, sizeof a);
    a.msg = (uint8_t)msg;
    a.channel = (uint8_t)ch;
    a.number = (uint16_t)num;
    uint16_t v = 0xFFFF;
    core_target_get(&C, core_target_key(&a, ch), &v);
    return v;
}

/* --- tests ----------------------------------------------------------------------------------- */

static void test_init(void)
{
    setup_test();
    /* 16 banks x (8 faders + 8 encoders + 32 buttons) + 4 presets x 11 row buttons */
    CHECK_EQ(C.stats.targets, 16 * 48 + 4 * 11);
    CHECK_EQ(C.stats.store_full, 0);
    CHECK_EQ(led_calls, CORE_LEDS + 1); /* every LED reported once, then BT */
    for (unsigned i = 0; i < CORE_LEDS; i++) /* Bluetooth on, nobody connected: BT blinks */
        CHECK_EQ(led[i], i == CORE_BTN_BT ? CORE_LED_SLOW : CORE_LED_OFF);
    CHECK_EQ(C.preset, 0);
    CHECK_EQ(C.page, 0);
    EXPECT("");
}

static void test_fader_basics(void)
{
    setup_test();
    fader(0, P(100)); /* no value yet: takes over */
    EXPECT("B0 14 64");
    fader(0, P(100) + 3); /* the same 7-bit value: nothing */
    EXPECT("");
    fader(0, P(101));
    EXPECT("B0 14 65");
    CHECK_EQ(target(CORE_MSG_CC, 0, 20), 101);
    fader(7, 0);
    EXPECT("B0 1B 00");
    CHECK(last_cause != 0); /* the output is in the event's cause chain */
}

static void test_pickup_across_pages(void)
{
    setup_test();
    fader(0, P(100));
    EXPECT("B0 14 64");

    press(CORE_BTN_SHIFT);
    tap(CORE_BTN_DOWN); /* page 2: channel 2 */
    release(CORE_BTN_SHIFT);
    CHECK_EQ(C.page, 1);
    midi_in(0xB1, 20, 30); /* page 2's fader 1 target is at 30 */
    wait_ms(400);

    fader(0, P(90)); /* 90, target 30: waits, move down */
    EXPECT("");
    CHECK_EQ(led[CORE_LED_ENC(0)], CORE_LED_FAST);
    fader(0, P(32)); /* two steps away: still waits */
    EXPECT("");
    fader(0, P(31)); /* within one step: picked up */
    EXPECT("B1 14 1F");
    CHECK_EQ(led[CORE_LED_ENC(0)], CORE_LED_OFF);
    fader(0, P(20));
    EXPECT("B1 14 14");

    press(CORE_BTN_SHIFT);
    tap(CORE_BTN_UP); /* back to page 1, whose target is still 100 */
    release(CORE_BTN_SHIFT);
    wait_ms(400);
    fader(0, P(25));
    EXPECT("");
    CHECK_EQ(led[CORE_LED_ENC(0)], CORE_LED_SLOW); /* move up */
    wait_ms(400); /* the gesture ends: the indication goes off */
    CHECK_EQ(led[CORE_LED_ENC(0)], CORE_LED_OFF);
    fader(0, P(110)); /* crosses 100 */
    EXPECT("B0 14 6E");
}

static void test_pickup_breaks_on_midi_in(void)
{
    setup_test();
    fader(1, P(50));
    EXPECT("B0 15 32");
    midi_in(0xB0, 21, 50); /* an echo of our own value keeps it synchronized */
    fader(1, P(52));
    EXPECT("B0 15 34");
    midi_in(0xB0, 21, 90); /* someone else moved it */
    fader(1, P(55));
    EXPECT("");
    fader(1, P(95)); /* crosses 90 */
    EXPECT("B0 15 5F");
}

static void test_fader_jump_and_inverted(void)
{
    core_config_test(&CFG);
    CFG.preset[0].page[0][5].fader.normal.lo = 127; /* inverted */
    CFG.preset[0].page[0][5].fader.normal.hi = 0;
    CFG.preset[0].page[0][6].fader.normal.mode = CORE_JUMP;
    start();
    fader(5, 0);
    EXPECT("B0 19 7F");
    fader(5, CORE_FADER_MAX);
    EXPECT("B0 19 00");
    midi_in(0xB0, 25, 100);
    fader(5, P(117)); /* value 10, target 100: to raise the value, move the fader down */
    EXPECT("");
    CHECK_EQ(led[CORE_LED_ENC(5)], CORE_LED_FAST);

    midi_in(0xB0, 26, 100);
    fader(6, P(10)); /* jump: sends at once */
    EXPECT("B0 1A 0A");
}

static void test_fader_scale(void)
{
    core_config_test(&CFG);
    CFG.preset[0].page[0][4].fader.normal.mode = CORE_SCALE;
    start();
    midi_in(0xB0, 24, 100);
    fader(4, P(50)); /* first position: no move yet */
    EXPECT("");
    CHECK_EQ(led[CORE_LED_ENC(4)], CORE_LED_SLOW);
    fader(4, P(51)); /* 100 + 1 * 27 / 77 rounds to 100: still one step, never stalls */
    EXPECT("B0 18 65");
    fader(4, P(60)); /* 101 + 9 * 26 / 76 = 104.1 */
    EXPECT("B0 18 68");
    fader(4, P(127)); /* crosses 104: synchronized */
    EXPECT("B0 18 7F");
    fader(4, P(126));
    EXPECT("B0 18 7E");
}

static void test_fader_14bit(void)
{
    core_config_test(&CFG);
    struct core_assign *a = &CFG.preset[0].page[0][2].fader.normal;
    a->msg = CORE_MSG_CC14;
    a->number = 1;
    a->hi = 16383;
    a = &CFG.preset[0].page[0][3].fader.normal;
    a->msg = CORE_MSG_PB;
    a->hi = 16383;
    start();
    fader(2, CORE_FADER_MAX);
    EXPECT("B0 01 7F | B0 21 7F");
    fader(2, 8192);
    EXPECT("B0 01 40 | B0 21 00");
    midi_in(0xB0, 1, 0x10);
    CHECK_EQ(target(CORE_MSG_CC14, 0, 1), 0x10 << 7);
    midi_in(0xB0, 33, 5);
    CHECK_EQ(target(CORE_MSG_CC14, 0, 1), (0x10 << 7) | 5);
    CHECK_EQ(target(CORE_MSG_CC, 0, 1), 0xFFFF); /* CC 1 7-bit is nobody's target */

    fader(3, 0);
    EXPECT("E0 00 00");
    fader(3, CORE_FADER_MAX);
    EXPECT("E0 7F 7F");
    midi_in(0xE0, 0, 0x40);
    CHECK_EQ(target(CORE_MSG_PB, 0, 0), 8192);
}

/* NRPN and RPN faders, bank 16 of the test configuration (channel 16): the full 14-bit position,
 * the parameter number only when the channel's selection is another one or a gesture begins. */
static void test_fader_nrpn(void)
{
    setup_test();
    core_select(&C, 3, 3);
    fader(0, CORE_FADER_MAX); /* NRPN 256: MSB 2, LSB 0 */
    EXPECT("BF 63 02 | BF 62 00 | BF 06 7F | BF 26 7F");
    fader(0, 8192);
    EXPECT("BF 06 40 | BF 26 00");
    fader(0, 8193); /* one position, one value: the LSB moves alone */
    EXPECT("BF 06 40 | BF 26 01");
    fader(1, 100); /* NRPN 257 on the same channel */
    EXPECT("BF 63 02 | BF 62 01 | BF 06 00 | BF 26 64");
    fader(0, 8200); /* back to 256: selected again */
    EXPECT("BF 63 02 | BF 62 00 | BF 06 40 | BF 26 08");
    fader(0, 8201);
    EXPECT("BF 06 40 | BF 26 09");
    wait_ms(400);
    fader(0, 8202); /* a new gesture selects again */
    EXPECT("BF 63 02 | BF 62 00 | BF 06 40 | BF 26 0A");
    fader(6, 16383); /* RPN 1 */
    EXPECT("BF 65 00 | BF 64 01 | BF 06 7F | BF 26 7F");
    fader(7, 0); /* CC 14-bit 27 */
    EXPECT("BF 1B 00 | BF 3B 00");
    CHECK_EQ(target(CORE_MSG_NRPN, 15, 256), 8202);
    CHECK_EQ(target(CORE_MSG_RPN, 15, 1), 16383);

    /* the instrument sends NRPN 257 = 0x10 << 7 | 5: fader 2 waits for it */
    midi_in(0xBF, 99, 2);
    midi_in(0xBF, 98, 1);
    midi_in(0xBF, 6, 0x10);
    CHECK_EQ(target(CORE_MSG_NRPN, 15, 257), 0x10 << 7);
    midi_in(0xBF, 38, 5);
    CHECK_EQ(target(CORE_MSG_NRPN, 15, 257), 0x10 << 7 | 5);
    CHECK_EQ(target(CORE_MSG_NRPN, 15, 256), 8202);
    fader(1, 200);
    EXPECT("");
    CHECK_EQ(led[CORE_BUTTONS + 1], CORE_LED_SLOW);
    fader(1, (0x10 << 7) + 12); /* crosses it: picked up */
    EXPECT("BF 63 02 | BF 62 01 | BF 06 10 | BF 26 0C");
    midi_in(0xBF, 6, 0x10);
    midi_in(0xBF, 38, 5);
    /* an LSB alone keeps the MSB; data after the null parameter goes nowhere */
    midi_in(0xBF, 38, 9);
    CHECK_EQ(target(CORE_MSG_NRPN, 15, 257), 0x10 << 7 | 9);
    midi_in(0xBF, 101, 127);
    midi_in(0xBF, 100, 127);
    midi_in(0xBF, 6, 0);
    CHECK_EQ(target(CORE_MSG_NRPN, 15, 257), 0x10 << 7 | 9);
    CHECK_EQ(target(CORE_MSG_RPN, 15, 1), 16383);
    /* an RPN's LSB alone (101/100 then 38) keeps the number's other half within its kind */
    midi_in(0xBF, 101, 0);
    midi_in(0xBF, 100, 1);
    midi_in(0xBF, 6, 0x20);
    CHECK_EQ(target(CORE_MSG_RPN, 15, 1), 0x20 << 7);
    midi_in(0xBF, 99, 2); /* NRPN MSB: the RPN's LSB does not carry over: NRPN 256 */
    midi_in(0xBF, 6, 0x30);
    CHECK_EQ(target(CORE_MSG_NRPN, 15, 256), 0x30 << 7);
    /* each transport has its own selection */
    midi_in_from(CORE_FROM_BLE, 0xBF, 99, 2);
    midi_in_from(CORE_FROM_BLE, 0xBF, 98, 3);
    midi_in(0xBF, 6, 0x11); /* USB: still NRPN 256 */
    CHECK_EQ(target(CORE_MSG_NRPN, 15, 256), 0x11 << 7);
    CHECK_EQ(target(CORE_MSG_NRPN, 15, 259), 0xFFFF);
    midi_in_from(CORE_FROM_BLE, 0xBF, 6, 0x21);
    CHECK_EQ(target(CORE_MSG_NRPN, 15, 259), 0x21 << 7);
    EXPECT(""); /* nothing echoed */
    unsigned bad = C.stats.bad_events;
    midi_in_from(CORE_TRANSPORTS, 0xBF, 6, 0x22);
    CHECK_EQ(C.stats.bad_events, bad + 1);
    CHECK_EQ(target(CORE_MSG_NRPN, 15, 259), 0x21 << 7);
}

/* NRPN encoders and buttons, and what else ends a channel's selection. */
static void test_nrpn_encoders_and_buttons(void)
{
    core_config_test(&CFG);
    struct core_assign *m = &CFG.preset[0].page[0][0].button[0].normal; /* M1: NRPN 0x1234 */
    m->msg = CORE_MSG_NRPN;
    m->number = 0x1234;
    m->mode = CORE_MOMENTARY;
    m->hi = 8192;
    struct core_assign *f = &CFG.preset[0].page[0][1].fader.normal; /* fader 2: CC 99 */
    f->number = 99;
    CFG.preset[0].page[0][2].fader.normal.number = 6; /* fader 3: CC 6 */
    start();
    tap(0);
    EXPECT("B0 63 24 | B0 62 34 | B0 06 40 | B0 26 00 | B0 06 00 | B0 26 00");
    tap(0); /* every press selects again */
    EXPECT("B0 63 24 | B0 62 34 | B0 06 40 | B0 26 00 | B0 06 00 | B0 26 00");
    CHECK_EQ(led[0], CORE_LED_OFF);
    press(0);
    fader(2, P(5)); /* a plain CC 6 changes the value, not the selection */
    release(0);
    EXPECT("B0 63 24 | B0 62 34 | B0 06 40 | B0 26 00 | B0 06 05 | B0 06 00 | B0 26 00");
    press(0);
    fader(1, P(7)); /* a plain CC 99 does change it */
    release(0);
    EXPECT("B0 63 24 | B0 62 34 | B0 06 40 | B0 26 00 | B0 63 07 | B0 63 24 | B0 62 34 | B0 06 00 | B0 26 00");

    /* bank 16's encoders: NRPN 384 + n from 8192, the 14-bit fine step 7 */
    core_select(&C, 3, 3);
    turn(0, 1);
    EXPECT("BF 63 03 | BF 62 00 | BF 06 40 | BF 26 07");
    after_ms(100);
    turn(0, 1);
    EXPECT("BF 06 40 | BF 26 0E");
    after_ms(100);
    turn(1, -1);
    EXPECT("BF 63 03 | BF 62 01 | BF 06 3F | BF 26 79");
    CHECK_EQ(target(CORE_MSG_NRPN, 15, 385), 8192 - 7);
}

/* paramNull gesture (SPEC Q-21 c): the null parameter, RPN 127/127, when a gesture that sent NRPN
 * or RPN ends; a button's press ends at its release. Off by default. */
static void test_nrpn_null(void)
{
    core_config_test(&CFG);
    CFG.preset[3].param_null = CORE_PARAM_NULL_GESTURE;
    struct core_assign *m = &CFG.preset[3].row[0].normal; /* Play: NRPN 5 toggle, channel 13 */
    m->msg = CORE_MSG_NRPN;
    m->number = 5;
    m->mode = CORE_TOGGLE;
    m->hi = 16383;
    start();
    fader(0, 100); /* preset 1: off */
    wait_ms(400);
    EXPECT("B0 14 01");
    core_select(&C, 3, 3);
    fader(0, 1000);
    fader(1, 2000);
    EXPECT("BF 63 02 | BF 62 00 | BF 06 07 | BF 26 68 | BF 63 02 | BF 62 01 | BF 06 0F | BF 26 50");
    wait_ms(200);
    EXPECT("");
    wait_ms(200); /* both gestures ended: one null for the channel */
    EXPECT("BF 65 7F | BF 64 7F");
    fader(0, 1001); /* a new gesture selects again */
    EXPECT("BF 63 02 | BF 62 00 | BF 06 07 | BF 26 69");
    wait_ms(400);
    EXPECT("BF 65 7F | BF 64 7F");
    fader(7, 0); /* CC 14-bit: no null */
    turn(0, 1); /* an encoder's NRPN gesture: null at its end */
    wait_ms(400);
    EXPECT("BF 1B 00 | BF 3B 00 | BF 63 03 | BF 62 00 | BF 06 40 | BF 26 07 | BF 65 7F | BF 64 7F");
    press(CORE_BTN_PLAY); /* a button: null at the release */
    EXPECT("BC 63 00 | BC 62 05 | BC 06 7F | BC 26 7F");
    release(CORE_BTN_PLAY);
    EXPECT("BC 65 7F | BC 64 7F");
    /* a fader that only waited (pickup) sent nothing: no null */
    midi_in(0xBF, 99, 2);
    midi_in(0xBF, 98, 2);
    midi_in(0xBF, 6, 0x40);
    fader(2, 100);
    wait_ms(400);
    EXPECT("");
}

/* The merge marks (core.h, out_merge; FR-IO-08): a fader's or an absolute encoder's value of a
 * continuous type is one group, a new one each value; selections sent with it are in the group;
 * nulls, buttons, notes, program changes and relative encoders are never marked. */
static void test_merge_marks(void)
{
    core_config_test(&CFG);
    CFG.preset[3].param_null = CORE_PARAM_NULL_GESTURE;
    struct core_strip *s = CFG.preset[0].page[0];
    s[1].fader.normal.msg = CORE_MSG_NOTE;
    s[2].fader.normal.msg = CORE_MSG_PROGRAM;
    s[3].encoder.normal.msg = CORE_MSG_REL_2C;
    s[3].encoder.normal.mode = CORE_ACC0;
    s[4].fader.normal.msg = CORE_MSG_PB;
    start();
    fader(0, P(1)); /* CC */
    MARKS("a");
    fader(0, P(2));
    fader(0, P(3));
    MARKS("ab");
    fader(1, P(50)); /* a note: never */
    fader(2, P(50)); /* a program change: never */
    turn(3, 1);      /* relative: never */
    MARKS("...");
    fader(4, 300); /* pitch bend */
    turn(0, 1);    /* an absolute encoder */
    MARKS("ab");
    tap(0); /* a button (a note toggle) */
    MARKS(".");

    core_select(&C, 3, 3); /* NRPN faders and encoders, 14-bit CC on fader 8, null at the end */
    fader(0, 1000);
    fader(0, 1010);
    fader(1, 2000);
    EXPECT("BF 63 02 | BF 62 00 | BF 06 07 | BF 26 68 | BF 06 07 | BF 26 72 | BF 63 02 | BF 62 01 | BF 06 0F | BF 26 50");
    MARKS("aaaabbcccc");
    fader(0, 1000);
    fader(0, 1010);
    fader(1, 2010);
    wait_ms(400);
    MARKS("aaaabbcccc..");
    fader(7, 0);
    turn(0, 1);
    wait_ms(400);
    MARKS("aabbbb..");
    press(CORE_BTN_PLAY);
    release(CORE_BTN_PLAY);
    MARKS("..");
}

static void test_encoder_acceleration_and_shift(void)
{
    setup_test();
    turn(0, 1); /* default 64, fresh gesture: x1 */
    EXPECT("B0 66 41");
    after_ms(5);
    turn(0, 1); /* 5 ms per click: acc1 x2 */
    EXPECT("B0 66 43");
    after_ms(100);
    turn(0, 1);
    EXPECT("B0 66 44");

    wait_ms(400);
    press(CORE_BTN_SHIFT); /* Shift layer: fine mode, no acceleration */
    turn(0, 1);
    EXPECT("B0 66 45");
    after_ms(5);
    turn(0, 1);
    EXPECT("B0 66 46");
    release(CORE_BTN_SHIFT);

    wait_ms(400);
    turn(0, 1); /* Normal gesture */
    EXPECT("B0 66 47");
    after_ms(5);
    press(CORE_BTN_SHIFT); /* mid-gesture: the layer stays Normal (gesture mode) */
    turn(0, 1);
    EXPECT("B0 66 49");
    release(CORE_BTN_SHIFT);

    wait_ms(400);
    midi_in(0xB0, 102, 126);
    turn(0, 1);
    EXPECT("B0 66 7F");
    wait_ms(400);
    turn(0, 1); /* clamped: nothing */
    EXPECT("");
    after_ms(100);
    turn(0, -1);
    EXPECT("B0 66 7E");
}

static void test_encoder_immediate_layer(void)
{
    core_config_test(&CFG);
    CFG.global.layer_mode = CORE_LAYER_IMMEDIATE;
    start();
    turn(0, 1);
    EXPECT("B0 66 41");
    after_ms(5);
    press(CORE_BTN_SHIFT); /* immediate: fine mode at once */
    turn(0, 1);
    EXPECT("B0 66 42");
    release(CORE_BTN_SHIFT);
}

static void test_encoder_modes(void)
{
    core_config_test(&CFG);
    struct core_strip *s = CFG.preset[0].page[0];
    s[1].encoder.normal.mode = CORE_DIV;
    s[1].encoder.normal.param = 4;
    s[1].encoder.normal.start = 0;
    s[2].encoder.normal.mode = CORE_STEP;
    s[2].encoder.normal.param = 10;
    s[2].encoder.normal.start = 0;
    s[3].encoder.normal.msg = CORE_MSG_REL_2C;
    s[3].encoder.normal.mode = CORE_ACC0;
    s[4].encoder.normal.msg = CORE_MSG_REL_BIN;
    s[4].encoder.normal.mode = CORE_ACC0;
    s[5].encoder.normal.msg = CORE_MSG_REL_SM;
    s[5].encoder.normal.mode = CORE_ACC0;
    s[6].encoder.normal.lo = 127; /* inverted: clockwise lowers the value */
    s[6].encoder.normal.hi = 0;
    s[6].encoder.normal.mode = CORE_ACC0;
    start();

    turn(1, 1);
    turn(1, 1);
    turn(1, 1);
    EXPECT("");
    turn(1, 1); /* 4 clicks: one step */
    EXPECT("B0 67 01");
    turn(1, -2);
    EXPECT("");
    turn(1, -2);
    EXPECT("B0 67 00");

    turn(2, 1);
    EXPECT("B0 68 0A");
    turn(2, 2);
    EXPECT("B0 68 1E");

    turn(3, 1);
    turn(3, -1);
    EXPECT("B0 69 01 | B0 69 7F");
    turn(4, 1);
    turn(4, -1);
    EXPECT("B0 6A 41 | B0 6A 3F");
    turn(5, 1);
    turn(5, -1);
    EXPECT("B0 6B 01 | B0 6B 41");

    turn(6, 1);
    EXPECT("B0 6C 3F"); /* 64 - 1 */
}

/* 14-bit targets: acc0 and fine step by the fine step, the range / 2048 (7 on 0-16383), or 1 with
 * global.fine14 = lsb (FR-CTL-04). */
static void test_encoder_14bit_and_fine(void)
{
    core_config_test(&CFG);
    struct core_control *e = &CFG.preset[0].page[0][0].encoder;
    e->normal.msg = CORE_MSG_CC14;
    e->normal.number = 1;
    e->normal.mode = CORE_ACC0;
    e->normal.hi = 16383;
    e->normal.start = 8192;
    e->shift = e->normal;
    e->shift.mode = CORE_FINE;
    start();
    turn(0, 1); /* 8192 + 7 = 8199: MSB 64, LSB 7 */
    EXPECT("B0 01 40 | B0 21 07");
    after_ms(5);
    turn(0, 1); /* fast, but acc0: still 7, 8206 */
    EXPECT("B0 01 40 | B0 21 0E");
    wait_ms(400);
    press(CORE_BTN_SHIFT);
    turn(0, 1); /* fine: 7, 8213 */
    EXPECT("B0 01 40 | B0 21 15");
    after_ms(5);
    turn(0, -2); /* 8199 */
    EXPECT("B0 01 40 | B0 21 07");
    release(CORE_BTN_SHIFT);

    CFG.global.fine14 = CORE_FINE14_LSB;
    start();
    turn(0, 1); /* 8193 */
    EXPECT("B0 01 40 | B0 21 01");
}

/* Acceleration on a 14-bit target: the speed index s = 255 - the ticks (156.25 us each) of 2/3 of
 * a click, acc2 adds 1.5 x (s - 1) per click to the fine step. */
static void test_encoder_14bit_acceleration(void)
{
    core_config_test(&CFG);
    struct core_control *e = &CFG.preset[0].page[0][0].encoder;
    e->normal.msg = CORE_MSG_CC14;
    e->normal.number = 1;
    e->normal.mode = CORE_ACC2;
    e->normal.hi = 16383;
    e->normal.start = 0;
    struct core_control *e2 = &CFG.preset[0].page[0][1].encoder;
    *e2 = *e;
    e2->normal.number = 2;
    e2->normal.hi = 1000; /* a range below 4095: the speed byte / (4095 / 1000) */
    start();
    turn(0, 1); /* fresh gesture: the fine step, 7 */
    EXPECT("B0 01 00 | B0 21 07");
    after_ms(10);
    turn(0, 1); /* 42 ticks: s = 213, 7 + 1.5 x 212 = 325: 332 */
    EXPECT("B0 01 02 | B0 21 4C");
    after_ms(100);
    turn(0, 1); /* slow: s = 0, the fine step: 339 */
    EXPECT("B0 01 02 | B0 21 53");
    after_ms(10);
    turn(0, -1); /* a reversal, fast: 339 - 325 = 14 */
    EXPECT("B0 01 00 | B0 21 0E");

    turn(1, 1); /* encoder 2, range 0-1000: fine step 1 */
    EXPECT("B0 02 00 | B0 22 01");
    after_ms(10);
    turn(1, 1); /* 213 / 4 = 53: 1 + 1.5 x 52 = 79: 80 */
    EXPECT("B0 02 00 | B0 22 50");
}

/* global.encAccel scales the speed: at 200 % a click every 10 ms counts as one every 5 ms. */
static void test_encoder_speed_scale(void)
{
    setup_test(); /* encoder 1: CC 102, acc1 */
    turn(0, 1);
    EXPECT("B0 66 41");
    after_ms(10);
    turn(0, 1); /* 100 %: 42 ticks, s = 213 < 140 + 78: one step */
    EXPECT("B0 66 42");

    CFG.global.enc_accel = 200;
    start();
    turn(0, 1);
    EXPECT("B0 66 41");
    after_ms(10);
    turn(0, 1); /* 200 %: 21 ticks, s = 234: acc1 step 2 = 2.5 per click: 2, then 3 */
    EXPECT("B0 66 43");
    after_ms(10);
    turn(0, 1);
    EXPECT("B0 66 46");
}

/* An assignment on the preset's channel follows it; one with its own channel keeps it, and its
 * target is its own (D-5). */
static void test_preset_channel(void)
{
    core_config_factory(&CFG);
    CFG.preset[0].channel = 4;
    CFG.preset[0].page[0][0].button[1].normal.channel = 9; /* S1 fixed on channel 10 */
    start();
    tap(0); /* M1: note 72 on the preset's channel, 5 */
    EXPECT("94 48 7F | 94 48 00");
    tap(1); /* S1: note 64 on its own channel */
    EXPECT("99 40 7F | 99 40 00");
    turn(0, 1); /* encoder 1: CC 102, starting at 64 */
    EXPECT("B4 66 41");
    wait_ms(400);
    midi_in(0xB0, 102, 10); /* channel 1: not its target any more */
    midi_in(0xB4, 102, 20);
    turn(0, 1);
    EXPECT("B4 66 15");
    CHECK_EQ(target(CORE_MSG_CC, 4, 102), 21);
    CHECK_EQ(target(CORE_MSG_CC, 0, 102), 0xFFFF); /* not stored */
    core_select(&C, 1, -1); /* preset 2 keeps its own channel, 2 */
    tap(0);
    EXPECT("91 48 7F | 91 48 00");
}

static void test_buttons(void)
{
    setup_test();
    tap(0); /* M1: note 24 toggle */
    EXPECT("90 18 7F");
    CHECK_EQ(led[0], CORE_LED_ON);
    tap(0);
    EXPECT("90 18 00");
    CHECK_EQ(led[0], CORE_LED_OFF);

    press(2); /* R1: note 40 momentary */
    EXPECT("90 28 7F");
    CHECK_EQ(led[2], CORE_LED_ON);
    release(2);
    EXPECT("90 28 00");
    CHECK_EQ(led[2], CORE_LED_OFF);

    tap(CORE_BTN_PLAY); /* the bottom row: notes 84- */
    EXPECT("90 54 7F | 90 54 00");
    tap(CORE_BTN_BT); /* not a MIDI control */
    EXPECT("");

    midi_in(0x90, 24, 100); /* remote: M1's LED follows, and the toggle turns it off */
    CHECK_EQ(led[0], CORE_LED_ON);
    tap(0);
    EXPECT("90 18 00");
    midi_in(0x95, 127, 1); /* nobody's target */
    EXPECT("");
}

static void test_button_held_across_page_switch(void)
{
    setup_test();
    press(2);
    EXPECT("90 28 7F");
    press(CORE_BTN_SHIFT);
    tap(CORE_BTN_DOWN);
    release(CORE_BTN_SHIFT);
    release(2); /* goes to the target of the press, on page 1 */
    EXPECT("90 28 00");
    tap(2);
    EXPECT("91 28 7F | 91 28 00");
}

static void test_button_flags(void)
{
    core_config_test(&CFG);
    struct core_control *row = CFG.preset[0].row;
    row[1].normal.flags = CORE_F_NO_RELEASE;
    row[2].normal.msg = CORE_MSG_PROGRAM;
    row[2].normal.hi = 5;
    row[3].normal.flags = CORE_F_LED_ON;
    row[4].normal.mode = CORE_TRIGGER;
    start();
    CHECK_EQ(led[CORE_BTN_REW], CORE_LED_ON);
    tap(CORE_BTN_STOP);
    EXPECT("90 55 7F");
    tap(CORE_BTN_REC);
    EXPECT("C0 05");
    tap(CORE_BTN_FF);
    EXPECT("90 58 7F");
}

/* FR-CTL-10: a note is released as Note On with velocity 0 unless the preset asks for Note Off;
 * a release goes out in the form of the preset its press was in. */
static void test_note_off_per_preset(void)
{
    core_config_test(&CFG);
    CFG.preset[1].note_off = CORE_NOTE_OFF_8N;
    start();
    tap(2); /* preset 1: R1, note 40 momentary, channel 1 */
    EXPECT("90 28 7F | 90 28 00");
    core_select(&C, 1, -1); /* preset 2 (channel 5 on page 1) sends Note Off */
    tap(2);
    EXPECT("94 28 7F | 84 28 00");
    tap(0); /* M1 toggle: on, then off */
    tap(0);
    EXPECT("94 18 7F | 84 18 00");

    press(2); /* held across a switch to preset 1: the release keeps preset 2's form */
    press(CORE_BTN_SHIFT);
    tap(CORE_BTN_REW);
    release(CORE_BTN_SHIFT);
    CHECK_EQ(C.preset, 0);
    release(2);
    EXPECT("94 28 7F | 84 28 00");
    press(2); /* and the other way round */
    press(CORE_BTN_SHIFT);
    tap(CORE_BTN_FF);
    release(CORE_BTN_SHIFT);
    CHECK_EQ(C.preset, 1);
    release(2);
    EXPECT("90 28 7F | 90 28 00");
}

/* core_notes_off(): every note our output left on ends, in the form of the preset that sent it
 * (before a received configuration replaces this one; ctl.c). */
static void test_notes_off(void)
{
    core_config_test(&CFG);
    CFG.preset[1].note_off = CORE_NOTE_OFF_8N;
    start();
    tap(0);   /* preset 1: M1 toggle on, note 24, channel 1 */
    press(2); /* R1 momentary, note 40: held */
    core_select(&C, 1, -1);
    tap(0);   /* preset 2: M1 toggle on, channel 5, Note Off form 8n */
    EXPECT("90 18 7F | 90 28 7F | 94 18 7F");
    CHECK_EQ(core_notes_off_limit(&C, 1), 1);
    EXPECT("90 18 00");
    CHECK_EQ(core_notes_off_limit(&C, 1), 1);
    EXPECT("90 28 00");
    CHECK_EQ(C.button[2].held, CORE_HELD_OTHER);
    CHECK_EQ(core_notes_off(&C), 1);
    EXPECT("84 18 00");
    CHECK_EQ(core_notes_off(&C), 0); /* forgotten */
    EXPECT("");
    tap(0);   /* cleanup reset the target: the next press turns it on again */
    EXPECT("94 18 7F");
    tap(0);
    EXPECT("84 18 00");
    CHECK_EQ(core_notes_off(&C), 0); /* a Note Off already ended it */
    EXPECT("");
}

static void test_navigation(void)
{
    setup_test();
    press(CORE_BTN_SHIFT);
    CHECK_EQ(led[CORE_BTN_SHIFT], CORE_LED_ON);
    CHECK_EQ(led[CORE_BTN_UP], CORE_LED_ON);  /* page 1 */
    CHECK_EQ(led[CORE_BTN_REW], CORE_LED_ON); /* preset 1 */
    CHECK_EQ(led[CORE_BTN_DOWN], CORE_LED_OFF);
    tap(CORE_BTN_LEFT); /* page 3 */
    CHECK_EQ(C.page, 2);
    CHECK_EQ(led[CORE_BTN_LEFT], CORE_LED_FAST);
    CHECK_EQ(led[CORE_BTN_UP], CORE_LED_OFF);
    wait_ms(1100);
    CHECK_EQ(led[CORE_BTN_LEFT], CORE_LED_ON);
    tap(CORE_BTN_BANK_LEFT); /* preset 3, its page 1 */
    CHECK_EQ(C.preset, 2);
    CHECK_EQ(C.page, 0);
    tap(CORE_BTN_REW); /* preset 1: back to its page 3 */
    CHECK_EQ(C.preset, 0);
    CHECK_EQ(C.page, 2);
    release(CORE_BTN_SHIFT);
    wait_ms(1100);
    CHECK_EQ(led[CORE_BTN_SHIFT], CORE_LED_OFF);
    CHECK_EQ(led[CORE_BTN_LEFT], CORE_LED_OFF);
    EXPECT(""); /* navigation sends nothing */

    tap(CORE_BTN_LEFT); /* without Shift: its own assignment, note 93 */
    EXPECT("90 5D 7F | 90 5D 00");
    fader(0, P(1)); /* preset 1 page 3: channel 3 */
    EXPECT("B2 14 01");

    core_select(&C, 3, 1);
    CHECK_EQ(C.preset, 3);
    CHECK_EQ(C.page, 1);
    core_select(&C, 0, -1);
    CHECK_EQ(C.page, 2);
}

static void test_shift_latch(void)
{
    core_config_test(&CFG);
    CFG.preset[0].shift_mode = CORE_SHIFT_LATCH;
    start();
    tap(CORE_BTN_SHIFT);
    CHECK_EQ(C.shift, 1);
    tap(CORE_BTN_DOWN);
    CHECK_EQ(C.page, 1);
    tap(CORE_BTN_SHIFT);
    CHECK_EQ(C.shift, 0);
}

static void test_shift_hybrid(void)
{
    core_config_test(&CFG);
    CFG.preset[0].shift_mode = CORE_SHIFT_HYBRID;
    start();
    press(CORE_BTN_SHIFT);
    after_ms(100);
    release(CORE_BTN_SHIFT); /* short: latched */
    CHECK_EQ(C.shift, 1);
    press(CORE_BTN_SHIFT); /* turns it off */
    CHECK_EQ(C.shift, 0);
    after_ms(100);
    release(CORE_BTN_SHIFT);
    CHECK_EQ(C.shift, 0);

    press(CORE_BTN_SHIFT);
    wait_ms(500);
    release(CORE_BTN_SHIFT); /* long: temporary */
    CHECK_EQ(C.shift, 0);

    press(CORE_BTN_SHIFT);
    tap(CORE_BTN_DOWN); /* used while held: temporary */
    after_ms(100);
    release(CORE_BTN_SHIFT);
    CHECK_EQ(C.shift, 0);
    CHECK_EQ(C.page, 1);
}

/* FR-SH-01: the mode is the preset's; a hold preset never keeps Shift latched. */
static void test_shift_mode_per_preset(void)
{
    core_config_test(&CFG);
    CFG.preset[1].shift_mode = CORE_SHIFT_LATCH;
    CFG.preset[2].shift_mode = CORE_SHIFT_LATCH;
    start();

    tap(CORE_BTN_SHIFT); /* preset 1 holds: a tap is only a tap */
    CHECK_EQ(C.shift, 0);
    press(CORE_BTN_SHIFT);
    tap(CORE_BTN_FF); /* hold, then preset 2 (latch) while held */
    CHECK_EQ(C.preset, 1);
    release(CORE_BTN_SHIFT); /* the press began in a hold preset: off */
    CHECK_EQ(C.shift, 0);

    tap(CORE_BTN_SHIFT); /* preset 2 latches */
    CHECK_EQ(C.shift, 1);
    tap(CORE_BTN_BANK_LEFT); /* latched Shift + «: preset 3, also latch: stays latched */
    CHECK_EQ(C.preset, 2);
    CHECK_EQ(C.shift, 1);
    tap(CORE_BTN_BANK_RIGHT); /* latched Shift + »: preset 4 holds: the latch is dropped */
    CHECK_EQ(C.preset, 3);
    CHECK_EQ(C.shift, 0);

    core_select(&C, 1, -1); /* back to preset 2 (latch) */
    tap(CORE_BTN_SHIFT);
    CHECK_EQ(C.shift, 1);
    press(CORE_BTN_SHIFT); /* a latch press... */
    CHECK_EQ(C.shift, 0);
    release(CORE_BTN_SHIFT);
    press(CORE_BTN_SHIFT); /* ...latch on, still held, then preset 1 (hold) */
    CHECK_EQ(C.shift, 1);
    core_select(&C, 0, -1);
    CHECK_EQ(C.shift, 1); /* still held: still active */
    release(CORE_BTN_SHIFT); /* released in a hold preset: off */
    CHECK_EQ(C.shift, 0);
}

/* The CCs the factory configuration must not use (core_config_factory.c). */
static bool service_cc(unsigned cc)
{
    return cc <= 2 || (cc >= 4 && cc <= 8) || cc == 10 || cc == 11 || (cc >= 32 && cc <= 69) ||
           cc == 84 || cc == 88 || (cc >= 96 && cc <= 101) || cc >= 120;
}

/* FR-MOD-06, layout C: a channel per preset with 64 different CCs over its pages, none of them a
 * service CC; the buttons on the same channel, as keys that are the same on every page. */
static void test_factory_layout(void)
{
    unsigned free_cc = 0;
    for (unsigned cc = 0; cc < 128; cc++)
        free_cc += !service_cc(cc);
    CHECK_EQ(free_cc, 64); /* so a preset uses every one of them */

    core_config_factory(&CFG);
    for (unsigned k = 0; k < CORE_PRESETS; k++) {
        const struct core_preset *p = &CFG.preset[k];
        uint8_t cc_used[128] = {0}, note_used[128] = {0};
        CHECK_EQ(p->channel, k);
        CHECK_EQ(p->shift_mode, CORE_SHIFT_HOLD);
        CHECK_EQ(p->note_off, CORE_NOTE_OFF_9N);
        for (unsigned pg = 0; pg < CORE_PAGES; pg++)
            for (unsigned n = 0; n < CORE_STRIPS; n++) {
                const struct core_strip *s = &p->page[pg][n];
                const struct core_assign *ctl[2] = {&s->fader.normal, &s->encoder.normal};
                for (unsigned i = 0; i < 2; i++) {
                    CHECK_EQ(ctl[i]->msg, CORE_MSG_CC);
                    CHECK_EQ(ctl[i]->channel, CORE_CHANNEL_PRESET);
                    CHECK_EQ(core_channel(&CFG, k, ctl[i]), k);
                    CHECK(!service_cc(ctl[i]->number));
                    CHECK_EQ(cc_used[ctl[i]->number]++, 0);
                }
                CHECK_EQ(s->encoder.normal.mode, CORE_ACC2);
                CHECK_EQ(s->encoder.shift_kind, CORE_SHIFT_OVERRIDE);
                CHECK_EQ(s->encoder.shift.mode, CORE_FINE);
                CHECK_EQ(core_target_key(&s->encoder.shift, k), core_target_key(&s->encoder.normal, k));
                for (unsigned b = 0; b < 4; b++) {
                    const struct core_assign *a = &s->button[b].normal;
                    CHECK_EQ(a->msg, CORE_MSG_NOTE);
                    CHECK_EQ(a->channel, CORE_CHANNEL_PRESET);
                    CHECK_EQ(a->mode, CORE_MOMENTARY);
                    CHECK_EQ(a->hi, 127);
                    CHECK_EQ(a->number, p->page[0][n].button[b].normal.number); /* every page */
                    if (pg == 0)
                        CHECK_EQ(note_used[a->number]++, 0);
                }
            }
        for (unsigned b = 0; b < CORE_ROW_BUTTONS; b++) {
            const struct core_assign *a = &p->row[b].normal;
            CHECK_EQ(a->msg, CORE_MSG_NOTE);
            CHECK_EQ(a->channel, CORE_CHANNEL_PRESET);
            CHECK_EQ(a->mode, CORE_MOMENTARY);
            CHECK_EQ(note_used[a->number]++, 0); /* not one of the keys */
        }
    }

    start();
    CHECK_EQ(C.stats.targets, 4 * (64 + 32 + 11));
    CHECK_EQ(C.stats.store_full, 0);
    fader(0, P(100)); /* preset 1 page 1: channel 1, CC 20 */
    EXPECT("B0 14 64");
    turn(7, 1); /* encoder 8: CC 109 */
    EXPECT("B0 6D 41");
    press(0); /* M1: a key, note 72 on channel 1 */
    EXPECT("90 48 7F");
    CHECK_EQ(led[0], CORE_LED_ON);
    release(0);
    EXPECT("90 48 00");
    CHECK_EQ(led[0], CORE_LED_OFF);
    press(CORE_BTN_SHIFT);
    tap(CORE_BTN_RIGHT); /* page 4 */
    release(CORE_BTN_SHIFT);
    wait_ms(400);
    fader(0, P(50)); /* page 4 fader 1: CC 87 */
    EXPECT("B0 57 32");
    tap(3); /* SELECT 1, the same key on page 4: note 48 */
    EXPECT("90 30 7F | 90 30 00");
    tap(CORE_BTN_PLAY); /* the bottom row: note 80 */
    EXPECT("90 50 7F | 90 50 00");
    press(CORE_BTN_SHIFT);
    tap(CORE_BTN_BANK_RIGHT); /* preset 4: channel 4 */
    release(CORE_BTN_SHIFT);
    turn(0, 1);
    tap(31); /* SELECT 8: note 55 */
    EXPECT("B3 66 41 | 93 37 7F | 93 37 00");
}

/* FR-CTL-06: a button sends a CC with its own values instead of a note, in any mode. */
static void test_cc_buttons(void)
{
    core_config_test(&CFG);
    struct core_strip *s = CFG.preset[0].page[0];
    s[0].button[0].normal.msg = CORE_MSG_CC; /* M1: CC 80 toggle, on = 64 */
    s[0].button[0].normal.number = 80;
    s[0].button[0].normal.hi = 64;
    s[0].button[1].normal.msg = CORE_MSG_CC; /* S1: CC 81 momentary, 10 / 100 */
    s[0].button[1].normal.number = 81;
    s[0].button[1].normal.mode = CORE_MOMENTARY;
    s[0].button[1].normal.lo = 10;
    s[0].button[1].normal.hi = 100;
    s[0].button[2].normal.msg = CORE_MSG_CC; /* R1: CC 82 trigger, 42 */
    s[0].button[2].normal.number = 82;
    s[0].button[2].normal.mode = CORE_TRIGGER;
    s[0].button[2].normal.hi = 42;
    start();

    tap(0);
    EXPECT("B0 50 40");
    CHECK_EQ(led[0], CORE_LED_ON);
    tap(0);
    EXPECT("B0 50 00");
    CHECK_EQ(led[0], CORE_LED_OFF);
    midi_in(0xB0, 80, 64); /* the synth turns it on: the LED follows, the next press turns it off */
    CHECK_EQ(led[0], CORE_LED_ON);
    tap(0);
    EXPECT("B0 50 00");

    press(1);
    EXPECT("B0 51 64");
    CHECK_EQ(led[1], CORE_LED_ON);
    release(1);
    EXPECT("B0 51 0A");
    CHECK_EQ(led[1], CORE_LED_OFF);

    tap(2);
    tap(2);
    EXPECT("B0 52 2A | B0 52 2A");
}

static void test_store_full_and_bad_events(void)
{
    core_config_test(&CFG);
    for (unsigned k = 0; k < CORE_PRESETS; k++)
        for (unsigned p = 0; p < CORE_PAGES; p++)
            for (unsigned n = 0; n < CORE_STRIPS; n++) {
                struct core_control *f = &CFG.preset[k].page[p][n].fader;
                f->shift = f->normal;
                f->shift.number = (uint8_t)(60 + n);
                f->shift_kind = CORE_SHIFT_OVERRIDE;
            }
    start();
    CHECK_EQ(C.stats.targets, CORE_TARGETS - CORE_TARGETS / 8);
    CHECK_EQ(C.stats.store_full, 16 * 48 + 4 * 11 + 128 - (CORE_TARGETS - CORE_TARGETS / 8));

    event(CORE_EV_BUTTON, 45, 1);
    event(CORE_EV_FADER, 8, 0);
    event(9, 0, 0);
    CHECK_EQ(C.stats.bad_events, 3);
    EXPECT("");

    struct core_event ev;
    memset(&ev, 0, sizeof ev);
    ev.t_us = T;
    ev.type = CORE_EV_BUTTON;
    ev.index = 0;
    ev.value = 1;
    ev.flags = CORE_EV_INJECTED;
    core_event(&C, &ev);
    CHECK_EQ(C.stats.injected, 1);
    EXPECT("90 18 7F");
}

/* D-3: the same events give the same MIDI, LEDs and log, byte for byte. */
static void scripted_run(char *midi, size_t midi_cap, uint8_t *log, size_t *log_len)
{
    setup_test();
    smclog_set_level(SMCLOG_MOD_input, SMCLOG_TRACE);
    smclog_set_level(SMCLOG_MOD_engine, SMCLOG_TRACE);
    fader(0, P(64));
    press(CORE_BTN_SHIFT);
    tap(CORE_BTN_FF);
    release(CORE_BTN_SHIFT);
    for (int i = 0; i < 10; i++) {
        after_ms(7);
        turn(3, 1);
    }
    midi_in(0xB4, 20, 3);
    fader(0, P(10));
    fader(0, P(2));
    tap(1);
    wait_ms(1500);
    snprintf(midi, midi_cap, "%s", out);
    out[0] = 0;
    *log_len = 0;
    size_t n;
    while ((n = smclog_drain(log + *log_len, 8192 - *log_len)) > 0)
        *log_len += n;
}

static void test_deterministic(void)
{
    static char m1[4096], m2[4096];
    static uint8_t l1[8192], l2[8192];
    size_t n1, n2;
    scripted_run(m1, sizeof m1, l1, &n1);
    scripted_run(m2, sizeof m2, l2, &n2);
    CHECK(strlen(m1) > 0);
    CHECK(strcmp(m1, m2) == 0);
    CHECK(n1 > 200);
    CHECK_EQ(n1, n2);
    CHECK(memcmp(l1, l2, n1) == 0);
}

static void test_names(void)
{
    CHECK_EQ(core_find_name('b', "shift"), CORE_BTN_SHIFT);
    CHECK_EQ(core_find_name('b', "sel8"), 31);
    CHECK_EQ(core_find_name('b', "enc1"), -1);
    CHECK_EQ(core_find_name('l', "enc3"), CORE_LED_ENC(2));
    CHECK_EQ(core_find_name('l', "up"), CORE_BTN_UP);
    CHECK_EQ(core_find_name('e', "enc8"), 7);
    CHECK_EQ(core_find_name('f', "fader1"), 0);
    CHECK_EQ(core_find_name('f', "fader9"), -1);
    CHECK_EQ(core_find_name('f', "fader10"), -1);
    CHECK(strcmp(core_button_name(CORE_BTN_LEFT), "left") == 0);
    CHECK(strcmp(core_led_name(CORE_LED_ENC(7)), "enc8") == 0);
    CHECK(core_led_name(CORE_LEDS) == NULL);
}

/* Settings by name (core_settings.h): the simulator's "set" and the device's CORE_SETUP. */
static void test_settings(void)
{
    static struct core_config a, b;
    struct core_settings s;

    core_settings_init(&s, CORE_CONFIG_TEST);
    core_settings_apply(&s, &a);
    core_config_test(&b);
    CHECK(memcmp(&a, &b, sizeof a) == 0); /* nothing changed: the configuration as it is */

    const char text[] = "config factory\nnote_off_p2 8n  shift_mode hybrid\tshift_mode_p3 latch "
                        "gesture_ms 250 hybrid_ms 60000 pickup jump layer_mode immediate channel_p4 16 "
                        "param_null_p3 gesture";
    CHECK_EQ(core_settings_parse(&s, text, sizeof text - 1), 0);
    CHECK_EQ(s.config, CORE_CONFIG_FACTORY);
    CHECK_EQ(s.count, 9);
    core_settings_apply(&s, &a);
    core_config_factory(&b);
    CHECK(memcmp(&a.preset[0].page, &b.preset[0].page, sizeof a.preset[0].page) == 0);
    CHECK_EQ(a.preset[0].note_off, CORE_NOTE_OFF_9N);
    CHECK_EQ(a.preset[1].note_off, CORE_NOTE_OFF_8N);
    CHECK_EQ(a.preset[0].shift_mode, CORE_SHIFT_HYBRID);
    CHECK_EQ(a.preset[2].shift_mode, CORE_SHIFT_LATCH); /* the later one wins */
    CHECK_EQ(a.preset[3].shift_mode, CORE_SHIFT_HYBRID);
    CHECK_EQ(a.global.gesture_ms, 250);
    CHECK_EQ(a.global.hybrid_ms, 60000);
    CHECK_EQ(a.global.pickup, CORE_JUMP);
    CHECK_EQ(a.global.layer_mode, CORE_LAYER_IMMEDIATE);
    CHECK_EQ(a.preset[3].channel, 15);
    CHECK_EQ(a.preset[2].channel, 2);
    CHECK_EQ(a.preset[2].param_null, CORE_PARAM_NULL_GESTURE);
    CHECK_EQ(a.preset[3].param_null, CORE_PARAM_NULL_OFF);

    /* refused: the number of the first pair not taken; the ones before stay */
    static const struct {
        const char *text;
        unsigned bad, count;
    } bad[] = {
        {"note_off 8n config test", 2, 1},          /* config after a setting */
        {"note_off_p5 8n", 1, 0},                   /* no preset 5 */
        {"pickup_p1 jump", 1, 0},                   /* not a per-preset setting */
        {"gesture_ms 0", 1, 0},
        {"gesture_ms 60001", 1, 0},
        {"gesture_ms 12x", 1, 0},
        {"channel 0", 1, 0},
        {"channel 17", 1, 0},
        {"shift_mode", 1, 0},                       /* no value */
        {"layer_mode gesture shift_mode hold note_off", 3, 2},
        {"shift_mode_p2_extra hold", 1, 0},
        {"config_and_a_very_long_name_indeed factory", 1, 0}, /* cut: matches nothing */
        {"config factorynotreally", 1, 0},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        core_settings_init(&s, CORE_CONFIG_TEST);
        CHECK_EQ(core_settings_parse(&s, bad[i].text, strlen(bad[i].text)), bad[i].bad);
        CHECK_EQ(s.count, bad[i].count);
    }
    core_settings_init(&s, CORE_CONFIG_TEST);
    CHECK_EQ(core_settings_parse(&s, "", 0), 0);
    CHECK_EQ(core_settings_parse(&s, "  \n ", 4), 0);
    CHECK_EQ(core_settings_parse(&s, "note_off 8n\0garbage", 19), 0); /* a NUL ends the text */
    CHECK_EQ(s.count, 1);
    for (unsigned i = 1; i < CORE_SETTINGS_MAX; i++)
        CHECK(core_settings_add(&s, "pickup", "scale"));
    CHECK(!core_settings_add(&s, "pickup", "jump")); /* full */
    char big[CORE_SETTINGS_TEXT_MAX + 2];
    memset(big, ' ', sizeof big);
    core_settings_init(&s, CORE_CONFIG_TEST);
    CHECK_EQ(core_settings_parse(&s, big, sizeof big), 1); /* too long */
}

/* Bluetooth (SPEC §5.5): the BT button's long press turns it on and off, at the press's 800 ms
 * whether or not the button is let go; a short press does nothing. Its LED: blinking while no
 * central is connected, lit while one is, off with Bluetooth off. MIDI in from BLE is ignored
 * while it is off; USB's is not. core_set_bluetooth() sets it without telling io. */
static void test_bluetooth(void)
{
    setup_test();
    core_set_bluetooth(&C, -1, false);
    CHECK_EQ(led[CORE_BTN_BT], CORE_LED_OFF);
    press(CORE_BTN_BT); /* no Bluetooth: nothing */
    wait_ms(1000);
    release(CORE_BTN_BT);
    CHECK_EQ(bt_calls, 0);
    CHECK_EQ(C.bt_on, 0);
    CHECK_EQ(led[CORE_BTN_BT], CORE_LED_OFF);
    core_set_bluetooth(&C, 1, false);
    CHECK_EQ(bt_calls, 0);
    CHECK_EQ(led[CORE_BTN_BT], CORE_LED_SLOW);
    event(CORE_EV_LINK, CORE_FROM_BLE, 1);
    CHECK_EQ(led[CORE_BTN_BT], CORE_LED_ON);
    event(CORE_EV_LINK, CORE_FROM_USB, 1); /* only BLE has a link */
    CHECK_EQ(C.stats.bad_events, 1);

    press(CORE_BTN_BT); /* short */
    after_ms(500);
    release(CORE_BTN_BT);
    CHECK_EQ(bt_calls, 0);
    EXPECT("");

    press(CORE_BTN_BT); /* long: on the tick at 800 ms, still held */
    wait_ms(790);
    CHECK_EQ(bt_calls, 0);
    wait_ms(10);
    CHECK_EQ(bt_calls, 1);
    CHECK_EQ(bt_last, 0);
    CHECK_EQ(led[CORE_BTN_BT], CORE_LED_OFF);
    wait_ms(2000); /* held on: once per press */
    release(CORE_BTN_BT);
    CHECK_EQ(bt_calls, 1);
    EXPECT("");

    midi_in_from(CORE_FROM_BLE, 0x90, 0x18, 0x64); /* M1's note: ignored */
    CHECK_EQ(led[CORE_BTN_M], CORE_LED_OFF);
    CHECK_EQ(C.stats.midi_in_off, 1);
    midi_in_from(CORE_FROM_USB, 0x90, 0x18, 0x64);
    CHECK_EQ(led[CORE_BTN_M], CORE_LED_ON);
    CHECK_EQ(C.stats.midi_in, 2);

    press(CORE_BTN_BT); /* long, the release the first event after 800 ms */
    after_ms(1000);
    release(CORE_BTN_BT);
    CHECK_EQ(bt_calls, 2);
    CHECK_EQ(bt_last, 1);
    CHECK_EQ(led[CORE_BTN_BT], CORE_LED_ON); /* the link stayed */
    event(CORE_EV_LINK, CORE_FROM_BLE, 0);
    CHECK_EQ(led[CORE_BTN_BT], CORE_LED_SLOW);
    midi_in_from(CORE_FROM_BLE, 0x80, 0x18, 0x00);
    CHECK_EQ(led[CORE_BTN_M], CORE_LED_OFF);

    core_set_bluetooth(&C, false, false);
    CHECK_EQ(led[CORE_BTN_BT], CORE_LED_OFF);
    CHECK_EQ(bt_calls, 2);
    EXPECT("");
}

/* --- live edits (core_reconfigured(), P-1) ----------------------------------------------------- */

static uint32_t home_slot(uint32_t key)
{
    uint32_t h = key * 2654435761u; /* core.c's slot_of() */
    return (h ^ h >> 16) & (CORE_TARGETS - 1);
}

/* Linear probing's invariant: every entry is reachable from its home slot without an empty one;
 * the count is right. */
static bool store_sound(const struct core *c)
{
    unsigned n = 0;
    for (uint32_t i = 0; i < CORE_TARGETS; i++) {
        if (!c->store[i].key)
            continue;
        n++;
        for (uint32_t j = home_slot(c->store[i].key); j != i; j = (j + 1) & (CORE_TARGETS - 1))
            if (!c->store[j].key)
                return false;
    }
    return n == c->stats.targets;
}

static const struct core_target *stored(const struct core *c, uint32_t key)
{
    for (unsigned i = 0; i < CORE_TARGETS; i++)
        if (c->store[i].key == key)
            return &c->store[i];
    return NULL;
}

/* Edits in place: the store holds exactly the targets of a fresh start, with the values of those
 * that stay; the page, Shift and the LEDs as they were. */
static void test_reconfigured(void)
{
    static struct core fresh;
    static struct core_target before[CORE_TARGETS];
    setup_test();
    for (unsigned f = 0; f < CORE_STRIPS; f++)
        fader(f, 1000 + 1500 * (int)f);
    turn(1, 5);
    midi_in_from(CORE_FROM_USB, 0xB0, 1, 99); /* a value from outside too */
    core_select(&C, 0, 1);
    press(CORE_BTN_SHIFT);
    unsigned leds = led_calls;
    out[0] = 0;
    core_reconfigured(&C); /* nothing changed: nothing changes */
    CHECK(store_sound(&C));
    CHECK_EQ(led_calls, leds);
    EXPECT("");

    uint32_t r = 12345;
    for (unsigned round = 0; round < 40; round++) {
        memcpy(before, C.store, sizeof before);
        for (unsigned k = 0; k < 25; k++) {
            r = r * 1103515245u + 12345u;
            struct core_strip *st = &CFG.preset[r >> 4 & 3].page[r >> 6 & 3][r >> 8 & 7];
            struct core_assign *a = r >> 11 & 1 ? &st->fader.normal : &st->button[r >> 12 & 3].normal;
            if (a->msg == CORE_MSG_CC || a->msg == CORE_MSG_NOTE)
                a->number = (uint16_t)(r >> 16 & 127);
            else if (a->msg == CORE_MSG_NONE && (r >> 13 & 1)) {
                a->msg = CORE_MSG_CC;
                a->number = (uint16_t)(r >> 16 & 127);
                a->hi = 127;
            }
        }
        core_reconfigured(&C);
        CHECK(store_sound(&C));
        core_init(&fresh, &CFG, NULL, NULL);
        CHECK_EQ(fresh.stats.targets, C.stats.targets);
        for (unsigned i = 0; i < CORE_TARGETS; i++)
            if (fresh.store[i].key)
                CHECK(stored(&C, fresh.store[i].key) != NULL);
        for (unsigned i = 0; i < CORE_TARGETS; i++) {
            const struct core_target *t = before[i].key ? stored(&C, before[i].key) : NULL;
            if (t)
                CHECK(t->known == before[i].known && t->value == before[i].value);
        }
    }
    CHECK_EQ(C.preset, 0);
    CHECK_EQ(C.page, 1);
    CHECK(C.shift);
    CHECK_EQ(C.stats.store_full, 0);
}

/* A control's notes end when it changes; a held button of it releases nothing. */
static void test_release_control(void)
{
    setup_test();
    const struct core_control *m1 = &CFG.preset[0].page[0][0].button[CORE_BTN_M];
    CHECK_EQ(m1->normal.msg, CORE_MSG_NOTE);
    press(0);
    out[0] = 0;
    CHECK_EQ(core_release_control(&C, 0, m1), 1);
    char want[32];
    snprintf(want, sizeof want, "90 %02X 00", m1->normal.number);
    EXPECT(want);
    release(0);
    EXPECT("");
    CHECK_EQ(core_release_control(&C, 0, m1), 0); /* nothing on any more */
    CHECK_EQ(core_notes_off(&C), 0);
}

/* Releasing a note before a live edit also resets aliases and the next toggle. */
static void test_cleanup_disarms_inverted_held_note(void)
{
    core_config_test(&CFG);
    struct core_assign *a = &CFG.preset[0].page[0][0].button[CORE_BTN_M].normal;
    a->mode = CORE_MOMENTARY;
    a->channel = CORE_CHANNEL_PRESET;
    a->lo = 127;
    a->hi = 0;
    start();
    press(0);
    EXPECT("90 18 00");
    CHECK_EQ(core_notes_off(&C), 0);
    CFG.preset[0].channel = 1;
    core_reconfigured(&C);
    release(0);
    EXPECT("");
    CHECK_EQ(core_notes_off(&C), 0);
}

static void test_cleanup_resets_target(void)
{
    for (unsigned variant = 0; variant < 4; variant++) {
        unsigned all = variant & 1u;
        core_config_test(&CFG);
        struct core_control *m1 = &CFG.preset[0].page[0][0].button[CORE_BTN_M];
        CFG.preset[0].page[0][1].button[CORE_BTN_M] = *m1;
        start();
        press(0);
        release(0);
        EXPECT("90 18 7F");
        CHECK_EQ(led[0], CORE_LED_ON);
        CHECK_EQ(led[4], CORE_LED_ON);
        CHECK_EQ(all ? core_notes_off(&C) : core_release_control(&C, 0, m1), 1);
        m1->normal.hi = 100;
        if (variant == 2)
            m1->normal.number = 25;
        if (variant == 3)
            CFG.preset[0].channel = m1->normal.channel = 1;
        core_reconfigured(&C);
        EXPECT("90 18 00");
        uint16_t value = 999;
        const struct core_assign *alias = &CFG.preset[0].page[0][1].button[CORE_BTN_M].normal;
        CHECK(core_target_get(&C, core_target_key(alias, 0), &value));
        CHECK_EQ(value, 0);
        CHECK_EQ(led[0], CORE_LED_OFF);
        CHECK_EQ(led[4], CORE_LED_OFF);
        press(0);
        release(0);
        EXPECT(variant == 2 ? "90 19 64" : variant == 3 ? "91 18 64" : "90 18 64");
        core_notes_off(&C);
        EXPECT(variant == 2 ? "90 19 00" : variant == 3 ? "91 18 00" : "90 18 00");
    }
}

int main(void)
{
    RUN(test_init);
    RUN(test_fader_basics);
    RUN(test_pickup_across_pages);
    RUN(test_pickup_breaks_on_midi_in);
    RUN(test_fader_jump_and_inverted);
    RUN(test_fader_scale);
    RUN(test_fader_14bit);
    RUN(test_fader_nrpn);
    RUN(test_nrpn_encoders_and_buttons);
    RUN(test_nrpn_null);
    RUN(test_merge_marks);
    RUN(test_encoder_acceleration_and_shift);
    RUN(test_encoder_immediate_layer);
    RUN(test_encoder_modes);
    RUN(test_encoder_14bit_and_fine);
    RUN(test_encoder_14bit_acceleration);
    RUN(test_encoder_speed_scale);
    RUN(test_preset_channel);
    RUN(test_buttons);
    RUN(test_button_held_across_page_switch);
    RUN(test_button_flags);
    RUN(test_cc_buttons);
    RUN(test_note_off_per_preset);
    RUN(test_notes_off);
    RUN(test_navigation);
    RUN(test_shift_latch);
    RUN(test_shift_hybrid);
    RUN(test_shift_mode_per_preset);
    RUN(test_bluetooth);
    RUN(test_factory_layout);
    RUN(test_settings);
    RUN(test_store_full_and_bad_events);
    RUN(test_deterministic);
    RUN(test_names);
    RUN(test_reconfigured);
    RUN(test_release_control);
    RUN(test_cleanup_resets_target);
    RUN(test_cleanup_disarms_inverted_held_note);
    printf("test_core: %s\n", check_failed ? "FAILED" : "all ok");
    return check_failed ? 1 : 0;
}
