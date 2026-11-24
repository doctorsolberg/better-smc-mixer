/*
 * Host tests of the scan layer, the event queue and input injection (firmware/input/,
 * docs/hwtest.md). A simulated matrix plays the switches and encoders behind the 595s, with the
 * stock pipeline (scan.h): each step it answers with the rows of the word latched at the step
 * before, the way PB0-PB3 would, then latches the word shifted at the step before.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "evq.h"
#include "inject.h"
#include "scan.h"
#include "smclog.h"
#include "smclog_host.h"

int check_failed;

uint32_t input_port_lock(void) { return 0; }
void input_port_unlock(uint32_t state) { (void)state; }

/* --- a simulated matrix ---------------------------------------------------------------------- */

static uint8_t closed[SCAN_COLS]; /* bit r: the switch at (column, row r) is closed */
static uint32_t shifted, latched; /* the 595 shift register and outputs */
static uint32_t now;

/* The stock column table's index of the one column bit word w selects (bit 24 + x for x < 8, bit
 * 16 + x - 8 for the rest), or SCAN_COLS for none or several. */
static unsigned table_index(uint32_t w)
{
    uint32_t sel = ~w & 0xFFFF0000u;
    if (sel == 0 || (sel & (sel - 1)))
        return SCAN_COLS;
    unsigned bit = (unsigned)__builtin_ctz(sel);
    return bit >= 24 ? bit - 24 : bit - 16 + 8;
}

/* The column, in the scan's numbering, that word w puts on the rows (scan.h: read as x + 1). */
static unsigned column_of(uint32_t w)
{
    unsigned x = table_index(w);
    return x < SCAN_COLS ? (x + 1) % SCAN_COLS : SCAN_COLS;
}

static uint8_t rows_now(void)
{
    unsigned c = column_of(latched);
    uint8_t low = c < SCAN_COLS ? closed[c] : 0;
    return (uint8_t)(~low & 0x0Fu); /* pull-ups: closed reads low */
}

/* One step of the platform's interrupt: read, latch, shift. */
static void step(struct scan *sc)
{
    now += SCAN_STEP_US;
    uint8_t rows = rows_now();
    latched = shifted;
    shifted = scan_step(sc, now, rows);
    scan_pass(sc, now);
}

static void pass(struct scan *sc)
{
    for (unsigned i = 0; i < SCAN_COLS; i++)
        step(sc);
}

static void set_switch(uint8_t pos, bool on)
{
    if (on)
        closed[pos >> 4] |= (uint8_t)(1u << (pos & 0x0Fu));
    else
        closed[pos >> 4] &= (uint8_t)~(1u << (pos & 0x0Fu));
}

/* Encoder n's lines (row 0 of columns 2n+1 and 2n+2, 0-based n). */
static void set_encoder(unsigned n, unsigned ab)
{
    set_switch((uint8_t)(((2 * n + 1) % SCAN_COLS) << 4), (ab >> 1) & 1u);
    set_switch((uint8_t)(((2 * n + 2) % SCAN_COLS) << 4), ab & 1u);
}

/* --- a sink that records events and can refuse ------------------------------------------------ */

#define MAX_EV 256 /* the fader test collects about 100 */
static struct core_event got[MAX_EV];
static unsigned n_got;
static bool refuse;

static bool sink(void *ctx, const struct core_event *ev)
{
    (void)ctx;
    if (refuse)
        return false;
    if (n_got < MAX_EV)
        got[n_got] = *ev;
    n_got++;
    return true;
}

static struct scan s;

/* The defaults with debounce 2, which the button tests count passes against. */
static struct scan_params test_params(void)
{
    struct scan_params p = scan_default_params;
    p.debounce = 2;
    return p;
}

static void fresh(const struct scan_params *p)
{
    struct scan_params d = test_params();
    memset(closed, 0, sizeof closed);
    n_got = 0;
    refuse = false;
    scan_init(&s, p ? p : &d, sink, NULL);
    shifted = latched = 0xFFFFFFFFu; /* what the platform latches at start: nothing selected */
    /* The first step reads column 15 with nothing selected; from then on, every pass() reads
     * columns 0-15 and ends with the evaluation. The first full pass primes. */
    step(&s);
    pass(&s);
    n_got = 0;
}

/* --- tests ------------------------------------------------------------------------------------ */

static void test_word(void)
{
    /* the stock column table: bits 24-31 for columns 0-7, 16-23 for 8-15; LED rows active low */
    CHECK_EQ(scan_word(0, 0), 0xFEFFFFFFu);
    CHECK_EQ(scan_word(7, 0), 0x7FFFFFFFu);
    CHECK_EQ(scan_word(8, 0x1234), 0xFFFEEDCBu);
    CHECK_EQ(scan_word(15, 0xFFFF), 0xFF7F0000u);
    CHECK_EQ(scan_word(16, 0), scan_word(0, 0));
    for (unsigned c = 0; c < SCAN_COLS; c++)
        CHECK_EQ(table_index(scan_word(c, 0)), c);
    fresh(NULL);
    /* the scan walks the columns in order, one selected at a time, and the rows it stores as
     * column c are those of the word for c - 1 */
    for (unsigned i = 0; i < 2 * SCAN_COLS; i++) {
        unsigned before = table_index(shifted);
        CHECK(before < SCAN_COLS);
        CHECK_EQ(column_of(latched), s.col);
        step(&s);
        CHECK_EQ(table_index(shifted), (before + 1) % SCAN_COLS);
    }
}

