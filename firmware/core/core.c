/*
 * The control core: pages, presets, Shift, pickup, encoders, buttons, the target store.
 * Behaviour and its SPEC references: docs/core.md.
 */
#include "core.h"

#include <string.h>

#include "smclog.h"
#include "smc_ram.h"

#define FLASH_US 1000000u     /* a selected page/preset button blinks this long (FR-PG-03) */
#define BT_LONG_US 800000u    /* the BT button's long press: the stock firmware's 41 scans of 20 ms
                               * (docs/hardware.md, ⚠️ static analysis) */
#define STORE_LIMIT (CORE_TARGETS - CORE_TARGETS / 8) /* keep probes short */

#if (CORE_TARGETS & (CORE_TARGETS - 1)) != 0
#error "CORE_TARGETS must be a power of two"
#endif

/* --- assignments ----------------------------------------------------------------------------- */

SMC_HOT static unsigned type_max(unsigned msg)
{
    switch (msg) {
    case CORE_MSG_NONE:
        return 0;
    case CORE_MSG_CC14:
    case CORE_MSG_PB:
    case CORE_MSG_NRPN:
    case CORE_MSG_RPN:
        return 16383;
    default:
        return 127;
    }
}

SMC_HOT static bool is_relative(unsigned msg)
{
    return msg == CORE_MSG_REL_2C || msg == CORE_MSG_REL_BIN || msg == CORE_MSG_REL_SM;
}

SMC_HOT unsigned core_channel(const struct core_config *cfg, unsigned preset, const struct core_assign *a)
{
    return (a->channel == CORE_CHANNEL_PRESET ? cfg->preset[preset].channel : a->channel) & 15u;
}

SMC_HOT uint32_t core_target_key(const struct core_assign *a, unsigned channel)
{
    unsigned num;
    switch (a->msg) {
    case CORE_MSG_CC:
    case CORE_MSG_CC14:
    case CORE_MSG_NOTE:
        num = a->number & 0x7Fu;
        break;
    case CORE_MSG_NRPN:
    case CORE_MSG_RPN:
        num = a->number & 0x3FFFu;
        break;
    case CORE_MSG_PB:
    case CORE_MSG_PROGRAM:
    case CORE_MSG_ATOUCH:
        num = 0;
        break;
    default:
        return 0;
    }
    return 0x80000000u | (uint32_t)a->msg << 24 | (uint32_t)(channel & 15u) << 16 | num;
}

/* The target of an assignment of preset `preset`, its channel resolved. */
SMC_HOT static uint32_t key_of(const struct core *c, const struct core_assign *a, unsigned preset)
{
    return core_target_key(a, core_channel(c->cfg, preset, a));
}

/* The assignment's range clamped to its message type; min/max are the ends, lo/hi the
 * direction (lo > hi: inverted). */
struct range {
    int lo, hi, min, max;
};

SMC_HOT static struct range range_of(const struct core_assign *a)
{
    int tm = (int)type_max(a->msg);
    struct range r;
    r.lo = a->lo < tm ? a->lo : tm;
    r.hi = a->hi < tm ? a->hi : tm;
    r.min = r.lo < r.hi ? r.lo : r.hi;
    r.max = r.lo < r.hi ? r.hi : r.lo;
    return r;
}

SMC_HOT static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* The Normal or Shift layer of a control; NULL when that layer does nothing. */
SMC_HOT static const struct core_assign *layer_of(const struct core_control *ctl, unsigned layer)
{
    const struct core_assign *a = &ctl->normal;
    if (layer) {
        if (ctl->shift_kind == CORE_SHIFT_BLOCK)
            return NULL;
        if (ctl->shift_kind == CORE_SHIFT_OVERRIDE)
            a = &ctl->shift;
    }
    return a->msg == CORE_MSG_NONE ? NULL : a;
}

SMC_HOT static const struct core_strip *strip_of(const struct core *c, unsigned n)
{
    return &c->cfg->preset[c->preset].page[c->page][n];
}

SMC_HOT static const struct core_control *button_control(const struct core *c, unsigned b)
{
    if (b < CORE_STRIP_BUTTONS)
        return &strip_of(c, b / 4)->button[b % 4];
    if (b < CORE_STRIP_BUTTONS + CORE_ROW_BUTTONS)
        return &c->cfg->preset[c->preset].row[b - CORE_STRIP_BUTTONS];
    return NULL;
}

SMC_HOT static bool is_nav(unsigned b)
{
    return b >= CORE_BTN_REW && b <= CORE_BTN_RIGHT;
}

/* --- the target store (D-5) ------------------------------------------------------------------ */

SMC_HOT static uint32_t slot_of(uint32_t key)
{
    uint32_t h = key * 2654435761u;
    return (h ^ h >> 16) & (CORE_TARGETS - 1);
}

SMC_HOT static const struct core_target *store_lookup(const struct core *c, uint32_t key)
{
    uint32_t i = slot_of(key);
    for (unsigned n = 0; n < CORE_TARGETS; n++, i = (i + 1) & (CORE_TARGETS - 1)) {
        if (c->store[i].key == key)
            return &c->store[i];
        if (c->store[i].key == 0)
            return NULL;
    }
    return NULL;
}

SMC_HOT static struct core_target *store_find(struct core *c, uint32_t key)
{
    return (struct core_target *)store_lookup(c, key);
}

SMC_HOT static void store_add(struct core *c, uint32_t key)
{
    struct core_target *t = key ? store_find(c, key) : NULL;
    if (t)
        t->used = 1;
    if (!key || t)
        return;
    if (c->stats.targets >= STORE_LIMIT) {
        c->stats.store_full++;
        return;
    }
    uint32_t i = slot_of(key);
    while (c->store[i].key)
        i = (i + 1) & (CORE_TARGETS - 1);
    c->store[i].key = key;
    c->store[i].used = 1;
    c->stats.targets++;
}

/* Empties entry i, moving the later entries of its probe run back so lookups still find them. */
static void store_remove(struct core *c, uint32_t i)
{
    uint32_t j = i;
    for (;;) {
        memset(&c->store[i], 0, sizeof c->store[i]);
        uint32_t h;
        do {
            j = (j + 1) & (CORE_TARGETS - 1);
            if (!c->store[j].key)
                return;
            h = slot_of(c->store[j].key);
            /* entry j stays if its home slot lies cyclically in (i, j] */
        } while (i <= j ? (i < h && h <= j) : (i < h || h <= j));
        c->store[i] = c->store[j];
        i = j;
    }
}

