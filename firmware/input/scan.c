/*
 * The scan layer (scan.h, docs/hwtest.md). Runs in the platform's scan interrupt: it logs with
 * cause 0 (smclog.h: the current cause belongs to the interrupted code); the core gives each event
 * its cause when it takes it.
 */
#include "scan.h"

#include <string.h>

#include "smc_ram.h"
#include "smclog.h"

/* docs/hardware.md, "Input matrix" (the stock table at 0x1E14329): column << 4 | row. */
/* Read by the scan interrupt, so in RAM on the device (smc_ram.h). */
SMC_RAM_DATA const uint8_t scan_button_pos[SCAN_BUTTONS] = {
    0x13, 0x23, 0x33, 0x43, /* strip 1: M S R SELECT */
    0xB3, 0xA3, 0x93, 0x83, /* strip 2 */
    0xC3, 0xD3, 0xE3, 0xF3, /* strip 3 */
    0x12, 0x22, 0x32, 0x42, /* strip 4 */
    0xB2, 0xA2, 0x92, 0x82, /* strip 5 */
    0xC2, 0xD2, 0xE2, 0xF2, /* strip 6 */
    0x61, 0x51, 0x41, 0x31, /* strip 7 */
    0x91, 0xA1, 0xB1, 0xC1, /* strip 8 */
    0x53, 0x63, 0x73, 0x03, 0x52, 0x62, 0x72, 0x02, 0x11, 0x21, 0xD1, /* Play ... right */
    0x71, 0x81, /* BT, Shift */
};

/* docs/hardware.md, "LEDs" (the stock table at 0x1E14B18): row << 4 | column, in the core's order
 * (buttons, then the 8 encoders). The LED walk of 2026-11-11 confirmed the 45 buttons' order; the
 * stock table lists the encoders right to left, so their 8 entries are reversed here. */
const uint8_t scan_led_pos[SCAN_LEDS] = {
    0x80, 0x90, 0xA0, 0xB0, 0x40, 0x30, 0x20, 0x10, /* strips 1, 2 */
    0x50, 0x81, 0x91, 0xA1, 0x31, 0x21, 0x11, 0x01, /* strips 3, 4 */
    0x41, 0x51, 0x82, 0x92, 0x22, 0x12, 0x02, 0xD2, /* strips 5, 6 */
    0x32, 0x42, 0x52, 0x83, 0x03, 0xD3, 0xC3, 0xB3, /* strips 7, 8 */
    0xC0, 0xD0, 0x00, 0xB1, 0xC1, 0xD1, 0xA2, 0xB2, 0xC2, 0x93, 0xA3, /* the bottom row */
    0x23, 0x13, /* BT, Shift */
    0x61, 0x62, 0x60, 0x70, 0x33, 0x63, 0x43, 0x53, /* encoders 1-8 */
};

const struct scan_params scan_default_params = {
    .debounce = SCAN_PASSES_FOR_US(4800u), /* 4.8-5.6 ms at 50 µs, 4.8-5.2 ms at 25 µs */
    .enc_transitions = 2,
    .enc_reverse = 0,
    .adc_bits = 10,
    .fader_shift = 3,       /* about 13 ms at a sample every 1.62 ms */
    .fader_hysteresis = 80, /* 5 raw counts: docs/hwtest.md, "The faders measured" */
    .led_steps = SCAN_STEPS_FOR_US(2000u), /* 2 ms a column: the 4 columns refresh at 125 Hz */
    .led_blank = SCAN_STEPS_FOR_US(200u),  /* 0.2 ms dark between columns */
};

/* Quadrature: (previous AB << 2 | current AB) -> step; 0 for no change and for a jump. Bit masks,
 * not a table, so that nothing is read from flash (smc_ram.h): +1 for 2, 4, 11, 13, -1 for 1, 7,
 * 8, 14. */
#define QDEC_UP 0x2814u
#define QDEC_DOWN 0x4182u

SMC_RAM_CODE static int qdec(unsigned idx)
{
    return (int)((QDEC_UP >> idx) & 1u) - (int)((QDEC_DOWN >> idx) & 1u);
}

