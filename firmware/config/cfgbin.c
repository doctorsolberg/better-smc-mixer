/* The configuration's binary form (cfgbin.h; docs/config.md, "Binary form"). */
#include "cfgbin.h"

#include <string.h>

#include "cfg_schema.h"
#include "smclog.h"

#define PER_PAGE (6 * 8) /* control ids on a page: 6 groups of 8 (config/schema.json) */
#define FIELD_FLAGS 5    /* the assignment's flags, in binary order */
#define NO_FIELD 255

/* --- the struct's fields through the schema's tables ------------------------------------------- */

static unsigned get(const void *base, const struct cfg_field *f)
{
    const uint8_t *p = (const uint8_t *)base + f->off;
    if (f->size == 1)
        return *p;
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

static void put(void *base, const struct cfg_field *f, unsigned v)
{
    uint8_t *p = (uint8_t *)base + f->off;
    if (f->size == 1) {
        *p = (uint8_t)v;
    } else {
        uint16_t w = (uint16_t)v;
        memcpy(p, &w, 2);
    }
}

static bool type_in(unsigned set, unsigned msg)
{
    switch (set) {
    case CFG_T_ANY:
        return true;
    case CFG_T_NOT_NONE:
        return CFG_TYPES_NOT_NONE >> msg & 1u;
    case CFG_T_NUMBER:
        return CFG_TYPES_NUMBER >> msg & 1u;
    default:
        return CFG_TYPES_VALUE >> msg & 1u;
    }
}

/* Whether a field applies to a kind, type and mode (all already in range). */
static bool applies(const struct cfg_field *f, unsigned kind, unsigned msg, unsigned mode)
{
    return (f->kinds >> kind & 1u) && type_in(f->types, msg) && (f->modes >> mode & 1u);
}

static unsigned field_max(const struct cfg_field *f, unsigned kind, unsigned msg)
{
    switch (f->max_from) {
    case CFG_TYPE_MAX:
        return cfg_type_max[msg];
    case CFG_NUMBER_MAX:
        return cfg_number_max[msg];
    case CFG_KIND_MODES:
        return cfg_kind_modes[kind] - 1u;
    default:
        return f->max;
    }
}

/* A field's value when it is not given: 0 where it does not apply (and for a required field, which
 * its minimum then refuses). a: the assignment so far (lo and hi come before the fields whose
 * default is their middle). */
static unsigned field_default(const struct cfg_field *f, unsigned kind, unsigned msg, unsigned mode,
                              const struct core_assign *a)
{
    if (!applies(f, kind, msg, mode))
        return 0;
    switch (f->def_from) {
    case CFG_MIDDLE:
        return ((unsigned)a->lo + a->hi + 1u) / 2u;
    case CFG_KIND_MODE:
        return cfg_kind_mode_default[kind];
    case CFG_TYPE_MAX:
        return cfg_type_max[msg];
    default:
        return f->def;
    }
}

static bool flags_ok(unsigned v)
{
    unsigned known = 0;
    for (unsigned i = 0; i < CFG_FLAG_MEMBERS; i++) {
        known |= cfg_flag_member[i].mask;
        if (!(cfg_flag_member[i].allowed >> ((v & cfg_flag_member[i].mask) >> cfg_flag_member[i].shift) & 1u))
            return false;
    }
    return !(v & ~known);
}

/* --- checks ------------------------------------------------------------------------------------ */

static const struct core_assign no_assign;

static int check_assign(const struct core_assign *a, unsigned kind, uint8_t *field)
{
    unsigned msg = a->msg, mode = a->mode;
    if (msg >= CFG_MSG_COUNT || !(cfg_kind_types[kind] >> msg & 1u)) {
        *field = 0;
        return CFGBIN_E_TYPE;
    }
    if (msg != CORE_MSG_NONE && mode >= cfg_kind_modes[kind]) {
        *field = 3;
        return CFGBIN_E_VALUE;
    }
    for (unsigned i = 1; i < CFG_ASSIGN_FIELDS; i++) {
        const struct cfg_field *f = &cfg_assign_field[i];
        unsigned v = get(a, f);
        bool ok;
        if (msg == CORE_MSG_NONE || !applies(f, kind, msg, mode))
            ok = v == 0;
        else if (i == FIELD_FLAGS)
            ok = flags_ok(v);
        else
            ok = v >= f->min && v <= field_max(f, kind, msg);
        if (!ok) {
            *field = (uint8_t)i;
            return CFGBIN_E_VALUE;
        }
    }
    return CFGBIN_OK;
}

static int check_control(const struct core_control *c, unsigned kind, bool nav, uint8_t *field)
{
    int r = check_assign(&c->normal, kind, field);
    if (r)
        return r;
    *field = NO_FIELD;
    if (c->shift_kind >= CFG_SHIFT_KINDS)
        return CFGBIN_E_VALUE;
    if (nav && c->shift_kind != CORE_SHIFT_INHERIT)
        return CFGBIN_E_SHIFT;
    if (c->shift_kind != CORE_SHIFT_OVERRIDE)
        return memcmp(&c->shift, &no_assign, sizeof no_assign) ? CFGBIN_E_VALUE : CFGBIN_OK;
    if (c->shift.msg == CORE_MSG_NONE)
        return CFGBIN_E_SHIFT;
    return check_assign(&c->shift, kind, field);
}

static int check_settings(const void *base, const struct cfg_field *fields, unsigned n, uint8_t *field)
{
    for (unsigned i = 0; i < n; i++) {
        unsigned v = get(base, &fields[i]);
        if (v < fields[i].min || v > fields[i].max) {
            *field = (uint8_t)i;
            return CFGBIN_E_VALUE;
        }
    }
    return CFGBIN_OK;
}

/* --- controls by binary address --------------------------------------------------------------- */

/* Whether (page, id) is a control here; its kind and whether it is a navigation button. */
static bool locate(unsigned page, unsigned id, unsigned *kind, bool *nav)
{
    if (page == CFG_ROW_PAGE) {
        if (id >= CORE_ROW_BUTTONS)
            return false;
        *kind = CFG_KIND_BUTTON;
        *nav = CFG_ROW_NAV >> id & 1u;
        return true;
    }
    if (page >= CORE_PAGES || id / 8 >= 6 || id % 8 >= CORE_STRIPS)
        return false;
    *kind = cfg_strip_control[id / 8].kind;
    *nav = false;
    return true;
}

static struct core_control *control(struct core_preset *p, unsigned page, unsigned id)
{
    if (page == CFG_ROW_PAGE)
        return &p->row[id];
    return (struct core_control *)((uint8_t *)&p->page[page][id % 8] + cfg_strip_control[id / 8].off);
}

/* The index of a control among its preset's (CFGBIN_CONTROLS of them, bitmaps). */
static unsigned slot(unsigned page, unsigned id)
{
    if (page == CFG_ROW_PAGE)
        return CORE_PAGES * PER_PAGE + id;
    return page * PER_PAGE + id;
}

#define SLOTS (CORE_PAGES * PER_PAGE + CORE_ROW_BUTTONS)

/* Calls fn for every control of a preset, in binary order. */
typedef void (*each_fn)(void *ctx, unsigned page, unsigned id, unsigned kind, bool nav);
static void each_control(each_fn fn, void *ctx)
{
    for (unsigned pg = 0; pg < CORE_PAGES; pg++)
        for (unsigned g = 0; g < 6; g++)
            for (unsigned n = 0; n < CORE_STRIPS; n++)
                fn(ctx, pg, g * 8 + n, cfg_strip_control[g].kind, false);
    for (unsigned i = 0; i < CORE_ROW_BUTTONS; i++)
        fn(ctx, CFG_ROW_PAGE, i, CFG_KIND_BUTTON, CFG_ROW_NAV >> i & 1u);
}

/* The encoder's default Shift layer: the Normal layer in fine mode (FR-CTL-04). */
static void default_shift(unsigned kind, const struct core_assign *normal, uint8_t *shift_kind,
                          struct core_assign *shift)
{
    if ((CFG_KINDS_FINE >> kind & 1u) && normal->msg != CORE_MSG_NONE) {
        *shift_kind = CORE_SHIFT_OVERRIDE;
        *shift = *normal;
        shift->mode = CFG_FINE_MODE;
        shift->param = 0;
    } else {
        *shift_kind = CORE_SHIFT_INHERIT;
        *shift = no_assign;
    }
}

/* --- check ------------------------------------------------------------------------------------- */

struct check_ctx {
    const struct core_preset *p;
    struct cfgbin_error *e;
};

static void check_one(void *ctx, unsigned page, unsigned id, unsigned kind, bool nav)
{
    struct check_ctx *c = ctx;
    if (c->e->code)
        return;
    int r = check_control(control((struct core_preset *)c->p, page, id), kind, nav, &c->e->field);
    if (r) {
        c->e->code = (uint8_t)r;
        c->e->page = (uint8_t)page;
        c->e->control = (uint8_t)id;
    }
}

static void error_init(struct cfgbin_error *e)
{
    memset(e, 0, sizeof *e);
    e->preset = e->page = e->control = e->field = 255;
}

int cfgbin_check(const struct core_config *cfg, struct cfgbin_error *err)
{
    struct cfgbin_error e;
    error_init(&e);
    int r = check_settings(&cfg->global, cfg_global_field, CFG_GLOBAL_FIELDS, &e.field);
    for (unsigned p = 0; !r && p < CORE_PRESETS; p++) {
        e.preset = (uint8_t)p;
        r = check_settings(&cfg->preset[p], cfg_preset_field, CFG_PRESET_FIELDS, &e.field);
        if (!r) {
            struct check_ctx c = {&cfg->preset[p], &e};
            each_control(check_one, &c);
            r = e.code;
        }
    }
    if (!r)
        e.preset = 255;
    e.code = (uint8_t)r;
    if (err)
        *err = e;
    return r;
}

/* --- encode ------------------------------------------------------------------------------------ */

struct writer {
    uint8_t *out;
    size_t cap, len;
    bool full;
};

static void w_bytes(struct writer *w, const uint8_t *p, size_t n)
{
    if (w->full || w->len + n > w->cap) {
        w->full = true;
        return;
    }
    memcpy(w->out + w->len, p, n);
    w->len += n;
}

/* Appends the fields of mask to a record being built. */
static unsigned pack(uint8_t *rec, unsigned n, const struct cfg_field *fields, unsigned count,
                     const unsigned *vals, unsigned mask)
{
    for (unsigned i = 0; i < count; i++) {
        if (!(mask >> i & 1u))
            continue;
        rec[n++] = (uint8_t)vals[i];
        if (fields[i].size == 2)
            rec[n++] = (uint8_t)(vals[i] >> 8);
    }
    return n;
}

static void record(struct writer *w, unsigned tag, const uint8_t *payload, unsigned n)
{
    uint8_t head[2] = {(uint8_t)tag, (uint8_t)n};
    w_bytes(w, head, 2);
    w_bytes(w, payload, n);
}

struct preset_ctx {
    const struct core_preset *p;
    bool any;         /* a control that is not all defaults */
    struct writer *w; /* NULL: look only */
};

static void look_one(void *ctx, unsigned page, unsigned id, unsigned kind, bool nav)
{
    struct preset_ctx *c = ctx;
    (void)nav;
    const struct core_control *ctl = control((struct core_preset *)c->p, page, id);
    uint8_t k;
    struct core_assign s;
    default_shift(kind, &ctl->normal, &k, &s);
    if (ctl->normal.msg != CORE_MSG_NONE)
        c->any = true;
    if (k != ctl->shift_kind || memcmp(&s, &ctl->shift, sizeof s))
        c->any = true;
}

static void write_one(void *ctx, unsigned page, unsigned id, unsigned kind, bool nav)
{
    struct preset_ctx *c = ctx;
    (void)nav;
    const struct core_control *ctl = control((struct core_preset *)c->p, page, id);
    const struct core_assign *a = &ctl->normal;
    unsigned vals[CFG_ASSIGN_FIELDS], base[CFG_ASSIGN_FIELDS], mask = 0;
    uint8_t rec[32];
    for (unsigned i = 0; i < CFG_ASSIGN_FIELDS; i++)
        vals[i] = get(a, &cfg_assign_field[i]);
    if (a->msg != CORE_MSG_NONE) {
        for (unsigned i = 0; i < CFG_ASSIGN_FIELDS; i++)
            if (vals[i] != field_default(&cfg_assign_field[i], kind, a->msg, a->mode, a))
                mask |= 1u << i;
        rec[0] = (uint8_t)page;
        rec[1] = (uint8_t)id;
        rec[2] = (uint8_t)mask;
        rec[3] = (uint8_t)(mask >> 8);
        record(c->w, CFG_TAG_ASSIGN, rec, pack(rec, 4, cfg_assign_field, CFG_ASSIGN_FIELDS, vals, mask));
    }
    uint8_t k;
    struct core_assign s;
    default_shift(kind, a, &k, &s);
    if (k == ctl->shift_kind && !memcmp(&s, &ctl->shift, sizeof s))
        return;
    rec[0] = (uint8_t)page;
    rec[1] = (uint8_t)id;
    rec[2] = ctl->shift_kind;
    unsigned n = 3;
    if (ctl->shift_kind == CORE_SHIFT_OVERRIDE) {
        mask = 0;
        for (unsigned i = 0; i < CFG_ASSIGN_FIELDS; i++) {
            base[i] = vals[i];
            vals[i] = get(&ctl->shift, &cfg_assign_field[i]);
            if (vals[i] != base[i])
                mask |= 1u << i;
        }
        rec[3] = (uint8_t)mask;
        rec[4] = (uint8_t)(mask >> 8);
        n = pack(rec, 5, cfg_assign_field, CFG_ASSIGN_FIELDS, vals, mask);
    }
    record(c->w, CFG_TAG_SHIFT, rec, n);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

size_t cfgbin_encode(const struct core_config *cfg, const struct cfgbin_meta *meta, uint8_t *out,
                     size_t cap, struct cfgbin_error *err)
{
    struct cfgbin_error e;
    if (cfgbin_check(cfg, &e)) {
        if (err)
            *err = e;
        return 0;
    }
    struct writer w = {out, cap, 0, false};
    uint8_t head[CFGBIN_HEADER] = {0};
    w_bytes(&w, head, sizeof head);
    unsigned vals[CFG_GLOBAL_FIELDS > CFG_PRESET_FIELDS ? CFG_GLOBAL_FIELDS : CFG_PRESET_FIELDS], mask = 0;
    uint8_t rec[16];
    for (unsigned i = 0; i < CFG_GLOBAL_FIELDS; i++) {
        vals[i] = get(&cfg->global, &cfg_global_field[i]);
        if (vals[i] != cfg_global_field[i].def)
            mask |= 1u << i;
    }
    if (mask) {
        rec[0] = (uint8_t)mask;
        record(&w, CFG_TAG_GLOBAL, rec, pack(rec, 1, cfg_global_field, CFG_GLOBAL_FIELDS, vals, mask));
    }
    for (unsigned p = 0; p < CORE_PRESETS; p++) {
        struct preset_ctx c;
        memset(&c, 0, sizeof c);
        c.p = &cfg->preset[p];
        each_control(look_one, &c);
        mask = 0;
        for (unsigned i = 0; i < CFG_PRESET_FIELDS; i++) {
            vals[i] = get(c.p, &cfg_preset_field[i]);
            if (vals[i] != cfg_preset_field[i].def)
                mask |= 1u << i;
        }
        if (!mask && !c.any)
            continue;
        rec[0] = (uint8_t)p;
        rec[1] = (uint8_t)mask;
        record(&w, CFG_TAG_PRESET, rec, pack(rec, 2, cfg_preset_field, CFG_PRESET_FIELDS, vals, mask));
        c.w = &w;
        each_control(write_one, &c);
    }
    if (w.full) {
        error_init(&e);
        e.code = CFGBIN_E_SPACE;
        if (err)
            *err = e;
        return 0;
    }
    memcpy(out, CFG_MAGIC, 4);
    out[4] = CFG_BINARY_VERSION;
    out[5] = CFGBIN_HEADER;
    put32(out + 8, (uint32_t)(w.len - CFGBIN_HEADER));
    put32(out + 16, meta ? meta->revision : 0);
    if (meta)
        memcpy(out + 20, meta->id, 16);
    put32(out + 12, cfgbin_crc32(0, out + 16, w.len - 16));
    if (err)
        error_init(err);
    return w.len;
}

/* --- decode ------------------------------------------------------------------------------------ */

struct decoder {
    struct core_config *cfg; /* NULL: check only */
    struct cfgbin_error e;
    bool patch;                /* cfgbin_patch(): records for one preset, applied in place */
    cfgbin_changing_fn changing; /* patch: called before a setting or control changes */
    void *ctx;
    int preset; /* the preset record in force, -1 none */
    uint8_t seen_global, seen_presets;
    uint8_t normal[CORE_PRESETS][(SLOTS + 7) / 8], shift[CORE_PRESETS][(SLOTS + 7) / 8];
    int last;                       /* the slot of the record before, if it was a Normal layer */
    struct core_assign last_normal; /* that layer */
};

static bool bit(const uint8_t *map, unsigned i)
{
    return map[i / 8] >> (i % 8) & 1u;
}

static void set_bit(uint8_t *map, unsigned i)
{
    map[i / 8] |= (uint8_t)(1u << (i % 8));
}

/* The fields of mask from data[*off..len) into vals; false if the record's length does not fit. */
static bool unpack(const uint8_t *data, unsigned len, unsigned off, const struct cfg_field *fields,
                   unsigned count, unsigned mask, unsigned *vals)
{
    for (unsigned i = 0; i < count; i++) {
        if (!(mask >> i & 1u))
            continue;
        if (off + fields[i].size > len)
            return false;
        vals[i] = data[off];
        if (fields[i].size == 2)
            vals[i] |= (unsigned)data[off + 1] << 8;
        off += fields[i].size;
    }
    return off == len;
}

static int fail(struct decoder *d, int code, unsigned field)
{
    d->e.code = (uint8_t)code;
    d->e.field = (uint8_t)field;
    return code;
}

static int settings(struct decoder *d, const uint8_t *p, unsigned n, unsigned at, const struct cfg_field *fields,
                    unsigned count, unsigned *vals)
{
    unsigned mask = p[at - 1];
    if (mask >> count)
        return fail(d, CFGBIN_E_FIELD, NO_FIELD);
    for (unsigned i = 0; i < count; i++)
        vals[i] = fields[i].def;
    if (!unpack(p, n, at, fields, count, mask, vals))
        return fail(d, CFGBIN_E_RECORD, NO_FIELD);
    for (unsigned i = 0; i < count; i++)
        if (vals[i] < fields[i].min || vals[i] > fields[i].max)
            return fail(d, CFGBIN_E_VALUE, i);
    return CFGBIN_OK;
}

static int control_record(struct decoder *d, unsigned tag, const uint8_t *p, unsigned n)
{
    if (d->preset < 0)
        return fail(d, CFGBIN_E_ORDER, NO_FIELD);
    if (n < 2)
        return fail(d, CFGBIN_E_RECORD, NO_FIELD);
    unsigned page = p[0], id = p[1], kind;
    bool nav;
    d->e.page = (uint8_t)page;
    d->e.control = (uint8_t)id;
    if (!locate(page, id, &kind, &nav))
        return fail(d, CFGBIN_E_WHERE, NO_FIELD);
    unsigned s = slot(page, id), pr = (unsigned)d->preset, vals[CFG_ASSIGN_FIELDS] = {0}, mask;
    int prev = d->last;
    d->last = -1;
    struct core_control *ctl = d->cfg ? control(&d->cfg->preset[pr], page, id) : NULL;
    if (ctl && d->changing && !bit(d->normal[pr], s) && !bit(d->shift[pr], s))
        d->changing(d->ctx, ctl); /* the first record of this control: it is about to change */
    struct core_assign a = no_assign;
    uint8_t field = NO_FIELD;
    if (tag == CFG_TAG_ASSIGN) {
        if (bit(d->normal[pr], s))
            return fail(d, CFGBIN_E_TWICE, NO_FIELD);
        if (bit(d->shift[pr], s))
            return fail(d, CFGBIN_E_ORDER, NO_FIELD);
        set_bit(d->normal[pr], s);
        if (n < 4)
            return fail(d, CFGBIN_E_RECORD, NO_FIELD);
        mask = p[2] | (unsigned)p[3] << 8;
        if (mask >> CFG_ASSIGN_FIELDS)
            return fail(d, CFGBIN_E_FIELD, NO_FIELD);
        if (!unpack(p, n, 4, cfg_assign_field, CFG_ASSIGN_FIELDS, mask, vals))
            return fail(d, CFGBIN_E_RECORD, NO_FIELD);
        unsigned msg = mask & 1u ? vals[0] : CORE_MSG_NONE;
        unsigned mode = mask >> 3 & 1u ? vals[3] : msg != CORE_MSG_NONE ? cfg_kind_mode_default[kind] : 0;
        a.msg = (uint8_t)msg;
        a.mode = (uint8_t)mode;
        if (msg < CFG_MSG_COUNT && (cfg_kind_types[kind] >> msg & 1u) && mode < cfg_kind_modes[kind])
            for (unsigned i = 0; i < CFG_ASSIGN_FIELDS; i++)
                put(&a, &cfg_assign_field[i], mask >> i & 1u ? vals[i]
                                                              : field_default(&cfg_assign_field[i], kind, msg, mode, &a));
        int r = check_assign(&a, kind, &field);
        if (r)
            return fail(d, r, field);
        if (ctl)
            ctl->normal = a;
        d->last = (int)s;
        d->last_normal = a;
        return CFGBIN_OK;
    }
    if (bit(d->shift[pr], s))
        return fail(d, CFGBIN_E_TWICE, NO_FIELD);
    if (bit(d->normal[pr], s) && prev != (int)s)
        return fail(d, CFGBIN_E_ORDER, NO_FIELD);
    set_bit(d->shift[pr], s);
    if (n < 3)
        return fail(d, CFGBIN_E_RECORD, NO_FIELD);
    unsigned k = p[2];
    if (k >= CFG_SHIFT_KINDS)
        return fail(d, CFGBIN_E_VALUE, NO_FIELD);
    if (nav)
        return fail(d, CFGBIN_E_SHIFT, NO_FIELD);
    if (k == CORE_SHIFT_OVERRIDE) {
        if (n < 5)
            return fail(d, CFGBIN_E_RECORD, NO_FIELD);
        mask = p[3] | (unsigned)p[4] << 8;
        if (mask >> CFG_ASSIGN_FIELDS)
            return fail(d, CFGBIN_E_FIELD, NO_FIELD);
        if (!unpack(p, n, 5, cfg_assign_field, CFG_ASSIGN_FIELDS, mask, vals))
            return fail(d, CFGBIN_E_RECORD, NO_FIELD);
        if (bit(d->normal[pr], s))
            a = d->last_normal;
        for (unsigned i = 0; i < CFG_ASSIGN_FIELDS; i++)
            if (mask >> i & 1u)
                put(&a, &cfg_assign_field[i], vals[i]);
        if (a.msg == CORE_MSG_NONE)
            return fail(d, CFGBIN_E_SHIFT, NO_FIELD);
        int r = check_assign(&a, kind, &field);
        if (r)
            return fail(d, r, field);
    } else if (n != 3) {
        return fail(d, CFGBIN_E_RECORD, NO_FIELD);
    }
    if (ctl) {
        ctl->shift_kind = (uint8_t)k;
        ctl->shift = a;
    }
    return CFGBIN_OK;
}

static int record_at(struct decoder *d, unsigned tag, const uint8_t *p, unsigned n)
{
    unsigned vals[CFG_GLOBAL_FIELDS > CFG_PRESET_FIELDS ? CFG_GLOBAL_FIELDS : CFG_PRESET_FIELDS];
    int r;
    switch (tag) {
    case CFG_TAG_GLOBAL:
        d->last = -1;
        if (d->seen_global++)
            return fail(d, CFGBIN_E_TWICE, NO_FIELD);
        if (n < 1)
            return fail(d, CFGBIN_E_RECORD, NO_FIELD);
        r = settings(d, p, n, 1, cfg_global_field, CFG_GLOBAL_FIELDS, vals);
        if (!r && d->cfg)
            for (unsigned i = 0; i < CFG_GLOBAL_FIELDS; i++)
                put(&d->cfg->global, &cfg_global_field[i], vals[i]);
        return r;
    case CFG_TAG_PRESET:
        d->last = -1;
        if (n < 2)
            return fail(d, CFGBIN_E_RECORD, NO_FIELD);
        d->e.preset = p[0];
        if (p[0] >= CORE_PRESETS || (d->patch && p[0] != (unsigned)d->preset))
            return fail(d, CFGBIN_E_WHERE, NO_FIELD);
        if (d->seen_presets >> p[0] & 1u)
            return fail(d, CFGBIN_E_TWICE, NO_FIELD);
        d->seen_presets |= (uint8_t)(1u << p[0]);
        r = settings(d, p, n, 2, cfg_preset_field, CFG_PRESET_FIELDS, vals);
        if (r)
            return r;
        d->preset = p[0];
        if (d->cfg && d->changing)
            d->changing(d->ctx, NULL);
        if (d->cfg)
            for (unsigned i = 0; i < CFG_PRESET_FIELDS; i++)
                put(&d->cfg->preset[p[0]], &cfg_preset_field[i], vals[i]);
        return CFGBIN_OK;
    case CFG_TAG_ASSIGN:
    case CFG_TAG_SHIFT:
        return control_record(d, tag, p, n);
    default:
        d->last = -1;
        return tag & CFG_TAG_OPTIONAL ? CFGBIN_OK : fail(d, CFGBIN_E_TAG, NO_FIELD);
    }
}

struct defaults_ctx {
    struct decoder *d;
    unsigned preset;
};

static void default_one(void *ctx, unsigned page, unsigned id, unsigned kind, bool nav)
{
    struct defaults_ctx *c = ctx;
    (void)nav;
    if (bit(c->d->shift[c->preset], slot(page, id)))
        return;
    struct core_control *ctl = control(&c->d->cfg->preset[c->preset], page, id);
    default_shift(kind, &ctl->normal, &ctl->shift_kind, &ctl->shift);
}

/* The records of data[off..len), one by one. */
static int records(struct decoder *d, const uint8_t *data, size_t off, size_t len)
{
    while (off < len) {
        d->e.offset = (uint32_t)off;
        d->e.page = d->e.control = 255;
        if (len - off < 2 || len - off - 2 < data[off + 1])
            return fail(d, CFGBIN_E_RECORD, NO_FIELD);
        int r = record_at(d, data[off], data + off + 2, data[off + 1]);
        if (r)
            return r;
        off += 2u + data[off + 1];
    }
    return CFGBIN_OK;
}

static int decode(struct decoder *d, const uint8_t *data, size_t len, struct cfgbin_meta *meta)
{
    if (len < CFGBIN_HEADER)
        return fail(d, CFGBIN_E_SHORT, NO_FIELD);
    if (memcmp(data, CFG_MAGIC, 4))
        return fail(d, CFGBIN_E_MAGIC, NO_FIELD);
    if (data[4] != CFG_BINARY_VERSION)
        return fail(d, CFGBIN_E_VERSION, NO_FIELD);
    size_t head = data[5];
    /* len - head, not head + length: that would wrap a 32-bit size_t for a header past the end */
    if (head < CFGBIN_HEADER || head > len || get32(data + 8) != len - head)
        return fail(d, CFGBIN_E_LENGTH, NO_FIELD);
    if (cfgbin_crc32(0, data + 16, len - 16) != get32(data + 12))
        return fail(d, CFGBIN_E_CRC, NO_FIELD);
    if (meta) {
        meta->revision = get32(data + 16);
        memcpy(meta->id, data + 20, 16);
    }
    if (d->cfg) {
        memset(d->cfg, 0, sizeof *d->cfg);
        for (unsigned i = 0; i < CFG_GLOBAL_FIELDS; i++)
            put(&d->cfg->global, &cfg_global_field[i], cfg_global_field[i].def);
        for (unsigned p = 0; p < CORE_PRESETS; p++)
            for (unsigned i = 0; i < CFG_PRESET_FIELDS; i++)
                put(&d->cfg->preset[p], &cfg_preset_field[i], cfg_preset_field[i].def);
    }
    int r = records(d, data, head, len);
    if (r)
        return r;
    d->e.offset = 0;
    d->e.preset = 255;
    if (d->cfg) {
        for (unsigned p = 0; p < CORE_PRESETS; p++) {
            struct defaults_ctx c = {d, p};
            each_control(default_one, &c);
        }
        r = cfgbin_check(d->cfg, &d->e); /* the records were checked one by one: never fails */
        if (r)
            return r;
    }
    return CFGBIN_OK;
}

int cfgbin_decode(const uint8_t *data, size_t len, struct core_config *cfg, struct cfgbin_meta *meta,
                  struct cfgbin_error *err)
{
    struct decoder d;
    memset(&d, 0, sizeof d);
    d.cfg = cfg;
    d.preset = -1;
    d.last = -1;
    error_init(&d.e);
    int r = decode(&d, data, len, meta);
    if (r) {
        LOG_WARN(cfg, "configuration refused: %s at byte %u (preset %u, page %u, control %u, field %u)",
                 cfgbin_error_name((unsigned)r), d.e.offset, d.e.preset, d.e.page, d.e.control, d.e.field);
    } else {
        LOG_DEBUG(cfg, "configuration of %u bytes %s", (unsigned)len, cfg ? "loaded" : "checked");
    }
    if (err)
        *err = d.e;
    return r;
}

/* A patch: a control it named is replaced whole, as in a complete form: no Normal layer is none,
 * no Shift layer the default. */
static void patched_one(void *ctx, unsigned page, unsigned id, unsigned kind, bool nav)
{
    struct defaults_ctx *c = ctx;
    unsigned s = slot(page, id);
    bool normal = bit(c->d->normal[c->preset], s), shift = bit(c->d->shift[c->preset], s);
    (void)nav;
    if (!normal && !shift)
        return;
    struct core_control *ctl = control(&c->d->cfg->preset[c->preset], page, id);
    if (!normal)
        ctl->normal = no_assign;
    if (!shift)
        default_shift(kind, &ctl->normal, &ctl->shift_kind, &ctl->shift);
}

int cfgbin_patch(const uint8_t *data, size_t len, unsigned preset, struct core_config *cfg,
                 cfgbin_changing_fn changing, void *ctx, struct cfgbin_error *err)
{
    struct decoder d;
    memset(&d, 0, sizeof d);
    d.cfg = cfg;
    d.patch = true;
    d.changing = changing;
    d.ctx = ctx;
    d.last = -1;
    error_init(&d.e);
    d.e.preset = (uint8_t)preset;
    int r;
    if (preset >= CORE_PRESETS) {
        r = fail(&d, CFGBIN_E_WHERE, NO_FIELD);
    } else {
        d.preset = (int)preset;
        r = records(&d, data, 0, len);
    }
    if (!r && cfg) {
        struct defaults_ctx c = {&d, preset};
        each_control(patched_one, &c);
    }
    if (r) {
        LOG_WARN(cfg, "edit refused: %s at byte %u (preset %u, page %u, control %u, field %u)",
                 cfgbin_error_name((unsigned)r), d.e.offset, d.e.preset, d.e.page, d.e.control, d.e.field);
    } else {
        LOG_DEBUG(cfg, "edit of %u bytes for preset %u %s", (unsigned)len, preset, cfg ? "applied" : "checked");
    }
    if (err)
        *err = d.e;
    return r;
}

int cfgbin_patch_visit(const uint8_t *data, size_t len, unsigned preset, const struct core_config *cfg,
                       cfgbin_changing_fn visit, void *ctx, struct cfgbin_error *err)
{
    int r = cfgbin_patch(data, len, preset, NULL, NULL, NULL, err);
    if (r)
        return r;
    for (size_t off = 0; off < len; off += 2u + data[off + 1]) {
        unsigned tag = data[off];
        if (tag == CFG_TAG_PRESET)
            visit(ctx, NULL);
        else if (tag == CFG_TAG_ASSIGN || tag == CFG_TAG_SHIFT)
            visit(ctx, control((struct core_preset *)&cfg->preset[preset], data[off + 2], data[off + 3]));
    }
    return CFGBIN_OK;
}

const char *cfgbin_error_name(unsigned code)
{
    static const char *const names[CFGBIN_CODES] = {
        "ok",    "short", "magic", "version", "length", "crc",   "record", "tag",
        "twice", "order", "where", "field",   "type",   "value", "shift",  "space",
    };
    return code < CFGBIN_CODES ? names[code] : "?";
}

uint32_t cfgbin_crc32(uint32_t crc, const uint8_t *p, size_t n)
{
    static const uint32_t nibble[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u,
        0x4DB26158u, 0x5005713Cu, 0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
    };
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        crc = crc >> 4 ^ nibble[crc & 15u];
        crc = crc >> 4 ^ nibble[crc & 15u];
    }
    return ~crc;
}
