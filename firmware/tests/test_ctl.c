/*
 * Host tests for the control path on the device (firmware/ctl/ctl.c): events from the queue into
 * the core on the factory configuration, MIDI out as USB-MIDI packets, LEDs drawn with their
 * blinking, the clock that never goes back, setups and received configurations. Run: make -C firmware test
 */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "ctl.h"
#include "smclog.h"
#include "smclog_host.h"

int check_failed;

/* evq.h: the platform's lock; one thread here. */
uint32_t input_port_lock(void) { return 0; }
void input_port_unlock(uint32_t state) { (void)state; }

static struct core_config CFG;
static struct evq Q;
static struct midiq OUT;
static struct ctl C;
static unsigned led[CORE_LEDS], led_calls, ready_calls;
static uint32_t T;    /* µs, the host log clock */
static uint32_t STEP; /* how far it moves at each read (0: it stands still) */

static void draw(void *ctx, unsigned i, bool on)
{
    (void)ctx;
    led[i] = on;
    led_calls++;
}

static void ready(void *ctx)
{
    (void)ctx;
    ready_calls++;
}

static const struct ctl_ops OPS = {.led = draw, .midi_ready = ready};

static uint32_t clock_now(void)
{
    uint32_t t = T;
    T += STEP;
    return t;
}

static void start(void)
{
    smclog_host_set_clock(clock_now);
    smclog_reset();
    T = 0;
    STEP = 0;
    memset(led, 0xFF, sizeof led);
    led_calls = ready_calls = 0;
    core_config_factory(&CFG);
    evq_init(&Q);
    midiq_init(&OUT);
    ctl_init(&C, &CFG, &Q, &OUT, &OPS);
}

static void push(uint32_t t_us, uint8_t type, unsigned index, int value)
{
    struct core_event ev;
    memset(&ev, 0, sizeof ev);
    ev.t_us = t_us;
    ev.type = type;
    ev.index = (uint8_t)index;
    ev.value = (int16_t)value;
    CHECK(evq_push(&Q, &ev));
}

static void push_midi(uint32_t t_us, uint8_t s, uint8_t d1, uint8_t d2)
{
    struct core_event ev;
    memset(&ev, 0, sizeof ev);
    ev.t_us = t_us;
    ev.type = CORE_EV_MIDI_IN;
    ev.index = CORE_FROM_USB;
    ev.midi[0] = s;
    ev.midi[1] = d1;
    ev.midi[2] = d2;
    CHECK(evq_push(&Q, &ev));
}

static void run(uint32_t now_us)
{
    T = now_us;
    ctl_run(&C, now_us);
}

/* The packets waiting in OUT as "0B B0 14 64 | ...", taken out. */
static void expect_out(int line, const char *want)
{
    char got[1024] = "";
    const uint8_t *p;
    size_t n = 0;
    while ((p = midiq_peek(&OUT)) != NULL) {
        n += (size_t)snprintf(got + n, sizeof got - n, "%s%02X %02X %02X %02X", n ? " | " : "", p[0],
                              p[1], p[2], p[3]);
        midiq_pop(&OUT);
    }
    if (strcmp(got, want) != 0) {
        check_failed++;
        fprintf(stderr, "test_ctl.c:%d: MIDI out\n  got:  \"%s\"\n  want: \"%s\"\n", line, got, want);
    }
}
#define EXPECT_OUT(want) expect_out(__LINE__, want)

static int P(int v) { return (v * CORE_FADER_MAX + 63) / 127; } /* fader position for a value */

static void test_init_draws_every_led(void)
{
    start();
    CHECK_EQ(led_calls, CORE_LEDS);
    for (unsigned i = 0; i < CORE_LEDS; i++)
        CHECK_EQ(led[i], 0);
    run(1000);
    EXPECT_OUT("");
    CHECK_EQ(ready_calls, 0); /* nothing queued: no wake-up */
    CHECK_EQ(led_calls, CORE_LEDS);
}

static void test_events_to_packets(void)
{
    start();
    push(1000, CORE_EV_FADER, 0, P(100)); /* preset 1 page 1: channel 1, CC 20 */
    push(2000, CORE_EV_BUTTON, 0, 1);     /* M1: the key 72 */
    run(2500);
    EXPECT_OUT("0B B0 14 64 | 09 90 48 7F");
    CHECK_EQ(ready_calls, 1); /* once per round */
    CHECK_EQ(led[0], 1);
    push(3000, CORE_EV_BUTTON, 0, 0);
    push(3000, CORE_EV_ENCODER, 7, 1);    /* encoder 8: CC 109 */
    run(3500);
    EXPECT_OUT("09 90 48 00 | 0B B0 6D 41");
    CHECK_EQ(led[0], 0);
    CHECK_EQ(ready_calls, 2);
    CHECK_EQ(C.stats.events, 4);
    CHECK_EQ(C.core.stats.midi_out, 4);
}

static void test_midi_in_feedback(void)
{
    start();
    push_midi(1000, 0x90, 72, 100); /* a sequencer plays the key under M1: its LED follows */
    run(1500);
    EXPECT_OUT(""); /* nothing echoed */
    CHECK_EQ(led[0], 1);
    push_midi(2000, 0x90, 72, 0);
    run(2500);
    CHECK_EQ(led[0], 0);
    CHECK_EQ(C.core.stats.midi_in, 2);
}

/* FR-PG-03: the selected page's arrow blinks fast for 1 s, lit first; ctl draws the phases. */
static void test_blink(void)
{
    start();
    push(10000, CORE_EV_BUTTON, CORE_BTN_SHIFT, 1);
    push(20000, CORE_EV_BUTTON, CORE_BTN_DOWN, 1); /* page 2 */
    push(30000, CORE_EV_BUTTON, CORE_BTN_DOWN, 0);
    run(30000);
    CHECK_EQ(C.core.page, 1);
    CHECK_EQ(C.led_state[CORE_BTN_DOWN], CORE_LED_FAST);
    CHECK_EQ(led[CORE_BTN_DOWN], 1);
    run(20000 + CTL_BLINK_FAST_US + 1000);
    CHECK_EQ(led[CORE_BTN_DOWN], 0);
    run(20000 + 2 * CTL_BLINK_FAST_US + 1000);
    CHECK_EQ(led[CORE_BTN_DOWN], 1);
    run(20000 + 3 * CTL_BLINK_FAST_US + 1000);
    CHECK_EQ(led[CORE_BTN_DOWN], 0);
    run(20000 + 1100000); /* the flash is over; Shift is still held: the page's arrow is lit */
    CHECK_EQ(C.led_state[CORE_BTN_DOWN], CORE_LED_ON);
    CHECK_EQ(led[CORE_BTN_DOWN], 1);
    unsigned calls = led_calls;
    run(20000 + 1200000);
    CHECK_EQ(led_calls, calls); /* steady: nothing redrawn */
}

