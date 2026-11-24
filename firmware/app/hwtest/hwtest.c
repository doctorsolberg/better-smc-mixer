/*
 * The hardware-test variant (variant.h, docs/hwtest.md): scans every input with raw input mode on
 * (LOG-07), logs each event, walks the LEDs and logs the scan's statistics every second, the USB
 * driver's and the command link's and the metrics heartbeat (metrics_app.h, LOG-06: the scan's
 * timing, the microsecond clock, the heap, the USB task's stack) every 10 seconds.
 *
 * Over the command link (docs/link.md) it serves LOG_RAW_INPUT, METRICS_NOW and INPUT_INJECT
 * (LOG-33): injected events go through the same queue and are logged as "(injected)". Channel
 * messages the host sends on the USB MIDI performance port are queued and logged too; it sends no
 * MIDI.
 *
 * The core is not wired in: this build answers what the hardware is (pins, matrix and LED
 * placement, encoder wiring, fader channels, timing), not how the controls behave.
 */
#include "system/includes.h"

#include "app_version.h"
#include "bootmark_br25.h"
#include "diag_br25.h"
#include "evq.h"
#include "inject.h"
#include "input_br25.h"
#include "metrics_app.h"
#include "scan.h"
#include "smclog.h"
#include "smclog_port.h"
#include "usb_app.h"
#include "variant.h"

#include <string.h>

#define POLL_MS 10       /* events out of the queue */
#define WALK_MS 1000     /* one LED, then one LED matrix place, per second */
#define STATS_MS 1000
#define USB_STATS_EVERY (METRICS_APP_MS / STATS_MS) /* statistics periods */

static struct evq queue;
static struct scan scan;
static struct inject inj;
static unsigned walk; /* 0-52: the LEDs by index; then the 64 places of the LED matrix */
static unsigned stats_count;
static volatile bool heartbeat_due; /* METRICS_NOW: poll() writes it */

/* The timers' context (poll, stats): the metrics heartbeat. */
static void heartbeat(void)
{
    static struct metrics_window w; /* larger than a frame should be */
    static uint32_t pushed_from, refused_from;
    metrics_app_begin(&w);
    uint32_t pushed = queue.pushed, refused = queue.refused; /* one word each */
    w.have_traffic = true;
    w.events = pushed - pushed_from;
    w.evq_refused = refused - refused_from;
    w.evq_max = evq_take_window_max(&queue);
    pushed_from = pushed;
    refused_from = refused;
    metrics_log(&w);
}

static void poll(void *priv)
{
    struct core_event ev;

    (void)priv;
    if (heartbeat_due) {
        heartbeat_due = false;
        heartbeat();
    }
    while (evq_pop(&queue, &ev)) {
        const char *how = ev.flags & CORE_EV_INJECTED ? " (injected)" : "";
        switch (ev.type) {
        case CORE_EV_BUTTON:
            LOG_INFO(input, "button %u %s%s", ev.index, ev.value ? "press" : "release", how);
            break;
        case CORE_EV_ENCODER:
            LOG_INFO(input, "encoder %u %d%s", ev.index, ev.value, how);
            break;
        case CORE_EV_FADER:
            LOG_INFO(input, "fader %u %u%s", ev.index, ev.value, how);
            break;
        case CORE_EV_MIDI_IN:
            LOG_INFO(input, "MIDI in from transport %u: %02x %02x %02x%s", ev.index, ev.midi[0],
                     ev.midi[1], ev.midi[2], how);
            break;
        default:
            LOG_WARN(input, "unexpected event: type %u index %u", ev.type, ev.index);
            break;
        }
    }
}

/* What lights: LED 0, 1, ... 52 by the stock table, then every (row, column) of the LED matrix.
 * The person compares it with the log (docs/hwtest.md, "LED walk"). */
static void led_walk(void *priv)
{
    (void)priv;
    scan_leds_clear(&scan);
    if (walk < SCAN_LEDS) {
        scan_led(&scan, walk, true);
        LOG_INFO(test, "LED walk: LED %u (row %u, column %u)", walk, scan_led_pos[walk] >> 4,
                 scan_led_pos[walk] & 0x0Fu);
    } else {
        unsigned place = walk - SCAN_LEDS;
        unsigned row = place % SCAN_LED_ROWS, col = place / SCAN_LED_ROWS;
        scan_led_at(&scan, row, col, true);
        LOG_INFO(test, "LED walk: row %u, column %u", row, col);
    }
    walk = (walk + 1) % (SCAN_LEDS + SCAN_LED_ROWS * SCAN_LED_COLS);
}

