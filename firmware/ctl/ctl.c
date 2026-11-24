/* The control path on the device (ctl.h). */
#include "ctl.h"

#include <string.h>

#include "smclog.h"
#include "smc_ram.h"
#include "smclog_port.h"
#include "usbmidi.h"

_Static_assert(CTL_UNIT_MAX + CTL_MIDI_LOW <= MIDIQ_SIZE - 1u,
               "the MIDI queue holds one event's unit above the low mark");
_Static_assert(CTL_UNIT_MAX <= BLEQ_SIZE - 1u, "the BLE queue holds a complete output unit");

#define LED_UNKNOWN 0xFFu
#define BARRIER() __asm__ __volatile__("" ::: "memory") /* one core: order for the compiler */

SMC_HOT static void show(struct ctl *c, unsigned led, bool on)
{
    if (c->led_shown[led] == (uint8_t)on)
        return;
    c->led_shown[led] = (uint8_t)on;
    if (c->ops->led) {
        uint32_t t0 = smclog_port_time_us();
        c->ops->led(c->ops->ctx, led, on);
        c->timing.io_sum_us += smclog_port_time_us() - t0;
    }
}

/* core_io: a LED changed state. A blink starts lit. */
SMC_HOT static void on_led(void *ctx, unsigned led, unsigned state)
{
    struct ctl *c = ctx;
    if (led >= CORE_LEDS)
        return;
    c->led_state[led] = (uint8_t)state;
    c->led_since[led] = c->core.now_us;
    show(c, led, state != CORE_LED_OFF);
}

/* core_io: one MIDI message for the host, on the performance port: into the event's unit. */
SMC_HOT static void on_midi(void *ctx, const uint8_t *msg, unsigned len, uint16_t cause)
{
    struct ctl *c = ctx;
    (void)cause; /* the core logged the message in the event's cause chain */

    if (c->unit_n >= CTL_UNIT_MAX) { /* more than CTL_UNIT_MAX says one event makes: a bug */
        c->stats.midi_refused++;
        LOG_ERROR(midi_usb, "more than %u messages from one event: %02x refused", CTL_UNIT_MAX, msg[0]);
        return;
    }
    if (!usbmidi_pack(USBMIDI_CABLE_PERFORMANCE, msg, len, c->unit[c->unit_n])) {
        c->stats.midi_bad++;
        LOG_ERROR(midi_usb, "not a channel message: %02x, %u bytes", msg[0], len);
        return;
    }
    uint8_t *m = c->unit_msg[c->unit_n];
    m[0] = msg[0];
    m[1] = len > 1 ? msg[1] : 0;
    m[2] = len > 2 ? msg[2] : 0;
    c->unit_mark[c->unit_n] = c->core.out_merge;
    c->unit_n++;
}

/* The unit into the BLE queue, all or none. */
SMC_HOT static void flush_ble(struct ctl *c, unsigned n)
{
    if (!bleq_push_unit(c->ble, (const uint8_t(*)[3])c->unit_msg, c->unit_mark, n, c->cur_t_us,
                        c->cur_timed)) {
        c->stats.ble_refused += n;
        if (!c->ble_refusing)
            LOG_WARN(midi_ble, "BLE queue full: %u messages of one event refused and counted", n);
        c->ble_refusing = true;
        return;
    }
    if (c->ble_refusing)
        LOG_INFO(midi_ble, "BLE queue takes messages again; %u refused so far", c->stats.ble_refused);
    c->ble_refusing = false;
    c->ble_new = true;
}

/* The messages of the event the core just took into the USB queue and, while Bluetooth is on
 * (core.h, bt_on; FR-IO-04), the BLE queue: all or none in each. */