static void test_time_never_goes_back(void)
{
    start();
    push(5000, CORE_EV_FADER, 0, P(10));
    push(4000, CORE_EV_BUTTON, 0, 1); /* stamped earlier, queued later */
    run(6000);
    CHECK_EQ(C.stats.time_raised, 1);
    run(5500); /* a tick earlier than the last event */
    CHECK_EQ(C.stats.time_raised, 2);
    CHECK_EQ(C.core.now_us, 6000);

    /* across the wrap of the µs clock: later, not earlier */
    start();
    run(0xFFFFFF00u);
    run(0x00000100u);
    CHECK_EQ(C.stats.time_raised, 0);
    CHECK_EQ(C.core.now_us, 0x100u);
}

/* CORE_SETUP (LOG-35): the core restarts on another configuration in the next round. */
static void test_setup(void)
{
    struct core_settings s;
    start();
    push(1000, CORE_EV_BUTTON, 0, 1); /* M1 on the factory configuration: a key, lit while held */
    run(1500);
    EXPECT_OUT("09 90 48 7F");
    CHECK_EQ(led[0], 1);

    core_settings_init(&s, CORE_CONFIG_FACTORY);
    CHECK_EQ(core_settings_parse(&s, "config test note_off 8n", 23), 0);
    CHECK(ctl_request_setup(&C, &s));
    CHECK(!ctl_request_setup(&C, &s)); /* the first one waits */
    CHECK_EQ(C.stats.setups, 0);
    unsigned calls = led_calls;
    run(2000);
    CHECK_EQ(C.stats.setups, 1);
    CHECK(!C.setup_pending);
    CHECK_EQ(CFG.preset[0].note_off, CORE_NOTE_OFF_8N);
    CHECK_EQ(led[0], 0);               /* the new core's targets are at their defaults */
    CHECK_EQ(led_calls, calls + 1);    /* only the LED that changed was redrawn */
    EXPECT_OUT("09 90 48 00"); /* setup releases the old key before restarting */
    push(3000, CORE_EV_BUTTON, 0, 0);  /* the release of the old press: the new core never saw it */
    push(4000, CORE_EV_BUTTON, 0, 1);  /* M1 on the test configuration: note 24, a toggle */
    push(4100, CORE_EV_BUTTON, 0, 0);
    push(5000, CORE_EV_BUTTON, 0, 1);
    run(5500);
    EXPECT_OUT("09 90 18 7F | 08 80 18 00");
    CHECK(ctl_request_setup(&C, &s)); /* free again */
    CHECK_EQ(C.stats.time_raised, 0);
}

/* A received configuration: applied in the next round, with its ID and the mixer's next revision.
 * An internal decode failure replaces it with the factory configuration. */
static void test_config(void)
{
    static uint8_t blob[CFGBIN_MAX], back[CFGBIN_MAX];
    static struct core_config want, factory;
    struct cfgbin_meta meta = {.revision = 5};
    struct cfgbin_error err;
    start();
    core_config_test(&want);
    core_config_factory(&factory);
    meta.id[0] = 0x11;
    size_t n = cfgbin_encode(&want, &meta, blob, sizeof blob, NULL);
    CHECK(n > CFGBIN_HEADER);
    CHECK(ctl_request_config(&C, blob, (uint32_t)n));
    CHECK(ctl_change_pending(&C));
    CHECK(!ctl_request_config(&C, blob, (uint32_t)n));
    struct core_settings s;
    core_settings_init(&s, CORE_CONFIG_FACTORY);
    CHECK(!ctl_request_setup(&C, &s));
    CHECK_EQ(ctl_encode_config(&C, back, sizeof back, &err), 0); /* busy */
    CHECK_EQ(err.code, CFGBIN_OK);
    run(1000);
    CHECK(!ctl_change_pending(&C));
    CHECK_EQ(C.stats.loads, 1);
    CHECK_EQ(C.source, CTL_SOURCE_RECEIVED);
    CHECK_EQ(C.meta.revision, 1);
    CHECK_EQ(C.meta.id[0], 0x11);
    CHECK(memcmp(&CFG, &want, sizeof CFG) == 0);
    CHECK_EQ(ctl_encode_config(&C, back, sizeof back, &err), n);
    meta.revision = 1;
    CHECK_EQ(cfgbin_encode(&want, &meta, blob, sizeof blob, NULL), n);
    CHECK(memcmp(back, blob, n) == 0);
    CHECK_EQ(ctl_encode_config(&C, back, 10, &err), 0); /* too small */
    CHECK_EQ(err.code, CFGBIN_E_SPACE);

    /* a key held through a received configuration: its Note Off goes out before the change
     * (M1 of the test configuration, now running, sends a note) */
    expect_out(__LINE__, "");
    push(1500, CORE_EV_BUTTON, 0, 1);
    run(1600);
    CHECK(midiq_peek(&OUT) != NULL);
    while (midiq_peek(&OUT))
        midiq_pop(&OUT);
    CHECK(ctl_request_config(&C, blob, (uint32_t)n));
    run(1700);
    CHECK_EQ(C.stats.loads, 2);
    CHECK_EQ(C.meta.revision, 2);
    CHECK(midiq_peek(&OUT) != NULL);
    {
        const uint8_t *p = midiq_peek(&OUT);
        CHECK((p[1] & 0xF0) == 0x80 || ((p[1] & 0xF0) == 0x90 && p[3] == 0)); /* a Note Off */
        midiq_pop(&OUT);
        CHECK(midiq_peek(&OUT) == NULL);
    }
    CHECK_EQ(C.core.stats.midi_out, 0); /* the core restarted after it */
    push(1800, CORE_EV_BUTTON, 0, 0); /* the release: the new core has no press to end */
    run(1900);
    expect_out(__LINE__, "");

    blob[0] = 'X';
    CHECK(ctl_request_config(&C, blob, (uint32_t)n));
    run(2000);
    CHECK_EQ(C.stats.loads, 3);
    CHECK_EQ(C.source, CTL_SOURCE_FACTORY);
    CHECK_EQ(C.meta.revision, 3);
    CHECK(memcmp(&CFG, &factory, sizeof CFG) == 0);
}

/* The last touch of a control (the autosave waits for a quiet panel): not MIDI in; a stored
 * configuration's source. */
