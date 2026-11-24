/*
 * Host tests for the metrics (firmware/metrics): the latency histogram and its percentiles,
 * durations, stack painting, the heartbeat's window and records, the microsecond clock.
 * Run: make -C firmware test
 */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "metrics.h"
#include "smclog.h"
#include "smclog_host.h"
#include "usclock.h"

int check_failed;

static uint32_t T;
static uint32_t clock_now(void) { return T; }

static uint32_t records(void)
{
    struct smclog_stats st;
    smclog_get_stats(&st);
    return st.records;
}

static void test_buckets(void)
{
    struct lat_hist h;
    static const uint32_t us[] = {0, 249, 250, 499, 500, 999, 1000, 1999, 2000, 2999,
                                  3000, 4999, 5000, 9999, 10000, 4000000000u};
    static const unsigned want[LAT_BUCKETS] = {2, 2, 2, 2, 2, 2, 2, 2};
    lat_hist_clear(&h);
    for (unsigned i = 0; i < sizeof us / sizeof us[0]; i++)
        lat_hist_add(&h, us[i]);
    for (unsigned i = 0; i < LAT_BUCKETS; i++)
        CHECK_EQ(h.bucket[i], want[i]);
    CHECK_EQ(h.n, 16);
    CHECK_EQ(h.min_us, 0);
    CHECK_EQ(h.max_us, 4000000000u);
    lat_hist_add(&h, 4000000000u);
    CHECK_EQ(h.sum_us, UINT32_MAX); /* saturated */
}

static void test_percentiles(void)
{
    struct lat_hist h;
    lat_hist_clear(&h);
    CHECK_EQ(lat_hist_percentile(&h, 500), 0); /* empty */
    lat_hist_add(&h, 120);
    CHECK_EQ(lat_hist_percentile(&h, 500), 120); /* the bucket's bound (250) is above the max */
    CHECK_EQ(lat_hist_percentile(&h, 990), 120);

    lat_hist_clear(&h);
    for (unsigned i = 0; i < 98; i++)
        lat_hist_add(&h, 300); /* [250, 500) */
    lat_hist_add(&h, 6000);    /* [5000, 10000) */
    lat_hist_add(&h, 7000);
    CHECK_EQ(lat_hist_percentile(&h, 500), 500);
    CHECK_EQ(lat_hist_percentile(&h, 980), 500);  /* the 98th of 100 */
    CHECK_EQ(lat_hist_percentile(&h, 990), 7000); /* the 99th: the bucket's bound is above the max */
    CHECK_EQ(lat_hist_percentile(&h, 1000), 7000);
    CHECK_EQ(lat_hist_percentile(&h, 0), 500); /* rank at least 1 */
    CHECK_EQ(h.sum_us / h.n, (98 * 300 + 13000) / 100);

    lat_hist_add(&h, 25000); /* the last bucket: the max */
    for (unsigned i = 0; i < 2000; i++)
        lat_hist_add(&h, 30000);
    CHECK_EQ(lat_hist_percentile(&h, 990), 30000);
    CHECK_EQ(lat_hist_percentile(&h, 10), 500); /* 2101 samples: rank 22 is in the first 98 */
}

/* The wide histogram (BLE): its own bounds, kept by lat_hist_clear_wide() */
static void test_wide(void)
{
    struct lat_hist h;
    static const uint32_t us[] = {0, 4999, 5000, 9999, 10000, 14999, 15000, 19999, 20000, 24999,
                                  25000, 34999, 35000, 49999, 50000, 4000000000u};
    lat_hist_clear_wide(&h);
    CHECK_EQ(h.wide, 1);
    for (unsigned i = 0; i < sizeof us / sizeof us[0]; i++)
        lat_hist_add(&h, us[i]);
    for (unsigned i = 0; i < LAT_BUCKETS; i++)
        CHECK_EQ(h.bucket[i], 2);
    lat_hist_clear_wide(&h);
    for (unsigned i = 0; i < 10; i++)
        lat_hist_add(&h, 12000u + i * 1000u); /* 12..21 ms */
    CHECK_EQ(lat_hist_percentile(&h, 500), 20000); /* the 5th: 15..19 ms */
    CHECK_EQ(lat_hist_percentile(&h, 990), 21000); /* the bound (25 ms) above the max */
    lat_hist_clear(&h);
    CHECK_EQ(h.wide, 0);
}