void scan_init(struct scan *s, const struct scan_params *p, scan_sink sink, void *ctx)
{
    memset(s, 0, sizeof *s);
    s->p = *p;
    if (s->p.debounce == 0)
        s->p.debounce = 1;
    if (s->p.enc_transitions == 0)
        s->p.enc_transitions = 1;
    if (s->p.adc_bits < 8 || s->p.adc_bits > 14)
        s->p.adc_bits = 10;
    if (s->p.led_steps < 2)
        s->p.led_steps = 2;
    if (s->p.led_blank == 0 || s->p.led_blank >= s->p.led_steps)
        s->p.led_blank = 1; /* at least one: a column's rows reach the outputs a step after its pin */
    s->sink = sink;
    s->ctx = ctx;
    for (unsigned i = 0; i < SCAN_FADERS; i++)
        s->fader_sent[i] = -1;
    s->col = SCAN_COLS - 1; /* the first steps read nothing useful */
    s->led_drive = -1;
    for (unsigned i = 0; i < SCAN_BUTTONS; i++)
        s->led_glow[scan_led_pos[i] & 0x0Fu] |= (uint16_t)(1u << (scan_led_pos[i] >> 4));
    scan_led_light(s, SCAN_LIGHT_LEVELS, 0);
}

/* A level's share of the full light in thousandths: the square of the level, so that the steps
 * look even (brightness is seen roughly as the square root of the light; a gamma of 2). */
static const uint16_t light_share[SCAN_LIGHT_LEVELS] = {10, 40, 90, 160, 250, 360, 490, 640, 810, 1000};

/* With the default times a column shows 71 steps (35 at a 50 µs step): the ten levels all differ.
 * A window under 10 steps would merge the top levels; none of the builds has one. */
unsigned scan_light_steps(const struct scan *s, unsigned level)
{
    /* What a column shows: its time less the blank and the step its rows take to reach the
     * outputs (scan.h). */
    unsigned window = (unsigned)s->p.led_steps - s->p.led_blank - 1u, on = 0;
    if (level < 1)
        level = 1;
    if (level > SCAN_LIGHT_LEVELS)
        level = SCAN_LIGHT_LEVELS;
    for (unsigned l = 1; l <= level; l++) {
        unsigned want = (window * light_share[l - 1] + 500u) / 1000u;
        on = want > on ? want : on + 1u; /* each level brighter than the one below */
    }
    return on < window ? on : window;
}

/* The words of a column's time that carry a LED's rows: word q shows at step q + 1, and the
 * column's last word shows in the next blank, so n steps shown take the first n words; the full
 * light puts the rows in every word, as before the light (test_leds). */
static uint8_t words_on(const struct scan *s, unsigned level)
{
    unsigned n = scan_light_steps(s, level), window = (unsigned)s->p.led_steps - s->p.led_blank - 1u;
    return (uint8_t)(n < window ? n : window + 1u);
}

void scan_led_light(struct scan *s, unsigned level, unsigned unlit)
{
    s->led_on = words_on(s, level);
    s->unlit_on = unlit ? words_on(s, unlit) : 0u;
}

SMC_RAM_CODE uint32_t scan_word(unsigned col, uint16_t led_on)
{
    col %= SCAN_COLS;
    unsigned bit = col < 8 ? 24 + col : 16 + (col - 8);
    return (~(1u << bit) & 0xFFFF0000u) | (uint16_t)~led_on;
}

SMC_RAM_CODE static bool send(struct scan *s, uint32_t t_us, uint8_t type, unsigned index, int value)
{
    struct core_event ev = {.t_us = t_us, .type = type, .index = (uint8_t)index,
                            .value = (int16_t)value};
    if (s->sink(s->ctx, &ev))
        return true;
    s->stats.refused++;
    return false;
}

SMC_RAM_CODE static bool closed(const struct scan *s, uint8_t pos)
{
    return (s->raw[pos >> 4] >> (pos & 0x0Fu)) & 1u;
}

SMC_RAM_CODE static void buttons(struct scan *s, uint32_t t_us)
{
    for (unsigned i = 0; i < SCAN_BUTTONS; i++) {
        bool now = closed(s, scan_button_pos[i]);
        bool was = (s->pressed >> i) & 1u;
        if (now == was) {
            if (s->count[i])
                s->stats.bounces++;
            s->count[i] = 0;
            continue;
        }
        if (s->count[i] < s->p.debounce)
            s->count[i]++;
        if (s->count[i] < s->p.debounce)
            continue;
        if (!send(s, t_us, CORE_EV_BUTTON, i, now))
            continue; /* the count stays: tried again on the next pass */
        s->pressed ^= (uint64_t)1 << i;
        s->count[i] = 0;
        s->stats.buttons++;
    }
}