static void test_last_touch(void)
{
    struct cfgbin_meta meta = {.revision = 77};
    start();
    CHECK_EQ(C.last_touch_us, 0);
    push_midi(500, 0x90, 1, 2);
    run(1000);
    CHECK_EQ(C.last_touch_us, 0);
    push(2000, CORE_EV_FADER, 1, 100);
    push(2500, CORE_EV_MIDI_IN, 0, 0);
    run(3000);
    CHECK_EQ(C.last_touch_us, 2000);
    push(0, CORE_EV_BUTTON, 0, 1); /* at time 0: still a touch */
    run(4000);
    CHECK_EQ(C.last_touch_us, 1);
    ctl_set_stored(&C, &meta);
    CHECK_EQ(C.source, CTL_SOURCE_STORED);
    CHECK_EQ(C.meta.revision, 77);
}

static void test_revision_wrap(void)
{
    const struct cfgbin_meta meta = {.revision = UINT32_MAX};
    const uint8_t rec[] = {1, 2, 0x40, CORE_SAVE_AUTO};
    start();
    ctl_set_stored(&C, &meta);
    CHECK(ctl_request_edit(&C, 0, rec, sizeof rec, UINT32_MAX));
    CHECK_EQ(C.meta.revision, UINT32_MAX);
    run(1000);
    CHECK_EQ(C.meta.revision, 0);
    CHECK(ctl_request_revert(&C, NULL, 0));
    run(2000);
    CHECK_EQ(C.meta.revision, 1);
}

/* The MIDI queue under load: a round takes events only while at most CTL_MIDI_LOW packets wait;
 * the rest waits in the input queue, the tick too, and the consumer is asked to wake the task. */
static void test_backpressure(void)
{
    start();
    for (unsigned i = 0; i < 20; i++) /* one CC each; nobody takes them yet */
        push(1000 + i, CORE_EV_FADER, 0, P(i % 2 ? 10 : 20));
    run(2000);
    CHECK_EQ(midiq_depth(&OUT), CTL_MIDI_LOW + 1); /* the event that found 8 waiting went in */
    CHECK_EQ(evq_depth(&Q), 20 - (CTL_MIDI_LOW + 1));
    CHECK_EQ(C.stats.midi_held, 1);
    CHECK_EQ(C.stats.midi_refused, 0);
    CHECK_EQ(OUT.refused, 0);
    CHECK(!midiq_room_due(&OUT)); /* still too many waiting */
    midiq_pop(&OUT);
    CHECK(midiq_room_due(&OUT));  /* 8 wait: the control task is woken, once */
    CHECK(!midiq_room_due(&OUT));
    EXPECT_OUT("0B B0 14 0A | 0B B0 14 14 | 0B B0 14 0A | 0B B0 14 14 | 0B B0 14 0A | "
               "0B B0 14 14 | 0B B0 14 0A | 0B B0 14 14");
    run(3000);
    EXPECT_OUT("0B B0 14 0A | 0B B0 14 14 | 0B B0 14 0A | 0B B0 14 14 | 0B B0 14 0A | "
               "0B B0 14 14 | 0B B0 14 0A | 0B B0 14 14 | 0B B0 14 0A");
    CHECK_EQ(C.stats.midi_held, 2);
    run(4000);
    EXPECT_OUT("0B B0 14 14 | 0B B0 14 0A");
    CHECK_EQ(evq_depth(&Q), 0);
    CHECK_EQ(C.stats.midi_held, 2);
    CHECK_EQ(C.stats.events, 20);

    /* the last value wins: while the round is held, a fader's newer positions merge into its
     * waiting event, which takes the newest one's stamp */
    start();
    evq_set_merge(&Q, true);
    for (unsigned i = 0; i < CTL_MIDI_LOW + 1; i++)
        push(1000 + i, CORE_EV_ENCODER, 7, 1); /* encoder 8, one CC each (never merged) */
    run(1100);
    CHECK_EQ(midiq_depth(&OUT), CTL_MIDI_LOW + 1);
    for (unsigned i = 0; i < 30; i++)
        push(2000 + i, CORE_EV_FADER, 0, P(30 + i)); /* fader 1 moves while the round waits */
    run(2100);
    CHECK_EQ(evq_depth(&Q), 1);
    CHECK_EQ(Q.merged, 29);
    CHECK_EQ(C.stats.midi_held, 2);
    while (midiq_peek(&OUT))
        midiq_pop(&OUT);
    STEP = 0;
    run(2500);
    CHECK_EQ(C.lat_core.max_us, 2500 - 2029); /* from the newest position */
    EXPECT_OUT("0B B0 14 3B"); /* 59: its newest position only */
}

/* BLE (docs/ble.md, "Design"): the same messages into the BLE queue with the core's merge marks;
 * Bluetooth off (the BT button's long press) leaves the BLE queue out (FR-IO-04); the BLE queue's
 * backpressure. */
static struct bleq B;
static unsigned ble_ready_calls, bt_calls;
static bool bt_state;
static void ble_ready(void *ctx)
{
    (void)ctx;
    ble_ready_calls++;
}
static void bluetooth(void *ctx, bool on)
{
    (void)ctx;
    bt_state = on;
    bt_calls++;
}
static const struct ctl_ops OPS_BLE = {.led = draw, .midi_ready = ready, .ble_ready = ble_ready,
                                       .bluetooth = bluetooth};

static void expect_ble(int line, const char *want)
{
    char got[2048] = "";
    const struct bleq_msg *e;
    size_t n = 0;
    while ((e = bleq_peek(&B)) != NULL) {
        n += (size_t)snprintf(got + n, sizeof got - n, "%s%02X %02X %02X", n ? " | " : "", e->m[0],
                              e->m[1], e->m[2]);
        bleq_pop(&B);
    }
    if (strcmp(got, want) != 0) {
        check_failed++;
        fprintf(stderr, "test_ctl.c:%d: BLE out\n  got:  \"%s\"\n  want: \"%s\"\n", line, got, want);
    }
}
#define EXPECT_BLE(want) expect_ble(__LINE__, want)

static void start_ble(void)
{
    start();
    ctl_init(&C, &CFG, &Q, &OUT, &OPS_BLE);
    bleq_init(&B);
    ctl_set_ble(&C, &B);
    ble_ready_calls = bt_calls = 0;
}

static void push_ble(uint32_t t_us, uint8_t s, uint8_t d1, uint8_t d2)
{
    struct core_event ev;
    memset(&ev, 0, sizeof ev);
    ev.t_us = t_us;
    ev.type = CORE_EV_MIDI_IN;
    ev.index = CORE_FROM_BLE;
    ev.midi[0] = s;
    ev.midi[1] = d1;
    ev.midi[2] = d2;
    CHECK(evq_push(&Q, &ev));
}

