/*
 * The control path on the device (docs/core.md, "On the device"): input events from the queue
 * (evq.h) go into the core; its MIDI goes out as USB-MIDI packets through a queue (midiq.h) to the
 * USB task, and its LED states are drawn through the platform, blinking included. Portable C; the
 * application runs ctl_run() in its control task (firmware/app/controller.c). Host tests:
 * firmware/tests/test_ctl.c.
 *
 *   ctl_init(&c, &config, &queue, &midi_out, &ops);
 *   for (;;) { wait for an event or 10 ms; ctl_run(&c, now_us); }
 *
 * Setup (LOG-35): another task can ask for the core to restart on a built-in configuration with
 * settings (core_settings.h, the CORE_SETUP debug command); ctl_run() first drains the old notes
 * in bounded chunks, then applies it before taking input. Requests stay pending during cleanup.
 *
 * A received configuration (the configuration protocol, link/cfglink.h, docs/link.md): another
 * task hands over a binary form that cfgbin_decode() has already checked; the next ctl_run()
 * decodes it into the configuration and restarts the core on it, the same way. The running
 * configuration's ID and revision (FR-APP-24) and where it came from are kept here, and
 * ctl_encode_config() writes its binary form for the host. RAM only: a reset starts on the
 * factory configuration again.
 *
 * Time: the core needs a clock that never goes back. Events are stamped where they arise (the
 * scan interrupt, the fader sampling, injection, USB) and can reach the queue slightly out of
 * order; a time earlier than the last one the core got is raised to it and counted.
 *
 * Metrics (LOG-06, metrics.h): ctl_run() measures each event's wait (its stamp → taken by the
 * core) and its own run time on the log clock (smclog_port_time_us(), the clock the events are
 * stamped with); the core's MIDI goes into the queue with the time of the event it answers, for
 * the consumer's input → USB latency. ctl_take_window() hands the window over to the heartbeat.
 *
 * The MIDI queue under load (docs/core.md, "The MIDI queue under load"): the messages one event (or
 * the tick) makes go into the queue as one unit, all or none, so an NRPN value is never split. A
 * round takes the next event only while at most CTL_MIDI_LOW packets wait for the USB; otherwise it
 * stops, leaves the events in the input queue (where a fader's waiting event takes its newer
 * positions: the newest value wins) and asks the queue's consumer to wake the control task
 * (struct ctl_ops has no part in it: the platform wires midiq_room_due() to the task's wake).
 *
 * BLE (docs/ble.md, "Design"): with a BLE queue given (ctl_set_ble()), each unit goes to it too,
 * with the core's merge marks (core.h, out_merge), while Bluetooth is on (core.h, bt_on: the BT
 * button's long press; off, the output goes to USB alone; FR-IO-04). A round also stops while fewer than CTL_UNIT_MAX messages fit the BLE
 * queue; its consumer drops what waits while no central is connected, so a missing connection never
 * holds the USB.
 */
#ifndef CTL_H
#define CTL_H

#include <stdbool.h>
#include <stdint.h>

#include "cfgbin.h"
#include "core.h"
#include "core_settings.h"
#include "bleq.h"
#include "evq.h"
#include "metrics.h"
#include "midiq.h"

#define CTL_BLINK_SLOW_US 125000u /* half a period: 4 blinks a second */
#define CTL_BLINK_FAST_US 50000u  /* 10 a second */
#define CTL_BLINK_BT_US 250000u   /* BT waiting for a connection: 2 a second, calmer (owner, 2026-11-23) */
/* Packets waiting for the USB at most for a round to take another event or the tick: keeps the
 * MIDI path short (NFR-01) while the input queue merges a fader's newer positions. */
#define CTL_MIDI_LOW 8u
/* timers(): 8 faders + 8 encoders, 2 null messages each, then one input:
 * at most 4 parameter messages + 2 for a channel-change null or a button-release null.
 * Selection emits no MIDI. This conservative additive bound also covers a tick.
 * MIDIQ_SIZE - 1 - CTL_MIDI_LOW must hold it. Cleanup uses separate bounded chunks. */
