/* The configuration protocol over the command link (cfglink.h, docs/link.md, "Configuration"). */
#include "cfglink.h"

#include <string.h>

#include "cfg_schema.h"
#include "link.h"
#include "smclog.h"

/* CFG_INFO's reply payload */
#define INFO_LEN 37

_Static_assert(4 + 1 + 4 + CFGLINK_CHUNK <= LINK_REPLY_MAX, "GET_CHUNK reply");
_Static_assert(3 + 4 + CFGLINK_CHUNK <= LINK_CMD_MAX, "PUT_CHUNK command");
_Static_assert(4 + INFO_LEN <= LINK_REPLY_MAX, "CFG_INFO reply");
_Static_assert(CFGLINK_CHUNK <= 255, "GET_CHUNK's count is a byte");

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

void cfglink_init(struct cfglink *c, const struct cfglink_ops *ops, uint8_t *buf, uint32_t cap,
                  struct cfgstore *store)
{
    memset(c, 0, sizeof *c);
    c->ops = ops;
    c->buf = buf;
    c->cap = cap;
    c->store = store;
    c->store_status = store ? CFGLINK_STORE_CLEAN : CFGLINK_STORE_NONE;
}

/* cfgstore_load()'s accept: a form this firmware takes */
static bool takes(void *ctx, const uint8_t *blob, uint32_t len)
{
    (void)ctx;
    return cfgbin_decode(blob, len, NULL, NULL, NULL) == CFGBIN_OK;
}

bool cfglink_load(struct cfglink *c, struct core_config *cfg, struct cfgbin_meta *meta)
{
    struct cfgbin_error err;
    uint32_t gen = 0;

    memset(meta, 0, sizeof *meta);
    if (c->store) {
        uint32_t n = cfgstore_load(c->store, c->buf, c->cap, &gen, takes, NULL);
        if (n) {
            int r = cfgbin_decode(c->buf, n, cfg, meta, &err);
            if (r == CFGBIN_OK) {
                LOG_INFO(cfg, "saved configuration loaded: generation %u, %u bytes, revision %u", gen, n,
                         meta->revision);
                return true;
            }
            /* takes() said yes: a bug */
            LOG_ERROR(cfg, "saved configuration refused when decoded: %s", cfgbin_error_name((unsigned)r));
            memset(meta, 0, sizeof *meta);
        } else {
            LOG_INFO(cfg, "no saved configuration: the factory one");
        }
    }
    core_config_factory(cfg);
    return false;
}

/* After a commit: the control task has taken the configuration once nothing waits any more. */
static void settle(struct cfglink *c)
{
    if (c->state == CFGLINK_APPLYING && !c->ops->pending(c->ops->ctx)) {
        c->state = CFGLINK_IDLE;
        LOG_DEBUG(cfg, "the received configuration has been taken");
    }
}

/* cfgbin's error after the status (9 bytes) */
static size_t put_error(uint8_t *o, const struct cfgbin_error *e)
{
    o[0] = e->code;
    o[1] = e->preset;
    o[2] = e->page;
    o[3] = e->control;
    o[4] = e->field;
    put32(o + 5, e->offset);
    return 9;
}

/* What an unsaved configuration waits for: the quiet pause, or SAVE. */
static uint8_t unsaved_status(const struct cfglink *c)
{
    return !c->ops->autosave || c->ops->autosave(c->ops->ctx) ? CFGLINK_STORE_WAITING : CFGLINK_STORE_UNSAVED;
}

/* The running configuration changed (a commit, an edit): it is not the saved one. */
static void mark_dirty(struct cfglink *c)
{
    if (!c->store)
        return;
    c->dirty = true;
    c->save_tries = 0;
    c->store_status = unsaved_status(c);
}

/* A BEGIN drops the transfer under way. */
static void drop(struct cfglink *c, const char *why)
{
    if (c->state == CFGLINK_PUT)
        LOG_WARN(cfg, "upload dropped (%s) at %u of %u bytes", why, c->got, c->len);
    c->state = CFGLINK_IDLE;
}

/* A BEGIN while saving: the save starts over after the next quiet pause. */
static void cancel_save(struct cfglink *c)
{
    if (c->state != CFGLINK_SAVING)
        return;
    cfgstore_cancel(c->store);
    c->state = CFGLINK_IDLE;
    c->save_now = false;
    c->store_status = unsaved_status(c);
    LOG_INFO(cfg, "save cancelled by a change");
}