SMC_HOT bool core_target_get(const struct core *c, uint32_t key, uint16_t *value)
{
    const struct core_target *t = key ? store_lookup(c, key) : NULL;
    if (!t || !t->known)
        return false;
    *value = t->value;
    return true;
}

SMC_HOT static void store_set(struct core *c, uint32_t key, unsigned v)
{
    struct core_target *t = key ? store_find(c, key) : NULL;
    if (!t)
        return;
    t->value = (uint16_t)v;
    t->known = 1;
    c->leds_dirty = 1;
}

/* --- output ---------------------------------------------------------------------------------- */

SMC_HOT static void emit(struct core *c, uint8_t s, uint8_t d1, uint8_t d2, unsigned len)
{
    const uint8_t m[3] = {s, d1, d2};
    c->stats.midi_out++;
    if (len == 3)
        LOG_DEBUG(engine, "out %02x %02x %02x", s, d1, d2);
    else
        LOG_DEBUG(engine, "out %02x %02x", s, d1);
    if (c->io && c->io->midi)
        c->io->midi(c->ctx, m, len, smclog_cause);
}

/* NRPN and RPN (FR-CTL-01; docs/core.md, "NRPN and RPN"). A value is CC 6 (MSB) then CC 38
 * (LSB), after the parameter number (CC 99/98, RPN 101/100) when the channel's selection is not
 * this parameter: param_out remembers what our output selected on each channel. */
SMC_HOT static bool is_param(unsigned msg)
{
    return msg == CORE_MSG_NRPN || msg == CORE_MSG_RPN;
}

SMC_HOT static uint16_t param_of(const struct core_assign *a)
{
    return (uint16_t)(CORE_PARAM_SET | (a->msg == CORE_MSG_RPN ? CORE_PARAM_RPN : 0u) | (a->number & 0x3FFFu));
}

/* The next value of a parameter assignment selects its parameter again: the first value of a
 * gesture and every button press, so a receiver that lost the selection (restarted, another
 * controller on its input) has it back at the next touch. */
SMC_HOT static void reselect(struct core *c, const struct core_assign *a, unsigned preset)
{
    if (a && is_param(a->msg))
        c->param_out[core_channel(c->cfg, preset, a)] = 0;
}

/* A plain CC we send on 98-101 selects something we do not track. */
SMC_HOT static void cc_sent(struct core *c, unsigned ch, unsigned num)
{
    if (num >= 98 && num <= 101)
        c->param_out[ch] = 0;
}

/* The null parameter (RPN 127/127): data entry after it goes nowhere (paramNull, SPEC Q-21 c). */
SMC_HOT static void send_null(struct core *c, unsigned ch)
{
    const uint16_t null = CORE_PARAM_SET | CORE_PARAM_RPN | CORE_PARAM_NULL_NUM;
    if (c->param_out[ch] == null)
        return; /* already: another control's gesture on the channel ended first */
    emit(c, (uint8_t)(0xB0 | ch), 101, 127, 3);
    emit(c, (uint8_t)(0xB0 | ch), 100, 127, 3);
    c->param_out[ch] = null;
}

/* A gesture sent a value of assignment a: with paramNull gesture in its preset, a parameter
 * assignment's null parameter is due when the gesture ends; one due on another channel goes now. */
SMC_HOT static void null_due(struct core *c, struct core_gesture *g, const struct core_assign *a)
{
    if (!is_param(a->msg) || c->cfg->preset[c->preset].param_null != CORE_PARAM_NULL_GESTURE)
        return;
    uint8_t ch = (uint8_t)(core_channel(c->cfg, c->preset, a) + 1u);
    if (g->null_ch && g->null_ch != ch)
        send_null(c, g->null_ch - 1u);
    g->null_ch = ch;
}

SMC_HOT static void null_end(struct core *c, struct core_gesture *g)
{
    if (g->null_ch)
        send_null(c, g->null_ch - 1u);
    g->null_ch = 0;
}

SMC_HOT static void send_param(struct core *c, const struct core_assign *a, unsigned ch, unsigned v)
{
    uint16_t p = param_of(a);
    if (c->param_out[ch] != p) {
        uint8_t cc = a->msg == CORE_MSG_RPN ? 101 : 99;
        emit(c, (uint8_t)(0xB0 | ch), cc, (uint8_t)(a->number >> 7 & 0x7F), 3);
        emit(c, (uint8_t)(0xB0 | ch), (uint8_t)(cc - 1), (uint8_t)(a->number & 0x7F), 3);
        c->param_out[ch] = p;
    }
    emit(c, (uint8_t)(0xB0 | ch), 6, (uint8_t)(v >> 7 & 0x7F), 3);
    emit(c, (uint8_t)(0xB0 | ch), 38, (uint8_t)(v & 0x7F), 3);
}

/* Notes our output left on (core.h, note_on): core_notes_off() ends them. */
SMC_HOT static void note_mark(struct core *c, unsigned ch, unsigned num, bool on, bool form_8n)
{
    uint8_t bit = (uint8_t)(1u << (num & 7u));
    uint8_t *o = &c->note_on[ch & 15u][num >> 3 & 15u], *f = &c->note_8n[ch & 15u][num >> 3 & 15u];
    *o = (uint8_t)(on ? *o | bit : *o & ~bit);
    *f = (uint8_t)(form_8n ? *f | bit : *f & ~bit);
}

SMC_HOT static void note_off(struct core *c, unsigned ch, unsigned num, bool form_8n)
{
    if (form_8n)
        emit(c, (uint8_t)(0x80 | ch), (uint8_t)num, 0, 3);
    else
        emit(c, (uint8_t)(0x90 | ch), (uint8_t)num, 0, 3); /* Note On, velocity 0: as the stock firmware */
    const struct core_assign a = {.msg = CORE_MSG_NOTE, .number = (uint16_t)num};
    store_set(c, core_target_key(&a, ch), 0);
}

/* Sends value v of an assignment of preset `preset` (its Note Off form, FR-CTL-10) and stores it
 * in its target. */