static void test_every_button(void)
{
    fresh(NULL);
    for (unsigned i = 0; i < SCAN_BUTTONS; i++) {
        set_switch(scan_button_pos[i], true);
        pass(&s);
        CHECK_EQ(n_got, 0); /* debounce: not after one pass */
        pass(&s);
        CHECK_EQ(n_got, 1);
        CHECK_EQ(got[0].type, CORE_EV_BUTTON);
        CHECK_EQ(got[0].index, i);
        CHECK_EQ(got[0].value, 1);
        CHECK_EQ(got[0].flags, 0);
        set_switch(scan_button_pos[i], false);
        pass(&s);
        pass(&s);
        CHECK_EQ(n_got, 2);
        CHECK_EQ(got[1].index, i);
        CHECK_EQ(got[1].value, 0);
        n_got = 0;
    }
    CHECK_EQ(s.stats.buttons, 2 * SCAN_BUTTONS);
    /* every position is distinct and in rows 1-3 */
    for (unsigned i = 0; i < SCAN_BUTTONS; i++) {
        CHECK((scan_button_pos[i] & 0x0Fu) >= 1 && (scan_button_pos[i] & 0x0Fu) <= 3);
        for (unsigned j = 0; j < i; j++)
            CHECK(scan_button_pos[i] != scan_button_pos[j]);
    }
}

static void test_bounce_and_chord(void)
{
    fresh(NULL);
    set_switch(scan_button_pos[CORE_BTN_PLAY], true);
    pass(&s);
    set_switch(scan_button_pos[CORE_BTN_PLAY], false); /* gone before it counted */
    pass(&s);
    pass(&s);
    CHECK_EQ(n_got, 0);
    CHECK_EQ(s.stats.bounces, 1);
    /* Shift and a strip button together: two presses in index order */
    set_switch(scan_button_pos[CORE_BTN_SHIFT], true);
    set_switch(scan_button_pos[5], true);
    pass(&s);
    pass(&s);
    CHECK_EQ(n_got, 2);
    CHECK_EQ(got[0].index, 5);
    CHECK_EQ(got[1].index, CORE_BTN_SHIFT);
    /* debounce 1: the first pass counts */
    struct scan_params p = scan_default_params;
    p.debounce = 1;
    fresh(&p);
    set_switch(scan_button_pos[0], true);
    pass(&s);
    CHECK_EQ(n_got, 1);
}

static void test_encoders(void)
{
    static const uint8_t cw[4] = {0, 1, 3, 2}; /* one direction of Gray code */
    fresh(NULL);
    /* Two passes a transition: one a pass is the scan's limit, where a change between the two
     * reads of a pair shows both lines at once (test_encoder_fast_turn has a realistic speed). */
    for (unsigned n = 0; n < SCAN_ENCODERS; n++) {
        n_got = 0;
        for (unsigned k = 1; k <= 8; k++) { /* 8 transitions: 4 clicks */
            set_encoder(n, cw[k % 4]);
            pass(&s);
            pass(&s);
        }
        CHECK_EQ(n_got, 4);
        for (unsigned i = 0; i < n_got && i < MAX_EV; i++) {
            CHECK_EQ(got[i].type, CORE_EV_ENCODER);
            CHECK_EQ(got[i].index, n);
        }
        int sum = 0;
        for (unsigned i = 0; i < n_got && i < MAX_EV; i++)
            sum += got[i].value;
        CHECK_EQ(sum, 4); /* cw[] is clockwise on the device (docs/hwtest.md, run of 2026-11-11) */
        /* and back */
        n_got = 0;
        for (int k = 7; k >= 0; k--) { /* 8 transitions the other way, back to 00 */
            set_encoder(n, cw[k % 4]);
            pass(&s);
            pass(&s);
        }
        sum = 0;
        for (unsigned i = 0; i < n_got && i < MAX_EV; i++)
            sum += got[i].value;
        CHECK_EQ(sum, -4);
    }
    CHECK_EQ(s.stats.enc_invalid, 0);
    CHECK_EQ(s.stats.buttons, 0); /* row 0 is not a button row */

    /* a jump (both lines at once) is counted and moves nothing */
    n_got = 0;
    set_encoder(0, 3);
    pass(&s);
    CHECK_EQ(s.stats.enc_invalid, 1);
    CHECK_EQ(n_got, 0);

}

/* A fast turn: a transition every 18 steps (900 us), at every phase against the pass. Encoder 8's
 * lines are columns 15 and 0, read at the two ends of a pass; paired across the pass boundary
 * they are a step apart like the others', so no transition is lost or read backwards. */