#define CTL_UNIT_MAX (2u * (CORE_STRIPS + CORE_STRIPS) + 6u)
/* What the core sent, with its source, for a client (P-5; docs/link.md, "What the mixer sent"):
 * the last CTL_SENT_RING - 1 messages, numbered (the writer may be filling the other slot). A
 * power of two. */
#define CTL_SENT_RING 64u
#define CTL_SENT_LEN 7u /* an entry as ctl_sent_read() gives it: the message, 3 bytes; preset,
                         * page (255 the row), control ID, layer; 255 each: no control */

/* Where the running configuration came from (CFG_INFO, docs/link.md) */
enum ctl_source {
    CTL_SOURCE_FACTORY = 0,  /* built in, as at start */
    CTL_SOURCE_TEST = 1,     /* the built-in test configuration (a setup) */
    CTL_SOURCE_RECEIVED = 2, /* received over the configuration protocol */
    CTL_SOURCE_STORED = 3,   /* loaded from flash at start (store/cfgstore.h) */
};
#define CTL_SOURCE_SETTINGS 0x80u /* flag: a setup changed some of the built-in one's settings */

/* The largest live edit (ctl_request_edit()): a CFG_SET's records */
#define CTL_EDIT_MAX 192u

struct ctl_ops {
    void *ctx;
    void (*led)(void *ctx, unsigned led, bool on); /* light or darken a LED (core.h indexes) */
    void (*midi_ready)(void *ctx);                 /* packets wait in the queue: wake its consumer */
    void (*ble_ready)(void *ctx);                  /* messages wait in the BLE queue (or NULL) */
    void (*bluetooth)(void *ctx, bool on);         /* the BT button turned Bluetooth on/off (or NULL) */
    /* the LED light changed (SPEC §5.7): ledLevel 1-10, ledUnlit 0-10; at the start and after every
     * change of the running configuration that changes it (or NULL) */
    void (*light)(void *ctx, unsigned level, unsigned unlit);
};

struct ctl_stats {
    uint32_t rounds;       /* ctl_run() calls */
    uint32_t events;       /* events from the queue */
    uint32_t time_raised;  /* event times raised to keep the clock from going back */
    uint32_t midi_refused; /* messages the full queue refused (whole units; none expected) */
    uint32_t midi_held;    /* rounds that stopped for the MIDI queue (backpressure) */
    uint32_t ble_refused;  /* messages the full BLE queue refused (whole units) */
    uint32_t ble_held;     /* rounds that stopped for the BLE queue */
    uint32_t midi_bad;     /* messages that are not channel messages (a core bug) */
    uint32_t setups;       /* core restarts on a setup request */
    uint32_t loads;        /* core restarts on a received configuration */
    uint32_t edits;        /* live edits applied without a restart */
    uint32_t selects;      /* selections asked for by a client (CFG_SELECT) applied */
};

/* Where a round's time goes (docs/core.md, "Fast sweeps"), on the log clock; a window since the
 * last ctl_log_timing(). Every time includes what preempted the control task meanwhile. */
struct ctl_timing {
    uint32_t events, ev_sum_us, ev_max_us;           /* core_event() per queued event */
    uint32_t faders, fader_sum_us, fader_max_us;     /* the fader events among them */
    uint32_t io_sum_us;                              /* in the MIDI queue and the LED calls */
    uint32_t busy_rounds, round_events_max, round_max_us; /* rounds that had events */
    uint32_t idle_sum_us, idle_max_us, idle_rounds;  /* the tick and the blinking, each round */
    uint32_t held_rounds;                            /* rounds that stopped for the MIDI queue */
    uint32_t alone_n, alone_sum_us, alone_max_us;    /* CTL_PROBE: core_event() with interrupts off */
};