SMC_HOT static void send_value_of(struct core *c, const struct core_assign *a, unsigned v, unsigned preset)
{
    uint8_t ch = (uint8_t)core_channel(c->cfg, preset, a), num = (uint8_t)(a->number & 0x7Fu);
    switch (a->msg) {
    case CORE_MSG_CC:
        emit(c, 0xB0 | ch, num, (uint8_t)(v & 0x7F), 3);
        cc_sent(c, ch, num);
        break;
    case CORE_MSG_NRPN:
    case CORE_MSG_RPN:
        send_param(c, a, ch, v);
        break;
    case CORE_MSG_CC14:
        emit(c, 0xB0 | ch, num & 0x1F, (uint8_t)(v >> 7 & 0x7F), 3);
        emit(c, 0xB0 | ch, (num & 0x1F) + 32, (uint8_t)(v & 0x7F), 3);
        break;
    case CORE_MSG_PB:
        emit(c, 0xE0 | ch, (uint8_t)(v & 0x7F), (uint8_t)(v >> 7 & 0x7F), 3);
        break;
    case CORE_MSG_NOTE:
        if (v & 0x7F) {
            emit(c, 0x90 | ch, num, (uint8_t)(v & 0x7F), 3);
            note_mark(c, ch, num, true, c->cfg->preset[preset].note_off == CORE_NOTE_OFF_8N);
            break;
        }
        note_off(c, ch, num, c->cfg->preset[preset].note_off == CORE_NOTE_OFF_8N);
        note_mark(c, ch, num, false, false);
        break;
    case CORE_MSG_PROGRAM:
        emit(c, 0xC0 | ch, (uint8_t)(v & 0x7F), 0, 2);
        break;
    case CORE_MSG_ATOUCH:
        emit(c, 0xD0 | ch, (uint8_t)(v & 0x7F), 0, 2);
        break;
    default:
        return;
    }
    store_set(c, key_of(c, a, preset), v);
}

SMC_HOT static void send_value(struct core *c, const struct core_assign *a, unsigned v)
{
    send_value_of(c, a, v, c->preset);
}

/* A fader's or an absolute encoder's value (FR-IO-08): CC, 14-bit CC, NRPN, RPN, pitch bend and
 * channel pressure carry out_merge while they go out, so a slow output may replace a waiting value
 * of the same target with this one (docs/ble.md, "Last value wins"). Notes and program changes
 * are never marked. */
SMC_HOT static void send_continuous(struct core *c, const struct core_assign *a, unsigned v)
{
    unsigned m = a->msg;
    if (m == CORE_MSG_CC || m == CORE_MSG_CC14 || is_param(m) || m == CORE_MSG_PB || m == CORE_MSG_ATOUCH) {
        if (++c->out_group == 0)
            c->out_group = 1;
        c->out_merge = c->out_group;
    }
    send_value(c, a, v);
    c->out_merge = 0;
}

SMC_HOT static void send_relative(struct core *c, const struct core_assign *a, int delta)
{
    uint8_t v;
    delta = clamp(delta, -63, 63);
    if (!delta)
        return;
    switch (a->msg) {
    case CORE_MSG_REL_2C:
        v = (uint8_t)(delta & 0x7F);
        break;
    case CORE_MSG_REL_BIN:
        v = (uint8_t)(64 + delta);
        break;
    default: /* CORE_MSG_REL_SM */
        v = (uint8_t)(delta > 0 ? delta : 64 - delta);
        break;
    }
    unsigned ch = core_channel(c->cfg, c->preset, a);
    emit(c, (uint8_t)(0xB0 | ch), a->number & 0x7Fu, v, 3);
    cc_sent(c, ch, a->number & 0x7Fu);
}

/* --- time ------------------------------------------------------------------------------------ */

SMC_HOT static uint32_t gesture_us(const struct core *c)
{
    unsigned ms = c->cfg->global.gesture_ms ? c->cfg->global.gesture_ms : 300;
    return ms * 1000u;
}

SMC_HOT static uint32_t since(const struct core *c, uint32_t t)
{
    return c->now_us - t; /* unsigned: correct across a wrap of the µs clock */
}

/* Starts a gesture after a rest (true) and fixes its layer (FR-SH-05). */
SMC_HOT static bool gesture_begin(struct core *c, struct core_gesture *g)
{
    bool fresh = !g->active || since(c, g->last_us) >= gesture_us(c);
    if (fresh) {
        g->active = 1;
        g->layer = c->shift;
    }
    if (c->cfg->global.layer_mode == CORE_LAYER_IMMEDIATE)
        g->layer = c->shift;
    g->last_us = c->now_us;
    return fresh;
}

SMC_HOT static void set_indication(struct core *c, struct core_fader *f, uint8_t state)
{
    if (f->indication != state) {
        f->indication = state;
        c->leds_dirty = 1;
    }
}

SMC_HOT static void timers(struct core *c)
{
    uint32_t g = gesture_us(c);
    for (unsigned n = 0; n < CORE_STRIPS; n++) {
        struct core_fader *f = &c->fader[n];
        if (f->g.active && since(c, f->g.last_us) >= g) {
            f->g.active = 0;
            f->waiting = 0;
            set_indication(c, f, CORE_LED_OFF);
            LOG_DEBUG(engine, "fader %u gesture end", n + 1);
            null_end(c, &f->g);
        }
        struct core_encoder *e = &c->encoder[n];
        if (e->g.active && since(c, e->g.last_us) >= g) {
            e->g.active = 0;
            LOG_DEBUG(engine, "encoder %u gesture end", n + 1);
            null_end(c, &e->g);
        }
    }
    if (c->flash_led >= 0 && since(c, c->flash_us) >= FLASH_US) {
        c->flash_led = -1;
        c->leds_dirty = 1;
    }
    if (c->button[CORE_BTN_BT].down && !c->bt_long && !c->bt_none && since(c, c->bt_down_us) >= BT_LONG_US) {
        c->bt_long = 1;
        c->bt_on = !c->bt_on;
        c->leds_dirty = 1;
        LOG_INFO(engine, "bluetooth %s", c->bt_on ? "on" : "off");
        if (c->io && c->io->bluetooth)
            c->io->bluetooth(c->ctx, c->bt_on);
    }
}

/* --- Shift, pages, presets ------------------------------------------------------------------- */

SMC_HOT static void set_shift(struct core *c, unsigned on)
{
    if (c->shift == on)
        return;
    c->shift = (uint8_t)on;
    c->leds_dirty = 1;
    LOG_INFO(engine, "shift %u", on);
}

SMC_HOT static void note_shift_use(struct core *c)
{
    if (c->shift_down)
        c->shift_used = 1;
}

SMC_HOT static unsigned shift_mode(const struct core *c)
{
    return c->cfg->preset[c->preset].shift_mode;
}