static void test_ble(void)
{
    start_ble();
    push(1000, CORE_EV_FADER, 0, P(100));
    push(2000, CORE_EV_BUTTON, 0, 1);
    push(2100, CORE_EV_FADER, 0, P(50));
    run(2500);
    EXPECT_OUT("0B B0 14 64 | 09 90 48 7F | 0B B0 14 32");
    CHECK_EQ(B.merged, 1); /* the fader's first value replaced */
    const struct bleq_msg *e = bleq_peek(&B);
    CHECK(e && e->timed && e->t_us == 2000);
    EXPECT_BLE("90 48 7F | B0 14 32");
    CHECK_EQ(ready_calls, 1);
    CHECK_EQ(ble_ready_calls, 1);

    /* Bluetooth off: a long press of BT; USB alone, MIDI in from BLE ignored */
    push(3000, CORE_EV_BUTTON, CORE_BTN_BT, 1);
    push(3000 + 900000, CORE_EV_BUTTON, CORE_BTN_BT, 0);
    run(3000 + 900500);
    CHECK_EQ(bt_calls, 1);
    CHECK(!bt_state && !C.core.bt_on);
    push(1000000, CORE_EV_BUTTON, 0, 0);
    push_ble(1000100, 0xB0, 0x14, 0x10);
    run(1000500);
    EXPECT_OUT("09 90 48 00");
    EXPECT_BLE("");
    CHECK_EQ(ble_ready_calls, 1);
    CHECK_EQ(C.core.stats.midi_in_off, 1);
    /* kept over a new configuration (a setup) */
    struct core_settings st;
    memset(&st, 0, sizeof st);
    CHECK(ctl_request_setup(&C, &st));
    run(1100000);
    CHECK(!C.core.bt_on);
    CHECK_EQ(bt_calls, 1);
    /* on again: both */
    push(1200000, CORE_EV_BUTTON, CORE_BTN_BT, 1);
    push(2100000, CORE_EV_BUTTON, CORE_BTN_BT, 0);
    push(2200000, CORE_EV_BUTTON, 0, 1);
    run(2200500);
    CHECK(bt_state && C.core.bt_on);
    CHECK_EQ(bt_calls, 2);
    EXPECT_OUT("09 90 48 7F");
    EXPECT_BLE("90 48 7F");
    CHECK_EQ(ready_calls, 3);
    CHECK_EQ(ble_ready_calls, 2);

    /* backpressure: buttons (never merged) while nobody sends over BLE */
    for (unsigned r = 0; r < 20; r++) {
        for (unsigned i = 0; i < 8; i++)
            push(3000000 + r * 8 + i, CORE_EV_BUTTON, 1, i % 2 ? 0 : 1); /* M2 */
        run(3001000 + r);
        while (midiq_peek(&OUT))
            midiq_pop(&OUT);
        if (C.stats.ble_held)
            break; /* the input queue keeps the rest */
    }
    CHECK(C.stats.ble_held > 0);
    CHECK_EQ(B.refused, 0);
    CHECK(bleq_room(&B) < CTL_UNIT_MAX);
    CHECK(!bleq_room_due(&B));
    bleq_drop_all(&B); /* the sender: no connection */
    CHECK(bleq_room_due(&B));
    run(3002000);
    while (midiq_peek(&OUT))
        midiq_pop(&OUT);
    run(3002100);

    struct metrics_window w;
    memset(&w, 0, sizeof w);
    ctl_take_window(&C, &w);
    CHECK(w.have_ble);
    CHECK_EQ(w.ble_merged, 1);
    CHECK_EQ(w.bleq_refused, 0);
    CHECK(w.bleq_max >= BLEQ_SIZE - CTL_UNIT_MAX);
}

/* LOG-06: each event's wait, the packets' event times, the round's run time, the window. */
/* The panel's state kept across a power-off (ctl.h, ctl_state()): packed after every round,
 * restored before the first. */
static void test_state(void)
{
    start();
    CHECK_EQ(ctl_state(&C), CTL_BT_NONE); /* no Bluetooth in this build, preset 1 page 1 */
    start_ble();
    CHECK_EQ(ctl_state(&C), CTL_STATE_AT_START);
    core_select(&C.core, 3, 1); /* preset 4 page 2 */
    core_select(&C.core, 1, 2); /* preset 2 page 3 */
    run(1000);
    uint32_t w = CTL_BT_ON | 1u << 2 | 2u << 4 | 2u << (8 + 2) | 1u << (8 + 6);
    CHECK_EQ(ctl_state(&C), w);
    push(2000, CORE_EV_BUTTON, CORE_BTN_BT, 1); /* Bluetooth off */
    push(2000 + 900000, CORE_EV_BUTTON, CORE_BTN_BT, 0);
    run(2000 + 900500);
    CHECK_EQ(ctl_state(&C), w & ~3u);

    /* a new start on that word: the same state, each preset's last page too */
    start_ble();
    ctl_restore_state(&C, w & ~3u);
    CHECK(!C.core.bt_on && !C.core.bt_none);
    CHECK_EQ(C.core.preset, 1);
    CHECK_EQ(C.core.page, 2);
    CHECK_EQ(C.core.page_of[3], 1);
    CHECK_EQ(C.core.page_of[0], 0);
    CHECK_EQ(ctl_state(&C), w & ~3u);
    CHECK_EQ(led[CORE_BTN_BT], 0); /* drawn dark */
    run(3000);
    CHECK_EQ(ctl_state(&C), w & ~3u);
    core_select(&C.core, 3, -1); /* preset 4 goes back to its last page */
    CHECK_EQ(C.core.page, 1);

    /* without Bluetooth in the build its part is passed over */
    start();
    ctl_restore_state(&C, w);
    CHECK(C.core.bt_none);
    CHECK_EQ(ctl_state(&C), (w & ~3u) | CTL_BT_NONE);
}

/* The panel as a client follows it (ctl_panel(), P-2) and a selection a client asks for (P-3). */
static void expect_panel(int line, const char *want)
{
    uint8_t b[CTL_PANEL_BYTES];
    char got[64] = "";
    size_t n = 0;
    ctl_panel_bytes(ctl_panel(&C), b);
    for (unsigned i = 0; i < CTL_PANEL_BYTES; i++)
        n += (size_t)snprintf(got + n, sizeof got - n, "%s%02X", i ? " " : "", b[i]);
    if (strcmp(got, want) != 0) {
        check_failed++;
        fprintf(stderr, "test_ctl.c:%d: panel\n  got:  \"%s\"\n  want: \"%s\"\n", line, got, want);
    }
}
#define EXPECT_PANEL(want) expect_panel(__LINE__, want)