static void test_encoder_fast_turn(void)
{
    static const uint8_t cw[4] = {0, 1, 3, 2};
    for (unsigned n = 0; n < SCAN_ENCODERS; n += SCAN_ENCODERS - 1) {
        for (unsigned phase = 0; phase < 18; phase++) {
            fresh(NULL);
            for (unsigned i = 0; i < phase; i++)
                step(&s);
            for (unsigned k = 1; k <= 40; k++) { /* 40 transitions: 20 clicks */
                set_encoder(n, cw[k % 4]);
                for (unsigned i = 0; i < 18; i++)
                    step(&s);
            }
            pass(&s);
            int sum = 0;
            for (unsigned i = 0; i < n_got && i < MAX_EV; i++)
                sum += got[i].value;
            CHECK_EQ(sum, 20);
            CHECK_EQ(s.stats.enc_invalid, 0);
        }
    }
}

/* A bouncing contact in a fast turn: one line drops back for a pass between two forward
 * transitions; the next skipped state still goes forward, by the sum of recent transitions. */
static void test_encoder_skip_after_bounce(void)
{
    fresh(NULL);
    static const uint8_t seq[] = {0, 1, 3, 1, 3, 2, 0, 1, 2, 0}; /* set_encoder: 11 bounces once,
                                                                  * then 10 -> 01 skips 11 */
    for (unsigned k = 0; k < sizeof seq; k++) {
        set_encoder(6, seq[k]);
        pass(&s);
    }
    int sum = 0;
    for (unsigned i = 0; i < n_got && i < MAX_EV; i++)
        sum += got[i].value;
    CHECK_EQ(sum, 4); /* 8 transitions forward net: 0-1-3-2-0 and 0-1-(3)-2-0 */
    CHECK_EQ(s.stats.enc_inferred, 1);
}

/* A fast turn where the state with both lines closed is shorter than a pass: the decoder sees
 * 10 -> 01 (both lines at once) and, the encoder having just turned, counts the skipped state. At
 * rest the same jump is lost. */
static void test_encoder_skip_inferred(void)
{
    static const uint8_t skip11[3] = {0, 1, 2}; /* set_encoder's 00, 10 then 01: 11 skipped */
    fresh(NULL);
    for (unsigned k = 1; k <= 12; k++) { /* 4 cycles, each 3 passes: 4 transitions a cycle */
        set_encoder(6, skip11[k % 3]);
        pass(&s);
    }
    int sum = 0;
    for (unsigned i = 0; i < n_got && i < MAX_EV; i++)
        sum += got[i].value;
    CHECK_EQ(sum, 8); /* 16 transitions: 8 clicks clockwise */
    CHECK_EQ(s.stats.enc_inferred, 4);
    CHECK_EQ(s.stats.enc_invalid, 0);
    /* and back: the other way, 01 -> 10 skipping 11 too */
    n_got = 0;
    for (int k = 11; k >= 0; k--) {
        set_encoder(6, skip11[k % 3]);
        pass(&s);
    }
    sum = 0;
    for (unsigned i = 0; i < n_got && i < MAX_EV; i++)
        sum += got[i].value;
    /* The turn back: the first jump still goes the old way (+2), the second meets a sum of 0 and
     * is lost, the rest go back: -1 +2 -1, -1 . -1, then -4 a cycle twice: 10 transitions back,
     * 5 clicks. */
    CHECK_EQ(sum, -5);
    CHECK_EQ(s.stats.enc_invalid, 1);
    /* at rest, a jump moves nothing */
    for (unsigned i = 0; i < SCAN_ENC_INFER_PASSES + 2; i++)
        pass(&s);
    n_got = 0;
    set_encoder(6, 3); /* from 00: both lines at once */
    pass(&s);
    CHECK_EQ(n_got, 0);
    CHECK_EQ(s.stats.enc_invalid, 2);
}

/* The encoder probe reads only its encoder's two columns, each every 2 steps; back in the full
 * scan, nothing it left behind turns into an event. */
static void test_encoder_probe(void)
{
    static const uint8_t cw[4] = {0, 1, 3, 2};
    fresh(NULL);
    s.raw_mode = SCAN_RAW_MATRIX | (7u << SCAN_RAW_PROBE_SHIFT); /* encoder 7: columns 13, 14 */
    for (unsigned i = 0; i < 4; i++)
        step(&s);
    set_encoder(6, 2);
    step(&s);
    step(&s);
    step(&s);
    step(&s); /* each column read again within 4 steps, 2 of them the pipeline's */
    CHECK_EQ(s.raw[13] & 1u, 1u);
    CHECK_EQ(s.raw[14] & 1u, 0u);
    set_encoder(6, 1);
    for (unsigned i = 0; i < 4; i++)
        step(&s);
    CHECK_EQ(s.raw[13] & 1u, 0u);
    CHECK_EQ(s.raw[14] & 1u, 1u);
    set_switch(scan_button_pos[0], true); /* column 1: not read in the probe */
    for (unsigned i = 0; i < 40; i++)
        step(&s);
    CHECK_EQ(s.raw[1], 0u);
    CHECK_EQ(n_got, 0);

    s.raw_mode = SCAN_RAW_MATRIX; /* the full scan again */
    for (unsigned i = 0; i < 8; i++)
        pass(&s);
    CHECK_EQ(n_got, 1); /* the button, and no encoder step from the probe's leftovers */
    CHECK_EQ(got[0].type, CORE_EV_BUTTON);
    CHECK_EQ(got[0].index, 0);
    n_got = 0;
    for (unsigned k = 2; k <= 3; k++) { /* from 01 on: two transitions clockwise, one click */
        set_encoder(6, cw[k % 4]);
        pass(&s);
        pass(&s);
    }
    CHECK_EQ(n_got, 1);
    CHECK_EQ(got[0].type, CORE_EV_ENCODER);
    CHECK_EQ(got[0].value, 1);
}