static void stats(void *priv)
{
    struct input_br25_stats hw;
    struct scan_stats st;
    uint16_t depth_max;
    uint32_t refused;

    (void)priv;
    input_br25_get_stats(&hw);
    uint32_t l = input_port_lock();
    st = scan.stats;
    depth_max = queue.depth_max;
    refused = queue.refused;
    input_port_unlock(l);
    unsigned tpu = hw.ticks_per_us ? hw.ticks_per_us : 1;
    LOG_INFO(input, "scan: %u steps, %u passes, %u skipped (SPI busy); handler max %u us, late max %u us",
             hw.steps, st.passes, hw.spi_busy, hw.isr_max / tpu, hw.late_max / tpu);
    LOG_INFO(input, "events: %u buttons, %u encoders, %u faders; %u bounces, %u encoder jumps, "
             "%u refused; queue max %u, refused %u",
             st.buttons, st.encoders, st.faders, st.bounces, st.enc_invalid, st.refused, depth_max,
             refused);
    LOG_INFO(input, "encoders: %u skipped states inferred while turning", st.enc_inferred);
    if (priv && ++stats_count % USB_STATS_EVERY == 0) {
        usb_app_log_stats();
        heartbeat();
    }
}

/* --- the command link (USB task) ------------------------------------------------------------ */

static void raw_input(void *ctx, uint8_t which)
{
    (void)ctx;
    scan.raw_mode = which; /* read by the scan interrupt: a single byte; scan.h's bits are link.h's */
}
_Static_assert(SCAN_RAW_MATRIX == LINK_RAW_MATRIX && SCAN_RAW_FADERS == LINK_RAW_FADERS &&
                   SCAN_RAW_PROBE_SHIFT == LINK_RAW_PROBE_SHIFT && SCAN_ENCODERS == LINK_RAW_PROBE_MAX,
               "raw input bits: scan.h and link.h differ");

static void metrics(void *ctx)
{
    (void)ctx;
    stats(NULL);
    usb_app_log_stats();
    heartbeat_due = true;
}

static void do_inject(void *ctx, uint32_t t_us, const uint8_t *p, size_t len, uint8_t out[3])
{
    (void)ctx;
    struct inject_reply r = inject_command(&inj, t_us, p, len);
    out[0] = r.status;
    out[1] = r.accepted;
    out[2] = r.refused;
}

/* USB task: MIDI from the host into the queue, logged by poll(). */
static void midi_in(const uint8_t msg[3])
{
    struct core_event ev;
    memset(&ev, 0, sizeof ev);
    ev.t_us = smclog_port_time_us();
    ev.type = CORE_EV_MIDI_IN;
    ev.index = CORE_FROM_USB;
    memcpy(ev.midi, msg, 3);
    evq_push(&queue, &ev); /* a full queue counts it */
}

/* RESET: the debug modes only while injection is on (LOG-34). */
static void do_reset(void *ctx, uint8_t how, uint8_t out[1])
{
    (void)ctx;
    if (how != LINK_RESET_SOFT && !inj.on) {
        out[0] = LINK_RESET_DISABLED;
        return;
    }
    diag_br25_reset(how);
    out[0] = LINK_RESET_OK;
}

static const struct link_ops ops = {.variant = 1,
                                    .version = APP_VERSION,
                                    .raw_input = raw_input,
                                    .metrics = metrics,
                                    .boot_log = diag_br25_log,
                                    .reset = do_reset,
                                    .inject = do_inject};
static const struct variant hwtest = {
    .usb_product = "SMC-Mixer hardware test", .usb_serial = "T1", .link = &ops, .midi = {.in = midi_in}};

const struct variant *variant_start(void)
{
    static int periodic = 1; /* stats(): called by the timer (not NULL) or METRICS_NOW (NULL) */

    bootmark(BOOTMARK_HW_START);
    LOG_INFO(boot, "SMC-Mixer hardware test");
    evq_init(&queue);
    inject_init(&inj, &queue);
    scan_init(&scan, &scan_default_params, evq_sink, &queue);
    /* LOG-07: the matrix from start; the faders (many records) when LOG_RAW_INPUT asks for them */
    scan.raw_mode = SCAN_RAW_MATRIX;
    bootmark(BOOTMARK_HW_SCAN_INIT);
    input_br25_start(&scan);
    bootmark(BOOTMARK_HW_SCAN);
    diag_br25_mark("scan");
    sys_timer_add(NULL, poll, POLL_MS);
    sys_timer_add(NULL, led_walk, WALK_MS);
    sys_timer_add(&periodic, stats, STATS_MS);
    bootmark(BOOTMARK_HW_TIMERS);
    return &hwtest;
}