SMC_HOT static void shift_press(struct core *c)
{
    c->shift_down = 1;
    c->shift_down_us = c->now_us;
    c->shift_used = 0;
    c->shift_press_off = 0;
    c->shift_press_mode = (uint8_t)shift_mode(c);
    switch (c->shift_press_mode) {
    case CORE_SHIFT_LATCH:
        set_shift(c, !c->shift);
        break;
    case CORE_SHIFT_HYBRID:
        if (c->shift) {
            c->shift_press_off = 1;
            set_shift(c, 0);
        } else {
            set_shift(c, 1);
        }
        break;
    default: /* CORE_SHIFT_HOLD */
        set_shift(c, 1);
        break;
    }
}

/* A press that began in a hold preset, or ends in one (the preset changed while Shift was held),
 * ends Shift: a hold preset never keeps it latched. */
SMC_HOT static void shift_release(struct core *c)
{
    c->shift_down = 0;
    if (shift_mode(c) == CORE_SHIFT_HOLD) {
        set_shift(c, 0);
        return;
    }
    switch (c->shift_press_mode) {
    case CORE_SHIFT_LATCH:
        break;
    case CORE_SHIFT_HYBRID: {
        unsigned ms = c->cfg->global.hybrid_ms ? c->cfg->global.hybrid_ms : 300;
        if (c->shift_press_off)
            break;
        if (since(c, c->shift_down_us) < ms * 1000u && !c->shift_used)
            LOG_INFO(engine, "shift latched");
        else
            set_shift(c, 0);
        break;
    }
    default:
        set_shift(c, 0);
        break;
    }
}

SMC_HOT static void select_bank(struct core *c, int preset, int page)
{
    if (preset >= 0 && preset < CORE_PRESETS && preset != c->preset) {
        c->preset = (uint8_t)preset;
        if (page < 0)
            page = c->page_of[preset];
        /* each preset's Shift mode governs in it: a hold preset drops a latched Shift */
        if (c->shift && !c->shift_down && shift_mode(c) == CORE_SHIFT_HOLD) {
            LOG_INFO(engine, "shift latch dropped: preset %u holds", c->preset + 1);
            set_shift(c, 0);
        }
    }
    if (page >= 0 && page < CORE_PAGES)
        c->page = (uint8_t)page;
    c->page_of[c->preset] = c->page;
    for (unsigned n = 0; n < CORE_STRIPS; n++) {
        c->fader[n].waiting = 0;
        set_indication(c, &c->fader[n], CORE_LED_OFF);
    }
    c->leds_dirty = 1;
    LOG_INFO(engine, "preset %u page %u", c->preset + 1, c->page + 1);
}

SMC_HOT static void navigate(struct core *c, unsigned b)
{
    if (b >= CORE_BTN_UP)
        select_bank(c, -1, (int)(b - CORE_BTN_UP));
    else
        select_bank(c, (int)(b - CORE_BTN_REW), -1);
    c->flash_led = (int8_t)b;
    c->flash_us = c->now_us;
}

/* --- faders (FR-CTL-03, FR-PG-02, FR-PG-04, FR-SH-03, FR-SH-04) ------------------------------ */

SMC_HOT static int map_pos(const struct range *r, unsigned pos)
{
    int32_t num = (int32_t)(r->hi - r->lo) * (int32_t)pos;
    int32_t half = CORE_FADER_MAX / 2;
    int32_t q = num >= 0 ? (num + half) / CORE_FADER_MAX : -((-num + half) / CORE_FADER_MAX);
    return r->lo + (int)q;
}

/* scale: the output moves from tv towards the end the fader moves to, proportionally. */
SMC_HOT static int scale_value(const struct range *r, int v0, int v1, int tv)
{
    if (v1 == v0)
        return tv;
    int end = v1 > v0 ? r->max : r->min;
    int dir = v1 > v0 ? 1 : -1;
    if (tv == end)
        return tv;
    int32_t num = (int32_t)(v1 - v0) * (end - tv), den = end - v0;
    int32_t q = (num >= 0) == (den >= 0) ? (num + den / 2) / den : (num - den / 2) / den;
    int out = tv + (int)q;
    if (out == tv)
        out += dir; /* never stall */
    return clamp(out, r->min, r->max);
}

SMC_HOT static void lock(struct core_fader *f, uint32_t key, int v)
{
    f->locked = 1;
    f->sync_key = key;
    f->sync_value = (uint16_t)v;
}

SMC_HOT static void on_fader(struct core *c, unsigned n, unsigned pos)
{
    struct core_fader *f = &c->fader[n];
    bool fresh = gesture_begin(c, &f->g);
    unsigned prev = f->have_pos ? f->pos : pos;
    f->pos = (uint16_t)pos;
    f->have_pos = 1;
    note_shift_use(c);
    if (fresh)
        LOG_DEBUG(engine, "fader %u gesture, layer %u", n + 1, f->g.layer);

    const struct core_assign *a = layer_of(&strip_of(c, n)->fader, f->g.layer);
    if (fresh)
        reselect(c, a, c->preset);
    uint32_t key = a ? key_of(c, a, c->preset) : 0;
    if (!key) {
        set_indication(c, f, CORE_LED_OFF);
        return;
    }
    struct range r = range_of(a);
    int v1 = map_pos(&r, pos), v0 = map_pos(&r, prev);
    uint16_t stored;
    bool known = core_target_get(c, key, &stored);
    int tv = stored;
    unsigned mode = a->mode != CORE_PICKUP_GLOBAL ? a->mode : c->cfg->global.pickup;
    bool synced = known && f->locked && f->sync_key == key && tv == f->sync_value;

    if (synced || !known || mode == CORE_JUMP) {
        if (!known || v1 != tv) {
            send_continuous(c, a, (unsigned)v1);
            null_due(c, &f->g, a);
        }
        lock(f, key, v1);
        set_indication(c, f, CORE_LED_OFF);
        return;
    }
    int tol = r.max - r.min >= 1024 ? (r.max - r.min) / 128 : 1;
    int d = v1 - tv;
    bool crossed = (v0 <= tv && tv <= v1) || (v1 <= tv && tv <= v0) || (d >= -tol && d <= tol);
    if (crossed) {
        LOG_INFO(engine, "fader %u picked up %x at %u", n + 1, key, v1);
        if (v1 != tv) {
            send_continuous(c, a, (unsigned)v1);
            null_due(c, &f->g, a);
        }
        lock(f, key, v1);
        f->waiting = 0;
        set_indication(c, f, CORE_LED_OFF);
        return;
    }
    if (mode == CORE_SCALE) {
        int out = scale_value(&r, v0, v1, tv);
        if (out != tv) {
            send_continuous(c, a, (unsigned)out);
            null_due(c, &f->g, a);
        }
        f->locked = 0;
        tv = out;
    }
    if (!f->waiting) {
        f->waiting = 1;
        LOG_INFO(engine, "fader %u waits: %x at %u, fader %u", n + 1, key, tv, v1);
    }
    bool up = (tv > v1) == (r.hi >= r.lo); /* which way the fader must move */
    set_indication(c, f, up ? CORE_LED_SLOW : CORE_LED_FAST);
}

