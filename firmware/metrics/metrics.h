/*
 * Metrics (SPEC LOG-06, docs/logging.md, "Metrics"): what a heartbeat reports and the small
 * accumulators behind it. Portable C11, no allocation; host tests in firmware/tests/test_metrics.c.
 *
 * A heartbeat covers a window: the time since the previous one. Each part of the firmware keeps
 * its own window accumulators and hands them over when the heartbeat takes them (and starts
 * again); the heartbeat fills a struct metrics_window and metrics_log() writes it as a few "metrics"
 * records at INFO. What a platform cannot measure stays out (the have_ flags): the host stand-in
 * (firmware/sim/devhost.c) has no scan, no hardware clock and no RTOS.
 *
 *   latency   lat_hist: input event → into the core, → its MIDI handed to USB, → in a BLE
 *             notification (wide buckets); 8 buckets
 *   durations span: count, min, max, sum (µs) of something that runs again and again
 *   stacks    stack_mark: the deepest stack pointer sampled in a task's stack
 *
 * Stacks are sampled, not painted: on BR25 the SDK's RTOS hands the free part of a stack (below
 * the stack pointer) from task to task at every switch and keeps a canary at its bottom, so a paint
 * pattern would neither stay nor be safe (docs/logging.md, "Metrics"). A sample is the address of
 * a local in code that runs often (the log's lock, the scan interrupt); it counts for the task
 * whose stack holds it. The deepest point is a lower bound of what the task uses: paths that are
 * never sampled can go deeper.
 */
#ifndef METRICS_H
#define METRICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- latency histogram ------------------------------------------------------------------------ */

#define LAT_BUCKETS 8
/* Upper bounds (exclusive) of the first 7 buckets, µs; the last bucket has the rest. Chosen around
 * SPEC NFR-01 (≤ 3 ms typical, ≤ 5 ms p99) and LOG-09 (≤ 0.2 ms for logging). */
extern const uint32_t lat_bound_us[LAT_BUCKETS - 1]; /* 250 500 1000 2000 3000 5000 10000 */
/* The wide histogram's, for BLE (SPEC NFR-02: median ≤ 25 ms; a connection event every 7.5 ms at
 * best). */
extern const uint32_t lat_bound_wide_us[LAT_BUCKETS - 1]; /* 5 10 15 20 25 35 50 ms */

struct lat_hist {
    uint32_t n;
    uint32_t sum_us; /* saturates */
    uint32_t min_us, max_us;
    uint32_t bucket[LAT_BUCKETS];
    uint8_t wide; /* the buckets are lat_bound_wide_us */
};

void lat_hist_clear(struct lat_hist *h);
/* Clears h as a wide histogram (lat_bound_wide_us). */
void lat_hist_clear_wide(struct lat_hist *h);
void lat_hist_add(struct lat_hist *h, uint32_t us);
/* An upper bound for the given percentile (per mille, 1-1000): the bound of the bucket it falls in,
 * at most max_us; max_us for the last bucket; 0 when empty. */
uint32_t lat_hist_percentile(const struct lat_hist *h, unsigned permille);

/* --- durations -------------------------------------------------------------------------------- */

struct span {
    uint32_t n;
    uint32_t sum; /* saturates */
    uint32_t min, max;
};

void span_clear(struct span *s);
void span_add(struct span *s, uint32_t v);

/* --- stacks ----------------------------------------------------------------------------------- */

/* A stack that grows down, [lo, hi), and the deepest sample in it. */
struct stack_mark {
    uintptr_t lo, hi;
    uintptr_t deepest;
};

/* sp: where the stack pointer is now (it must lie in [lo, hi)). */
void stack_mark_init(struct stack_mark *m, uintptr_t lo, uintptr_t hi, uintptr_t sp);
/* True if sp lies in this stack (and is kept if it is the deepest). */
bool stack_mark_sample(struct stack_mark *m, uintptr_t sp);
/* Bytes below the deepest sample: the headroom seen so far. */
size_t stack_mark_free(const struct stack_mark *m);