static void test_encoder_direction_reverses(void)
{
    struct scan_params p = scan_default_params;
    int v[2];
    for (unsigned rev = 0; rev < 2; rev++) {
        p.enc_reverse = (uint8_t)(rev << 7);
        fresh(&p);
        set_encoder(7, 1); /* encoder 8: columns 15 and 0 */
        pass(&s);
        pass(&s);
        set_encoder(7, 3);
        pass(&s);
        pass(&s);
        CHECK_EQ(n_got, 1);
        v[rev] = got[0].value;
    }
    CHECK_EQ(v[0], -v[1]);
}

static void test_refused_events_retry(void)
{
    fresh(NULL);
    refuse = true;
    set_switch(scan_button_pos[3], true);
    set_encoder(1, 1);
    pass(&s);
    set_encoder(1, 3);
    pass(&s);
    pass(&s);
    CHECK_EQ(n_got, 0);
    CHECK(s.stats.refused >= 2);
    refuse = false;
    pass(&s);
    CHECK_EQ(n_got, 2); /* the press and the click, not lost */
    CHECK_EQ(got[0].type, CORE_EV_BUTTON);
    CHECK_EQ(got[1].type, CORE_EV_ENCODER);
}

static void test_faders(void)
{
    fresh(NULL);
    scan_fader(&s, 1, 0, 512); /* 10-bit mid: sent at once */
    CHECK_EQ(n_got, 1);
    CHECK_EQ(got[0].type, CORE_EV_FADER);
    CHECK_EQ(got[0].value, 512 << 4);
    for (unsigned i = 0; i < 20; i++) /* noise inside the hysteresis */
        scan_fader(&s, 2 + i, 0, (uint16_t)(512 + (i & 1)));
    CHECK_EQ(n_got, 1);
    for (unsigned i = 0; i < 40; i++) /* a move: the filter follows, a few events */
        scan_fader(&s, 30 + i, 0, 700);
    CHECK(n_got >= 2);
    int off = got[n_got - 1].value - (700 << 4); /* settles within the hysteresis */
    CHECK(off >= -scan_default_params.fader_hysteresis && off <= scan_default_params.fader_hysteresis);
    for (unsigned i = 0; i < 40; i++) /* the ends are reached exactly */
        scan_fader(&s, 80 + i, 0, 1023);
    CHECK_EQ(got[n_got - 1].value, CORE_FADER_MAX);
    for (unsigned i = 0; i < 60; i++)
        scan_fader(&s, 130 + i, 0, 0);
    CHECK_EQ(got[n_got - 1].value, 0);
    unsigned before = n_got;
    scan_fader(&s, 200, 9, 100); /* no fader 9 */
    CHECK_EQ(n_got, before);
}

/* The pass is evaluated by scan_pass(), after the last column's word went out; one the caller
 * leaves is evaluated at the next step. */
static void test_pass_after_shift(void)
{
    fresh(NULL);
    pass(&s);
    pass(&s); /* primed */
    set_switch(scan_button_pos[0], true);
    for (unsigned i = 0; i < SCAN_COLS; i++) { /* steps alone */
        now += SCAN_STEP_US;
        latched = shifted;
        shifted = scan_step(&s, now, rows_now());
    }
    CHECK_EQ(n_got, 0);
    CHECK_EQ(s.pass_due, 1);
    scan_pass(&s, now);
    CHECK_EQ(s.pass_due, 0);
    scan_pass(&s, now); /* nothing more */
    for (unsigned i = 0; i < scan_default_params.debounce; i++)
        pass(&s);
    CHECK(n_got >= 1);
    CHECK_EQ(got[n_got - 1].type, CORE_EV_BUTTON);
    CHECK_EQ(got[n_got - 1].value, 1);
    set_switch(scan_button_pos[0], false);
    unsigned before = n_got;
    for (unsigned i = 0; i < SCAN_COLS; i++) { /* the caller forgets scan_pass() */
        now += SCAN_STEP_US;
        latched = shifted;
        shifted = scan_step(&s, now, rows_now());
    }
    for (unsigned k = 0; k < 4; k++) {
        now += SCAN_STEP_US;
        latched = shifted;
        shifted = scan_step(&s, now, rows_now()); /* the next step runs the pass first */
        for (unsigned i = 1; i < SCAN_COLS; i++) {
            now += SCAN_STEP_US;
            latched = shifted;
            shifted = scan_step(&s, now, rows_now());
        }
    }
    CHECK(n_got > before);
    CHECK_EQ(got[n_got - 1].type, CORE_EV_BUTTON);
    CHECK_EQ(got[n_got - 1].value, 0);
}