static size_t info(struct cfglink *c, uint8_t *o)
{
    struct cfgbin_meta meta;
    uint8_t source = 0;
    bool pending = c->ops->pending(c->ops->ctx);

    memset(&meta, 0, sizeof meta);
    c->ops->active(c->ops->ctx, &source, &meta);
    o[1] = CFGLINK_VERSION;
    o[2] = CFG_BINARY_VERSION;
    o[3] = (uint8_t)CFG_SCHEMA_VERSION;
    o[4] = (uint8_t)(CFG_SCHEMA_VERSION >> 8);
    put32(o + 5, c->cap);
    o[9] = CFGLINK_CHUNK;
    o[10] = pending ? CFGLINK_APPLYING : c->state;
    o[11] = source;
    put32(o + 12, meta.revision);
    memcpy(o + 16, meta.id, 16);
    /* the setting may have changed since: the status the configuration in force says */
    o[32] = c->dirty && c->state != CFGLINK_SAVING ? unsaved_status(c) : c->store_status;
    put32(o + 33, c->store ? cfgstore_generation(c->store) : 0);
    return INFO_LEN;
}

_Static_assert(CFGLINK_STATE_LEN <= LINK_EVENT_STATE_MAX, "an EVENT carries the state");
_Static_assert(CFGLINK_SENT_LEN == LINK_SENT_ENTRY && CFGLINK_SENT_MAX == LINK_SENT_ENTRIES,
               "a SENT record carries a CFG_SENT reply's entries");
_Static_assert(1u + 5u + CFGLINK_SENT_MAX * CFGLINK_SENT_LEN <= LINK_REPLY_MAX - 4u, "CFG_SENT's reply fits");
_Static_assert(CFG_ROW_PAGE == 255, "the bottom row's page in the state (ctl_panel_bytes())");

/* CFG_STATE: the panel's state after the status (docs/link.md, "The panel's state"). */
static size_t state(struct cfglink *c, uint8_t *o)
{
    struct cfgbin_meta meta;
    uint8_t source = 0;

    if (!c->ops->panel) {
        o[0] = CFGLINK_UNAVAILABLE;
        return 1;
    }
    if (c->ops->pending(c->ops->ctx)) {
        o[0] = CFGLINK_BUSY; /* no coherent identity yet; do not emit a transient zero identity */
        return 1;
    }
    memset(&meta, 0, sizeof meta);
    c->ops->panel(c->ops->ctx, o + 1);
    c->ops->active(c->ops->ctx, &source, &meta);
    uint8_t *q = o + 1 + CFGLINK_PANEL_BYTES;
    q[0] = c->dirty && c->state != CFGLINK_SAVING ? unsaved_status(c) : c->store_status;
    put32(q + 1, meta.revision);
    memcpy(q + 5, meta.id, 16);
    return 1 + CFGLINK_STATE_LEN;
}

/* CFG_SELECT: preset and page (0xFF keeps one), switched by the control task in its next round. */
static size_t select_bank(struct cfglink *c, const uint8_t *p, uint8_t *o, uint8_t *link_status)
{
    if (!c->ops->select) {
        o[0] = CFGLINK_UNAVAILABLE;
        return 1;
    }
    int preset = p[0] == 0xFF ? -1 : p[0], page = p[1] == 0xFF ? -1 : p[1];
    if (!c->ops->select(c->ops->ctx, preset, page)) {
        *link_status = LINK_BAD_ARG;
        return 0;
    }
    c->stats.selects++;
    LOG_DEBUG(cfg, "select: preset %u page %u (255: keep)", p[0], p[1]);
    return 1;
}

/* CFG_SENT: what the mixer sent from a number on (docs/link.md, "What the mixer sent"). */
static size_t sent(struct cfglink *c, const uint8_t *p, uint8_t *o)
{
    uint32_t first = 0;

    if (!c->ops->sent) {
        o[0] = CFGLINK_UNAVAILABLE;
        return 1;
    }
    uint32_t from = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    unsigned n = c->ops->sent(c->ops->ctx, from, (uint8_t(*)[CFGLINK_SENT_LEN])(o + 6), CFGLINK_SENT_MAX,
                              &first);
    put32(o + 1, first);
    o[5] = (uint8_t)n;
    return 6u + n * CFGLINK_SENT_LEN;
}