SMC_HOT static void flush_unit(struct ctl *c)
{
    unsigned n = c->unit_n;
    if (!n)
        return;
    c->unit_n = 0;
    uint32_t t0 = smclog_port_time_us();
    if (c->ble && c->core.bt_on)
        flush_ble(c, n);
    bool queued = midiq_push_unit(c->out, (const uint8_t(*)[4])c->unit, n, c->cur_t_us, c->cur_timed);
    c->timing.io_sum_us += smclog_port_time_us() - t0;
    if (!queued) {
        c->stats.midi_refused += n;
        if (!c->refusing)
            LOG_WARN(midi_usb, "MIDI out queue full: %u messages of one event refused and counted", n);
        c->refusing = true;
        return;
    }
    if (c->refusing)
        LOG_INFO(midi_usb, "MIDI out queue takes messages again; %u refused so far",
                 c->stats.midi_refused);
    c->refusing = false;
    c->midi_new = true;
    /* what was sent, with its source (P-5): each entry written before the count takes it in */
    uint32_t w = c->sent_w;
    for (unsigned i = 0; i < n; i++, w++) {
        volatile uint8_t *e = c->sent[w % CTL_SENT_RING];
        for (unsigned k = 0; k < 3; k++)
            e[k] = c->unit_msg[i][k];
        for (unsigned k = 0; k < 4; k++)
            e[3 + k] = c->src[k];
        c->sent_w = w + 1u;
    }
}

_Static_assert((CTL_SENT_RING & (CTL_SENT_RING - 1u)) == 0, "the ring wraps with the u32 count");

unsigned ctl_sent_read(const struct ctl *c, uint32_t from, uint8_t (*out)[CTL_SENT_LEN], unsigned max,
                       uint32_t *first)
{
    uint32_t w = c->sent_w;
    if (from == CTL_SENT_NOW || (int32_t)(w - from) < 0) { /* now, or a number not reached yet */
        *first = w;
        return 0;
    }
    if (w - from > CTL_SENT_RING - 1u) /* the oldest slot may be the one being written */
        from = w - (CTL_SENT_RING - 1u);
    unsigned n = w - from < max ? (unsigned)(w - from) : max;
    for (unsigned i = 0; i < n; i++)
        for (unsigned k = 0; k < CTL_SENT_LEN; k++)
            out[i][k] = c->sent[(from + i) % CTL_SENT_RING][k];
    /* The writer may have reused slots meanwhile: entry s is whole only while the writer has not
     * begun entry s + CTL_SENT_RING, i.e. the count is still below it. */
    uint32_t w2 = c->sent_w;
    unsigned skip = 0;
    while (skip < n && w2 - (from + skip) >= CTL_SENT_RING)
        skip++;
    if (skip) {
        for (unsigned i = skip; i < n; i++)
            for (unsigned k = 0; k < CTL_SENT_LEN; k++)
                out[i - skip][k] = out[i][k];
        n -= skip;
        from += skip;
    }
    *first = from;
    return n;
}

/* Backpressure: true while at most CTL_MIDI_LOW packets wait, so the next event's unit fits and the
 * MIDI path stays short; else the consumer is asked to wake the control task once they have gone. */
SMC_HOT static bool midi_room(struct ctl *c)
{
    if (midiq_depth(c->out) > CTL_MIDI_LOW) {
        midiq_want_room(c->out, CTL_MIDI_LOW);
        if (midiq_depth(c->out) > CTL_MIDI_LOW) /* taken meanwhile: a spare wake-up later */
            return false;
    }
    /* timers() can turn Bluetooth on during the next invocation. */
    if (c->ble && (c->core.bt_on || c->core.button[CORE_BTN_BT].down) &&
        bleq_room(c->ble) < CTL_UNIT_MAX) {
        bleq_want_room(c->ble, BLEQ_SIZE - 1u - CTL_UNIT_MAX);
        if (bleq_room(c->ble) < CTL_UNIT_MAX) {
            c->stats.ble_held++;
            return false;
        }
    }
    return true;
}

static void on_bluetooth(void *ctx, unsigned on)
{
    struct ctl *c = ctx;
    if (c->ops->bluetooth)
        c->ops->bluetooth(c->ops->ctx, on != 0);
}

static const struct core_io io = {on_midi, on_led, on_bluetooth};

/* The time to give the core: never earlier than the last one. */
SMC_HOT static uint32_t core_time(struct ctl *c, uint32_t t)
{
    if (c->have_time && (int32_t)(t - c->last_us) < 0) {
        if (c->stats.time_raised++ == 0)
            LOG_DEBUG(engine, "event time %u before %u: raised (counting the rest)", t, c->last_us);
        return c->last_us;
    }
    c->have_time = true;
    c->last_us = t;
    return t;
}

