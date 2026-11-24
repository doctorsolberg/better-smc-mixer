/*
 * The controller (variant.h, the app variant; docs/core.md, "On the device"): the panel scan, the
 * core on the factory configuration and USB MIDI.
 *
 * - The scan (input/scan.h on platform/br25/input_br25.c, the same as the hardware test's) puts
 *   events, the faders' included, into the queue from its interrupt and wakes the control task
 *   through input_br25_wake() (the interrupt may not call the RTOS: input_br25.h).
 * - The control task "smc_ctl" waits for that, at most one OS tick (10 ms). Each time it runs it
 *   runs ctl_run() (ctl/ctl.h): every queued event into the core, a tick, the LEDs. The core's MIDI goes into a packet queue (midi/midiq.h); the USB task
 *   sends it to the host.
 * - MIDI from the host (USB task) and injected events (the command link, LOG-33) go into the same
 *   queue and wake the control task.
 * - BLE=1 (docs/ble.md): the core's MIDI also goes into the BLE queue (midi/bleq.h, with the
 *   merge), which the BLE sender empties into notifications (ble/ble_br25.c); MIDI from a BLE
 *   central enters the input queue as CORE_FROM_BLE, and so do a central's arrival and departure
 *   (CORE_EV_LINK: the BT LED). The BT button's long press turns Bluetooth off and on (the core;
 *   SPEC §5.5): off, the output goes to USB alone and the module neither advertises nor keeps a
 *   central (FR-IO-04).
 * - The command link offers raw input (LOG-07), metrics (LOG-06) and injection, as in the hardware
 *   test, and CORE_SETUP (LOG-35): while injection is on, the core can be restarted on a built-in
 *   configuration with settings (core_settings.h), in RAM only, for scenarios run on the device.
 * - The configuration protocol (link/cfglink.h, docs/link.md, "Configuration"): the host reads
 *   the running configuration and replaces it, in RAM only (no flash write: SPEC C-6). A received
 *   one is checked in the USB task and applied by the control task between rounds (ctl.h).
 * - Every 10 seconds, and on METRICS_NOW: the statistics since start, and the metrics heartbeat
 *   (metrics_app.h, LOG-06), which the control task writes after its round: the queue of MIDI
 *   packets is its own.
 *
 * ✅ Runs on the mixer since 2026-11-11 (docs/core.md, "Scenarios on the device").
 */
#include "system/includes.h"

#include "app_version.h"
#include "app_tasks.h"
#include "ble_br25.h"
#include "bleq.h"
#include "bootsel.h"
#include "cfgbin.h"
#include "cfglink.h"
#include "cfgstore.h"
#include "core.h"
#include "core_settings.h"
#include "ctl.h"
#include "diag_br25.h"
#include "evq.h"
#include "flash_br25.h"
#include "inject.h"
#include "input_br25.h"
#include "metrics_app.h"
#include "midiq.h"
#include "scan.h"
#include "smc_ram.h"
#include "smclog.h"
#include "smclog_port.h"
#include "statestore.h"
#include "usb_app.h"
#include "variant.h"

#include <string.h>

#define CTL_WAIT_TICKS 1 /* 10 ms: ticks and blinking at least this often */
#define STATS_MS METRICS_APP_MS

static struct core_config config;
static struct evq queue;
static struct scan scan;
static struct inject inj;
static struct midiq midi_out;
#ifdef SMC_BLE
static struct bleq ble_out; /* 1.7 KB of RAM: BLE=1 only */
#define BLE_OUT (&ble_out)
#else
#define BLE_OUT NULL
#endif
static struct ctl ctl;
static struct cfglink cfgl;
static uint8_t cfg_buf[CFGLINK_BUF_BYTES]; /* the configuration protocol's transfer (cfglink.h) */
static struct cfgstore store; /* the configuration's flash slots (cfgstore.h) */
static struct statestore state_store; /* the panel's state (statestore.h) */
static OS_SEM ctl_sem;
static volatile bool heartbeat_due;