static size_t get_begin(struct cfglink *c, uint8_t *o)
{
    struct cfgbin_error err;

    if (c->state == CFGLINK_APPLYING || c->ops->pending(c->ops->ctx)) {
        o[0] = CFGLINK_BUSY; /* the configuration may be rewritten: the transfer under way stays */
        return 1;
    }
    cancel_save(c);
    drop(c, "a download begins");
    memset(&err, 0, sizeof err);
    size_t n = c->ops->get(c->ops->ctx, c->buf, c->cap, &err);
    if (!n) {
        if (err.code == CFGBIN_OK) {
            o[0] = CFGLINK_BUSY;
            return 1;
        }
        LOG_ERROR(cfg, "the running configuration does not encode: %s", cfgbin_error_name(err.code));
        o[0] = CFGLINK_INVALID;
        return 1 + put_error(o + 1, &err);
    }
    c->state = CFGLINK_GET;
    c->len = (uint32_t)n;
    c->crc = cfgbin_crc32(0, c->buf, n);
    c->stats.gets++;
    LOG_INFO(cfg, "download: %u bytes, CRC-32 %x", c->len, c->crc);
    put32(o + 1, c->len);
    put32(o + 5, c->crc);
    return 9;
}

static size_t get_chunk(struct cfglink *c, const uint8_t *p, uint8_t *o)
{
    uint32_t at = get32(p);
    unsigned count = p[4] < CFGLINK_CHUNK ? p[4] : CFGLINK_CHUNK;

    put32(o + 1, at);
    if (c->state != CFGLINK_GET) {
        o[0] = CFGLINK_NO_TRANSFER;
        return 5;
    }
    if (at > c->len) {
        o[0] = CFGLINK_BAD_OFFSET;
        return 5;
    }
    if (count > c->len - at)
        count = c->len - at;
    memcpy(o + 5, c->buf + at, count);
    return 5 + count;
}

static size_t put_begin(struct cfglink *c, const uint8_t *p, uint8_t *o)
{
    uint32_t n = get32(p);

    if (c->state == CFGLINK_APPLYING) {
        o[0] = CFGLINK_BUSY;
        return 1;
    }
    cancel_save(c);
    drop(c, "a new upload begins");
    if (n < CFGBIN_HEADER || n > c->cap) {
        LOG_WARN(cfg, "upload of %u bytes refused: %u to %u taken", n, CFGBIN_HEADER, c->cap);
        o[0] = CFGLINK_SIZE;
        return 1;
    }
    c->state = CFGLINK_PUT;
    c->len = n;
    c->crc = get32(p + 4);
    c->got = 0;
    c->stats.puts++;
    LOG_INFO(cfg, "upload: %u bytes announced", n);
    return 1;
}

static size_t put_chunk(struct cfglink *c, const uint8_t *p, size_t n, uint8_t *o)
{
    uint32_t at = get32(p);

    p += 4;
    n -= 4;
    if (c->state != CFGLINK_PUT) {
        o[0] = CFGLINK_NO_TRANSFER;
    } else if (at != c->got) {
        o[0] = CFGLINK_BAD_OFFSET; /* the host goes on from where the reply says */
    } else if (n > c->len - c->got) {
        o[0] = CFGLINK_SIZE;
    } else {
        memcpy(c->buf + c->got, p, n);
        c->got += (uint32_t)n;
    }
    put32(o + 1, c->state == CFGLINK_PUT ? c->got : 0);
    return 5;
}

static size_t put_commit(struct cfglink *c, uint8_t *o)
{
    struct cfgbin_error err;
    struct cfgbin_meta meta;

    if (c->state != CFGLINK_PUT) {
        o[0] = c->state == CFGLINK_APPLYING ? CFGLINK_BUSY : CFGLINK_NO_TRANSFER;
        return 1;
    }
    if (c->got != c->len) {
        LOG_WARN(cfg, "commit refused: %u of %u bytes received", c->got, c->len);
        c->stats.refused++;
        o[0] = CFGLINK_INCOMPLETE;
        return 1;
    }
    uint32_t crc = cfgbin_crc32(0, c->buf, c->len);
    if (crc != c->crc) {
        LOG_WARN(cfg, "commit refused: CRC-32 %x, %x announced", crc, c->crc);
        c->stats.refused++;
        c->state = CFGLINK_IDLE; /* the bytes are wrong: the host starts over */
        o[0] = CFGLINK_CRC;
        return 1;
    }
    int r = cfgbin_decode(c->buf, c->len, NULL, &meta, &err); /* logs where (module cfg) */
    if (r != CFGBIN_OK) {
        LOG_WARN(cfg, "commit refused: the configuration is not valid (%s)", cfgbin_error_name((unsigned)r));
        c->stats.refused++;
        c->state = CFGLINK_IDLE;
        o[0] = CFGLINK_INVALID;
        return 1 + put_error(o + 1, &err);
    }
    if (!c->ops->apply(c->ops->ctx, c->buf, c->len)) {
        o[0] = CFGLINK_BUSY; /* a setup waits: the upload stays, commit again */
        return 1;
    }
    c->state = CFGLINK_APPLYING;
    c->stats.commits++;
    mark_dirty(c);
    LOG_INFO(cfg, "upload complete: %u bytes, revision %u; applied in the next round", c->len,
             meta.revision);
    return 1;
}