struct ctl {
    struct core core;
    struct core_config *cfg; /* the configuration the core runs on; a setup rewrites it */
    const struct ctl_ops *ops;
    struct evq *in;
    struct midiq *out;
    struct bleq *ble; /* or NULL: no BLE */
    uint32_t last_us; /* the last time the core got */
    bool have_time;
    bool midi_new;  /* packets queued in this round */
    bool ble_new;   /* messages queued for BLE in this round */
    bool refusing;  /* the queue refused the last unit (logged once per run of refusals) */
    bool ble_refusing;
    uint8_t unit[CTL_UNIT_MAX][4]; /* the packets of the event the core is taking */
    uint8_t unit_msg[CTL_UNIT_MAX][3]; /* the same messages for BLE */
    uint8_t unit_mark[CTL_UNIT_MAX];   /* their merge marks (core.h, out_merge) */
    unsigned unit_n;
    uint8_t led_state[CORE_LEDS]; /* enum core_led_state, as the core set it */
    uint8_t led_shown[CORE_LEDS]; /* 0 dark, 1 lit, 0xFF not drawn yet */
    uint8_t light_shown[2];        /* the LED light last given to ops->light: level, unlit (0xFF none) */
    uint32_t led_since[CORE_LEDS]; /* a blink starts lit at this time */
    struct core_settings setup;    /* the setup asked for, valid while setup_pending and !load */
    const uint8_t *load;           /* a received configuration asked for (checked), or NULL */
    uint32_t load_len;
    uint8_t load_source;           /* what it becomes: CTL_SOURCE_RECEIVED, or _STORED (a revert) */
    bool load_factory;             /* a revert with nothing saved: the factory configuration */
    uint8_t edit[CTL_EDIT_MAX];    /* a live edit asked for (checked): its records */
    uint32_t edit_len;             /* 0: none */
    uint8_t edit_preset;
    bool cleanup_more;             /* this round exhausted its cleanup output budget */
    volatile bool setup_pending;   /* a setup or a received configuration waits for the next round */
    struct cfgbin_meta meta;       /* the running configuration's ID (zeros if none) and revision */
    uint8_t source;                /* enum ctl_source, with CTL_SOURCE_SETTINGS */
    /* the event time (log clock) of the last control event (not MIDI in), 0: none yet; read by
     * other tasks (the configuration's autosave waits for a quiet panel) */
    volatile uint32_t last_touch_us;
    /* the panel's state to keep across a power-off (ctl_state()), after every round; read by other
     * tasks (the autosave) */
    volatile uint32_t state_word;
    /* the panel as a client follows it (ctl_panel(), CTL_PANEL_*), after every round; read by
     * other tasks (the configuration protocol's state and events, P-2) */
    volatile uint32_t panel_word;
    uint32_t touch_word; /* the touch part of panel_word, as the last event left it */
    /* a selection asked for (ctl_request_select()): CTL_SELECT_* fields, written whole by the
     * requesting task; applied when its count differs from select_done */
    volatile uint32_t select_req;
    uint8_t select_done;
    /* what the core sent (ctl_sent_read()): entry n at n % CTL_SENT_RING, written before sent_w
     * counts it; read by other tasks */
    volatile uint8_t sent[CTL_SENT_RING][CTL_SENT_LEN];
    volatile uint32_t sent_w; /* the next entry's number */
    uint8_t src[4];           /* the source of the event the core is taking: preset, page, control, layer */
    struct ctl_stats stats;
    struct ctl_timing timing;
    /* metrics: the window since ctl_take_window() */
    bool cur_timed;          /* the core is taking an input event: its MIDI carries cur_t_us */
    uint32_t cur_t_us;
    struct lat_hist lat_core; /* event stamp → taken by the core */
    uint32_t busy_us;        /* ctl_run()'s run time */
    struct {
        uint32_t events, midi_refused, evq_refused, ble_refused, ble_merged;
    } win_from; /* the counters at the window's start */
};

/* Starts the core on cfg (which must stay valid; a setup rewrites it); the core reports every LED
 * once, so every LED is drawn. */