/* prime: the first full pass only records where the encoders stand. */
SMC_RAM_CODE static void encoders(struct scan *s, uint32_t t_us, bool prime)
{
    for (unsigned n = 0; n < SCAN_ENCODERS; n++) {
        /* A is the line on column 2n + 2 (0-based n): it leads when turned clockwise, so clockwise
         * counts up (docs/hwtest.md, run of 2026-11-11). */
        unsigned ca = (2 * n + 2) % SCAN_COLS, cb = 2 * n + 1;
        unsigned a = s->raw[ca] & 1u;
        /* Encoder 8 (columns 15 and 0): column 15 of the pass before, read a step before column
         * 0, not this pass's, read 15 steps after it. The prime has no earlier read. */
        unsigned b = (ca < cb && !prime ? s->raw_last_before : s->raw[cb]) & 1u;
        unsigned ab = a << 1 | b;
        unsigned idx = (unsigned)s->enc_ab[n] << 2 | ab;
        s->enc_ab[n] = (uint8_t)ab;
        if (prime)
            continue;
        if (s->enc_quiet[n] < 0xFFu)
            s->enc_quiet[n]++;
        int step;
        if (idx == 0x3 || idx == 0x6 || idx == 0x9 || idx == 0xC) {
            /* Both lines changed since the last pass: a state shorter than a pass was skipped
             * (docs/hwtest.md, "What the run of 2026-11-11 found": in fast spins the state with
             * both contacts closed lasts under 0.8 ms). While the encoder is turning, it went on
             * the way it was going: two transitions. The way is the sum of the recent
             * transitions, not the last one: a contact's bounce (+1 -1) must not turn it (encoder
             * 8 drops out often). At rest the direction is lost. */
            if (!s->enc_trend[n] || s->enc_quiet[n] > SCAN_ENC_INFER_PASSES) {
                s->stats.enc_invalid++;
                continue;
            }
            s->stats.enc_inferred++;
            step = s->enc_trend[n] > 0 ? 2 : -2;
        } else {
            step = qdec(idx);
            if (!step)
                continue;
            int tr = s->enc_trend[n] + step;
            s->enc_trend[n] = (int8_t)(tr > 3 ? 3 : tr < -3 ? -3 : tr);
        }
        s->enc_quiet[n] = 0;
        if ((s->p.enc_reverse >> n) & 1u)
            step = -step;
        s->enc_part[n] = (int8_t)(s->enc_part[n] + step);
        while (s->enc_part[n] >= s->p.enc_transitions) {
            s->enc_part[n] = (int8_t)(s->enc_part[n] - s->p.enc_transitions);
            s->enc_clicks[n]++;
        }
        while (s->enc_part[n] <= -s->p.enc_transitions) {
            s->enc_part[n] = (int8_t)(s->enc_part[n] + s->p.enc_transitions);
            s->enc_clicks[n]--;
        }
    }
    for (unsigned n = 0; n < SCAN_ENCODERS; n++) {
        if (s->enc_clicks[n] && send(s, t_us, CORE_EV_ENCODER, n, s->enc_clicks[n])) {
            s->enc_clicks[n] = 0; /* clicks a refused event carried are sent with the next one */
            s->stats.encoders++;
        }
    }
}

SMC_RAM_CODE void scan_pass(struct scan *s, uint32_t t_us)
{
    if (!s->pass_due)
        return;
    bool prime = s->pass_due == 2;
    s->pass_due = 0;
    buttons(s, t_us);
    encoders(s, t_us, prime);
}

SMC_RAM_CODE static uint32_t scan_leds_step(struct scan *s, unsigned word_col);

/* A step of the encoder probe (SCAN_RAW_PROBE_SHIFT): stores and logs the read if it is one of the
 * probe's columns, and returns the column the word must select: the read two steps later is
 * column x + 1 for a word built for x (the stock pipeline, scan_word()). */
