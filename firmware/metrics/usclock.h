/*
 * A microsecond clock from a periodic hardware timer and a coarse reference (SPEC LOG-02: record
 * times in µs; LOG-06: latencies below a millisecond). On BR25 the timer is the scan timer (TIMER3,
 * a 50 µs period, platform/br25/input_br25.c) and the reference the SDK's millisecond clock.
 *
 *   the timer's interrupt, first thing:  usclock_period(&c);           a period has ended
 *   now and then, in the same interrupt: usclock_sync(&c, ref_us);     catch up if behind
 *   anywhere, with interrupts off:       t = usclock_read(&c, cnt, pending, cnt_again);
 *
 * The clock counts the timer's periods and adds the count within the current one. A read takes the
 * counter twice around the timer's pending flag: a period that ended but whose interrupt has not
 * run yet is counted, and a counter that wrapped between the reads is not taken for the new period.
 *
 * Periods are lost when the interrupt is held off for longer than a period (interrupts disabled,
 * code in flash not running while flash is written): the clock then falls behind. A sync compares
 * it with the reference, which never runs ahead of real time; behind by more than
 * USCLOCK_RESYNC_US, the clock jumps forward to it and counts a resync. It never goes back: a read
 * earlier than the last one returns the last one. A clock that runs ahead (a wrong timer rate) is
 * not corrected; the drift the syncs see shows it in the metrics.
 *
 * Portable C11; the platform reads the registers. Host tests: firmware/tests/test_metrics.c.
 * ⚠️ Not run on hardware.
 */
#ifndef USCLOCK_H
#define USCLOCK_H

#include <stdbool.h>
#include <stdint.h>

#define USCLOCK_RESYNC_US 2000u

struct usclock {
    uint32_t base_us;      /* the clock at the start of the timer's current period */
    uint32_t period_us;
    uint32_t ticks_per_us; /* the timer's counter rate */
    uint32_t last_us;      /* the latest time read */
    uint32_t resyncs;      /* since start */
    /* since usclock_take_window() */
    uint32_t win_resyncs;
    int32_t drift_min_us, drift_max_us; /* clock − reference at the syncs */
    bool have_drift;
};

/* The clock reads start_us when the timer starts counting its first period. */
void usclock_init(struct usclock *c, uint32_t start_us, uint32_t period_us, uint32_t ticks_per_us);

/* The timer's interrupt, before anything reads the clock: one period has ended. */
void usclock_period(struct usclock *c);

/* The timer's interrupt, after usclock_period(): ref_us is the reference now. */
void usclock_sync(struct usclock *c, uint32_t ref_us);

/* With interrupts off (no usclock_period() in between): cnt1, the timer's counter; pending, its
 * period-ended flag read after cnt1; cnt2, the counter read after that. Never earlier than the
 * previous read. */
uint32_t usclock_read(struct usclock *c, uint32_t cnt1, bool pending, uint32_t cnt2);

/* The window's resyncs and drift (no drift seen: both 0), then a new window. */
void usclock_take_window(struct usclock *c, uint32_t *resyncs, int32_t *drift_min_us,
                         int32_t *drift_max_us);

#endif /* USCLOCK_H */
