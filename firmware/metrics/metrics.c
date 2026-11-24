/* Metrics (metrics.h). */
#include "metrics.h"

#include "smc_ram.h"

#include <string.h>

#include "smclog.h"

const uint32_t lat_bound_us[LAT_BUCKETS - 1] = {250, 500, 1000, 2000, 3000, 5000, 10000};
const uint32_t lat_bound_wide_us[LAT_BUCKETS - 1] = {5000, 10000, 15000, 20000, 25000, 35000, 50000};

static uint32_t add_sat(uint32_t a, uint32_t b)
{
    uint32_t s = a + b;
    return s < a ? UINT32_MAX : s;
}

void lat_hist_clear(struct lat_hist *h)
{
    memset(h, 0, sizeof *h);
}

void lat_hist_clear_wide(struct lat_hist *h)
{
    memset(h, 0, sizeof *h);
    h->wide = 1;
}

SMC_HOT void lat_hist_add(struct lat_hist *h, uint32_t us)
{
    const uint32_t *bound = h->wide ? lat_bound_wide_us : lat_bound_us;
    unsigned i = 0;
    while (i < LAT_BUCKETS - 1 && us >= bound[i])
        i++;
    h->bucket[i]++;
    if (h->n == 0 || us < h->min_us)
        h->min_us = us;
    if (us > h->max_us)
        h->max_us = us;
    h->n++;
    h->sum_us = add_sat(h->sum_us, us);
}

uint32_t lat_hist_percentile(const struct lat_hist *h, unsigned permille)
{
    if (h->n == 0)
        return 0;
    if (permille > 1000)
        permille = 1000;
    /* the rank of the sample, from 1: ceil(n × permille / 1000), at least 1 (in 32 bits) */
    uint32_t rank = h->n / 1000u * permille + (h->n % 1000u * permille + 999u) / 1000u;
    if (rank == 0)
        rank = 1;
    const uint32_t *bound = h->wide ? lat_bound_wide_us : lat_bound_us;
    uint32_t seen = 0;
    for (unsigned i = 0; i < LAT_BUCKETS - 1; i++) {
        seen += h->bucket[i];
        if (seen >= rank)
            return bound[i] < h->max_us ? bound[i] : h->max_us;
    }
    return h->max_us;
}

void span_clear(struct span *s)
{
    memset(s, 0, sizeof *s);
}

SMC_RAM_CODE void span_add(struct span *s, uint32_t v)
{
    if (s->n == 0 || v < s->min)
        s->min = v;
    if (v > s->max)
        s->max = v;
    s->n++;
    s->sum = add_sat(s->sum, v);
}

void stack_mark_init(struct stack_mark *m, uintptr_t lo, uintptr_t hi, uintptr_t sp)
{
    m->lo = lo;
    m->hi = hi;
    m->deepest = sp;
}

SMC_RAM_CODE bool stack_mark_sample(struct stack_mark *m, uintptr_t sp)
{
    if (sp < m->lo || sp >= m->hi)
        return false;
    if (sp < m->deepest)
        m->deepest = sp;
    return true;
}

size_t stack_mark_free(const struct stack_mark *m)
{
    return m->deepest - m->lo;
}

void metrics_state_init(struct metrics_state *st, uint32_t now_us)
{
    struct smclog_stats ls;
    memset(st, 0, sizeof *st);
    st->last_us = now_us;
    smclog_get_stats(&ls);
    st->log_records = ls.records;
    st->log_lost = ls.lost;
}

void metrics_begin(struct metrics_state *st, struct metrics_window *w, uint32_t now_us)
{
    struct smclog_stats ls;
    memset(w, 0, sizeof *w);
    w->seq = ++st->seq;
    w->window_us = now_us - st->last_us;
    st->last_us = now_us;
    smclog_get_stats(&ls);
    w->log_records = ls.records - st->log_records;
    w->log_lost = ls.lost - st->log_lost;
    w->log_used_max = ls.used_max;
    w->log_ring = SMCLOG_RING_SIZE;
    st->log_records = ls.records;
    st->log_lost = ls.lost;
}

/* Per mille of the window, at most 1000. */
static uint32_t share(uint32_t busy_us, uint32_t window_us)
{
    uint32_t ms = window_us / 1000u;
    if (ms == 0)
        return 0;
    uint32_t pm = busy_us / ms;
    return pm > 1000u ? 1000u : pm;
}

static uint32_t avg(uint32_t sum, uint32_t n)
{
    return n ? sum / n : 0;
}