/* CFG_SET: a live edit, checked here, applied by the control task. */
static size_t set(struct cfglink *c, const uint8_t *p, size_t n, uint8_t *o)
{
    struct cfgbin_error err;
    uint32_t revision = get32(p);
    unsigned preset = p[4];

    if (!c->ops->edit) {
        o[0] = CFGLINK_UNAVAILABLE;
        return 1;
    }
    if (c->state == CFGLINK_APPLYING || c->ops->pending(c->ops->ctx)) {
        o[0] = CFGLINK_BUSY;
        return 1;
    }
    if (cfgbin_patch(p + 5, n - 5, preset, NULL, NULL, NULL, &err) != CFGBIN_OK) { /* logs where */
        c->stats.refused++;
        o[0] = CFGLINK_INVALID;
        return 1 + put_error(o + 1, &err);
    }
    if (!c->ops->edit(c->ops->ctx, preset, p + 5, (uint32_t)(n - 5), revision)) {
        o[0] = CFGLINK_BUSY;
        return 1;
    }
    cancel_save(c);
    c->stats.edits++;
    mark_dirty(c);
    LOG_DEBUG(cfg, "edit: preset %u, %u bytes, revision %u", preset + 1u, (unsigned)(n - 5), revision); /* legacy request, not running */
    return 1;
}

/* CFG_SAVE: the running configuration saved now (by the next poll), whatever the setting. */
static size_t save(struct cfglink *c, uint8_t *o)
{
    struct cfgbin_meta meta;
    uint8_t source = 0;

    if (c->state == CFGLINK_APPLYING || c->ops->pending(c->ops->ctx)) {
        o[0] = CFGLINK_BUSY; /* a change waits: what is to be saved is not known yet */
        return 1;
    }
    c->ops->active(c->ops->ctx, &source, &meta);
    if (!c->store || (source != CFGLINK_SOURCE_RECEIVED && source != CFGLINK_SOURCE_FACTORY &&
                      source != CFGLINK_SOURCE_STORED)) {
        o[0] = CFGLINK_UNAVAILABLE; /* no flash, or a setup's configuration: never saved */
        return 1;
    }
    if (source != CFGLINK_SOURCE_RECEIVED)
        return 1; /* what the mixer starts with already: an edit would have made it received */
    if (c->state == CFGLINK_SAVING || (!c->dirty && c->store_status != CFGLINK_STORE_FAILED))
        return 1; /* under way, or saved already */
    c->dirty = true;
    c->save_tries = 0;
    c->save_now = true;
    LOG_INFO(cfg, "save asked for");
    return 1;
}

/* CFG_REVERT: what the mixer starts with (the newest saved configuration, or the factory one)
 * replaces the running one, as an upload would. */
static size_t revert(struct cfglink *c, uint8_t *o)
{
    uint32_t gen = 0, n = 0;

    if (!c->ops->revert) {
        o[0] = CFGLINK_UNAVAILABLE;
        return 1;
    }
    if (c->state == CFGLINK_APPLYING || c->ops->pending(c->ops->ctx)) {
        o[0] = CFGLINK_BUSY;
        return 1;
    }
    cancel_save(c);
    drop(c, "a revert");
    c->state = CFGLINK_IDLE;
    if (c->store)
        n = cfgstore_load(c->store, c->buf, c->cap, &gen, takes, NULL);
    if (!c->ops->revert(c->ops->ctx, n ? c->buf : NULL, n)) {
        o[0] = CFGLINK_BUSY;
        return 1;
    }
    c->state = CFGLINK_APPLYING; /* the buffer holds it until the control task has taken it */
    c->dirty = false;
    c->save_now = false;
    c->save_tries = 0;
    c->store_status = c->store ? CFGLINK_STORE_CLEAN : CFGLINK_STORE_NONE;
    c->stats.reverts++;
    if (n)
        LOG_INFO(cfg, "revert: the saved configuration, generation %u, %u bytes", gen, n);
    else
        LOG_INFO(cfg, "revert: nothing saved, the factory configuration");
    return 1;
}