SMC_HOT static uint32_t pack_state(const struct core *k)
{
    uint32_t w = k->bt_none ? CTL_BT_NONE : k->bt_on ? CTL_BT_ON : CTL_BT_OFF;
    w |= (uint32_t)(k->preset & 3u) << 2 | (uint32_t)(k->page & 3u) << 4;
    for (unsigned p = 0; p < CORE_PRESETS; p++)
        w |= (uint32_t)(k->page_of[p] & 3u) << (8 + 2 * p);
    return w;
}

_Static_assert(CORE_PRESETS == 4 && CORE_PAGES == 4, "the state word packs 2 bits per preset and page");

/* ctl_panel(): the core's part, then the last touch's (touch_word). */
SMC_HOT static uint32_t pack_panel(const struct ctl *c)
{
    const struct core *k = &c->core;
    uint32_t bt = k->bt_none ? CTL_BT_NONE : k->bt_on ? CTL_BT_ON : CTL_BT_OFF;
    return (k->preset & 3u) | (uint32_t)(k->page & 3u) << 2 | (uint32_t)(k->shift ? 1u : 0u) << 4 |
           bt << 5 | (uint32_t)(k->bt_link ? 1u : 0u) << 7 | c->touch_word;
}

/* A touch of a configurable control (docs/link.md, "The panel's state"): a strip or row button's
 * press, an encoder's turn, a fader's move; counted, and its place kept by the binary form's
 * address, before the core takes it (the preset, page and Shift it was used in). */
SMC_HOT static void note_touch(struct ctl *c, const struct core_event *ev)
{
    unsigned row = 0, page = c->core.page, id;
    if (ev->type == CORE_EV_FADER && ev->index < CORE_STRIPS) {
        id = ev->index;
    } else if (ev->type == CORE_EV_ENCODER && ev->index < CORE_STRIPS) {
        id = 8u + ev->index;
    } else if (ev->type == CORE_EV_BUTTON && ev->value && ev->index < 4u * CORE_STRIPS) {
        id = (2u + ev->index % 4u) * 8u + ev->index / 4u;
    } else if (ev->type == CORE_EV_BUTTON && ev->value && ev->index >= CORE_BTN_PLAY &&
               ev->index < CORE_BTN_PLAY + CORE_ROW_BUTTONS) {
        id = ev->index - CORE_BTN_PLAY;
        row = 1;
        page = 0;
    } else {
        return;
    }
    uint32_t n = (c->touch_word >> 8) + 1u;
    c->touch_word = (n & 0xFFu) << 8 | (uint32_t)(c->core.preset & 3u) << 16 |
                    (uint32_t)(c->core.shift ? 1u : 0u) << 18 | (uint32_t)row << 19 |
                    (uint32_t)(page & 3u) << 20 | (uint32_t)(id & 0x3Fu) << 22 | 1u << 28;
}

/* The source of what the core sends while it takes ev (P-5): the control that ev addresses, as the
 * binary form names it, a press or a release alike; none (255) for anything else. */
SMC_HOT static void set_source(struct ctl *c, const struct core_event *ev)
{
    unsigned page = c->core.page, id;
    if (ev->type == CORE_EV_FADER && ev->index < CORE_STRIPS) {
        id = ev->index;
    } else if (ev->type == CORE_EV_ENCODER && ev->index < CORE_STRIPS) {
        id = 8u + ev->index;
    } else if (ev->type == CORE_EV_BUTTON && ev->index < 4u * CORE_STRIPS) {
        id = (2u + ev->index % 4u) * 8u + ev->index / 4u;
    } else if (ev->type == CORE_EV_BUTTON && ev->index >= CORE_BTN_PLAY &&
               ev->index < CORE_BTN_PLAY + CORE_ROW_BUTTONS) {
        id = ev->index - CORE_BTN_PLAY;
        page = 255u; /* the binary form's CFG_ROW_PAGE */
    } else {
        memset(c->src, 0xFF, sizeof c->src);
        return;
    }
    c->src[0] = (uint8_t)c->core.preset;
    c->src[1] = (uint8_t)page;
    c->src[2] = (uint8_t)id;
    c->src[3] = c->core.shift ? 1u : 0u;
}