/* --- encoders (FR-CTL-04) -------------------------------------------------------------------- */

/* Acceleration (docs/core.md, "Encoder acceleration"). A click's interval becomes a speed index,
 * s = 255 - ticks, where a tick is 156.25 us of 2/3 of the interval (0 when slow, at most 247).
 * Each mode's curve turns s into a level k (1 = no acceleration), and each level above 1 adds 1.5
 * values per click to the fine step. */
#define ACC_US_MAX 1000000u /* slower than this per click is slow anyway (and keeps the product small) */
#define ACC_S_MAX 247       /* the fastest speed index: an interval under 8 ticks counts as 8 */
#define ACC_S_FLOOR 39      /* 7-bit: the speed is at least this (one step) */
#define ACC_EXTRA_Q8 384    /* 1.5 in Q8: the values per click of each level above 1 */
#define FINE14_DIV 2048     /* the fine step of a 14-bit target: its range / this */

/* The speed index of a click interval; global.encAccel scales the speed (percent). */
SMC_HOT static int acc_speed(const struct core *c, uint32_t per_click_us)
{
    unsigned pct = c->cfg->global.enc_accel ? c->cfg->global.enc_accel : 100;
    if (per_click_us > ACC_US_MAX)
        per_click_us = ACC_US_MAX;
    /* ticks = per_click_us * 2/3 / 156.25 * 100 / pct */
    uint32_t ticks = per_click_us * 3200u / (7500u * pct);
    int s = ticks >= 255 ? 0 : 255 - (int)ticks;
    return s > ACC_S_MAX ? ACC_S_MAX : s;
}

/* The level of 7-bit targets (and relative types): 1-6. */
SMC_HOT static int acc_step7(unsigned mode, int s)
{
    int off = mode == CORE_ACC1 ? 140 : mode == CORE_ACC2 ? 100 : 0;
    if (s < ACC_S_FLOOR)
        s = ACC_S_FLOOR;
    if (mode == CORE_ACC0 || s < off)
        return 1;
    int k = (s - off) / ACC_S_FLOOR;
    return k < 1 ? 1 : k > 6 ? 6 : k;
}

/* The level of 14-bit targets: the speed index itself, less on a range below 4095, halved by acc1
 * and quadrupled by acc3. */
SMC_HOT static int acc_step14(unsigned mode, int s, int span)
{
    int base = s;
    if (span < 4095)
        base /= 4095 / (span > 0 ? span : 1);
    int k = mode == CORE_ACC1 ? base / 2 : mode == CORE_ACC2 ? base : mode == CORE_ACC3 ? base * 4 : 1;
    return k > 0 ? k : 1;
}

SMC_HOT static void on_encoder(struct core *c, unsigned n, int clicks)
{
    struct core_encoder *e = &c->encoder[n];
    uint32_t dt = since(c, e->g.last_us);
    bool fresh = gesture_begin(c, &e->g);
    note_shift_use(c);
    if (fresh) {
        e->rem = 0;
        LOG_DEBUG(engine, "encoder %u gesture, layer %u", n + 1, e->g.layer);
    }
    const struct core_assign *a = layer_of(&strip_of(c, n)->encoder, e->g.layer);
    if (fresh)
        reselect(c, a, c->preset);
    if (!a || !clicks)
        return;

    struct range r = range_of(a);
    unsigned aclicks = (unsigned)(clicks < 0 ? -clicks : clicks);
    int span = r.max - r.min;
    bool wide = !is_relative(a->msg) && type_max(a->msg) > 127;
    int8_t dir = clicks > 0 ? 1 : -1;
    /* The fine step: 1 for 7-bit targets, the range / 2048 for 14-bit ones (FR-CTL-04). */
    int fine = 1;
    if (wide && c->cfg->global.fine14 != CORE_FINE14_LSB && span / FINE14_DIV > 1)
        fine = span / FINE14_DIV;
    int delta;
    switch (a->mode) {
    case CORE_ACC0:
    case CORE_ACC1:
    case CORE_ACC2:
    case CORE_ACC3:
    case CORE_FINE: {
        if (dir != e->dir)
            e->rem = 0; /* a reversal starts slow */
        int32_t q8 = (int32_t)fine * 256;
        if (a->mode != CORE_FINE && !fresh) {
            int s = acc_speed(c, dt / aclicks);
            int k = wide ? acc_step14(a->mode, s, span) : acc_step7(a->mode, s);
            q8 += (int32_t)ACC_EXTRA_Q8 * (k - 1);
        }
        int32_t sum = e->rem + (int32_t)clicks * q8;
        delta = (int)(sum / 256);
        e->rem = (int16_t)(sum - (int32_t)delta * 256);
        break;
    }
    case CORE_DIV: {
        int div = a->param ? a->param : 2;
        e->rem = (int16_t)(e->rem + clicks);
        delta = e->rem / div;
        e->rem = (int16_t)(e->rem - delta * div);
        break;
    }
    default: /* CORE_STEP */
        delta = clicks * (a->param ? a->param : 1);
        break;
    }
    e->dir = dir;
    if (!delta)
        return;
    if (is_relative(a->msg)) {
        send_relative(c, a, delta);
        return;
    }
    uint32_t key = key_of(c, a, c->preset);
    uint16_t stored;
    bool known = core_target_get(c, key, &stored);
    int tv = known ? stored : clamp(a->start, r.min, r.max);
    int nv = clamp(tv + (r.hi >= r.lo ? delta : -delta), r.min, r.max);
    LOG_DEBUG(engine, "encoder %u %x %d: %u -> %u", n + 1, key, delta, tv, nv);
    if (nv != tv || !known) {
        send_continuous(c, a, (unsigned)nv);
        null_due(c, &e->g, a);
    }
}

/* --- buttons (FR-CTL-06) --------------------------------------------------------------------- */

