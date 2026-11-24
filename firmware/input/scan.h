/*
 * The scan layer: the input matrix, button debounce, encoder decoding, fader filtering and the
 * LED frame (docs/hwtest.md). Portable C11; the platform drives the pins and calls these
 * functions from its scan interrupt. What it produces are core events (core/core.h), handed to a
 * sink (normally the event queue, evq.h).
 *
 * Hardware layout from the stock firmware (docs/hardware.md, ⚠️ until our hardware test confirms
 * it):
 * - four 74HC595s take a 32-bit word: the upper 16 bits select one input column (that bit low;
 *   column c is bit 24 + c for c < 8 and bit 16 + c - 8 for the rest, the stock column table), the
 *   lower 16 bits are the LED rows, active low (a lit LED is a 0);
 * - the input rows are read on PB0-PB3, a closed switch reads low;
 * - row 0 carries the encoders: encoder n (1-8) has its lines on columns 2n-1 and 2n (encoder 8:
 *   15 and 0); rows 1-3 carry the 45 buttons;
 * - the LED columns are driven one at a time from four pins, with a blank phase between them.
 *
 * Usage, per scan step (the platform's timer interrupt), the stock firmware's pipeline:
 *
 *   rows = read PB0-PB3;                  // the outputs latched at the previous step
 *   latch;                                // the word shifted at the previous step reaches them
 *   word = scan_step(&s, now_us, rows);
 *   start shifting word out to the 595s;
 *   drive LED column s.led_drive;         // -1: none (blank)
 *   scan_pass(&s, now_us);                // after the last column: buttons, encoders
 *
 * A word is shifted at one step, latched at the next and its rows are read at the one after. So
 * the rows stored as column c were read while the word for column c - 1 was latched. The stock
 * firmware numbers its columns the same way, and its tables (scan_button_pos, the encoder
 * columns) are in that numbering.
 *
 * The defaults follow SCAN_STEP_US: one matrix pass is 16 steps (0.4 ms at 25 µs). The button
 * debounce, the LED column time and the encoder's "still turning" window are kept in time, not
 * in steps, so changing the step keeps their behaviour.
 */
#ifndef SCAN_H
#define SCAN_H

#include <stdbool.h>
#include <stdint.h>

#include "core.h"

#define SCAN_COLS 16     /* input matrix columns */
#define SCAN_ROWS 4      /* input matrix rows */
#define SCAN_LED_COLS 4  /* LED columns */
#define SCAN_LED_ROWS 16 /* LED rows */
#define SCAN_BUTTONS CORE_BUTTONS
#define SCAN_ENCODERS CORE_STRIPS
#define SCAN_FADERS CORE_STRIPS
#define SCAN_LEDS CORE_LEDS
#ifndef SCAN_STEP_US
#define SCAN_STEP_US 25  /* the scan step; 50 before 2026-11-23 (docs/hwtest.md, "Encoders at speed") */
#endif
_Static_assert(SCAN_STEP_US == 25 || SCAN_STEP_US == 50, "SCAN_STEP_US: 25 or 50");
#define SCAN_PASS_US (16u * SCAN_STEP_US) /* a full matrix pass */
/* Whole passes or steps at least this long (rounded up), for the time-based defaults. */
#define SCAN_PASSES_FOR_US(us) (((us) + SCAN_PASS_US - 1u) / SCAN_PASS_US)
#define SCAN_STEPS_FOR_US(us) (((us) + SCAN_STEP_US - 1u) / SCAN_STEP_US)

/* The stock firmware's tables (docs/hardware.md): each button's place in the input matrix as
 * column << 4 | row, and each LED's place in the LED matrix as row << 4 | column. Indexes are the
 * core's (core.h): buttons 0-44, LEDs 0-44 under the buttons, 45-52 at the encoders. */
extern const uint8_t scan_button_pos[SCAN_BUTTONS];
extern const uint8_t scan_led_pos[SCAN_LEDS];

struct scan_params {
    uint8_t debounce;        /* matrix passes a new button state must last (1 = none) */
    uint8_t enc_transitions; /* quadrature transitions per click */
    uint8_t enc_reverse;     /* bit n: encoder n counts the other way */
    uint8_t adc_bits;        /* fader ADC resolution, 8-14 */
    uint8_t fader_shift;       /* fader filter: y += (x - y) / 2^shift (0 = none), after a median of 3 */
    uint16_t fader_hysteresis; /* 14-bit units a fader must move from rest or to turn back */
    uint8_t led_steps;       /* scan steps each LED column is shown for, the blank included */
    uint8_t led_blank;       /* of these, the first ones are blank: no column driven, rows off */
};

/* Defaults: the stock firmware's encoder divider (2), our first guesses for the rest, at
 * SCAN_STEP_US a step (docs/hwtest.md: tuned with raw input mode, LOG-07). */
extern const struct scan_params scan_default_params;

struct scan_stats {
    uint32_t steps, passes;          /* scan steps, full matrix passes */
    uint32_t buttons, encoders, faders; /* events sent */
    uint32_t bounces;                /* raw button changes that did not last */
    uint32_t enc_invalid;            /* both lines of an encoder changed between passes, at rest: lost */
    uint32_t enc_inferred;           /* the same while turning: taken as one state skipped */
    uint32_t refused;                /* events the sink refused (retried on the next pass) */
};

/* Takes an event; false if it cannot (queue full). */
typedef bool (*scan_sink)(void *ctx, const struct core_event *ev);