/* --- a heartbeat ------------------------------------------------------------------------------ */

#define METRICS_STACKS 3
#define METRICS_STACK_UNKNOWN 0xFFFFu /* its bounds could not be found: not sampled */

struct metrics_window {
    uint32_t seq;       /* heartbeats since start, from 1 */
    uint32_t window_us; /* since the previous heartbeat (or start) */

    /* CPU: the time our own code ran in the window, µs */
    uint32_t busy_scan_us, busy_ctl_us, busy_usb_us;

    bool have_scan;
    uint32_t scan_steps, scan_skipped;  /* timer steps; skipped: SPI still busy */
    uint32_t scan_step_avg_ns, scan_step_max_ns; /* the step handler's run time */
    uint32_t scan_late_max_us;          /* timer period → handler */
    struct span scan_pass_us;           /* time between full matrix passes */

    bool have_load; /* the CPU load, sampled at each scan step */
    uint32_t load_samples, load_busy;   /* samples; busy: a task other than the idle one ran */

    bool have_adc; /* the fader ADC */
    uint32_t adc_samples;               /* conversions handed to the scan */
    uint32_t adc_deferred;              /* steps the ADC was the SDK's when ours was due */
    uint32_t adc_lost, adc_timeouts;    /* ours dropped: taken by the SDK; given up */
    uint32_t adc_steps_max;             /* scan steps the longest conversion took */

    bool have_clock; /* the µs clock (usclock.h) */
    uint32_t clock_resyncs;             /* in the window */
    int32_t clock_drift_min_us, clock_drift_max_us; /* clock − reference at the syncs */

    struct lat_hist lat_core; /* input event → taken by the core */
    struct lat_hist lat_usb;  /* input event → its MIDI handed to USB */

    bool have_ble; /* BLE-MIDI (docs/ble.md) */
    struct lat_hist lat_ble;  /* input event → its MIDI in a notification the stack took (wide) */
    uint32_t ble_out, ble_notify;          /* messages and notifications sent */
    uint32_t ble_merged, ble_dropped;      /* older values replaced; dropped: no connection */
    uint32_t bleq_max, bleq_refused;       /* the BLE queue: the most waiting, refused */
    uint32_t ble_in, ble_in_bad;           /* messages received, faults counted by the decoder */

    bool have_traffic;
    uint32_t events;                       /* input events into the core */
    uint32_t midi_out, midi_in, midi_dropped; /* USB MIDI packets; dropped: no host */
    uint32_t evq_max, midiq_max;           /* the most waiting in the window */
    uint32_t evq_refused, midiq_refused;   /* refused in the window */

    uint32_t log_records, log_lost;        /* in the window */
    uint32_t log_used_max, log_ring;       /* ring bytes: the most used since start, the size */

    bool have_memory;
    uint32_t heap_free, heap_min;          /* bytes; min: the lowest since start */

    unsigned stacks;
    struct {
        const char *name;
        uint16_t size;   /* bytes */
        uint16_t unused; /* bytes below the deepest sample, or METRICS_STACK_UNKNOWN */
    } stack[METRICS_STACKS];
};

/* What metrics_begin() keeps between heartbeats. */
struct metrics_state {
    uint32_t seq;
    uint32_t last_us;
    uint32_t log_records, log_lost;
};

/* Starts at now_us (the log clock). */
void metrics_state_init(struct metrics_state *st, uint32_t now_us);

/* Clears w and fills what every platform has: seq, the window up to now_us, the log's counters
 * (smclog_get_stats). The caller adds the rest and calls metrics_log(). */
void metrics_begin(struct metrics_state *st, struct metrics_window *w, uint32_t now_us);

/* The heartbeat's records (module "metrics", INFO); docs/logging.md lists them. */
void metrics_log(const struct metrics_window *w);

#endif /* METRICS_H */