void ctl_init(struct ctl *c, struct core_config *cfg, struct evq *in, struct midiq *out,
              const struct ctl_ops *ops);

/* After ctl_init(), before the first round: the core's output goes to q too (BLE), while
 * Bluetooth is on; it is on from here (the BT button's long press turns it off). */
void ctl_set_ble(struct ctl *c, struct bleq *q);

/* After ctl_init(), before the first round: the configuration given to ctl_init() was loaded from
 * flash, with this ID and revision (source CTL_SOURCE_STORED). */
void ctl_set_stored(struct ctl *c, const struct cfgbin_meta *meta);

/* The panel's state kept across a power-off (the owner, 2026-11-12; store/statestore.h): one word,
 * bits 0-1 Bluetooth (CTL_BT_*), 2-3 the preset, 4-5 the page (0-based), 8-15 each preset's last
 * page (2 bits each, preset 1 lowest). */
#define CTL_BT_OFF 0u
#define CTL_BT_ON 1u
#define CTL_BT_NONE 2u /* no Bluetooth in this build: nothing to restore */
#define CTL_STATE_AT_START 0x1u /* what a start with nothing saved runs: Bluetooth on, preset 1 page 1 */

/* Any task: the state as the last round left it. */
static inline uint32_t ctl_state(const struct ctl *c) { return c->state_word; }

/* The panel as a client follows it (docs/link.md, "The panel's state"; P-2): one word, so another
 * task reads it whole. The preset and the page (0-based), Shift, Bluetooth (CTL_BT_*) and whether
 * a central is connected; the touches counted (mod 256) and where the last one was: its preset,
 * its layer (1 Shift), the binary form's page (CTL_PANEL_ROW: the bottom row) and control ID. */
#define CTL_PANEL_PRESET(w) ((w) & 3u)
#define CTL_PANEL_PAGE(w) ((w) >> 2 & 3u)
#define CTL_PANEL_SHIFT(w) ((w) >> 4 & 1u)
#define CTL_PANEL_BT(w) ((w) >> 5 & 3u)
#define CTL_PANEL_BT_LINK(w) ((w) >> 7 & 1u)
#define CTL_PANEL_TOUCHES(w) ((w) >> 8 & 0xFFu)
#define CTL_PANEL_TOUCHED(w) ((w) >> 28 & 1u) /* 0: no touch yet */
#define CTL_PANEL_T_PRESET(w) ((w) >> 16 & 3u)
#define CTL_PANEL_T_LAYER(w) ((w) >> 18 & 1u)
#define CTL_PANEL_T_ROW(w) ((w) >> 19 & 1u)
#define CTL_PANEL_T_PAGE(w) ((w) >> 20 & 3u)
#define CTL_PANEL_T_CONTROL(w) ((w) >> 22 & 0x3Fu)

/* Any task: the panel as the last round left it. */
static inline uint32_t ctl_panel(const struct ctl *c) { return c->panel_word; }

/* The panel word as the first CTL_PANEL_BYTES bytes of the configuration protocol's state
 * (docs/link.md, "The panel's state"): preset, page, Shift, Bluetooth (bit 7: a central), the
 * touches, then the last touch's preset, page (255: the row), control ID and layer (255 each
 * before the first touch). */
#define CTL_PANEL_BYTES 9u
void ctl_panel_bytes(uint32_t w, uint8_t out[CTL_PANEL_BYTES]);

/* Any task: the core is to select preset and page (0-based; -1 keeps one, a preset alone goes to
 * its last page; core_select()) at the start of the next round, as the panel's buttons do (CFG_SELECT,
 * P-3). A newer request replaces one not applied yet. False if either is out of range. One
 * requesting task at a time. */
bool ctl_request_select(struct ctl *c, int preset, int page);

/* Any task: what the core sent (P-5), from entry number from on: at most max entries of
 * CTL_SENT_LEN bytes into out, the first one's number in *first (later than from when the ring no
 * longer holds those: first - from were lost). from = CTL_SENT_NOW gives none, *first the next
 * number. One writer (the control task); readers need no lock. */