static void test_span(void)
{
    struct span s;
    span_clear(&s);
    span_add(&s, 800);
    span_add(&s, 750);
    span_add(&s, 1650);
    CHECK_EQ(s.n, 3);
    CHECK_EQ(s.min, 750);
    CHECK_EQ(s.max, 1650);
    CHECK_EQ(s.sum, 3200);
    span_add(&s, UINT32_MAX);
    CHECK_EQ(s.sum, UINT32_MAX);
}

static void test_stack(void)
{
    struct stack_mark m;
    stack_mark_init(&m, 0x1000, 0x1C00, 0x1B80); /* 3 KB; the task starts 128 bytes down */
    CHECK_EQ(stack_mark_free(&m), 0xB80);
    CHECK(stack_mark_sample(&m, 0x1A00));
    CHECK(stack_mark_sample(&m, 0x1B00)); /* not deeper: kept the deepest */
    CHECK_EQ(stack_mark_free(&m), 0xA00);
    CHECK(!stack_mark_sample(&m, 0x0FFC)); /* another stack (or the interrupt stack) */
    CHECK(!stack_mark_sample(&m, 0x1C00));
    CHECK_EQ(stack_mark_free(&m), 0xA00);
    CHECK(stack_mark_sample(&m, 0x1000)); /* the bottom reached */
    CHECK_EQ(stack_mark_free(&m), 0);
}

static void test_usclock_read(void)
{
    struct usclock c;
    usclock_init(&c, 1000, 50, 24); /* 50 µs periods, 24 ticks a microsecond */
    CHECK_EQ(usclock_read(&c, 0, false, 24), 1001);
    CHECK_EQ(usclock_read(&c, 240, false, 264), 1011);
    /* the period ended, its interrupt has not run: counted */
    CHECK_EQ(usclock_read(&c, 1190, true, 48), 1052);
    usclock_period(&c); /* the interrupt */
    CHECK_EQ(c.base_us, 1050);
    CHECK_EQ(usclock_read(&c, 72, false, 72), 1053);
    /* the counter wrapped after the flag was read: the first count is this period's */
    CHECK_EQ(usclock_read(&c, 1190, false, 10), 1099);
    /* never back */
    usclock_period(&c);
    CHECK_EQ(usclock_read(&c, 0, false, 0), 1100);
    CHECK_EQ(usclock_read(&c, 0, false, 0), 1100);
    c.last_us = 1130;
    CHECK_EQ(usclock_read(&c, 0, false, 24), 1130);
    CHECK_EQ(usclock_read(&c, 0, false, 960), 1140);
    /* across the wrap of 32 bits */
    usclock_init(&c, 0xFFFFFFE0u, 50, 1);
    CHECK_EQ(usclock_read(&c, 0, false, 16), 0xFFFFFFF0u);
    usclock_period(&c);
    CHECK_EQ(usclock_read(&c, 0, false, 0), 0x12u);
    /* a zero rate is taken as 1 */
    usclock_init(&c, 0, 50, 0);
    CHECK_EQ(usclock_read(&c, 7, false, 7), 7);
}