/* A fader parked at its top end, with the noise seen on the mixer (run 19: raw 1018..1023): the
 * filtered value dips under the end's snap now and then, which must not send it back and forth. */
static void test_fader_parked_at_end(void)
{
    static const uint16_t noise[] = {1023, 1022, 1018, 1019, 1018, 1021, 1022, 1023, 1018, 1019,
                                     1020, 1018, 1022, 1023, 1023, 1018, 1018, 1019, 1021, 1022};
    fresh(NULL);
    for (unsigned i = 0; i < 40; i++)
        scan_fader(&s, i, 0, 1023);
    CHECK_EQ(n_got, 1);
    CHECK_EQ(got[0].value, CORE_FADER_MAX);
    for (unsigned r = 0; r < 50; r++)
        for (unsigned i = 0; i < sizeof noise / sizeof noise[0]; i++)
            scan_fader(&s, 100 + r * 20 + i, 0, noise[i]);
    CHECK_EQ(n_got, 1);
    for (unsigned i = 0; i < 40; i++) /* a real move away from the end is still sent */
        scan_fader(&s, 2000 + i, 0, 1000);
    CHECK(n_got >= 2);
    CHECK(got[n_got - 1].value < CORE_FADER_MAX - 2 * scan_default_params.fader_hysteresis);
}

/* The ADC's wrong bit decisions near mid-scale (docs/hwtest.md, "The faders measured"): single
 * samples 10-55 counts off. The median of 3 drops them, so a fader at rest sends nothing. */
static void test_fader_outliers(void)
{
    static const uint16_t rec[] = {502, 501, 502, 512, 502, 499, 502, 479, 501, 502,
                                   447, 502, 501, 495, 502, 503, 501, 512, 502, 500};
    fresh(NULL);
    for (unsigned i = 0; i < 40; i++)
        scan_fader(&s, i, 0, 502);
    CHECK_EQ(n_got, 1);
    for (unsigned r = 0; r < 50; r++)
        for (unsigned i = 0; i < sizeof rec / sizeof rec[0]; i++)
            scan_fader(&s, 100 + r * 20 + i, 0, rec[i]);
    CHECK_EQ(n_got, 1);
    /* bursts of them: up to 5 in a row, as recorded on the controller (2026-11-11) */
    for (unsigned len = 2; len <= 5; len++)
        for (unsigned r = 0; r < 10; r++)
            for (unsigned i = 0; i < 60; i++)
                scan_fader(&s, 2000 + len * 1000 + r * 60 + i, 0, (uint16_t)(i < len ? 512 : 502));
    CHECK_EQ(n_got, 1);
    /* a real jump still arrives, within the gate's samples and the filter's */
    for (unsigned i = 0; i < SCAN_FADER_GATE_SAMPLES + 40; i++)
        scan_fader(&s, 9000 + i, 0, 700);
    CHECK(n_got >= 2);
    CHECK(got[n_got - 1].value > (700 - 6) << 4);
}

/* Directional hysteresis: from rest a fader must move 5 counts; then every count its way is sent,
 * a turn back takes 5 counts again, and after a rest so does going on. */
static void test_fader_hysteresis(void)
{
    const int h = scan_default_params.fader_hysteresis >> 4; /* in ADC counts: 5 */
    fresh(NULL);
    unsigned t = 0;
    for (unsigned i = 0; i < 40; i++)
        scan_fader(&s, t++, 0, 400);
    CHECK_EQ(n_got, 1);
    unsigned v = 400, first_move = 0;
    for (unsigned step = 1; step <= 20; step++) { /* up a count every 30 samples: a slow move */
        v++;
        for (unsigned i = 0; i < 30; i++)
            scan_fader(&s, t++, 0, (uint16_t)v);
        if (n_got > 1 && !first_move)
            first_move = step;
    }
    CHECK(first_move >= (unsigned)h && first_move <= (unsigned)h + 1);
    CHECK(n_got >= 1 + 20 - (unsigned)h - 1); /* then one event a count */
    for (unsigned i = 1; i < n_got && i < MAX_EV; i++)
        CHECK(got[i].value > got[i - 1].value); /* never back while it goes up */
    unsigned before = n_got;
    for (unsigned step = 1; step < (unsigned)h - 1; step++) { /* back by less than the hysteresis */
        v--;
        for (unsigned i = 0; i < 30; i++)
            scan_fader(&s, t++, 0, (uint16_t)v);
    }
    CHECK_EQ(n_got, before);
    for (unsigned step = 0; step < 4; step++) { /* and on: past it */
        v--;
        for (unsigned i = 0; i < 30; i++)
            scan_fader(&s, t++, 0, (uint16_t)v);
    }
    CHECK(n_got > before);
    CHECK(got[n_got - 1].value < got[before - 1].value);
    /* at rest the way is forgotten: a count on is not sent */
    for (unsigned i = 0; i < SCAN_FADER_REST_SAMPLES + 10; i++)
        scan_fader(&s, t++, 0, (uint16_t)v);
    before = n_got;
    for (unsigned i = 0; i < 60; i++)
        scan_fader(&s, t++, 0, (uint16_t)(v - 1));
    CHECK_EQ(n_got, before);
}