/* The wake's delay (docs/core.md, "Fast sweeps"): the first wake since the last round, set by the
 * wake interrupt, taken by the control task. A wake that comes between the task's read and its
 * clear is missed: a diagnostic, not a count. */
static volatile uint32_t wake_t_us;
static volatile bool wake_set;
static struct {
    uint32_t n, sum_us, max_us;
} wake_delay;

/* The scan's sink, in its interrupt: queue, and wake the control task through the waker when
 * the queue was empty. An event that finds others waiting needs no wake: the control task takes
 * every queued event before it waits again, and the event that made the queue non-empty woke it
 * (docs/core.md, "Fader events merged"). */
SMC_RAM_CODE static bool sink(void *ctx, const struct core_event *ev)
{
    struct evq *q = ctx;
    if (!evq_push(q, ev))
        return false; /* counted; the scan offers it again */
    if (q->was_empty) /* read in the scan interrupt, which nothing preempts */
        input_br25_wake();
    return true;
}

/* The waker's handler (input_br25_set_waker()). */
static void wake_ctl(void)
{
    if (!wake_set) {
        wake_t_us = smclog_port_time_us();
        wake_set = true;
    }
    os_sem_post(&ctl_sem);
}

static void show_led(void *ctx, unsigned led, bool on)
{
    (void)ctx;
    scan_led(&scan, led, on); /* the frame the scan interrupt draws */
}

static void midi_ready(void *ctx)
{
    (void)ctx;
    usb_app_wake();
}

static void ble_ready(void *ctx)
{
    (void)ctx;
    ble_br25_wake();
}

static void bluetooth(void *ctx, bool on)
{
    (void)ctx;
    ble_br25_enable(on);
}

static const struct ctl_ops ctl_ops = {.led = show_led, .midi_ready = midi_ready, .ble_ready = ble_ready,
                                       .bluetooth = bluetooth};

/* The USB task: the MIDI queue has room again after the control task stopped for it. */
static void midi_room(void)
{
    os_sem_post(&ctl_sem);
}

/* The control task: the metrics heartbeat (LOG-06) after a round. */
static void heartbeat(void)
{
    static struct metrics_window w; /* larger than a frame should be */
    metrics_app_begin(&w);
    ble_br25_take_window(&w);
    ctl_take_window(&ctl, &w);
    metrics_log(&w);
    ctl_log_timing(&ctl);
    LOG_INFO(engine, "control wake: %u from the scan, to the round avg %u max %u us", wake_delay.n,
             wake_delay.n ? wake_delay.sum_us / wake_delay.n : 0, wake_delay.max_us);
    memset(&wake_delay, 0, sizeof wake_delay);
}

static void ctl_task(void *priv)
{
    (void)priv;
    metrics_app_task_start(APP_TASK_CTL, APP_TASK_CTL_STACK);
    for (;;) {
        os_sem_pend(&ctl_sem, CTL_WAIT_TICKS);
        uint32_t now = smclog_port_time_us();
        if (wake_set) {
            uint32_t d = (int32_t)(now - wake_t_us) > 0 ? now - wake_t_us : 0; /* a wake after now */
            wake_set = false;
            wake_delay.n++;
            wake_delay.sum_us += d;
            if (d > wake_delay.max_us)
                wake_delay.max_us = d;
        }
        ctl_run(&ctl, now);
        if (heartbeat_due) {
            heartbeat_due = false;
            heartbeat();
        }
    }
}

/* Any task: the control task writes a heartbeat after its next round. */
static void request_heartbeat(void)
{
    heartbeat_due = true;
    os_sem_post(&ctl_sem);
}