static void test_usclock_sync(void)
{
    struct usclock c;
    uint32_t resyncs;
    int32_t lo, hi;
    usclock_init(&c, 10000, 50, 24);
    usclock_take_window(&c, &resyncs, &lo, &hi);
    CHECK_EQ(resyncs, 0);
    CHECK_EQ(lo, 0);
    CHECK_EQ(hi, 0);

    usclock_sync(&c, 9400);  /* ahead of the millisecond reference: as expected */
    usclock_sync(&c, 11500); /* behind by 1.5 ms: kept */
    CHECK_EQ(c.base_us, 10000);
    usclock_sync(&c, 12500); /* behind by 2.5 ms: periods were lost */
    CHECK_EQ(c.base_us, 12500);
    CHECK_EQ(c.resyncs, 1);
    CHECK_EQ(usclock_read(&c, 0, false, 0), 12500);
    usclock_take_window(&c, &resyncs, &lo, &hi);
    CHECK_EQ(resyncs, 1);
    CHECK_EQ(lo, -2500);
    CHECK_EQ(hi, 600);
    usclock_take_window(&c, &resyncs, &lo, &hi); /* a new window */
    CHECK_EQ(resyncs, 0);
    CHECK_EQ(hi, 0);
    CHECK_EQ(c.resyncs, 1);
}

static void test_window(void)
{
    struct metrics_state st;
    struct metrics_window w;
    smclog_host_set_clock(clock_now);
    smclog_reset();
    smclog_init();
    T = 1000;
    metrics_state_init(&st, T);
    LOG_INFO(test, "one");
    LOG_INFO(test, "two");
    T = 11000;
    metrics_begin(&st, &w, T);
    CHECK_EQ(w.seq, 1);
    CHECK_EQ(w.window_us, 10000);
    CHECK_EQ(w.log_records, 2);
    CHECK_EQ(w.log_lost, 0);
    CHECK_EQ(w.log_ring, SMCLOG_RING_SIZE);
    CHECK(w.log_used_max > 0);
    CHECK(!w.have_scan && !w.have_clock && !w.have_traffic && !w.have_memory);
    CHECK_EQ(w.stacks, 0);
    T = 15000;
    metrics_begin(&st, &w, T);
    CHECK_EQ(w.seq, 2);
    CHECK_EQ(w.window_us, 4000);
    CHECK_EQ(w.log_records, 0);
}

static void test_records(void)
{
    struct metrics_state st;
    struct metrics_window w;
    smclog_reset();
    smclog_init();
    T = 0;
    metrics_state_init(&st, T);
    T = 10000000;
    metrics_begin(&st, &w, T);
    uint32_t before = records();
    metrics_log(&w); /* heartbeat, two empty latencies, the log */
    CHECK_EQ(records() - before, 4);

    metrics_begin(&st, &w, T + 10000000);
    w.have_scan = w.have_clock = w.have_traffic = w.have_memory = true;
    lat_hist_add(&w.lat_core, 300);
    lat_hist_add(&w.lat_usb, 900);
    w.stacks = 2;
    w.stack[0].name = "smc_usb";
    w.stack[0].size = 2048;
    w.stack[0].unused = 1200;
    w.stack[1].name = "smc_ctl";
    w.stack[1].size = 3072;
    w.stack[1].unused = METRICS_STACK_UNKNOWN;
    w.busy_ctl_us = 25000; /* 2.5 ‰ of 10 s */
    before = records();
    metrics_log(&w); /* + 2 scan, clock, 2 buckets, traffic, heap, 2 stacks */
    CHECK_EQ(records() - before, 13);
    CHECK_EQ(w.lat_usb.n, 1); /* metrics_log does not change w */

    w.have_load = w.have_adc = true; /* + the CPU load and the fader ADC */
    w.load_samples = 200000;
    w.load_busy = 30000;
    w.adc_samples = 0; /* no division by zero */
    before = records();
    metrics_log(&w);
    CHECK_EQ(records() - before, 15);
}

int main(void)
{
    RUN(test_buckets);
    RUN(test_percentiles);
    RUN(test_wide);
    RUN(test_span);
    RUN(test_stack);
    RUN(test_usclock_read);
    RUN(test_usclock_sync);
    RUN(test_window);
    RUN(test_records);
    printf("test_metrics: %s\n", check_failed ? "FAILED" : "all ok");
    return check_failed ? 1 : 0;
}