static void test_leds(void)
{
    fresh(NULL);
    for (unsigned i = 0; i < SCAN_LEDS; i++)
        for (unsigned j = 0; j < i; j++)
            CHECK(scan_led_pos[i] != scan_led_pos[j]);
    /* the LED walk of 2026-11-11: encoder 1's LED is the stock table's last, encoder 8's its
     * 46th */
    CHECK_EQ(scan_led_pos[SCAN_BUTTONS], 0x61);
    CHECK_EQ(scan_led_pos[SCAN_LEDS - 1], 0x53);
    scan_led(&s, CORE_BTN_SHIFT, true);
    uint8_t pos = scan_led_pos[CORE_BTN_SHIFT];
    CHECK_EQ(s.led_rows[pos & 0x0Fu], 1u << (pos >> 4));
    scan_led(&s, CORE_BTN_SHIFT, false);
    CHECK_EQ(s.led_rows[pos & 0x0Fu], 0);
    for (unsigned i = 0; i < SCAN_LEDS; i++)
        scan_led(&s, i, true);
    unsigned lit = 0;
    for (unsigned c = 0; c < SCAN_LED_COLS; c++)
        lit += (unsigned)__builtin_popcount(s.led_rows[c]);
    CHECK_EQ(lit, SCAN_LEDS);
    scan_leds_clear(&s);
    CHECK_EQ(s.led_rows[0] | s.led_rows[1] | s.led_rows[2] | s.led_rows[3], 0);

    /* Each column its own rows; then two full LED cycles through the pipeline. What lights after
     * a step: the column the platform drives (led_drive) with the rows latched at that step. */
    for (unsigned c = 0; c < SCAN_LED_COLS; c++) {
        scan_led_at(&s, c, c, true);
        scan_led_at(&s, 15 - c, c, true);
    }
    step(&s); /* from here on, every latched word was made with this frame */
    unsigned blanks = 0, shown[SCAN_LED_COLS] = {0};
    int last = s.led_drive;
    for (unsigned i = 0; i < 2u * SCAN_LED_COLS * s.p.led_steps; i++) {
        step(&s);
        int d = s.led_drive;
        uint16_t rows_lit = (uint16_t)~latched;
        CHECK_EQ((uint16_t)~shifted, d < 0 ? 0 : s.led_rows[d]); /* the word carries d's rows */
        if (d < 0) {
            blanks++;
        } else {
            CHECK(rows_lit == 0 || rows_lit == s.led_rows[d]); /* never another column's rows */
            if (rows_lit)
                shown[d]++;
            if (last >= 0)
                CHECK_EQ(d, last); /* a column is followed by a blank, never by a column */
        }
        last = d;
    }
    CHECK_EQ(blanks, 2u * SCAN_LED_COLS * s.p.led_blank);
    for (unsigned c = 0; c < SCAN_LED_COLS; c++) /* the first step of a column still shows blank */
        CHECK_EQ(shown[c], 2u * (s.p.led_steps - s.p.led_blank - 1u));
}

static void test_raw_mode_logs(void)
{
    fresh(NULL);
    struct smclog_stats a, b;
    smclog_get_stats(&a);
    set_switch(scan_button_pos[0], true);
    pass(&s);
    smclog_get_stats(&b);
    CHECK_EQ(b.records, a.records); /* off: nothing */
    s.raw_mode = SCAN_RAW_MATRIX;
    set_switch(scan_button_pos[1], true);
    pass(&s);
    smclog_get_stats(&a);
    CHECK(a.records > b.records);
    pass(&s); /* no change: no record */
    smclog_get_stats(&b);
    CHECK_EQ(b.records, a.records);
    scan_fader(&s, now, 3, 77); /* the faders are not asked for */
    smclog_get_stats(&a);
    CHECK_EQ(a.records, b.records);
    s.raw_mode = SCAN_RAW_FADERS;
    set_switch(scan_button_pos[2], true);
    pass(&s); /* the matrix is not asked for */
    smclog_get_stats(&b);
    CHECK_EQ(b.records, a.records);
    scan_fader(&s, now, 3, 78);
    smclog_get_stats(&a);
    CHECK(a.records > b.records);
}

static void test_queue(void)
{
    static struct evq q;
    evq_init(&q);
    struct core_event ev = {.type = CORE_EV_BUTTON}, out;
    for (unsigned i = 0; i < EVQ_SIZE - 1; i++) {
        ev.index = (uint8_t)i;
        CHECK(evq_push(&q, &ev));
    }
    CHECK(!evq_push(&q, &ev));
    CHECK_EQ(q.refused, 1);
    CHECK_EQ(evq_depth(&q), EVQ_SIZE - 1);
    CHECK_EQ(q.depth_max, EVQ_SIZE - 1);
    for (unsigned i = 0; i < EVQ_SIZE - 1; i++) {
        CHECK(evq_pop(&q, &out));
        CHECK_EQ(out.index, i);
    }
    CHECK(!evq_pop(&q, &out));
    CHECK(evq_sink(&q, &ev)); /* wraps around */
    CHECK(evq_pop(&q, &out));
}