void ctl_panel_bytes(uint32_t w, uint8_t out[CTL_PANEL_BYTES])
{
    bool t = CTL_PANEL_TOUCHED(w);
    out[0] = (uint8_t)CTL_PANEL_PRESET(w);
    out[1] = (uint8_t)CTL_PANEL_PAGE(w);
    out[2] = (uint8_t)CTL_PANEL_SHIFT(w);
    out[3] = (uint8_t)(CTL_PANEL_BT(w) | CTL_PANEL_BT_LINK(w) << 7);
    out[4] = (uint8_t)CTL_PANEL_TOUCHES(w);
    out[5] = t ? (uint8_t)CTL_PANEL_T_PRESET(w) : 0xFFu;
    out[6] = !t ? 0xFFu : CTL_PANEL_T_ROW(w) ? 255u /* the binary form's CFG_ROW_PAGE */ : (uint8_t)CTL_PANEL_T_PAGE(w);
    out[7] = t ? (uint8_t)CTL_PANEL_T_CONTROL(w) : 0xFFu;
    out[8] = t ? (uint8_t)CTL_PANEL_T_LAYER(w) : 0xFFu;
}

bool ctl_request_select(struct ctl *c, int preset, int page)
{
    if (preset < -1 || preset >= CORE_PRESETS || page < -1 || page >= CORE_PAGES)
        return false;
    uint32_t count = ((c->select_req >> 16) + 1u) & 0xFFu;
    c->select_req = (uint32_t)(preset & 0xFF) | (uint32_t)(page & 0xFF) << 8 | count << 16;
    return true;
}

/* A selection a client asked for, if a new one waits. */
SMC_HOT static void apply_select(struct ctl *c)
{
    uint32_t r = c->select_req;
    if ((uint8_t)(r >> 16) == c->select_done)
        return;
    c->select_done = (uint8_t)(r >> 16);
    int preset = (int)(r & 0xFFu), page = (int)(r >> 8 & 0xFFu);
    c->stats.selects++;
    LOG_INFO(engine, "select asked: preset %d page %d (0: keep)", preset == 0xFF ? 0 : preset + 1,
             page == 0xFF ? 0 : page + 1);
    core_select(&c->core, preset == 0xFF ? -1 : preset, page == 0xFF ? -1 : page);
    flush_unit(c);
}

void ctl_init(struct ctl *c, struct core_config *cfg, struct evq *in, struct midiq *out,
              const struct ctl_ops *ops)
{
    memset(c, 0, sizeof *c);
    c->cfg = cfg;
    c->ops = ops;
    c->in = in;
    c->out = out;
    memset(c->led_shown, LED_UNKNOWN, sizeof c->led_shown);
    memset(c->src, 0xFF, sizeof c->src);
    core_init(&c->core, cfg, &io, c); /* no Bluetooth until ctl_set_ble() */
    flush_unit(c);
    c->state_word = pack_state(&c->core);
    c->panel_word = pack_panel(c);
}

void ctl_restore_state(struct ctl *c, uint32_t word)
{
    unsigned bt = word & 3u;
    if (c->ble && bt <= CTL_BT_ON)
        core_set_bluetooth(&c->core, (int)bt, false);
    for (unsigned p = 0; p < CORE_PRESETS; p++)
        core_select(&c->core, (int)p, (int)(word >> (8 + 2 * p) & 3u));
    core_select(&c->core, (int)(word >> 2 & 3u), (int)(word >> 4 & 3u));
    flush_unit(c);
    c->state_word = pack_state(&c->core);
    c->panel_word = pack_panel(c);
    LOG_INFO(engine, "state restored: bluetooth %s, preset %u page %u",
             c->core.bt_none ? "none" : c->core.bt_on ? "on" : "off", c->core.preset + 1u, c->core.page + 1u);
}

void ctl_set_ble(struct ctl *c, struct bleq *q)
{
    c->ble = q;
    core_set_bluetooth(&c->core, q ? 1 : -1, false); /* on unless ctl_restore_state() says off */
    c->state_word = pack_state(&c->core);
    c->panel_word = pack_panel(c);
}