uint8_t cfglink_command(void *cfg, uint8_t cmd, const uint8_t *p, size_t n, uint8_t *out, size_t *len)
{
    struct cfglink *c = cfg;
    size_t k;

    settle(c);
    if (cmd != LINK_CMD_CFG_INFO && cmd != LINK_CMD_CFG_ABORT && cmd != LINK_CMD_CFG_SAVE &&
        cmd != LINK_CMD_CFG_STATE && cmd != LINK_CMD_CFG_SELECT && cmd != LINK_CMD_CFG_SENT)
        c->heard = true; /* a change: the autosave waits for it to go quiet (a status poll does not) */
    out[0] = CFGLINK_OK;
    switch (cmd) {
    case LINK_CMD_CFG_INFO:
        if (n)
            return LINK_BAD_LENGTH;
        k = info(c, out);
        break;
    case LINK_CMD_CFG_GET_BEGIN:
        if (n)
            return LINK_BAD_LENGTH;
        k = get_begin(c, out);
        break;
    case LINK_CMD_CFG_GET_CHUNK:
        if (n != 5)
            return LINK_BAD_LENGTH;
        k = get_chunk(c, p, out);
        break;
    case LINK_CMD_CFG_PUT_BEGIN:
        if (n != 8)
            return LINK_BAD_LENGTH;
        k = put_begin(c, p, out);
        break;
    case LINK_CMD_CFG_PUT_CHUNK:
        if (n < 5 || n > 4 + CFGLINK_CHUNK)
            return LINK_BAD_LENGTH;
        k = put_chunk(c, p, n, out);
        break;
    case LINK_CMD_CFG_PUT_COMMIT:
        if (n)
            return LINK_BAD_LENGTH;
        k = put_commit(c, out);
        break;
    case LINK_CMD_CFG_ABORT:
        if (n)
            return LINK_BAD_LENGTH;
        if (c->state == CFGLINK_APPLYING) {
            out[0] = CFGLINK_BUSY;
        } else if (c->state != CFGLINK_SAVING) { /* nothing to abort: the save goes on */
            if (c->state == CFGLINK_PUT)
                LOG_INFO(cfg, "upload aborted at %u of %u bytes", c->got, c->len);
            c->state = CFGLINK_IDLE;
        }
        k = 1;
        break;
    case LINK_CMD_CFG_SET:
        if (n < 7) /* revision, preset, a record at least */
            return LINK_BAD_LENGTH;
        k = set(c, p, n, out);
        break;
    case LINK_CMD_CFG_SAVE:
        if (n)
            return LINK_BAD_LENGTH;
        k = save(c, out);
        break;
    case LINK_CMD_CFG_REVERT:
        if (n)
            return LINK_BAD_LENGTH;
        k = revert(c, out);
        break;
    case LINK_CMD_CFG_STATE:
        if (n)
            return LINK_BAD_LENGTH;
        k = state(c, out);
        break;
    case LINK_CMD_CFG_SENT:
        if (n != 4)
            return LINK_BAD_LENGTH;
        k = sent(c, p, out);
        break;
    case LINK_CMD_CFG_SELECT: {
        uint8_t st = LINK_OK;
        if (n != 2)
            return LINK_BAD_LENGTH;
        k = select_bank(c, p, out, &st);
        if (st != LINK_OK)
            return st;
        break;
    }
    default:
        return LINK_UNKNOWN_CMD;
    }
    if (out[0] != CFGLINK_OK)
        LOG_DEBUG(cfg, "command %02x: configuration status %u", cmd, out[0]);
    *len += k;
    return LINK_OK;
}

/* The save under way: one step, unless a control was touched within the quiet time. */
static void save_step(struct cfglink *c, uint32_t now, bool panel_quiet)
{
    if (!panel_quiet)
        return;
    int r = cfgstore_step(c->store);
    if (r == CFGSTORE_MORE)
        return;
    c->state = CFGLINK_IDLE;
    if (r == CFGSTORE_DONE) {
        c->dirty = false;
        c->save_tries = 0;
        c->store_status = CFGLINK_STORE_CLEAN;
        c->stats.saved++;
        return;
    }
    c->stats.save_failed++;
    if (++c->save_tries >= CFGLINK_SAVE_TRIES) {
        c->dirty = false;
        c->store_status = CFGLINK_STORE_FAILED;
        LOG_ERROR(cfg, "the configuration is not saved: %u saves failed; again after the next upload",
                  c->save_tries);
    } else {
        c->store_status = unsaved_status(c);
        c->save_now = c->manual; /* SAVE's tries go on at once; the autosave's after another pause */
        c->quiet_from = now;
    }
}