static void test_panel(void)
{
    start();
    /* preset page shift bt | touches, then the last touch: preset page control layer */
    EXPECT_PANEL("00 00 00 02 00 FF FF FF FF");
    push(1000, CORE_EV_FADER, 2, 300); /* fader 3: control 2 */
    run(1500);
    EXPECT_PANEL("00 00 00 02 01 00 00 02 00");
    push(2000, CORE_EV_ENCODER, 4, 1); /* encoder 5: 8 + 4 */
    push(2100, CORE_EV_BUTTON, 2 * 4 + 1, 1); /* strip 3's S: group 3, 3 * 8 + 2 */
    run(2500);
    EXPECT_PANEL("00 00 00 02 03 00 00 1A 00");
    push(3000, CORE_EV_BUTTON, 2 * 4 + 1, 0); /* a release is no touch */
    push_midi(3000, 0xB0, 1, 2);              /* nor is MIDI in */
    run(3500);
    EXPECT_PANEL("00 00 00 02 03 00 00 1A 00");
    push(4000, CORE_EV_BUTTON, CORE_BTN_SHIFT, 1); /* Shift is no touch; it shows in the state */
    run(4500);
    EXPECT_PANEL("00 00 01 02 03 00 00 1A 00");
    push(5000, CORE_EV_BUTTON, CORE_BTN_STOP, 1); /* the row's second button, on the Shift layer */
    run(5500);
    EXPECT_PANEL("00 00 01 02 04 00 FF 01 01");
    push(6000, CORE_EV_BUTTON, CORE_BTN_STOP, 0);
    push(6000, CORE_EV_BUTTON, CORE_BTN_SHIFT, 0);
    run(6500);
    EXPECT_PANEL("00 00 00 02 04 00 FF 01 01");

    /* a selection: applied in the next round; a newer one replaces one not applied yet */
    CHECK(!ctl_request_select(&C, 4, 0));
    CHECK(!ctl_request_select(&C, 0, 4));
    CHECK(!ctl_request_select(&C, -2, 0));
    CHECK(ctl_request_select(&C, 1, 1));
    CHECK(ctl_request_select(&C, 2, 3));
    EXPECT_PANEL("00 00 00 02 04 00 FF 01 01");
    run(7000);
    EXPECT_PANEL("02 03 00 02 04 00 FF 01 01");
    CHECK_EQ(C.stats.selects, 1);
    run(7500); /* applied once */
    CHECK_EQ(C.stats.selects, 1);
    CHECK(ctl_request_select(&C, -1, 0)); /* the page alone */
    run(8000);
    EXPECT_PANEL("02 00 00 02 04 00 FF 01 01");
    CHECK(ctl_request_select(&C, 0, -1)); /* a preset alone: its last page */
    run(8500);
    EXPECT_PANEL("00 00 00 02 04 00 FF 01 01");
    CHECK(ctl_request_select(&C, 2, -1));
    run(9000);
    EXPECT_PANEL("02 00 00 02 04 00 FF 01 01");
    push(9500, CORE_EV_FADER, 7, 900); /* where the panel is now: preset 3, page 1 */
    run(10000);
    EXPECT_PANEL("02 00 00 02 05 02 00 07 00");
    CHECK_EQ(ctl_state(&C) >> 2 & 3u, 2); /* the state kept across a power-off follows */

    /* the count wraps */
    for (unsigned i = 0; i < 251; i++) {
        push(11000 + 10 * i, CORE_EV_ENCODER, 0, 1);
        run(11005 + 10 * i);
        while (midiq_peek(&OUT))
            midiq_pop(&OUT);
    }
    EXPECT_PANEL("02 00 00 02 00 02 00 08 00");
}

/* What the core sent, with its source (ctl_sent_read(), P-5), as "first: B0 0E 40 00 00 08 00 | ...". */
static void expect_sent(int line, uint32_t from, unsigned max, const char *want)
{
    uint8_t e[CTL_SENT_RING][CTL_SENT_LEN];
    uint32_t first = 12345;
    char got[2048];
    unsigned n = ctl_sent_read(&C, from, e, max, &first);
    size_t k = (size_t)snprintf(got, sizeof got, "%u:", (unsigned)first);
    for (unsigned i = 0; i < n; i++) {
        k += (size_t)snprintf(got + k, sizeof got - k, "%s", i ? " |" : "");
        for (unsigned b = 0; b < CTL_SENT_LEN; b++)
            k += (size_t)snprintf(got + k, sizeof got - k, " %02X", e[i][b]);
    }
    if (strcmp(got, want) != 0) {
        check_failed++;
        fprintf(stderr, "test_ctl.c:%d: sent\n  got:  \"%s\"\n  want: \"%s\"\n", line, got, want);
    }
}
#define EXPECT_SENT(from, max, want) expect_sent(__LINE__, from, max, want)

static void test_sent(void)
{
    start();
    EXPECT_SENT(CTL_SENT_NOW, 16, "0:");
    EXPECT_SENT(0, 16, "0:");
    push(1000, CORE_EV_BUTTON, 2 * 4 + 1, 1); /* strip 3's S, pressed and released: control 0x1A */
    push(1100, CORE_EV_BUTTON, 2 * 4 + 1, 0);
    push(1200, CORE_EV_FADER, 2, P(100));     /* fader 3: control 2 */
    run(1500);
    EXPECT_OUT("09 90 42 7F | 09 90 42 00 | 0B B0 16 64");
    EXPECT_SENT(0, 16, "0: 90 42 7F 00 00 1A 00 | 90 42 00 00 00 1A 00 | B0 16 64 00 00 02 00");
    EXPECT_SENT(1, 1, "1: 90 42 00 00 00 1A 00");
    EXPECT_SENT(3, 16, "3:");
    EXPECT_SENT(4, 16, "3:"); /* not reached yet */
    EXPECT_SENT(CTL_SENT_NOW, 16, "3:");

    /* a selection and the row on the Shift layer send nothing; an encoder on preset 2, page 2 */
    CHECK(ctl_request_select(&C, 1, 1));
    push(2000, CORE_EV_BUTTON, CORE_BTN_SHIFT, 1);
    push(2100, CORE_EV_BUTTON, CORE_BTN_PLAY + 4, 1);
    push(2200, CORE_EV_BUTTON, CORE_BTN_PLAY + 4, 0);
    push(2300, CORE_EV_BUTTON, CORE_BTN_SHIFT, 0);
    push(2400, CORE_EV_ENCODER, 5, 1); /* encoder 6: control 13 */
    run(2500);
    while (midiq_peek(&OUT))
        midiq_pop(&OUT);
    EXPECT_SENT(3, 16, "3: B1 73 41 01 01 0D 00");

    /* the ring keeps the last 63: a reader further behind skips the oldest */
    for (unsigned i = 0; i < 70; i++) {
        push(3000 + 10 * i, CORE_EV_ENCODER, 0, i & 1 ? -1 : 1);
        run(3005 + 10 * i);
        while (midiq_peek(&OUT))
            midiq_pop(&OUT);
    }
    uint8_t e[CTL_SENT_RING][CTL_SENT_LEN];
    uint32_t first = 0, w = C.sent_w;
    CHECK(w >= 70);
    CHECK_EQ(ctl_sent_read(&C, 0, e, CTL_SENT_RING, &first), CTL_SENT_RING - 1);
    CHECK_EQ(first, w - (CTL_SENT_RING - 1));
    CHECK_EQ(ctl_sent_read(&C, w - 2, e, 16, &first), 2);
    CHECK(first == w - 2 && e[1][0] == 0xB1 && e[1][3] == 1 && e[1][4] == 1 && e[1][5] == 8 && e[1][6] == 0);
}