void ctl_set_stored(struct ctl *c, const struct cfgbin_meta *meta)
{
    c->meta = *meta;
    c->source = CTL_SOURCE_STORED;
}

SMC_HOT bool ctl_request_setup(struct ctl *c, const struct core_settings *s)
{
    if (c->setup_pending)
        return false;
    c->setup = *s;
    c->load = NULL;
    c->load_factory = false;
    c->edit_len = 0;
    BARRIER();
    c->setup_pending = true;
    return true;
}

static bool request_load(struct ctl *c, const uint8_t *blob, uint32_t len, uint8_t source)
{
    if (c->setup_pending)
        return false;
    c->load = blob;
    c->load_len = len;
    c->load_source = source;
    c->load_factory = blob == NULL;
    c->edit_len = 0;
    BARRIER();
    c->setup_pending = true;
    return true;
}

bool ctl_request_config(struct ctl *c, const uint8_t *blob, uint32_t len)
{
    return blob && request_load(c, blob, len, CTL_SOURCE_RECEIVED);
}

bool ctl_request_revert(struct ctl *c, const uint8_t *blob, uint32_t len)
{
    return request_load(c, blob, blob ? len : 0, blob ? CTL_SOURCE_STORED : CTL_SOURCE_FACTORY);
}

bool ctl_request_edit(struct ctl *c, unsigned preset, const uint8_t *rec, uint32_t len, uint32_t revision)
{
    (void)revision; /* legacy CFG_SET field; the controller assigns the running revision */
    if (c->setup_pending || len == 0 || len > CTL_EDIT_MAX || preset >= CORE_PRESETS)
        return false;
    memcpy(c->edit, rec, len);
    c->edit_len = len;
    c->edit_preset = (uint8_t)preset;
    c->load = NULL;
    c->load_factory = false;
    BARRIER();
    c->setup_pending = true;
    return true;
}

bool ctl_change_pending(const struct ctl *c)
{
    return c->setup_pending;
}

size_t ctl_encode_config(const struct ctl *c, uint8_t *out, size_t cap, struct cfgbin_error *err)
{
    if (c->setup_pending) {
        if (err) {
            memset(err, 0xFF, sizeof *err);
            err->code = CFGBIN_OK;
        }
        return 0;
    }
    BARRIER();
    return cfgbin_encode(c->cfg, &c->meta, out, cap, err);
}

/* The core restarts on the configuration, keeping Bluetooth (not the configuration's). */
static void restart(struct ctl *c)
{
    int bt_on = c->core.bt_none ? -1 : c->core.bt_on;
    bool bt_link = c->core.bt_link;
    core_init(&c->core, c->cfg, &io, c); /* every LED reported: drawn where it changed */
    core_set_bluetooth(&c->core, bt_on, bt_link);
    flush_unit(c);
}

/* A received configuration, checked by the requesting task: decoded into the configuration. */
static void apply_config(struct ctl *c)
{
    struct cfgbin_error err;
    struct cfgbin_meta meta;
    c->meta.revision++; /* uint32_t wraps to zero */
    c->stats.loads++;
    if (c->load_factory) {
        LOG_INFO(engine, "configuration reverted: nothing saved, the factory configuration");
        core_config_factory(c->cfg);
        memset(c->meta.id, 0, sizeof c->meta.id);
        c->source = CTL_SOURCE_FACTORY;
        c->load_factory = false;
        return;
    }
    int r = cfgbin_decode(c->load, c->load_len, c->cfg, &meta, &err);

    if (r != CFGBIN_OK) { /* it was checked: a bug. The configuration is undefined now. */
        LOG_ERROR(engine, "received configuration refused when applied: %s at byte %u; the factory "
                  "configuration instead", cfgbin_error_name((unsigned)r), err.offset);
        core_config_factory(c->cfg);
        memset(c->meta.id, 0, sizeof c->meta.id);
        c->source = CTL_SOURCE_FACTORY;
    } else {
        meta.revision = c->meta.revision;
        if (c->load_source == CTL_SOURCE_STORED)
            LOG_INFO(engine, "configuration reverted to the saved one: %u bytes, revision %u", c->load_len,
                     meta.revision);
        else
            LOG_INFO(engine, "configuration received: %u bytes, revision %u", c->load_len, meta.revision);
        c->meta = meta;
        c->source = c->load_source;
    }
    c->load = NULL;
}