static void stats(void *priv)
{
    struct input_br25_stats hw;
    struct scan_stats st;
    uint16_t depth_max;
    uint32_t refused, pushed, merged;

    (void)priv;
    input_br25_get_stats(&hw);
    uint32_t l = input_port_lock();
    st = scan.stats;
    depth_max = queue.depth_max;
    refused = queue.refused;
    pushed = queue.pushed;
    merged = queue.merged;
    input_port_unlock(l);
    unsigned tpu = hw.ticks_per_us ? hw.ticks_per_us : 1;
    LOG_INFO(input, "scan: %u steps, %u skipped (SPI busy); handler max %u us, late max %u us; "
             "%u bounces, %u encoder jumps; queue max %u, refused %u",
             hw.steps, hw.spi_busy, hw.isr_max / tpu, hw.late_max / tpu, st.bounces,
             st.enc_invalid, depth_max, refused);
    LOG_INFO(input, "encoders: %u skipped states inferred while turning", st.enc_inferred);
    LOG_INFO(input, "input queue: %u queued, %u fader events merged into a waiting one, %u refused",
             pushed, merged, refused);
    LOG_INFO(input, "wake: %u asked by the scan, %u taken by its interrupt; its handler %u us in all, "
             "%u us at most", hw.wake_asked, hw.wake_taken, hw.wake_us_sum, hw.wake_us_max);
    ctl_log_stats(&ctl);
    ble_br25_log_stats();
}

static void every_10s(void *priv)
{
    (void)priv;
    stats(NULL);
    request_heartbeat();
}

/* --- USB task -------------------------------------------------------------------------------- */

static void midi_in_from(unsigned transport, const uint8_t msg[3])
{
    struct core_event ev;
    memset(&ev, 0, sizeof ev);
    ev.t_us = smclog_port_time_us();
    ev.type = CORE_EV_MIDI_IN;
    ev.index = (uint8_t)transport;
    memcpy(ev.midi, msg, 3);
    if (evq_push(&queue, &ev)) /* a full queue counts it */
        os_sem_post(&ctl_sem);
}

static void midi_in(const uint8_t msg[3])
{
    midi_in_from(CORE_FROM_USB, msg);
}

/* The BT stack's task: a channel message from the BLE central. */
static void ble_in(const uint8_t msg[3])
{
    midi_in_from(CORE_FROM_BLE, msg);
}

/* The BT stack's task: a central connected or left. */
static void ble_link(bool up)
{
    struct core_event ev;
    memset(&ev, 0, sizeof ev);
    ev.t_us = smclog_port_time_us();
    ev.type = CORE_EV_LINK;
    ev.index = CORE_FROM_BLE;
    ev.value = up;
    if (evq_push(&queue, &ev)) /* a full queue counts it */
        os_sem_post(&ctl_sem);
}

static const struct ble_br25_midi ble_midi = {.out = BLE_OUT, .in = ble_in, .room = midi_room,
                                              .link = ble_link};

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
    request_heartbeat();
}

static void do_inject(void *ctx, uint32_t t_us, const uint8_t *p, size_t len, uint8_t out[3])
{
    (void)ctx;
    struct inject_reply r = inject_command(&inj, t_us, p, len);
    if (r.accepted)
        os_sem_post(&ctl_sem);
    out[0] = r.status;
    out[1] = r.accepted;
    out[2] = r.refused;
}

/* CORE_SETUP: parsed here, applied by the control task in its next round (ctl.h). */
static void do_setup(void *ctx, const uint8_t *p, size_t len, uint8_t out[2])
{
    struct core_settings s;
    (void)ctx;
    out[1] = 0;
    if (!inj.on) { /* a debug command, like injection (LOG-34) */
        out[0] = LINK_SETUP_DISABLED;
        return;
    }
    core_settings_init(&s, CORE_CONFIG_FACTORY);
    unsigned bad = core_settings_parse(&s, (const char *)p, len);
    if (bad) {
        out[0] = LINK_SETUP_BAD;
        out[1] = (uint8_t)(bad > 255 ? 255 : bad);
        return;
    }
    if (!ctl_request_setup(&ctl, &s)) {
        out[0] = LINK_SETUP_BUSY;
        return;
    }
    os_sem_post(&ctl_sem);
    out[0] = LINK_SETUP_OK;
}

/* The configuration protocol on the control path (cfglink.h): from the USB task, the one task that
 * asks for setups and configurations. */
static size_t cfg_get(void *ctx, uint8_t *out, size_t cap, struct cfgbin_error *err)
{
    (void)ctx;
    return ctl_encode_config(&ctl, out, cap, err);
}