static void test_metrics(void)
{
    struct metrics_window w;
    uint32_t t = 0;
    start();
    push(1000, CORE_EV_BUTTON, 0, 1);     /* M1: a key */
    push(1200, CORE_EV_FADER, 0, P(100)); /* a CC */
    run(3000);
    CHECK_EQ(C.lat_core.n, 2);
    CHECK_EQ(C.lat_core.min_us, 1800);
    CHECK_EQ(C.lat_core.max_us, 2000);
    CHECK(midiq_peek(&OUT) && midiq_peek_time(&OUT, &t));
    CHECK_EQ(t, 1000);
    midiq_pop(&OUT);
    CHECK(midiq_peek(&OUT) && midiq_peek_time(&OUT, &t));
    CHECK_EQ(t, 1200);
    midiq_pop(&OUT);

    push(5000, CORE_EV_BUTTON, 0, 0); /* stamped later than now: waited 0 */
    run(4000);
    EXPECT_OUT("09 90 48 00");
    CHECK_EQ(C.lat_core.min_us, 0);
    CHECK_EQ(C.busy_us, 0); /* the clock stood still */

    STEP = 10;
    push(5500, CORE_EV_BUTTON, 0, 1);
    run(6000);
    STEP = 0;
    CHECK(C.busy_us >= 10);
    CHECK_EQ(C.lat_core.n, 4);

    memset(&w, 0, sizeof w);
    ctl_take_window(&C, &w);
    CHECK(w.have_traffic);
    CHECK_EQ(w.events, 4);
    CHECK_EQ(w.lat_core.n, 4);
    CHECK_EQ(w.lat_core.max_us, 2000);
    CHECK(w.busy_ctl_us >= 10);
    CHECK_EQ(w.evq_max, 2);
    CHECK_EQ(w.midiq_max, 2);
    CHECK_EQ(w.evq_refused, 0);
    CHECK_EQ(w.midiq_refused, 0);
    CHECK_EQ(C.lat_core.n, 0); /* a new window */
    CHECK_EQ(C.busy_us, 0);
    ctl_take_window(&C, &w);
    CHECK_EQ(w.events, 0);
    CHECK_EQ(w.evq_max, 0);
    CHECK_EQ(w.midiq_max, 1); /* the last key's packet still waits */
}

/* Timer output is additive to the event: sixteen channel nulls then a new NRPN value. */
static void timer_and_input_output(bool button_release)
{
    static struct bleq b;
    for (unsigned with_ble = 0; with_ble < 2; with_ble++) {
        start();
        CFG.global.pickup = CORE_JUMP;
        CFG.preset[0].param_null = CORE_PARAM_NULL_GESTURE;
        for (unsigned i = 0; i < CORE_STRIPS; i++) {
            struct core_strip *s = &CFG.preset[0].page[0][i];
            s->fader.normal.msg = s->encoder.normal.msg = CORE_MSG_NRPN;
            s->fader.normal.channel = (uint8_t)i;
            s->encoder.normal.channel = (uint8_t)(i + CORE_STRIPS);
            s->fader.normal.number = s->encoder.normal.number = 100;
            s->fader.normal.hi = s->encoder.normal.hi = 16383;
        }
        if (button_release) {
            struct core_assign *a = &CFG.preset[0].page[0][0].button[CORE_BTN_M].normal;
            a->msg = CORE_MSG_NRPN;
            a->channel = 0;
            a->number = 200;
            a->mode = CORE_MOMENTARY;
            a->lo = 0;
            a->hi = 16383;
        }
        ctl_init(&C, &CFG, &Q, &OUT, &OPS);
        bleq_init(&b);
        if (with_ble)
            ctl_set_ble(&C, &b);
        if (button_release) {
            push(999, CORE_EV_BUTTON, 0, 1);
            run(999);
            midiq_drop_all(&OUT);
            bleq_drop_all(&b);
        }
        for (unsigned i = 0; i < 16; i++) {
            push(1000 + i, i < 8 ? CORE_EV_FADER : CORE_EV_ENCODER, i % 8, 1);
            run(1000 + i);
            midiq_drop_all(&OUT);
            bleq_drop_all(&b);
        }
        /* Block USB or BLE independently, leaving the event untouched until room returns. */
        if (with_ble) {
            const uint8_t filler[1][3] = {{0xBF, 1, 0}}, mark[1] = {0};
            for (unsigned i = 0; i < BLEQ_SIZE - CTL_UNIT_MAX; i++)
                CHECK(bleq_push_unit(&b, filler, mark, 1, 0, false));
        } else {
            const uint8_t filler[4] = {0x0B, 0xBF, 1, 0};
            for (unsigned i = 0; i <= CTL_MIDI_LOW; i++)
                CHECK(midiq_push(&OUT, filler));
        }
        push(400000, button_release ? CORE_EV_BUTTON : CORE_EV_FADER, 0, button_release ? 0 : 16383);
        run(400000);
        CHECK_EQ(evq_depth(&Q), 1);
        CHECK_EQ(C.core.stats.midi_out, button_release ? 68 : 64);
        midiq_drop_all(&OUT);
        bleq_drop_all(&b);
        /* Admission at exactly the USB low watermark and BLE reservation. */
        unsigned ble_filler = with_ble ? BLEQ_SIZE - 1u - CTL_UNIT_MAX : 0;
        const uint8_t ble_msg[1][3] = {{0xBF, 1, 0}}, mark[1] = {0};
        for (unsigned i = 0; i < ble_filler; i++)
            CHECK(bleq_push_unit(&b, ble_msg, mark, 1, 0, false));
        const uint8_t filler[4] = {0x0B, 0xBF, 1, 0};
        for (unsigned i = 0; i < CTL_MIDI_LOW; i++)
            CHECK(midiq_push(&OUT, filler));
        run(400000);
        CHECK_EQ(C.stats.midi_refused, 0);
        CHECK_EQ(C.stats.ble_refused, 0);
        const unsigned messages = button_release ? 38 : 36;
        CHECK_EQ(midiq_depth(&OUT), CTL_MIDI_LOW + messages);
        CHECK_EQ(bleq_depth(&b), with_ble ? ble_filler + messages : 0);
        CHECK_EQ(evq_depth(&Q), 0);
        for (unsigned i = 0; i < CTL_MIDI_LOW; i++)
            midiq_pop(&OUT);
        for (unsigned i = 0; i < ble_filler; i++)
            bleq_pop(&b);
        for (unsigned i = 0; i < messages; i++) {
            const uint8_t *p = midiq_peek(&OUT);
            CHECK(p != NULL);
            if (!p)
                break;
            unsigned ch = i < 32 ? i / 4 + (i % 4 >= 2 ? 8 : 0) : 0;
            unsigned cc = i < 32 || i >= 36 ? (i % 2 ? 100 : 101) :
                          i == 32 ? 99 : i == 33 ? 98 : i == 34 ? 6 : 38;
            unsigned value = i < 32 || i >= 36 ? 127 :
                             i == 32 ? (button_release ? 1 : 0) :
                             i == 33 ? (button_release ? 72 : 100) : button_release ? 0 : 127;
            CHECK_EQ(p[1], 0xB0u | ch);
            CHECK_EQ(p[2], cc);
            CHECK_EQ(p[3], value);
            uint32_t stamp = 0;
            CHECK(midiq_peek_time(&OUT, &stamp));
            CHECK_EQ(stamp, 400000);
            if (with_ble) {
                const struct bleq_msg *m = bleq_peek(&b);
                CHECK(m && memcmp(m->m, p + 1, 3) == 0);
                CHECK(m && m->timed && m->t_us == 400000);
                if (m)
                    bleq_pop(&b);
            }
            midiq_pop(&OUT);
        }
        CHECK_EQ(midiq_depth(&OUT), 0);
        CHECK_EQ(bleq_depth(&b), 0);
    }
}