SMC_HOT static void press_assign(struct core *c, const struct core_assign *a)
{
    struct range r = range_of(a);
    reselect(c, a, c->preset);
    if (a->msg == CORE_MSG_PROGRAM || a->mode != CORE_TOGGLE) {
        send_value(c, a, (unsigned)r.hi);
        return;
    }
    uint16_t v; /* on = not at lo, as the LED shows it */
    bool on = core_target_get(c, key_of(c, a, c->preset), &v) && v != r.lo;
    send_value(c, a, (unsigned)(on ? r.lo : r.hi));
}

/* A release belongs to its press: the press's assignment and preset, even after a switch. */
SMC_HOT static void release_assign(struct core *c, const struct core_assign *a, unsigned preset)
{
    if (a->mode == CORE_MOMENTARY && a->msg != CORE_MSG_PROGRAM && !(a->flags & CORE_F_NO_RELEASE))
        send_value_of(c, a, (unsigned)range_of(a).lo, preset);
    /* a press is a gesture: it ends here (paramNull of the press's preset) */
    if (is_param(a->msg) && c->cfg->preset[preset].param_null == CORE_PARAM_NULL_GESTURE)
        send_null(c, core_channel(c->cfg, preset, a));
}

SMC_HOT static void on_button(struct core *c, unsigned b, bool press)
{
    struct core_button_state *s = &c->button[b];
    if (press == (s->down != 0)) {
        LOG_WARN(input, "button %u: %u again", b, press);
        return;
    }
    s->down = press;
    if (b == CORE_BTN_SHIFT) {
        if (press)
            shift_press(c);
        else
            shift_release(c);
        return;
    }
    if (!press) {
        if (s->held == CORE_HELD_ASSIGN)
            release_assign(c, &s->a, s->preset);
        s->held = CORE_HELD_NONE;
        return;
    }
    note_shift_use(c);
    if (b == CORE_BTN_BT) { /* a long press turns Bluetooth on and off: timers() */
        s->held = CORE_HELD_OTHER;
        c->bt_long = 0;
        c->bt_down_us = c->now_us;
        return;
    }
    if (c->shift && is_nav(b)) {
        s->held = CORE_HELD_NAV;
        navigate(c, b);
        return;
    }
    const struct core_assign *a = layer_of(button_control(c, b), c->shift);
    if (!a) {
        s->held = CORE_HELD_NONE;
        return;
    }
    s->held = CORE_HELD_ASSIGN;
    s->preset = c->preset;
    s->a = *a;
    press_assign(c, &s->a);
}

/* --- incoming MIDI (FR-IO-06) ---------------------------------------------------------------- */

SMC_HOT static uint32_t remote_key(uint8_t msg, unsigned ch, unsigned num)
{
    struct core_assign a;
    memset(&a, 0, sizeof a);
    a.msg = msg;
    a.channel = (uint8_t)ch;
    a.number = (uint16_t)num;
    return core_target_key(&a, ch);
}

SMC_HOT static void remote_set(struct core *c, uint8_t msg, unsigned ch, unsigned num, unsigned v)
{
    uint32_t key = remote_key(msg, ch, num);
    struct core_target *t = store_find(c, key);
    if (!t || (t->known && t->value == v)) /* nobody addresses it, or nothing changes */
        return;
    t->value = (uint16_t)v;
    t->known = 1;
    c->leds_dirty = 1;
    LOG_DEBUG(engine, "target %x = %u (midi in)", key, v);
}

/* The LSB of a 14-bit value: it needs the MSB's value. */
SMC_HOT static void remote_lsb(struct core *c, uint8_t msg, unsigned ch, unsigned num, unsigned lsb)
{
    uint32_t key = remote_key(msg, ch, num);
    struct core_target *t = store_find(c, key);
    if (t && t->known && (t->value & 0x7Fu) != lsb) {
        t->value = (uint16_t)((t->value & 0x3F80u) | lsb);
        c->leds_dirty = 1;
        LOG_DEBUG(engine, "target %x = %u (midi in)", key, t->value);
    }
}

/* NRPN and RPN in: the sender's selection (CC 99/98, 101/100) per transport and channel, then
 * data entry (CC 6, 38) into the selected parameter's target. Increment and decrement (CC 96/97)
 * are not followed. */
SMC_HOT static void remote_param(struct core *c, unsigned tr, unsigned ch, unsigned d1, unsigned d2)
{
    uint16_t *p = &c->param_in[tr][ch];
    unsigned rpn = d1 == 101 || d1 == 100 ? CORE_PARAM_RPN : 0u;
    /* the other half of the number survives only within the same kind */
    unsigned keep = (*p & CORE_PARAM_SET) && (*p & CORE_PARAM_RPN) == rpn ? *p & 0x3FFFu : 0u;
    switch (d1) {
    case 99:
    case 101:
        *p = (uint16_t)(CORE_PARAM_SET | rpn | d2 << 7 | (keep & 0x7Fu));
        return;
    case 98:
    case 100:
        *p = (uint16_t)(CORE_PARAM_SET | rpn | (keep & 0x3F80u) | d2);
        return;
    case 6:
    case 38:
        break;
    default:
        return;
    }
    if (!(*p & CORE_PARAM_SET) || (*p & 0x3FFFu) == CORE_PARAM_NULL_NUM)
        return;
    uint8_t msg = *p & CORE_PARAM_RPN ? CORE_MSG_RPN : CORE_MSG_NRPN;
    if (d1 == 6)
        remote_set(c, msg, ch, *p & 0x3FFFu, d2 << 7);
    else
        remote_lsb(c, msg, ch, *p & 0x3FFFu, d2);
}

SMC_HOT static void on_midi_in(struct core *c, unsigned tr, const uint8_t *m)
{
    unsigned ch = m[0] & 15u, d1 = m[1] & 0x7Fu, d2 = m[2] & 0x7Fu;
    c->stats.midi_in++;
    if (tr == CORE_FROM_BLE && !c->bt_on) { /* sent before the link went down */
        c->stats.midi_in_off++;
        LOG_DEBUG(engine, "midi in from BLE ignored: bluetooth off");
        return;
    }
    switch (m[0] & 0xF0u) {
    case 0x80:
        remote_set(c, CORE_MSG_NOTE, ch, d1, 0);
        break;
    case 0x90:
        remote_set(c, CORE_MSG_NOTE, ch, d1, d2);
        break;
    case 0xB0:
        remote_set(c, CORE_MSG_CC, ch, d1, d2);
        if (d1 < 32)
            remote_set(c, CORE_MSG_CC14, ch, d1, d2 << 7);
        else if (d1 < 64)
            remote_lsb(c, CORE_MSG_CC14, ch, d1 - 32, d2);
        remote_param(c, tr, ch, d1, d2);
        break;
    case 0xC0:
        remote_set(c, CORE_MSG_PROGRAM, ch, 0, d1);
        break;
    case 0xD0:
        remote_set(c, CORE_MSG_ATOUCH, ch, 0, d1);
        break;
    case 0xE0:
        remote_set(c, CORE_MSG_PB, ch, 0, d1 | d2 << 7);
        break;
    default: /* system messages: not targets */
        break;
    }
}