static bool cfg_apply(void *ctx, const uint8_t *blob, uint32_t len)
{
    (void)ctx;
    if (!ctl_request_config(&ctl, blob, len))
        return false;
    os_sem_post(&ctl_sem);
    return true;
}

static bool cfg_pending(void *ctx)
{
    (void)ctx;
    return ctl_change_pending(&ctl);
}

static void cfg_active(void *ctx, uint8_t *source, struct cfgbin_meta *meta)
{
    (void)ctx;
    if (ctl_change_pending(&ctl))
        return; /* being rewritten: CFG_INFO says APPLYING, or a setup waits */
    *source = ctl.source;
    *meta = ctl.meta;
}

static uint32_t cfg_touched(void *ctx)
{
    (void)ctx;
    return ctl.last_touch_us;
}

_Static_assert(CTL_SOURCE_RECEIVED == CFGLINK_SOURCE_RECEIVED, "the source cfglink saves");
_Static_assert(CTL_SOURCE_FACTORY == CFGLINK_SOURCE_FACTORY && CTL_SOURCE_STORED == CFGLINK_SOURCE_STORED,
               "the sources cfglink knows");

static uint32_t cfg_state(void *ctx)
{
    (void)ctx;
    return ctl_state(&ctl);
}

static bool cfg_edit(void *ctx, unsigned preset, const uint8_t *rec, uint32_t len, uint32_t revision)
{
    (void)ctx;
    if (!ctl_request_edit(&ctl, preset, rec, len, revision))
        return false;
    os_sem_post(&ctl_sem);
    return true;
}

static bool cfg_revert(void *ctx, const uint8_t *blob, uint32_t len)
{
    (void)ctx;
    if (!ctl_request_revert(&ctl, blob, len))
        return false;
    os_sem_post(&ctl_sem);
    return true;
}

static bool cfg_autosave(void *ctx)
{
    (void)ctx;
    return ctl.cfg->global.save == CORE_SAVE_AUTO; /* one byte, read across tasks */
}

static void cfg_panel(void *ctx, uint8_t out[CFGLINK_PANEL_BYTES])
{
    (void)ctx;
    ctl_panel_bytes(ctl_panel(&ctl), out);
}

static bool cfg_select(void *ctx, int preset, int page)
{
    (void)ctx;
    if (!ctl_request_select(&ctl, preset, page))
        return false;
    os_sem_post(&ctl_sem);
    return true;
}

static unsigned cfg_sent(void *ctx, uint32_t from, uint8_t (*out)[CFGLINK_SENT_LEN], unsigned max,
                         uint32_t *first)
{
    (void)ctx;
    return ctl_sent_read(&ctl, from, out, max, first);
}

_Static_assert(CTL_PANEL_BYTES == CFGLINK_PANEL_BYTES, "the panel's part of the state");
_Static_assert(CTL_SENT_LEN == CFGLINK_SENT_LEN && CTL_SENT_NOW == CFGLINK_SENT_NOW, "a sent entry");

static const struct cfglink_ops cfg_ops = {.get = cfg_get,
                                           .apply = cfg_apply,
                                           .pending = cfg_pending,
                                           .active = cfg_active,
                                           .touched_at = cfg_touched,
                                           .state = cfg_state,
                                           .edit = cfg_edit,
                                           .revert = cfg_revert,
                                           .autosave = cfg_autosave,
                                           .panel = cfg_panel,
                                           .select = cfg_select,
                                           .sent = cfg_sent};

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

/* STOCK: going back to M-Vave's firmware (store/bootsel.h; docs/image.md, "Going back"). Not while
 * a configuration is being saved: the reset that follows would cut it. Sector 0's 4 KB come from
 * the heap for the command alone (the BLE stack's heap is the scarce one: build_app.sh prints it). */