/* cfgbin_patch()'s changing: a control's notes end before it changes; a preset's settings (its
 * channel, its Note Off form) end every note. */
static void edit_changing(void *ctx, const struct core_control *old)
{
    struct ctl *c = ctx;
    if (c->cleanup_more)
        return;
    if (old) {
        /* Two note targets at most. Revisit the unchanged controls next round when fewer
         * than two slots remain; already released notes consume no output. */
        if (CTL_UNIT_MAX - c->unit_n < 2u) {
            c->cleanup_more = true;
            return;
        }
        core_release_control(&c->core, c->edit_preset, old);
    } else {
        unsigned room = CTL_UNIT_MAX - c->unit_n;
        c->cleanup_more = core_notes_off_limit(&c->core, room) == room;
    }
}

/* A live edit, checked by the requesting task: applied in place, the core keeps its state. */
static void apply_edit(struct ctl *c)
{
    struct cfgbin_error err;
    int r = cfgbin_patch(c->edit, c->edit_len, c->edit_preset, c->cfg, NULL, NULL, &err);
    c->meta.revision++;
    c->stats.edits++;
    if (r != CFGBIN_OK) { /* it was checked: a bug. The configuration is half changed now. */
        LOG_ERROR(engine, "edit refused when applied: %s at byte %u; the factory configuration instead",
                  cfgbin_error_name((unsigned)r), err.offset);
        core_config_factory(c->cfg);
        memset(c->meta.id, 0, sizeof c->meta.id);
        c->source = CTL_SOURCE_FACTORY;
        c->edit_len = 0;
        restart(c);
        return;
    }
    LOG_INFO(engine, "edit: preset %u, %u bytes, revision %u", c->edit_preset + 1u, c->edit_len, c->meta.revision);
    if ((c->source & ~CTL_SOURCE_SETTINGS) != CTL_SOURCE_TEST)
        c->source = CTL_SOURCE_RECEIVED;
    c->edit_len = 0;
    core_reconfigured(&c->core);
    flush_unit(c);
}

SMC_HOT static bool apply_setup(struct ctl *c)
{
    BARRIER();
    if (!midi_room(c))
        return false;
    c->cleanup_more = false;
    if (c->edit_len) {
        struct cfgbin_error err;
        /* Visit before mutation, retaining the old channels and assignments across rounds. */
        if (cfgbin_patch_visit(c->edit, c->edit_len, c->edit_preset, c->cfg, edit_changing, c, &err))
            /* Invalid internal request: drain all notes before apply_edit's factory fallback. */
            c->cleanup_more = core_notes_off_limit(&c->core, CTL_UNIT_MAX) == CTL_UNIT_MAX;
    } else {
        c->cleanup_more = core_notes_off_limit(&c->core, CTL_UNIT_MAX) == CTL_UNIT_MAX;
    }
    flush_unit(c);
    if (c->cleanup_more)
        return false;
    if (c->edit_len) {
        apply_edit(c);
        BARRIER();
        c->setup_pending = false;
        return true;
    }
    if (c->load || c->load_factory) {
        apply_config(c);
    } else {
        c->stats.setups++;
        LOG_INFO(engine, "setup: %s configuration, %u settings changed",
                 c->setup.config == CORE_CONFIG_TEST ? "test" : "factory", c->setup.count);
        core_settings_apply(&c->setup, c->cfg);
        memset(c->meta.id, 0, sizeof c->meta.id);
        c->meta.revision++;
        c->source = (uint8_t)((c->setup.config == CORE_CONFIG_TEST ? CTL_SOURCE_TEST : CTL_SOURCE_FACTORY) |
                              (c->setup.count ? CTL_SOURCE_SETTINGS : 0u));
    }
    restart(c);
    BARRIER();
    c->setup_pending = false;
    return true;
}