static void test_timer_and_input_output(void)
{
    timer_and_input_output(false);
}

/* The exact 38-message bound: 32 timer nulls, NRPN release select/data, release null. */
static void test_timer_and_button_release_output(void)
{
    timer_and_input_output(true);
}

/* Configuration replacement must drain accumulated notes before forgetting the old core. */
static void cleanup_backpressure(unsigned change)
{
    static struct bleq b;
    for (unsigned notes = 33; notes <= 128; notes += 95) {
        for (unsigned block_ble = 0; block_ble < 2; block_ble++) {
            start();
            for (unsigned pg = 0; pg < CORE_PAGES; pg++)
                for (unsigned i = 0; i < 32; i++) {
                    struct core_assign *a = &CFG.preset[0].page[pg][i / 4].button[i % 4].normal;
                    a->msg = CORE_MSG_NOTE;
                    a->channel = 0;
                    a->number = (uint16_t)(pg * 32 + i);
                    a->mode = CORE_TOGGLE;
                }
            ctl_init(&C, &CFG, &Q, &OUT, &OPS);
            bleq_init(&b);
            ctl_set_ble(&C, &b);
            for (unsigned i = 0; i < notes; i++) {
                CHECK(ctl_request_select(&C, 0, (int)(i / 32)));
                push(1000 + 2 * i, CORE_EV_BUTTON, i % 32, 1);
                push(1001 + 2 * i, CORE_EV_BUTTON, i % 32, 0);
                run(1001 + 2 * i);
                midiq_drop_all(&OUT);
                bleq_drop_all(&b);
            }
            unsigned old_events = C.stats.events;
            if (block_ble) {
                const uint8_t msg[1][3] = {{0xBF, 1, 0}}, mark[1] = {0};
                for (unsigned i = 0; i < BLEQ_SIZE - 1; i++)
                    CHECK(bleq_push_unit(&b, msg, mark, 1, 0, false));
            } else {
                const uint8_t pkt[4] = {0x0B, 0xBF, 1, 0};
                for (unsigned i = 0; i < MIDIQ_SIZE - 1; i++)
                    CHECK(midiq_push(&OUT, pkt));
            }
            static uint8_t blob[CFGBIN_MAX];
            struct core_settings settings;
            core_settings_init(&settings, CORE_CONFIG_FACTORY);
            if (change == 0)
                CHECK(ctl_request_revert(&C, NULL, 0));
            else if (change == 1) {
                size_t len = cfgbin_encode(&CFG, NULL, blob, sizeof blob, NULL);
                CHECK(len > 0);
                CHECK(ctl_request_config(&C, blob, (uint32_t)len));
            } else if (change == 2)
                CHECK(ctl_request_setup(&C, &settings));
            else {
                const uint8_t preset[] = {2, 2, 0, 0};
                CHECK(ctl_request_edit(&C, 0, preset, sizeof preset, 1));
            }
            push(2000, CORE_EV_ENCODER, 0, 1);
            run(2000);
            CHECK(ctl_change_pending(&C));
            CHECK_EQ(C.stats.events, old_events);
            CHECK_EQ(C.stats.loads, 0);
            CHECK_EQ(C.stats.setups, 0);
            CHECK_EQ(C.stats.edits, 0);
            CHECK_EQ(C.stats.midi_refused, 0);
            CHECK_EQ(C.stats.ble_refused, 0);
            midiq_drop_all(&OUT);
            bleq_drop_all(&b);
            unsigned usb_off = 0, ble_off = 0;
            bool seen_usb[128] = {0}, seen_ble[128] = {0};
            for (unsigned round = 0; round < 16; round++) {
                run(2100 + round);
                const uint8_t *p;
                while ((p = midiq_peek(&OUT))) {
                    if (p[1] == 0x90 && !p[3]) {
                        CHECK(p[2] < notes && !seen_usb[p[2]]);
                        seen_usb[p[2]] = true;
                        usb_off++;
                    }
                    midiq_pop(&OUT);
                }
                const struct bleq_msg *m;
                while ((m = bleq_peek(&b))) {
                    if (m->m[0] == 0x90 && !m->m[2]) {
                        CHECK(m->m[1] < notes && !seen_ble[m->m[1]]);
                        seen_ble[m->m[1]] = true;
                        ble_off++;
                    }
                    bleq_pop(&b);
                }
                if (!ctl_change_pending(&C))
                    break;
                CHECK_EQ(C.stats.events, old_events);
                CHECK_EQ(C.stats.loads, 0);
            }
            CHECK(!ctl_change_pending(&C));
            CHECK_EQ(usb_off, notes);
            CHECK_EQ(ble_off, notes);
            CHECK_EQ(C.stats.loads, change < 2 ? 1 : 0);
            CHECK_EQ(C.stats.setups, change == 2 ? 1 : 0);
            CHECK_EQ(C.stats.edits, change == 3 ? 1 : 0);
            CHECK_EQ(C.stats.midi_refused, 0);
            CHECK_EQ(C.stats.ble_refused, 0);
        }
    }
}