/* One literal set per path: a call site's format must be a string literal. */
enum { LAT_CORE, LAT_USB, LAT_BLE };
static void log_latency(const struct lat_hist *h, unsigned path)
{
    const uint32_t *b = h->bucket;
    uint32_t mean = avg(h->sum_us, h->n), p50 = lat_hist_percentile(h, 500),
             p99 = lat_hist_percentile(h, 990);
    if (path == LAT_BLE) {
        LOG_INFO(metrics, "latency input to BLE: %u events; min %u, avg %u, max %u us; "
                 "p50 <= %u, p99 <= %u us", h->n, h->min_us, mean, h->max_us, p50, p99);
        if (h->n)
            LOG_INFO(metrics, "latency input to BLE, events by bucket: <5 ms %u, <10 %u, <15 %u, "
                     "<20 %u, <25 %u, <35 %u, <50 %u, more %u", b[0], b[1], b[2], b[3], b[4], b[5],
                     b[6], b[7]);
    } else if (path == LAT_CORE) {
        LOG_INFO(metrics, "latency input to core: %u events; min %u, avg %u, max %u us; "
                 "p50 <= %u, p99 <= %u us", h->n, h->min_us, mean, h->max_us, p50, p99);
        if (h->n)
            LOG_INFO(metrics, "latency input to core, events by bucket: <0.25 ms %u, <0.5 %u, <1 %u, "
                     "<2 %u, <3 %u, <5 %u, <10 %u, more %u", b[0], b[1], b[2], b[3], b[4], b[5],
                     b[6], b[7]);
    } else {
        LOG_INFO(metrics, "latency input to USB: %u events; min %u, avg %u, max %u us; "
                 "p50 <= %u, p99 <= %u us", h->n, h->min_us, mean, h->max_us, p50, p99);
        if (h->n)
            LOG_INFO(metrics, "latency input to USB, events by bucket: <0.25 ms %u, <0.5 %u, <1 %u, "
                     "<2 %u, <3 %u, <5 %u, <10 %u, more %u", b[0], b[1], b[2], b[3], b[4], b[5],
                     b[6], b[7]);
    }
}

void metrics_log(const struct metrics_window *w)
{
    LOG_INFO(metrics, "heartbeat %u: %u ms; CPU of our code, per mille: scan %u, control %u, USB %u",
             w->seq, w->window_us / 1000u, share(w->busy_scan_us, w->window_us),
             share(w->busy_ctl_us, w->window_us), share(w->busy_usb_us, w->window_us));
    if (w->have_scan) {
        LOG_INFO(metrics, "scan steps: %u, %u skipped; handler avg %u, max %u ns; late max %u us",
                 w->scan_steps, w->scan_skipped, w->scan_step_avg_ns, w->scan_step_max_ns,
                 w->scan_late_max_us);
        LOG_INFO(metrics, "scan passes: %u; every min %u, avg %u, max %u us", w->scan_pass_us.n,
                 w->scan_pass_us.min, avg(w->scan_pass_us.sum, w->scan_pass_us.n),
                 w->scan_pass_us.max);
    }
    if (w->have_load)
        LOG_INFO(metrics, "CPU load: %u per mille (%u of %u samples not idle)",
                 w->load_samples ? (uint32_t)((uint64_t)w->load_busy * 1000u / w->load_samples) : 0u,
                 w->load_busy, w->load_samples);
    if (w->have_adc)
        LOG_INFO(metrics, "fader ADC: %u samples, each fader every %u us; %u steps the SDK had it, "
                 "%u lost to it, %u timeouts; longest conversion %u steps", w->adc_samples,
                 w->adc_samples ? (uint32_t)((uint64_t)w->window_us * 8u / w->adc_samples) : 0u,
                 w->adc_deferred, w->adc_lost, w->adc_timeouts, w->adc_steps_max);
    if (w->have_clock)
        LOG_INFO(metrics, "clock: %u resyncs; drift from the reference %d..%d us", w->clock_resyncs,
                 w->clock_drift_min_us, w->clock_drift_max_us);
    log_latency(&w->lat_core, LAT_CORE);
    log_latency(&w->lat_usb, LAT_USB);
    if (w->have_traffic)
        LOG_INFO(metrics, "traffic: %u events; MIDI %u out, %u in, %u dropped; queues max: input %u, "
                 "MIDI out %u; refused %u, %u", w->events, w->midi_out, w->midi_in, w->midi_dropped,
                 w->evq_max, w->midiq_max, w->evq_refused, w->midiq_refused);
    if (w->have_ble) {
        log_latency(&w->lat_ble, LAT_BLE);
        LOG_INFO(metrics, "BLE: %u messages in %u notifications, %u merged, %u dropped; queue max %u, "
                 "refused %u; %u in, %u faults", w->ble_out, w->ble_notify, w->ble_merged,
                 w->ble_dropped, w->bleq_max, w->bleq_refused, w->ble_in, w->ble_in_bad);
    }
    LOG_INFO(metrics, "log: %u records, %u lost; ring max %u of %u bytes", w->log_records,
             w->log_lost, w->log_used_max, w->log_ring);
    if (w->have_memory)
        LOG_INFO(metrics, "heap: %u bytes free, %u lowest", w->heap_free, w->heap_min);
    for (unsigned i = 0; i < w->stacks && i < METRICS_STACKS; i++) {
        if (w->stack[i].unused == METRICS_STACK_UNKNOWN)
            LOG_INFO(metrics, "stack %s: %u bytes, not measured (bounds not found)", w->stack[i].name,
                     w->stack[i].size);
        else
            LOG_INFO(metrics, "stack %s: %u of %u bytes free below the deepest point sampled",
                     w->stack[i].name, w->stack[i].unused, w->stack[i].size);
    }
}