static void test_queue_merge(void)
{
    static struct evq q;
    struct core_event f = {.type = CORE_EV_FADER, .index = 2}, b = {.type = CORE_EV_BUTTON}, out;

    evq_init(&q);
    f.t_us = 100;
    f.value = 10;
    CHECK(evq_push(&q, &f));
    CHECK(q.was_empty);
    f.t_us = 200;
    f.value = 20;
    CHECK(evq_push(&q, &f)); /* merging off: queued */
    CHECK(!q.was_empty);
    CHECK_EQ(evq_depth(&q), 2);

    evq_init(&q);
    evq_set_merge(&q, true);
    f.t_us = 100, f.value = 10;
    CHECK(evq_push(&q, &f));
    f.index = 5, f.t_us = 150, f.value = 50;
    CHECK(evq_push(&q, &f));
    f.index = 2, f.t_us = 200, f.value = 20;
    CHECK(evq_push(&q, &f)); /* into fader 2's waiting event */
    CHECK_EQ(evq_depth(&q), 2);
    CHECK_EQ(q.merged, 1);
    CHECK(evq_push(&q, &b)); /* a button ends merging for what came before it */
    f.t_us = 300, f.value = 30;
    CHECK(evq_push(&q, &f));
    CHECK_EQ(evq_depth(&q), 4);
    f.index = 2, f.value = 31;
    f.flags = CORE_EV_INJECTED; /* never merged */
    CHECK(evq_push(&q, &f));
    f.flags = 0;
    CHECK_EQ(evq_depth(&q), 5);

    CHECK(evq_pop(&q, &out));
    CHECK_EQ(out.index, 2);
    CHECK_EQ(out.value, 20);   /* the newest position */
    CHECK_EQ(out.t_us, 200);   /* and its stamp: latency counts from the position sent */
    CHECK(evq_pop(&q, &out));
    CHECK_EQ(out.index, 5);
    CHECK(evq_pop(&q, &out));
    CHECK_EQ(out.type, CORE_EV_BUTTON);
    CHECK(evq_pop(&q, &out));
    CHECK_EQ(out.value, 30);
    CHECK(evq_pop(&q, &out));
    CHECK_EQ(out.value, 31);
    CHECK(!evq_pop(&q, &out));

    /* taken: the next event of that fader queues anew */
    f.value = 40;
    CHECK(evq_push(&q, &f));
    CHECK(q.was_empty);
    CHECK(evq_pop(&q, &out));
    f.value = 41;
    CHECK(evq_push(&q, &f));
    CHECK_EQ(evq_depth(&q), 1);
    CHECK(evq_pop(&q, &out));
    CHECK_EQ(out.value, 41);

    /* a full queue still merges */
    evq_init(&q);
    evq_set_merge(&q, true);
    f.index = 0, f.value = 1;
    CHECK(evq_push(&q, &f));
    for (unsigned i = 1; i < EVQ_SIZE - 1; i++)
        CHECK(evq_push(&q, &b));
    CHECK(!evq_push(&q, &b));
    f.index = 1;
    CHECK(!evq_push(&q, &f)); /* no waiting event of fader 2 after the buttons */
    CHECK_EQ(q.refused, 2);
}