/* A save begins: the running configuration encoded into the buffer. */
static void save_begin(struct cfglink *c)
{
    struct cfgbin_error err;
    struct cfgbin_meta meta;
    uint8_t source = 0;

    c->ops->active(c->ops->ctx, &source, &meta);
    if (c->state == CFGLINK_GET)
        LOG_DEBUG(cfg, "download snapshot dropped for the save");
    c->state = CFGLINK_IDLE;
    memset(&err, 0, sizeof err);
    size_t n = c->ops->get(c->ops->ctx, c->buf, c->cap, &err);
    if (!n) {
        if (err.code == CFGBIN_OK)
            return; /* a change waits: later */
        LOG_ERROR(cfg, "not saved: the running configuration does not encode: %s", cfgbin_error_name(err.code));
        c->dirty = false;
        c->store_status = CFGLINK_STORE_FAILED;
        return;
    }
    if (!cfgstore_begin(c->store, c->buf, (uint32_t)n)) {
        LOG_ERROR(cfg, "not saved: %u bytes do not fit a slot", (unsigned)n);
        c->dirty = false;
        c->store_status = CFGLINK_STORE_FAILED;
        return;
    }
    c->state = CFGLINK_SAVING;
    c->store_status = CFGLINK_STORE_WRITING;
    c->manual = c->save_now;
    c->save_now = false;
    LOG_INFO(cfg, "saving the configuration: %u bytes, revision %u, generation %u", (unsigned)n,
             meta.revision, c->store->generation + 1);
}

void cfglink_set_state(struct cfglink *c, struct statestore *s)
{
    c->state_store = s && c->ops->state ? s : NULL;
    if (c->state_store)
        c->state_saved = c->ops->state(c->ops->ctx);
}

/* The panel's state, when it changed and the panel is quiet: one record (statestore.h). */
static void state_save(struct cfglink *c)
{
    uint32_t w = c->ops->state(c->ops->ctx);
    if (w == c->state_saved)
        return;
    if (w != c->state_tried) {
        c->state_tried = w;
        c->state_tries = 0;
    }
    if (c->state_tries >= CFGLINK_SAVE_TRIES)
        return; /* given up on this one; the next change tries again */
    if (statestore_save(c->state_store, w)) {
        c->state_saved = w;
        c->state_tries = 0;
        c->stats.state_saved++;
    } else {
        c->state_tries++;
        c->stats.state_failed++;
    }
}

void cfglink_poll(void *cfg, uint32_t now_us)
{
    struct cfglink *c = cfg;

    settle(c);
    if (!c->store)
        return;
    if (c->heard) {
        c->heard = false;
        c->quiet_from = now_us;
    }
    uint32_t touched = c->ops->touched_at ? c->ops->touched_at(c->ops->ctx) : 0;
    bool panel_quiet = touched == 0 || now_us - touched >= CFGLINK_QUIET_US;
    if (c->state == CFGLINK_SAVING) {
        save_step(c, now_us, panel_quiet);
        return;
    }
    if (c->state_store && panel_quiet && !c->ops->pending(c->ops->ctx))
        state_save(c);
    if (!c->dirty || (c->state != CFGLINK_IDLE && c->state != CFGLINK_GET) || c->ops->pending(c->ops->ctx))
        return;
    struct cfgbin_meta meta;
    uint8_t source = 0;
    c->ops->active(c->ops->ctx, &source, &meta);
    if (source != CFGLINK_SOURCE_RECEIVED) {
        /* a setup replaced the received configuration before it was saved */
        LOG_INFO(cfg, "not saved: the running configuration is no longer the received one (source %u)", source);
        c->dirty = c->save_now = false;
        c->store_status = CFGLINK_STORE_CLEAN;
        return;
    }
    c->store_status = unsaved_status(c); /* the setting may have changed */
    if (!c->save_now && (c->store_status != CFGLINK_STORE_WAITING || !panel_quiet ||
                         now_us - c->quiet_from < CFGLINK_QUIET_US))
        return;
    save_begin(c);
}