SMC_RAM_CODE static unsigned probe_step(struct scan *s, uint8_t rows)
{
    uint8_t c = s->probe_label[0];
    unsigned n = s->probe - 1u;
    unsigned ca = (2u * n + 2u) % SCAN_COLS, cb = 2u * n + 1u;
    unsigned want = s->probe_flip ? cb : ca;
    s->probe_flip ^= 1u;
    s->probe_label[0] = s->probe_label[1];
    s->probe_label[1] = (uint8_t)want;
    if (c < SCAN_COLS) {
        uint8_t v = (uint8_t)(~rows & ((1u << SCAN_ROWS) - 1u));
        if (v != s->raw[c]) {
            s->raw[c] = v;
            LOGC_INFO(0, input, "raw: column %u rows %x", c, v);
        }
    }
    return (want + SCAN_COLS - 1u) % SCAN_COLS;
}

/* Into the probe or out of it. The two reads already on their way belong to the old order. */
SMC_RAM_CODE static void probe_switch(struct scan *s, uint8_t probe)
{
    s->probe = probe;
    s->probe_label[0] = s->probe_label[1] = 0xFFu;
    s->probe_flip = 0;
    s->pass_due = 0;
    memset(s->enc_quiet, 0xFF, sizeof s->enc_quiet); /* no encoder counts as turning across it */
    if (!probe) {
        /* The full scan again: two junk reads first (labelled 15 and 0), so the encoders are
         * primed on the third pass, which is all real reads. */
        s->col = SCAN_COLS - 1;
        s->reprime = 3;
    }
}

SMC_RAM_CODE uint32_t scan_step(struct scan *s, uint32_t t_us, uint8_t rows)
{
    scan_pass(s, t_us); /* one the caller left: before its column 0 is overwritten */
    s->stats.steps++;
    uint8_t probe = (uint8_t)(s->raw_mode >> SCAN_RAW_PROBE_SHIFT);
    if (probe > SCAN_ENCODERS)
        probe = 0;
    if (probe != s->probe)
        probe_switch(s, probe);
    if (s->probe)
        return scan_leds_step(s, probe_step(s, rows));
    uint8_t c = s->col;
    if (c == SCAN_COLS - 1)
        s->raw_last_before = s->raw[c]; /* encoder 8's pairing (encoders()) */
    s->raw[c] = (uint8_t)(~rows & ((1u << SCAN_ROWS) - 1u));
    if ((s->raw_mode & SCAN_RAW_MATRIX) && s->raw[c] != s->logged[c]) {
        LOGC_INFO(0, input, "raw: column %u rows %x", c, s->raw[c]);
        s->logged[c] = s->raw[c];
    }
    if (c == SCAN_COLS - 1) {
        /* The first steps read what the platform latched at start (nothing selected): evaluation
         * starts with the first full pass, which only primes the encoders. */
        if (++s->stats.passes >= 2) {
            if (s->reprime) {
                if (--s->reprime == 0)
                    s->pass_due = 2; /* after a probe: prime again */
            } else {
                s->pass_due = s->stats.passes == 2 ? 2 : 1; /* for scan_pass(), after the shift */
            }
        }
    }
    s->col = (uint8_t)((c + 1) % SCAN_COLS);
    return scan_leds_step(s, s->col);
}

/* The LED column's phase for this step, and the word: column word_col and the LED rows. */
SMC_RAM_CODE static uint32_t scan_leds_step(struct scan *s, unsigned word_col)
{
    /* The LED column: blank first, so the outgoing column's rows never meet the next column. */
    if (++s->led_phase >= s->p.led_steps) {
        s->led_phase = 0;
        s->led_col = (uint8_t)((s->led_col + 1) % SCAN_LED_COLS);
    }
    if (s->led_phase < s->p.led_blank) {
        s->led_drive = -1;
        return scan_word(word_col, 0);
    }
    s->led_drive = (int8_t)s->led_col;
    /* The light: a LED's rows go out for the first steps of the column's time (scan_led_light). */
    unsigned q = (unsigned)s->led_phase - s->p.led_blank;
    uint16_t lit = s->led_rows[s->led_col], rows = q < s->led_on ? lit : 0;
    if (q < s->unlit_on)
        rows |= (uint16_t)(~lit & s->led_glow[s->led_col]);
    return scan_word(word_col, rows);
}

/* The median of three samples: a single wrong conversion (the ADC's bit errors near mid-scale,
 * 10-55 counts) never reaches the filter. */
SMC_RAM_CODE static uint16_t median3(uint16_t a, uint16_t b, uint16_t c)
{
    if (a > b) {
        uint16_t t = a;
        a = b;
        b = t;
    }
    return c <= a ? a : c >= b ? b : c;
}