static void test_config_cleanup_backpressure(void)
{
    for (unsigned change = 0; change < 4; change++)
        cleanup_backpressure(change);
}

static void test_edit_batch_cleanup(void)
{
    start_ble();
    uint8_t rec[20 * 6];
    for (unsigned i = 0; i < 20; i++) {
        struct core_control *ctl = &CFG.preset[0].page[0][i / 4].button[i % 4];
        ctl->normal.number = (uint16_t)i;
        ctl->normal.channel = 0;
        ctl->normal.mode = CORE_TOGGLE;
        ctl->shift_kind = CORE_SHIFT_OVERRIDE;
        ctl->shift = ctl->normal;
        ctl->shift.number = (uint16_t)(i + 20);
        const uint8_t none[] = {3, 4, 0, (uint8_t)((2 + i % 4) * 8 + i / 4), 0, 0};
        memcpy(rec + i * 6, none, sizeof none);
    }
    ctl_init(&C, &CFG, &Q, &OUT, &OPS_BLE);
    ctl_set_ble(&C, &B);
    for (unsigned layer = 0; layer < 2; layer++) {
        if (layer) {
            push(1500, CORE_EV_BUTTON, CORE_BTN_SHIFT, 1);
            run(1500);
        }
        for (unsigned i = 0; i < 20; i++) {
            push(1000 + layer * 1000 + i * 2, CORE_EV_BUTTON, i, 1);
            push(1001 + layer * 1000 + i * 2, CORE_EV_BUTTON, i, 0);
            run(1001 + layer * 1000 + i * 2);
            midiq_drop_all(&OUT);
            bleq_drop_all(&B);
        }
    }
    CHECK(ctl_request_edit(&C, 0, rec, sizeof rec, 7));
    unsigned off = 0;
    for (unsigned round = 0; round < 4; round++) {
        run(3000 + round);
        CHECK_EQ(midiq_depth(&OUT), bleq_depth(&B));
        while (midiq_peek(&OUT)) {
            const uint8_t *p = midiq_peek(&OUT);
            const struct bleq_msg *m = bleq_peek(&B);
            CHECK(p[1] == 0x90 && p[3] == 0 && p[2] == off / 2 + (off % 2 ? 20 : 0));
            CHECK(m && memcmp(m->m, p + 1, 3) == 0);
            off++;
            midiq_pop(&OUT);
            bleq_pop(&B);
        }
        if (!ctl_change_pending(&C))
            break;
        CHECK_EQ(C.stats.edits, 0);
        CHECK_EQ(CFG.preset[0].page[0][0].button[0].normal.msg, CORE_MSG_NOTE);
    }
    CHECK_EQ(off, 40);
    CHECK(!ctl_change_pending(&C));
    CHECK_EQ(C.stats.edits, 1);
    CHECK_EQ(C.meta.revision, 1);
    CHECK_EQ(C.stats.midi_refused, 0);
    CHECK_EQ(C.stats.ble_refused, 0);
}

static void test_timing(void)
{
    start();
    memset(&C.timing, 0, sizeof C.timing);
    STEP = 10; /* every clock read 10 µs on */
    push(1000, CORE_EV_BUTTON, 0, 1);
    push(1100, CORE_EV_FADER, 0, P(100));
    push(1200, CORE_EV_FADER, 1, P(50));
    run(2000);
    run(3000); /* a round with no event */
    STEP = 0;
    const struct ctl_timing *m = &C.timing;
    CHECK_EQ(m->events, 3);
    CHECK_EQ(m->faders, 2);
    CHECK(m->ev_max_us >= 10 && m->fader_max_us >= 10);
    CHECK(m->ev_sum_us >= 3 * 10 && m->fader_sum_us >= 2 * 10);
    CHECK(m->io_sum_us >= 3 * 10); /* three packets queued */
    CHECK_EQ(m->busy_rounds, 1);
    CHECK_EQ(m->round_events_max, 3);
    CHECK(m->round_max_us >= m->ev_sum_us);
    CHECK_EQ(m->idle_rounds, 2);
    CHECK(m->idle_max_us >= 10);
    ctl_log_timing(&C);
    CHECK_EQ(C.timing.events, 0); /* a new window */
    CHECK_EQ(C.timing.busy_rounds, 0);
    CHECK_EQ(C.timing.idle_rounds, 0);
    while (midiq_peek(&OUT))
        midiq_pop(&OUT);
}

/* BT waiting for a central blinks at half the slow rate (2 a second): 4 a second annoyed the
 * owner (2026-11-23). The encoders' pickup indication keeps the slow rate. */
static void test_bt_blink(void)
{
    start_ble();
    run(1000);
    CHECK(C.core.bt_on && !C.core.bt_link);
    CHECK_EQ(C.led_state[CORE_BTN_BT], CORE_LED_SLOW);
    uint32_t t0 = C.led_since[CORE_BTN_BT];
    run(t0 + CTL_BLINK_SLOW_US + 1000); /* the slow half period is over: BT still lit */
    CHECK_EQ(led[CORE_BTN_BT], 1);
    run(t0 + CTL_BLINK_BT_US + 1000);
    CHECK_EQ(led[CORE_BTN_BT], 0);
    run(t0 + 2 * CTL_BLINK_BT_US + 1000);
    CHECK_EQ(led[CORE_BTN_BT], 1);
    CHECK_EQ(CTL_BLINK_BT_US, 2 * CTL_BLINK_SLOW_US);
}

int main(void)
{
    RUN(test_init_draws_every_led);
    RUN(test_events_to_packets);
    RUN(test_midi_in_feedback);
    RUN(test_blink);
    RUN(test_time_never_goes_back);
    RUN(test_backpressure);
    RUN(test_ble);
    RUN(test_bt_blink);
    RUN(test_setup);
    RUN(test_config);
    RUN(test_last_touch);
    RUN(test_revision_wrap);
    RUN(test_state);
    RUN(test_panel);
    RUN(test_sent);
    RUN(test_metrics);
    RUN(test_timer_and_input_output);
    RUN(test_timer_and_button_release_output);
    RUN(test_config_cleanup_backpressure);
    RUN(test_edit_batch_cleanup);
    RUN(test_timing);
    printf("test_ctl: %s\n", check_failed ? "FAILED" : "all ok");
    return check_failed ? 1 : 0;
}