static void do_stock(void *ctx, uint8_t what, uint8_t out[LINK_STOCK_REPLY])
{
    (void)ctx;
    struct bootsel_info i;
    memset(&i, 0, sizeof i);
    int st;
    uint8_t *boot_sector = cfgstore_busy(&store) ? NULL : malloc(BOOTSEL_SECTOR);
    if (!boot_sector) {
        st = BOOTSEL_BUSY;
    } else if (what == LINK_STOCK_CHECK) {
        st = bootsel_check(flash_br25_boot_ops(), boot_sector, &i);
        LOG_INFO(boot, "going back to M-Vave's firmware, the check: %s (sector 0 points at 0x%x, its area 0x%x bytes)",
                 bootsel_name(st), (unsigned)i.pointer, (unsigned)i.stock_size);
    } else {
        LOG_WARN(boot, "going back to M-Vave's firmware: sector 0 is written");
        uint32_t t0 = smclog_port_time_us();
        st = bootsel_to_stock(flash_br25_boot_ops(), boot_sector, &i);
        LOG_WARN(boot, "going back to M-Vave's firmware: %s, sector 0 points at 0x%x, %u tries, %u us", bootsel_name(st),
                 (unsigned)i.pointer, (unsigned)i.tries, (unsigned)(smclog_port_time_us() - t0));
        if (st == BOOTSEL_OK)
            diag_br25_reset(LINK_RESET_SOFT);
    }
    free(boot_sector);
    out[0] = (uint8_t)st;
    for (unsigned k = 0; k < 4; k++)
        out[1 + k] = (uint8_t)(i.pointer >> (8 * k));
    out[5] = i.tries;
}

static const struct link_ops ops = {.variant = 0,
                                    .version = APP_VERSION,
                                    .raw_input = raw_input,
                                    .metrics = metrics,
                                    .boot_log = diag_br25_log,
                                    .reset = do_reset,
                                    .stock = do_stock,
                                    .inject = do_inject,
                                    .setup = do_setup,
                                    .config = cfglink_command,
                                    .cfg = &cfgl,
                                    .cfg_poll = cfglink_poll};
static const struct variant app = {
    .usb_product = "SMC-Mixer", .usb_serial = "C1", .link = &ops, .midi = {.in = midi_in, .out = &midi_out, .room = midi_room}};

const struct variant *variant_start(void)
{
    struct cfgbin_meta meta;
    LOG_INFO(boot, "SMC-Mixer controller: USB MIDI");
    cfgstore_init(&store, flash_br25_ops());
    cfglink_init(&cfgl, &cfg_ops, cfg_buf, sizeof cfg_buf, &store);
    bool stored = cfglink_load(&cfgl, &config, &meta); /* the factory configuration if none */
    evq_init(&queue);
    evq_set_merge(&queue, true);
    midiq_init(&midi_out);
    inject_init(&inj, &queue);
    os_sem_create(&ctl_sem, 0);
    scan_init(&scan, &scan_default_params, sink, &queue);
    ctl_init(&ctl, &config, &queue, &midi_out, &ctl_ops); /* draws every LED */
    if (stored)
        ctl_set_stored(&ctl, &meta);
    ble_br25_set_midi(&ble_midi); /* without BLE=1: nothing (ble_stub.c) */
    /* the command link over BLE MIDI, served by the USB task (without BLE=1: none) */
    usb_app_set_ble_link(ble_br25_link_pipe(usb_app_wake), ble_br25_wake);
#ifdef SMC_BLE
    bleq_init(&ble_out);
    ctl_set_ble(&ctl, &ble_out); /* without a sender the queue would fill and hold the round */
#endif
    /* the panel's state as it was left: Bluetooth, the preset and the page */
    uint32_t state;
    statestore_init(&state_store, flash_br25_ops());
    if (statestore_load(&state_store, &state))
        ctl_restore_state(&ctl, state);
    ble_br25_enable(!ctl.core.bt_none && ctl.core.bt_on); /* before the stack is up: it advertises or not */
    cfglink_set_state(&cfgl, &state_store);
    diag_br25_mark("core");
    input_br25_set_waker(wake_ctl);
    input_br25_start(&scan);
    diag_br25_mark("scan");
    int err = task_create(ctl_task, NULL, APP_TASK_CTL);
    if (err)
        LOG_ERROR(boot, "control task not created: error %d", err);
    sys_timer_add(NULL, every_10s, STATS_MS);
    diag_br25_mark("control task");
    return &app;
}