SMC_HOT static void blink(struct ctl *c, uint32_t now)
{
    for (unsigned i = 0; i < CORE_LEDS; i++) {
        unsigned s = c->led_state[i];
        if (s != CORE_LED_SLOW && s != CORE_LED_FAST)
            continue;
        uint32_t half = s == CORE_LED_FAST ? CTL_BLINK_FAST_US
                        : i == CORE_BTN_BT ? CTL_BLINK_BT_US
                                           : CTL_BLINK_SLOW_US;
        show(c, i, (now - c->led_since[i]) / half % 2 == 0);
    }
}

SMC_HOT static void time_event(struct ctl_timing *m, uint8_t type, uint32_t us)
{
    m->events++;
    m->ev_sum_us += us;
    if (us > m->ev_max_us)
        m->ev_max_us = us;
    if (type == CORE_EV_FADER) {
        m->faders++;
        m->fader_sum_us += us;
        if (us > m->fader_max_us)
            m->fader_max_us = us;
    }
}

SMC_HOT void ctl_run(struct ctl *c, uint32_t now_us)
{
    struct core_event ev;
    struct ctl_timing *m = &c->timing;
    uint32_t n = 0;

    bool held = false;

    c->stats.rounds++;
    if (c->setup_pending && !apply_setup(c)) {
        held = true;
        goto round_end;
    }
    apply_select(c);
    for (;;) {
        if (!evq_depth(c->in))
            break;
        if (!midi_room(c)) {
            held = true;
            break;
        }
        if (!evq_pop(c->in, &ev))
            break;
        c->stats.events++;
        n++;
        uint32_t t0 = smclog_port_time_us();
        int32_t wait = (int32_t)(t0 - ev.t_us);
        lat_hist_add(&c->lat_core, wait > 0 ? (uint32_t)wait : 0u);
        c->cur_timed = true;
        c->cur_t_us = ev.t_us;
        if (ev.type != CORE_EV_MIDI_IN && ev.type != CORE_EV_TICK)
            c->last_touch_us = ev.t_us ? ev.t_us : 1u;
        note_touch(c, &ev);
        set_source(c, &ev);
        ev.t_us = core_time(c, ev.t_us);
#ifdef SMC_CTL_PROBE
        /* Every 32nd event with interrupts off: the core's own time, nothing preempting it (a
         * diagnostic build: it holds the scan off for that long). */
        if ((c->stats.events & 31u) == 0) {
            uint32_t l = smclog_port_lock();
            uint32_t a0 = smclog_port_time_us();
            core_event(&c->core, &ev);
            uint32_t a = smclog_port_time_us() - a0;
            smclog_port_unlock(l);
            m->alone_n++;
            m->alone_sum_us += a;
            if (a > m->alone_max_us)
                m->alone_max_us = a;
        } else
#endif
            core_event(&c->core, &ev);
        flush_unit(c);
        memset(c->src, 0xFF, sizeof c->src); /* the tick, a selection, a setup: no control */
        time_event(m, ev.type, smclog_port_time_us() - t0);
    }
round_end:
    c->cur_timed = false;
    uint32_t t1 = smclog_port_time_us();
    if (!held && midi_room(c)) { /* held: the tick waits for the next round, time catches up */
        memset(&ev, 0, sizeof ev);
        ev.type = CORE_EV_TICK;
        ev.t_us = core_time(c, now_us);
        core_event(&c->core, &ev);
        flush_unit(c);
    } else {
        held = true;
    }
    if (held) {
        c->stats.midi_held++;
        m->held_rounds++;
    }
    blink(c, c->last_us);
    uint32_t idle = smclog_port_time_us() - t1;
    m->idle_rounds++;
    m->idle_sum_us += idle;
    if (idle > m->idle_max_us)
        m->idle_max_us = idle;
    if (n) {
        uint32_t round = smclog_port_time_us() - now_us;
        m->busy_rounds++;
        if (n > m->round_events_max)
            m->round_events_max = n;
        if (round > m->round_max_us)
            m->round_max_us = round;
    }
    if (c->midi_new) {
        c->midi_new = false;
        if (c->ops->midi_ready)
            c->ops->midi_ready(c->ops->ctx);
    }
    if (c->ble_new) {
        c->ble_new = false;
        if (c->ops->ble_ready)
            c->ops->ble_ready(c->ops->ctx);
    }
    c->state_word = pack_state(&c->core);
    c->panel_word = pack_panel(c);
    int32_t took = (int32_t)(smclog_port_time_us() - now_us);
    if (took > 0)
        c->busy_us += (uint32_t)took;
}