/* --- LEDs (FR-CTL-07, FR-PG-03, FR-PG-04, FR-SH-06) ------------------------------------------ */

SMC_HOT static uint8_t led_state(const struct core *c, unsigned led)
{
    if ((int)led == c->flash_led)
        return CORE_LED_FAST;
    if (led >= CORE_BUTTONS)
        return c->fader[led - CORE_BUTTONS].indication;
    if (led == CORE_BTN_SHIFT)
        return c->shift ? CORE_LED_ON : CORE_LED_OFF;
    if (led == CORE_BTN_BT)
        return !c->bt_on ? CORE_LED_OFF : c->bt_link ? CORE_LED_ON : CORE_LED_SLOW;
    if (c->shift && is_nav(led)) {
        if (led >= CORE_BTN_UP)
            return led - CORE_BTN_UP == c->page ? CORE_LED_ON : CORE_LED_OFF;
        return led - CORE_BTN_REW == c->preset ? CORE_LED_ON : CORE_LED_OFF;
    }
    const struct core_assign *a = layer_of(button_control(c, led), c->shift);
    if (!a || (a->flags & CORE_F_LED_OFF))
        return CORE_LED_OFF;
    if (a->flags & CORE_F_LED_ON)
        return CORE_LED_ON;
    uint16_t v;
    if (!core_target_get(c, key_of(c, a, c->preset), &v))
        return CORE_LED_OFF;
    return v != range_of(a).lo ? CORE_LED_ON : CORE_LED_OFF;
}

SMC_HOT static void leds_refresh(struct core *c, bool all)
{
    c->leds_dirty = 0;
    for (unsigned i = 0; i < CORE_LEDS; i++) {
        uint8_t s = led_state(c, i);
        if (!all && s == c->led[i])
            continue;
        c->led[i] = s;
        LOG_TRACE(engine, "led %u = %u", i, s);
        if (c->io && c->io->led)
            c->io->led(c->ctx, i, s);
    }
}

SMC_HOT void core_select(struct core *c, int preset, int page)
{
    select_bank(c, preset, page);
    if (c->leds_dirty)
        leds_refresh(c, false);
}

/* --- entry points ---------------------------------------------------------------------------- */

SMC_HOT static void add_control(struct core *c, const struct core_control *ctl, unsigned preset)
{
    uint32_t key = key_of(c, &ctl->normal, preset);
    store_add(c, key);
    if (ctl->shift_kind == CORE_SHIFT_OVERRIDE && key_of(c, &ctl->shift, preset) != key)
        store_add(c, key_of(c, &ctl->shift, preset));
}

/* Marks the targets of a control that are stored already (core_reconfigured()). */
static void mark_control(struct core *c, const struct core_control *ctl, unsigned preset)
{
    struct core_target *t = store_find(c, key_of(c, &ctl->normal, preset));
    if (t)
        t->used = 1;
    if (ctl->shift_kind == CORE_SHIFT_OVERRIDE && (t = store_find(c, key_of(c, &ctl->shift, preset))) != NULL)
        t->used = 1;
}

/* Calls fn for every control of the configuration. */
static void each_control(struct core *c, void (*fn)(struct core *, const struct core_control *, unsigned))
{
    for (unsigned k = 0; k < CORE_PRESETS; k++) {
        const struct core_preset *p = &c->cfg->preset[k];
        for (unsigned pg = 0; pg < CORE_PAGES; pg++) {
            for (unsigned n = 0; n < CORE_STRIPS; n++) {
                const struct core_strip *s = &p->page[pg][n];
                fn(c, &s->fader, k);
                fn(c, &s->encoder, k);
                for (unsigned b = 0; b < 4; b++)
                    fn(c, &s->button[b], k);
            }
        }
        for (unsigned b = 0; b < CORE_ROW_BUTTONS; b++)
            fn(c, &p->row[b], k);
    }
}

SMC_HOT void core_init(struct core *c, const struct core_config *cfg, const struct core_io *io, void *ctx)
{
    memset(c, 0, sizeof *c);
    c->cfg = cfg;
    c->io = io;
    c->ctx = ctx;
    c->flash_led = -1;
    c->bt_none = 1; /* until core_set_bluetooth() */
    each_control(c, add_control);
    LOG_INFO(engine, "core start: %u targets, layer mode %u, pickup %u", c->stats.targets,
             cfg->global.layer_mode, cfg->global.pickup);
    LOG_INFO(engine, "presets 1-4: shift modes %u %u %u %u, note off forms %u %u %u %u",
             cfg->preset[0].shift_mode, cfg->preset[1].shift_mode, cfg->preset[2].shift_mode,
             cfg->preset[3].shift_mode, cfg->preset[0].note_off, cfg->preset[1].note_off,
             cfg->preset[2].note_off, cfg->preset[3].note_off);
    LOG_INFO(engine, "presets 1-4: channels %u %u %u %u", cfg->preset[0].channel + 1u, cfg->preset[1].channel + 1u,
             cfg->preset[2].channel + 1u, cfg->preset[3].channel + 1u);
    if (c->stats.store_full)
        LOG_WARN(engine, "target store full: %u targets not stored", c->stats.store_full);
    leds_refresh(c, true);
}

void core_reconfigured(struct core *c)
{
    unsigned before = c->stats.targets, dropped = 0;
    for (unsigned i = 0; i < CORE_TARGETS; i++)
        c->store[i].used = 0;
    each_control(c, mark_control);
    for (unsigned i = 0; i < CORE_TARGETS; i++)
        while (c->store[i].key && !c->store[i].used) {
            store_remove(c, i);
            c->stats.targets--;
            dropped++;
        }
    c->stats.store_full = 0;
    each_control(c, add_control);
    bool own = smclog_cause == 0;
    if (own)
        smclog_cause_begin();
    LOG_INFO(engine, "configuration edited: %u targets, %u dropped, %u added", c->stats.targets, dropped,
             c->stats.targets + dropped - before);
    if (c->stats.store_full)
        LOG_WARN(engine, "target store full: %u targets not stored", c->stats.store_full);
    leds_refresh(c, false);
    if (own)
        smclog_cause_end();
}