/* A median of 3, a gate against bursts of wrong conversions, the filter, then a directional
 * hysteresis: while the fader keeps its way every
 * change is sent; from rest, or to turn back, it must move fader_hysteresis first. So it is quiet
 * at rest and keeps the ADC's resolution while it moves (docs/hwtest.md, "The faders measured"). */
SMC_RAM_CODE void scan_fader(struct scan *s, uint32_t t_us, unsigned fader, uint16_t adc)
{
    if (fader >= SCAN_FADERS)
        return;
    if ((s->raw_mode & SCAN_RAW_FADERS) && adc != s->fader_raw[fader]) {
        LOGC_INFO(0, input, "raw: fader %u adc %u", fader, adc);
        s->fader_raw[fader] = adc;
    }
    uint16_t *prev = s->fader_prev[fader];
    if (s->fader_sent[fader] < 0)
        prev[0] = prev[1] = adc;
    uint16_t m = median3(prev[0], prev[1], adc);
    prev[0] = prev[1];
    prev[1] = adc;
    uint32_t x = (uint32_t)m << (14u - s->p.adc_bits);
    if (x > CORE_FADER_MAX)
        x = CORE_FADER_MAX;
    if (s->fader_sent[fader] < 0) {
        s->fader_acc[fader] = x << 8;
    } else {
        int32_t d = (int32_t)(x << 8) - (int32_t)s->fader_acc[fader];
        int32_t gate = (int32_t)SCAN_FADER_GATE_COUNTS << (14u - s->p.adc_bits + 8u);
        bool take = true;
        if (d > gate || d < -gate) { /* a burst of wrong conversions, or a fast move: wait */
            int8_t side = d > 0 ? 1 : -1;
            if (side != s->fader_side[fader])
                s->fader_beyond[fader] = 0;
            s->fader_side[fader] = side;
            if (s->fader_beyond[fader] < 0xFFu)
                s->fader_beyond[fader]++;
            take = s->fader_beyond[fader] >= SCAN_FADER_GATE_SAMPLES;
        } else {
            s->fader_beyond[fader] = 0;
            s->fader_side[fader] = 0;
        }
        if (take)
            s->fader_acc[fader] = (uint32_t)((int32_t)s->fader_acc[fader] + d / (1 << s->p.fader_shift));
    }
    int y = (int)((s->fader_acc[fader] + 128u) >> 8);
    int h = s->p.fader_hysteresis;
    /* The ends are always reachable. */
    if (y <= h)
        y = 0;
    else if (y >= CORE_FADER_MAX - h)
        y = CORE_FADER_MAX;
    if (s->fader_quiet[fader] < 0xFFu)
        s->fader_quiet[fader]++;
    if (s->fader_quiet[fader] > SCAN_FADER_REST_SAMPLES)
        s->fader_dir[fader] = 0;
    int last = s->fader_sent[fader];
    if (last >= 0 && y == last)
        return;
    int way = last < 0 || y > last ? 1 : -1;
    int move = last < 0 ? CORE_FADER_MAX : (y > last ? y - last : last - y);
    int need;
    if (last == 0 || last == CORE_FADER_MAX)
        need = 2 * h; /* leaving an end: a parked fader's noise dips under the snap (run 19) */
    else if (y == 0 || y == CORE_FADER_MAX)
        need = 0;
    else
        need = way == s->fader_dir[fader] ? 1 << (14u - s->p.adc_bits) : h; /* one count on */
    if (move < need)
        return;
    if (send(s, t_us, CORE_EV_FADER, fader, y)) {
        s->fader_sent[fader] = (int16_t)y;
        s->fader_dir[fader] = last < 0 ? 0 : (int8_t)way; /* the first value is no move */
        s->fader_quiet[fader] = 0;
        s->stats.faders++;
    }
}

void scan_led_at(struct scan *s, unsigned row, unsigned col, bool on)
{
    if (row >= SCAN_LED_ROWS || col >= SCAN_LED_COLS)
        return;
    if (on)
        s->led_rows[col] |= (uint16_t)(1u << row);
    else
        s->led_rows[col] &= (uint16_t)~(1u << row);
}

void scan_led(struct scan *s, unsigned led, bool on)
{
    if (led < SCAN_LEDS)
        scan_led_at(s, scan_led_pos[led] >> 4, scan_led_pos[led] & 0x0Fu, on);
}

void scan_leds_clear(struct scan *s)
{
    memset(s->led_rows, 0, sizeof s->led_rows);
}