/* Raw input mode (LOG-07), bit by bit: the same bits as LOG_RAW_INPUT's (link.h). */
enum {
    SCAN_RAW_MATRIX = 1u << 0, /* each change of a column's rows, the encoder lines among them */
    SCAN_RAW_FADERS = 1u << 1, /* each change of a fader's ADC value: many records, on demand */
    /* Bits 4-7, a diagnostic: 1-8 probes that encoder. The scan then reads only its two columns,
     * in turn, each every 2 steps, and logs each change of them; buttons and encoders
     * are not evaluated. 0 goes back to the full scan (docs/hwtest.md, "Encoder probe"). */
    SCAN_RAW_PROBE_SHIFT = 4,
};

/* An encoder that moved within this many passes (6.4 ms) is still turning: a jump is taken as
 * one skipped state in its direction (encoders(), docs/hwtest.md). */
#define SCAN_ENC_INFER_PASSES SCAN_PASSES_FOR_US(6400u)

/* A fader not sent for this many samples (about 0.3 s at 1.6 ms a sample) is at rest: moving
 * either way takes the hysteresis again (scan_fader(), docs/hwtest.md). */
#define SCAN_FADER_REST_SAMPLES 200u

/* The fader gate: a sample more than this many ADC counts from the filter's value is held back
 * until this many in a row lie beyond on the same side. The ADC's wrong conversions near
 * mid-scale come in bursts of up to 5 or so; a real move only waits that long at its start. */
#define SCAN_FADER_GATE_COUNTS 6
#define SCAN_FADER_GATE_SAMPLES 6u

struct scan {
    struct scan_params p;
    scan_sink sink;
    void *ctx;
    uint8_t raw_mode; /* LOG-07: which raw inputs are logged, SCAN_RAW_* bits */

    uint8_t col;              /* the column the last word selected */
    uint8_t pass_due;         /* a full pass waits for scan_pass(): 1 to evaluate, 2 to prime */
    uint8_t raw[SCAN_COLS];   /* the last read of each column: bit r = row r closed */
    uint8_t raw_last_before;  /* the last column's read of the pass before (encoder 8) */
    uint8_t probe;            /* the encoder probed, 1-8; 0: the full scan */
    uint8_t probe_label[2];   /* the columns the next two reads come from (0xFF: not the probe's) */
    uint8_t probe_flip;       /* which of the two columns is asked for next */
    uint8_t reprime;          /* passes to wait after a probe before the encoders are primed */
    uint8_t logged[SCAN_COLS]; /* raw mode: what was last logged */
    uint64_t pressed;         /* debounced button states, bit = button index */
    uint8_t count[SCAN_BUTTONS]; /* passes the raw state has differed from the debounced one */
    uint8_t enc_ab[SCAN_ENCODERS];
    int8_t enc_part[SCAN_ENCODERS];    /* transitions towards the next click */
    int8_t enc_trend[SCAN_ENCODERS];   /* recent transitions summed, held within +-3: the way it turns */
    uint8_t enc_quiet[SCAN_ENCODERS];  /* passes since the last transition (saturates) */
    int16_t enc_clicks[SCAN_ENCODERS]; /* clicks not sent yet */
    uint16_t fader_prev[SCAN_FADERS][2]; /* the two samples before: the median of 3 */
    uint32_t fader_acc[SCAN_FADERS];   /* filter state: 14-bit value << 8 */
    int16_t fader_sent[SCAN_FADERS];   /* -1: nothing sent yet */
    int8_t fader_dir[SCAN_FADERS];     /* the way it last moved, +1 or -1; 0: at rest */
    int8_t fader_side[SCAN_FADERS];    /* the gate: the side the samples held back lie on */
    uint8_t fader_beyond[SCAN_FADERS]; /* the gate: samples in a row beyond it (saturates) */
    uint8_t fader_quiet[SCAN_FADERS];  /* samples since it was last sent (saturates) */
    uint16_t fader_raw[SCAN_FADERS];   /* raw mode: the last ADC value logged */
    uint16_t led_rows[SCAN_LED_COLS];  /* the LED frame: bit r = row r lit, for each column */
    uint8_t led_phase;                 /* the step within the current LED column's time */
    uint8_t led_col;                   /* the current LED column */
    int8_t led_drive;                  /* what the platform drives now: -1 no column, else 0-3 */
    struct scan_stats stats;
};

void scan_init(struct scan *s, const struct scan_params *p, scan_sink sink, void *ctx);

/* One scan step. rows: the row pins as read (bit r = row r, low = closed), stored as column s.col.
 * Returns the next word for the 595 chain, which carries the LED rows of s.led_drive (all off while
 * blank). It is cheap, so the word goes out early in the step. */
uint32_t scan_step(struct scan *s, uint32_t t_us, uint8_t rows);

/* After the last column's step, once its word is on its way: the pass is evaluated (buttons,
 * encoders), which costs far more than a step (docs/hwtest.md, run 20). Nothing to do at the other
 * steps. A pass the caller leaves is evaluated at the start of the next scan_step(). */
void scan_pass(struct scan *s, uint32_t t_us);

/* One fader sample, raw from the ADC. */
void scan_fader(struct scan *s, uint32_t t_us, unsigned fader, uint16_t adc);

/* LEDs, by the core's index or by place. The platform shows the frame column by column. */
void scan_led(struct scan *s, unsigned led, bool on);
void scan_led_at(struct scan *s, unsigned row, unsigned col, bool on);
void scan_leds_clear(struct scan *s);

/* The 595 word that selects column col, with the LEDs of led_on lit (bit r = row r): exposed for
 * tests and the platform's start-up. */
uint32_t scan_word(unsigned col, uint16_t led_on);

#endif /* SCAN_H */