#define CTL_SENT_NOW 0xFFFFFFFFu
unsigned ctl_sent_read(const struct ctl *c, uint32_t from, uint8_t (*out)[CTL_SENT_LEN], unsigned max,
                       uint32_t *first);

/* After ctl_init() and ctl_set_ble(), before the first round: Bluetooth (if this build has it),
 * each preset's last page, then the preset and the page, as word (ctl_state()) says. */
void ctl_restore_state(struct ctl *c, uint32_t word);

/* Any task: the core is to restart on s (copied) in the next round: its configuration written
 * into cfg, the targets back to their defaults, LEDs redrawn where they change. False while the
 * previous request has not been applied. One requesting task at a time. */
bool ctl_request_setup(struct ctl *c, const struct core_settings *s);

/* Any task: the core is to restart on the binary form blob[0..len), which cfgbin_decode() has
 * checked, in the next round: decoded into cfg, its ID kept, the revision advanced, the targets back
 * to their defaults, LEDs redrawn where they change. blob must stay unchanged until ctl_change_pending() is
 * false. False while a setup or another configuration waits. One requesting task at a time, the
 * same one as ctl_request_setup()'s. */
bool ctl_request_config(struct ctl *c, const uint8_t *blob, uint32_t len);

/* Any task: what the mixer starts with replaces the running configuration in the next round (CFG_REVERT,
 * Q-26), as ctl_request_config() does: blob (checked; it must stay unchanged until
 * ctl_change_pending() is false) is the saved configuration with its ID, source CTL_SOURCE_STORED;
 * blob NULL the factory configuration. Both advance the running revision. False while a change waits. */
bool ctl_request_revert(struct ctl *c, const uint8_t *blob, uint32_t len);

/* Any task: a live edit (CFG_SET, P-1; docs/core.md, "Live edits"): records of the binary form for
 * preset `preset` (cfgbin_patch(), checked already; copied, at most CTL_EDIT_MAX bytes) applied to
 * the configuration in the next round, without a restart: the notes of the controls they change
 * end first, then core_reconfigured() keeps the core's state. The running revision advances (the
 * legacy wire argument is ignored); the ID stays, and it counts as received unless a setup's. False
 * while a change waits or if len is too long. */
bool ctl_request_edit(struct ctl *c, unsigned preset, const uint8_t *rec, uint32_t len, uint32_t revision);

/* Whether a setup, received configuration or edit is waiting, including bounded note cleanup. */
bool ctl_change_pending(const struct ctl *c);

/* The requesting task: the running configuration's binary form (canonical, with its ID and
 * revision) into out, at most cap bytes. Returns its length; 0 while a change waits (the control
 * task may be rewriting the configuration) or if out is too small (err says which, when not NULL;
 * code CFGBIN_OK for the first). */
size_t ctl_encode_config(const struct ctl *c, uint8_t *out, size_t cap, struct cfgbin_error *err);

/* One round: cleanup and a requested configuration change (possibly over several rounds), then queued events into the
 * core, oldest first, and a tick at now_us, each while at most CTL_MIDI_LOW packets wait (the rest
 * waits for the next round); the blinking LEDs; then midi_ready if packets were queued. now_us: the log clock now
 * (smclog_port_time_us(), which stamps the events); the round's run time is measured from it. */
void ctl_run(struct ctl *c, uint32_t now_us);

/* The statistics to the log (LOG-06): the round's counters, the core's and the MIDI queue's. */
void ctl_log_stats(const struct ctl *c);

/* The control task, between rounds: the timing window (struct ctl_timing) to the log, then a new
 * one. */
void ctl_log_timing(struct ctl *c);

/* The control task, between rounds: the window's metrics into w (lat_core, busy_ctl_us, the
 * traffic part: events, the queues' maxima and refusals; with BLE, the BLE queue's maximum, merges
 * and refusals), then a new window. */
void ctl_take_window(struct ctl *c, struct metrics_window *w);

#endif /* CTL_H */