SMC_HOT void ctl_take_window(struct ctl *c, struct metrics_window *w)
{
    w->lat_core = c->lat_core;
    lat_hist_clear(&c->lat_core);
    w->busy_ctl_us = c->busy_us;
    c->busy_us = 0;
    w->have_traffic = true;
    w->events = c->stats.events - c->win_from.events;
    w->midiq_refused = c->stats.midi_refused - c->win_from.midi_refused;
    uint32_t evq_refused = c->in->refused; /* written under the queue's lock: one word */
    w->evq_refused = evq_refused - c->win_from.evq_refused;
    w->evq_max = evq_take_window_max(c->in);
    w->midiq_max = midiq_take_window_max(c->out);
    c->win_from.events = c->stats.events;
    c->win_from.midi_refused = c->stats.midi_refused;
    c->win_from.evq_refused = evq_refused;
    if (c->ble) {
        w->have_ble = true;
        w->bleq_max = bleq_take_window_max(c->ble);
        w->bleq_refused = c->stats.ble_refused - c->win_from.ble_refused;
        w->ble_merged = c->ble->merged - c->win_from.ble_merged;
        c->win_from.ble_refused = c->stats.ble_refused;
        c->win_from.ble_merged = c->ble->merged;
    }
}

SMC_HOT static uint32_t avg(uint32_t sum, uint32_t n)
{
    return n ? sum / n : 0;
}

SMC_HOT void ctl_log_timing(struct ctl *c)
{
    const struct ctl_timing *m = &c->timing;
    LOG_INFO(engine, "control timing: %u events, core avg %u max %u us; %u faders, avg %u max %u "
             "us; MIDI queue and LEDs %u us in all", m->events, avg(m->ev_sum_us, m->events),
             m->ev_max_us, m->faders, avg(m->fader_sum_us, m->faders), m->fader_max_us,
             m->io_sum_us);
    LOG_INFO(engine, "control rounds: %u with events, at most %u events and %u us in one; tick and "
             "blink avg %u max %u us; %u held for the MIDI queue", m->busy_rounds,
             m->round_events_max, m->round_max_us, avg(m->idle_sum_us, m->idle_rounds),
             m->idle_max_us, m->held_rounds);
#ifdef SMC_CTL_PROBE
    LOG_INFO(engine, "control probe: %u events with interrupts off, core avg %u max %u us", m->alone_n,
             avg(m->alone_sum_us, m->alone_n), m->alone_max_us);
#endif
    memset(&c->timing, 0, sizeof c->timing);
}

SMC_HOT void ctl_log_stats(const struct ctl *c)
{
    const struct core_stats *k = &c->core.stats;
    LOG_INFO(engine, "control: %u rounds, %u events (%u injected, %u bad), %u times raised, %u "
             "setups; MIDI %u out, %u in", c->stats.rounds, c->stats.events, k->injected,
             k->bad_events, c->stats.time_raised, c->stats.setups, k->midi_out, k->midi_in);
    LOG_INFO(engine, "control: %u configurations received, source %u, revision %u", c->stats.loads,
             c->source, c->meta.revision);
    LOG_INFO(midi_usb, "MIDI out queue: %u queued, %u refused, %u not channel messages; most "
             "waiting %u; %u rounds held", c->out->pushed, c->stats.midi_refused, c->stats.midi_bad,
             c->out->depth_max, c->stats.midi_held);
    if (c->ble)
        LOG_INFO(midi_ble, "BLE queue: %u queued, %u refused, %u merged; most waiting %u; %u rounds "
                 "held; bluetooth %u, %u in ignored while off", c->ble->pushed, c->stats.ble_refused,
                 c->ble->merged, c->ble->depth_max, c->stats.ble_held, c->core.bt_on,
                 c->core.stats.midi_in_off);
}