unsigned core_release_control(struct core *c, unsigned preset, const struct core_control *ctl)
{
    unsigned n = 0;
    for (unsigned layer = 0; layer < 2; layer++) {
        const struct core_assign *a = layer_of(ctl, layer);
        if (!a || a->msg != CORE_MSG_NOTE)
            continue;
        unsigned ch = core_channel(c->cfg, preset, a), num = a->number & 0x7Fu;
        uint32_t key = core_target_key(a, ch);
        for (unsigned b = 0; b < CORE_BUTTONS; b++) {
            struct core_button_state *s = &c->button[b];
            if (s->held == CORE_HELD_ASSIGN && s->preset == preset && s->a.msg == CORE_MSG_NOTE &&
                core_target_key(&s->a, core_channel(c->cfg, preset, &s->a)) == key)
                s->held = CORE_HELD_OTHER; /* its Note Off goes now: the release sends nothing */
        }
        uint8_t bit = (uint8_t)(1u << (num & 7u));
        if (c->note_on[ch][num >> 3] & bit) {
            note_off(c, ch, num, c->note_8n[ch][num >> 3] & bit);
            note_mark(c, ch, num, false, false);
            n++;
        }
    }
    if (n)
        LOG_INFO(engine, "notes off before a control changes: %u", n);
    return n;
}

void core_set_bluetooth(struct core *c, int on, bool link)
{
    c->bt_on = on > 0;
    c->bt_none = on < 0;
    c->bt_link = link;
    leds_refresh(c, false);
}

unsigned core_notes_off_limit(struct core *c, unsigned max)
{
    unsigned n = 0;
    bool own = smclog_cause == 0;
    if (own)
        smclog_cause_begin();
    /* A preset edit may change the channel even when an inverted note's press emitted zero.
     * Disarm every held note, not only those represented in the sounding-note bitmap. */
    for (unsigned b = 0; b < CORE_BUTTONS; b++)
        if (c->button[b].held == CORE_HELD_ASSIGN && c->button[b].a.msg == CORE_MSG_NOTE)
            c->button[b].held = CORE_HELD_OTHER;
    for (unsigned ch = 0; ch < 16; ch++)
        for (unsigned num = 0; num < 128; num++)
            if (n < max && (c->note_on[ch][num >> 3] & (1u << (num & 7u)))) {
                note_off(c, ch, num, c->note_8n[ch][num >> 3] & (1u << (num & 7u)));
                note_mark(c, ch, num, false, false);
                n++;
            }
    if (n)
        LOG_INFO(engine, "notes off before the configuration changes: %u", n);
    if (own)
        smclog_cause_end();
    return n;
}

unsigned core_notes_off(struct core *c)
{
    return core_notes_off_limit(c, 16u * 128u);
}

/* Input records (LOG-04 chain heads). Faders and encoders by strip number (1-8), buttons by
 * index (0-44, docs/core.md), MIDI by transport (0 USB, 1 BLE). tools/smcsim replays these. */
SMC_HOT static void log_input(const struct core_event *ev)
{
    unsigned i = ev->index, n = ev->index + 1u;
    int v = ev->value;
    const uint8_t *m = ev->midi;
    if (ev->flags & CORE_EV_INJECTED) {
        switch (ev->type) {
        case CORE_EV_BUTTON:
            if (v)
                LOG_INFO(input, "button %u down (injected)", i);
            else
                LOG_INFO(input, "button %u up (injected)", i);
            break;
        case CORE_EV_ENCODER:
            LOG_DEBUG(input, "encoder %u %+d (injected)", n, v);
            break;
        case CORE_EV_FADER:
            LOG_DEBUG(input, "fader %u = %d (injected)", n, v);
            break;
        case CORE_EV_MIDI_IN:
            LOG_DEBUG(input, "midi in %u: %02x %02x %02x (injected)", i, m[0], m[1], m[2]);
            break;
        default:
            break;
        }
        return;
    }
    switch (ev->type) {
    case CORE_EV_BUTTON:
        if (v)
            LOG_INFO(input, "button %u down", i);
        else
            LOG_INFO(input, "button %u up", i);
        break;
    case CORE_EV_ENCODER:
        LOG_DEBUG(input, "encoder %u %+d", n, v);
        break;
    case CORE_EV_FADER:
        LOG_DEBUG(input, "fader %u = %d", n, v);
        break;
    case CORE_EV_MIDI_IN:
        LOG_DEBUG(input, "midi in %u: %02x %02x %02x", i, m[0], m[1], m[2]);
        break;
    case CORE_EV_LINK:
        LOG_INFO(input, "link %u = %d", i, v);
        break;
    default:
        break;
    }
}

SMC_HOT void core_event(struct core *c, const struct core_event *ev)
{
    bool own = smclog_cause == 0 && ev->type != CORE_EV_TICK;
    if (own)
        smclog_cause_begin();
    c->now_us = ev->t_us;
    c->stats.events++;
    if (ev->flags & CORE_EV_INJECTED)
        c->stats.injected++;
    log_input(ev);
    timers(c);

    bool ok = true;
    switch (ev->type) {
    case CORE_EV_TICK:
        break;
    case CORE_EV_BUTTON:
        if ((ok = ev->index < CORE_BUTTONS))
            on_button(c, ev->index, ev->value != 0);
        break;
    case CORE_EV_ENCODER:
        if ((ok = ev->index < CORE_STRIPS))
            on_encoder(c, ev->index, ev->value);
        break;
    case CORE_EV_FADER:
        if ((ok = ev->index < CORE_STRIPS))
            on_fader(c, ev->index, (unsigned)clamp(ev->value, 0, CORE_FADER_MAX));
        break;
    case CORE_EV_MIDI_IN:
        if ((ok = ev->index < CORE_TRANSPORTS))
            on_midi_in(c, ev->index, ev->midi);
        break;
    case CORE_EV_LINK:
        if ((ok = ev->index == CORE_FROM_BLE)) {
            c->bt_link = ev->value != 0;
            c->leds_dirty = 1;
        }
        break;
    default:
        ok = false;
        break;
    }
    if (!ok) {
        c->stats.bad_events++;
        LOG_WARN(input, "bad event: type %u index %u", ev->type, ev->index);
    }
    if (c->leds_dirty)
        leds_refresh(c, false);
    if (own)
        smclog_cause_end();
}