static void test_inject(void)
{
    static struct evq q;
    static struct inject in;
    evq_init(&q);
    inject_init(&in, &q);
    const uint8_t press[] = {INJECT_OP_EVENTS, CORE_EV_BUTTON, CORE_BTN_PLAY, 1, 0, 0};
    struct inject_reply r = inject_command(&in, 10, press, sizeof press);
    CHECK_EQ(r.status, INJECT_DISABLED); /* off until enabled (LOG-34) */
    CHECK_EQ(evq_depth(&q), 0);

    const uint8_t on[] = {INJECT_OP_ON};
    CHECK_EQ(inject_command(&in, 11, on, 1).status, INJECT_OK);
    const uint8_t many[] = {
        INJECT_OP_EVENTS,
        CORE_EV_BUTTON, 44, 1, 0, 0,
        CORE_EV_ENCODER, 3, 0xFE, 0xFF, 0, /* -2 */
        CORE_EV_FADER, 7, 0xFF, 0x3F, 0,   /* 16383 */
        CORE_EV_MIDI_IN, CORE_FROM_BLE, 0x90, 60, 100,
    };
    r = inject_command(&in, 12, many, sizeof many);
    CHECK_EQ(r.status, INJECT_OK);
    CHECK_EQ(r.accepted, 4);
    struct core_event ev;
    CHECK(evq_pop(&q, &ev));
    CHECK_EQ(ev.type, CORE_EV_BUTTON);
    CHECK_EQ(ev.index, 44);
    CHECK_EQ(ev.value, 1);
    CHECK_EQ(ev.flags, CORE_EV_INJECTED);
    CHECK_EQ(ev.t_us, 12);
    CHECK(evq_pop(&q, &ev));
    CHECK_EQ(ev.value, -2);
    CHECK(evq_pop(&q, &ev));
    CHECK_EQ(ev.value, CORE_FADER_MAX);
    CHECK(evq_pop(&q, &ev));
    CHECK_EQ(ev.midi[0], 0x90);
    CHECK_EQ(ev.midi[2], 100);
    CHECK_EQ(ev.index, CORE_FROM_BLE);

    /* checked before anything is queued */
    const uint8_t bad[] = {INJECT_OP_EVENTS, CORE_EV_BUTTON, 1, 1, 0, 0, CORE_EV_FADER, 8, 0, 0, 0};
    CHECK_EQ(inject_command(&in, 13, bad, sizeof bad).status, INJECT_BAD_EVENT);
    CHECK_EQ(evq_depth(&q), 0);
    const uint8_t tick[] = {INJECT_OP_EVENTS, CORE_EV_TICK, 0, 0, 0, 0};
    CHECK_EQ(inject_command(&in, 13, tick, sizeof tick).status, INJECT_BAD_EVENT);
    const uint8_t zero_click[] = {INJECT_OP_EVENTS, CORE_EV_ENCODER, 0, 0, 0, 0};
    CHECK_EQ(inject_command(&in, 13, zero_click, sizeof zero_click).status, INJECT_BAD_EVENT);
    const uint8_t short_one[] = {INJECT_OP_EVENTS, CORE_EV_BUTTON, 1, 1};
    CHECK_EQ(inject_command(&in, 13, short_one, sizeof short_one).status, INJECT_BAD_FORMAT);
    const uint8_t op9[] = {9};
    CHECK_EQ(inject_command(&in, 13, op9, 1).status, INJECT_BAD_FORMAT);
    CHECK_EQ(inject_command(&in, 13, NULL, 0).status, INJECT_BAD_FORMAT);

    /* a full queue refuses the rest, counted */
    for (unsigned i = 0; i < EVQ_SIZE - 3; i++)
        evq_push(&q, &ev);
    const uint8_t three[] = {INJECT_OP_EVENTS, CORE_EV_BUTTON, 0, 1, 0, 0, CORE_EV_BUTTON, 0, 0, 0, 0,
                             CORE_EV_BUTTON, 1, 1, 0, 0};
    r = inject_command(&in, 14, three, sizeof three);
    CHECK_EQ(r.status, INJECT_FULL);
    CHECK_EQ(r.accepted, 2);
    CHECK_EQ(r.refused, 1);
    CHECK_EQ(in.refused, 1);

    const uint8_t off[] = {INJECT_OP_OFF};
    CHECK_EQ(inject_command(&in, 15, off, 1).status, INJECT_OK);
    CHECK_EQ(inject_command(&in, 16, press, sizeof press).status, INJECT_DISABLED);
}

static uint32_t clock_us;
static uint32_t host_now(void) { return clock_us++; }

/* The defaults are times, not step counts: the same behaviour at a 25 or a 50 µs step. */
static void test_defaults_in_time(void)
{
    const struct scan_params *p = &scan_default_params;
    unsigned debounce_us = p->debounce * SCAN_PASS_US;
    CHECK(debounce_us >= 4800u && debounce_us < 4800u + SCAN_PASS_US);
    CHECK_EQ(p->led_steps * SCAN_STEP_US, 2000u);  /* 2 ms a column, 125 Hz refresh */
    CHECK_EQ(p->led_blank * SCAN_STEP_US, 200u);   /* 0.2 ms dark between columns */
    CHECK(SCAN_ENC_INFER_PASSES * SCAN_PASS_US >= 6400u);
    CHECK(SCAN_ENC_INFER_PASSES * SCAN_PASS_US < 6400u + SCAN_PASS_US);
    CHECK(p->led_steps <= 255u && SCAN_ENC_INFER_PASSES < 255u); /* uint8_t counters */
}

int main(void)
{
    smclog_host_set_clock(host_now);
    smclog_reset();
    smclog_init();
    RUN(test_word);
    RUN(test_every_button);
    RUN(test_bounce_and_chord);
    RUN(test_encoders);
    RUN(test_encoder_fast_turn);
    RUN(test_encoder_probe);
    RUN(test_encoder_skip_inferred);
    RUN(test_encoder_skip_after_bounce);
    RUN(test_encoder_direction_reverses);
    RUN(test_refused_events_retry);
    RUN(test_faders);
    RUN(test_fader_parked_at_end);
    RUN(test_fader_outliers);
    RUN(test_fader_hysteresis);
    RUN(test_pass_after_shift);
    RUN(test_leds);
    RUN(test_defaults_in_time);
    RUN(test_raw_mode_logs);
    RUN(test_queue);
    RUN(test_queue_merge);
    RUN(test_inject);
    printf("test_input: %s\n", check_failed ? "FAILED" : "all ok");
    return check_failed ? 1 : 0;
}
