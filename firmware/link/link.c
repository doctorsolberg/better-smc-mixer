/* The command link: frames in, commands, replies and the log stream out (link.h, docs/link.md).
 * The CDC's COBS frames or SysEx (link_init_sysex). */
#include "link.h"

#include <string.h>

#include "smclog.h"
#include "smclog_port.h"

#define REPLY_HDR 4 /* kind, seq, cmd, status */

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

void link_init(struct link *l, const struct link_ops *ops)
{
    memset(l, 0, sizeof *l);
    l->ops = ops;
}

void link_init_sysex(struct link *l, const struct link_ops *ops)
{
    link_init(l, ops);
    l->sysex = true;
}

size_t link_pack7(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t o = 0;

    for (size_t i = 0; i < n; i += 7) {
        size_t k = n - i < 7 ? n - i : 7;
        uint8_t top = 0;
        for (size_t j = 0; j < k; j++)
            top |= (uint8_t)((in[i + j] >> 7) << j);
        out[o++] = top;
        for (size_t j = 0; j < k; j++)
            out[o++] = in[i + j] & 0x7Fu;
    }
    return o;
}

int link_unpack7(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    size_t o = 0;

    for (size_t i = 0; i < n; i += 8) {
        size_t k = n - i < 8 ? n - i : 8;
        uint8_t top = in[i];
        if (k < 2 || (top & 0x80u))
            return -1;
        for (size_t j = 1; j < k; j++) {
            if ((in[i + j] & 0x80u) || o == cap)
                return -1;
            out[o++] = (uint8_t)(in[i + j] | (((top >> (j - 1)) & 1u) << 7));
        }
    }
    return (int)o;
}

/* A record of n bytes (with room for 2 more: its CRC goes there) as a whole SysEx into out. */
static size_t sysex_frame(uint8_t *rec, size_t n, uint8_t *out, size_t cap)
{
    uint16_t crc = smclog_crc16(rec, n);
    size_t o;

    if (cap < LINK_SYSEX_LEN(n))
        return 0;
    rec[n] = (uint8_t)crc;
    rec[n + 1] = (uint8_t)(crc >> 8);
    out[0] = 0xF0;
    out[1] = LINK_SYSEX_MFR;
    out[2] = LINK_SYSEX_TAG0;
    out[3] = LINK_SYSEX_TAG1;
    o = 1 + LINK_SYSEX_HDR + link_pack7(rec, n + 2, out + 1 + LINK_SYSEX_HDR);
    out[o++] = 0xF7;
    return o;
}

void link_reset(struct link *l)
{
    if (l->subscribed)
        LOG_INFO(link, "link reset: log stream off");
    l->rx_len = 0;
    l->rx_skip = false;
    l->rx_in = false;
    l->last_valid = false;
    l->reply_pending = false;
    l->subscribed = false;
    l->header_pending = false;
    if (l->events || l->sent_on)
        LOG_INFO(link, "link reset: events off");
    l->events = false;
    l->ev_len = 0;
    l->sent_on = false;
    l->sx_len = 0;
}

/* COBS-decodes in[0..n) into out; -1 when it is not valid COBS or does not fit. */
static int cobs_decode(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    size_t i = 0, o = 0;

    while (i < n) {
        uint8_t code = in[i++];
        if (code == 0 || i + code - 1u > n)
            return -1;
        for (unsigned k = 1; k < code; k++) {
            if (o == cap)
                return -1;
            out[o++] = in[i++];
        }
        if (code < 0xFF && i < n) {
            if (o == cap)
                return -1;
            out[o++] = 0;
        }
    }
    return (int)o;
}

static uint32_t caps(const struct link *l)
{
    const struct link_ops *ops = l->ops;

    return (l->sysex ? 0 : LINK_CAP_LOG) | (ops->raw_input ? LINK_CAP_RAW_INPUT : 0) |
           (ops->inject ? LINK_CAP_INJECT : 0) | (ops->metrics ? LINK_CAP_METRICS : 0) |
           (ops->setup ? LINK_CAP_SETUP : 0) | (ops->boot_log ? LINK_CAP_BOOT_LOG : 0) |
           (ops->reset ? LINK_CAP_RESET : 0) | (ops->config ? LINK_CAP_CONFIG : 0) |
           (ops->config ? LINK_CAP_EVENTS | LINK_CAP_SENT : 0) | (ops->stock ? LINK_CAP_STOCK : 0);
}

/* The panel's state (CFG_STATE through the configuration protocol) into out + 2, after room for an
 * EVENT's kind and count; its length, 0 when there is none. */
static size_t panel_state(struct link *l, uint8_t *out)
{
    const struct link_ops *ops = l->ops;
    size_t len = 0;

    if (!ops->config || ops->config(ops->cfg, LINK_CMD_CFG_STATE, NULL, 0, out + 1, &len) != LINK_OK ||
        len < 2 || out[1] != 0 /* CFGLINK_OK */ || len - 1 > LINK_EVENT_STATE_MAX)
        return 0;
    return len - 1;
}

/* EVENTS on: the next EVENT, framed into l->ev, when the state differs from the last one sent and
 * the gap has passed (the first one at once). */
static void event_due(struct link *l)
{
    uint8_t *rec = l->out; /* free outside command() */
    uint32_t now = smclog_port_time_us();

    if (l->ev_sent && now - l->ev_at < LINK_EVENT_GAP_US)
        return;
    size_t n = panel_state(l, rec);
    if (!n || (l->ev_sent && n == l->ev_state_len && !memcmp(rec + 2, l->ev_state, n)))
        return;
    memcpy(l->ev_state, rec + 2, n);
    l->ev_state_len = (uint8_t)n;
    rec[0] = LINK_KIND_EVENT;
    rec[1] = l->ev_count++;
    l->ev_len = (uint8_t)(l->sysex ? sysex_frame(rec, n + 2, l->ev, sizeof l->ev)
                                   : smclog_frame(rec, n + 2, l->ev, sizeof l->ev));
    l->ev_sent = true;
    l->ev_at = now;
}

/* CFG_SENT through the configuration protocol from l->sent_next, as a SENT record into out: kind,
 * first u32, lost u8, count u8, the entries. Its length; 0 when there is none (or no CFG_SENT). */
static size_t sent_record(struct link *l, uint8_t *out, uint32_t from)
{
    const struct link_ops *ops = l->ops;
    uint8_t p[4];
    size_t len = 0;

    put32(p, from);
    /* the reply at out + 1: status, first u32, count u8, entries */
    if (!ops->config || ops->config(ops->cfg, LINK_CMD_CFG_SENT, p, 4, out + 1, &len) != LINK_OK ||
        len < 6 || out[1] != 0 /* CFGLINK_OK */ || out[6] > LINK_SENT_ENTRIES ||
        len != 6u + out[6] * LINK_SENT_ENTRY)
        return 0;
    uint32_t first = get32(out + 2);
    uint32_t lost = first - from;
    memmove(out + 1, out + 2, 4);
    out[5] = (uint8_t)(lost > 255u ? 255u : lost);
    return len + 1u;
}

/* EVENTS' bit 1: the next SENT record, framed into l->sx, when entries wait and the gap has
 * passed. */
static void sent_due(struct link *l)
{
    uint8_t *rec = l->out; /* free outside command() */
    uint32_t now = smclog_port_time_us();

    if (l->sent_any && now - l->sent_at < LINK_SENT_GAP_US)
        return;
    size_t n = sent_record(l, rec, l->sent_next);
    if (!n || !rec[6])
        return;
    rec[0] = LINK_KIND_SENT;
    l->sent_next = get32(rec + 1) + rec[6];
    l->sx_len = (uint8_t)(l->sysex ? sysex_frame(rec, n, l->sx, sizeof l->sx)
                                   : smclog_frame(rec, n, l->sx, sizeof l->sx));
    l->sent_any = true;
    l->sent_at = now;
}

/* Runs one command; writes the reply's payload after out[REPLY_HDR] and its end into *len.
 * Returns the status. */
static uint8_t execute(struct link *l, uint8_t cmd, const uint8_t *p, size_t n, uint8_t *out,
                       size_t *len)
{
    const struct link_ops *ops = l->ops;
    uint8_t *o = out + REPLY_HDR;

    switch (cmd) {
    case LINK_CMD_PING:
        if (n > LINK_PING_MAX)
            return LINK_BAD_LENGTH;
        put32(o, smclog_port_time_us());
        memcpy(o + 4, p, n);
        *len += 4 + n;
        return LINK_OK;

    case LINK_CMD_GET_INFO: {
        size_t idlen = 0;
        const uint8_t *id = smclog_port_build_id(&idlen);
        if (n)
            return LINK_BAD_LENGTH;
        if (!id)
            idlen = 0;
        if (idlen > LINK_BUILD_ID_MAX)
            idlen = LINK_BUILD_ID_MAX;
        o[0] = LINK_VERSION;
        o[1] = SMCLOG_FORMAT_VERSION;
        o[2] = ops->variant;
        put32(o + 3, caps(l));
        put32(o + 7, SMCLOG_RING_SIZE);
        put32(o + 11, SMCLOG_TIMER_HZ);
        o[15] = (uint8_t)idlen;
        if (idlen)
            memcpy(o + 16, id, idlen);
        /* then the version's text (docs/link.md): its length (0: none), at most 16 bytes */
        size_t vlen = ops->version ? strlen(ops->version) : 0;
        if (vlen > LINK_VERSION_TEXT_MAX)
            vlen = LINK_VERSION_TEXT_MAX;
        o[16 + idlen] = (uint8_t)vlen;
        if (vlen)
            memcpy(o + 17 + idlen, ops->version, vlen);
        *len += 17 + idlen + vlen;
        return LINK_OK;
    }

    case LINK_CMD_LOG_SUBSCRIBE:
        if (l->sysex)
            return LINK_UNAVAILABLE; /* the log stream is the CDC's */
        if (n)
            return LINK_BAD_LENGTH;
        l->subscribed = true;
        l->header_pending = true; /* every stream starts with a header (LOG-12) */
        LOG_INFO(link, "log stream on");
        return LINK_OK;

    case LINK_CMD_LOG_UNSUBSCRIBE:
        if (n)
            return LINK_BAD_LENGTH;
        if (l->subscribed)
            LOG_INFO(link, "log stream off");
        l->subscribed = false;
        l->header_pending = false;
        return LINK_OK;

    case LINK_CMD_EVENTS: {
        if (n != 1)
            return LINK_BAD_LENGTH;
        uint8_t what = p[0];
        bool on = what & LINK_EVENTS_PANEL, sent = what & LINK_EVENTS_SENT;
        if (what & ~(LINK_EVENTS_PANEL | LINK_EVENTS_SENT))
            return LINK_BAD_ARG;
        /* l->cmd as scratch: the command is in it, but read already */
        if (on && !l->events && !panel_state(l, l->cmd))
            return LINK_UNAVAILABLE;
        if (sent && !l->sent_on) {
            if (!sent_record(l, l->cmd, 0xFFFFFFFFu)) /* CTL_SENT_NOW: the next number */
                return LINK_UNAVAILABLE;
            l->sent_next = get32(l->cmd + 1);
            l->sent_any = false;
        }
        if (on && !l->events) {
            l->ev_sent = false; /* the first EVENT at once: where the panel stands */
            l->ev_count = 0;
        }
        if (what && (on != l->events || sent != l->sent_on))
            LOG_INFO(link, "events on: panel %u, sent %u", on ? 1u : 0u, sent ? 1u : 0u);
        else if (!what && (l->events || l->sent_on))
            LOG_INFO(link, "events off");
        l->events = on;
        l->ev_len = 0;
        l->sent_on = sent;
        l->sx_len = 0;
        return LINK_OK;
    }

    case LINK_CMD_LOG_SET_LEVEL:
        if (n != 2)
            return LINK_BAD_LENGTH;
        if ((p[0] >= SMCLOG_MOD_COUNT && p[0] != LINK_ALL_MODULES) || p[1] > SMCLOG_TRACE)
            return LINK_BAD_ARG;
        for (unsigned m = 0; m < SMCLOG_MOD_COUNT; m++)
            if (p[0] == LINK_ALL_MODULES || p[0] == m)
                smclog_set_level((enum smclog_module)m, (enum smclog_level)p[1]);
        LOG_INFO(link, "log level of module %u (255: all) set to %u", p[0], p[1]);
        return LINK_OK;

    case LINK_CMD_LOG_GET_LEVELS:
        if (n)
            return LINK_BAD_LENGTH;
        o[0] = SMCLOG_MOD_COUNT;
        memcpy(o + 1, smclog_levels, SMCLOG_MOD_COUNT);
        *len += 1 + SMCLOG_MOD_COUNT;
        return LINK_OK;

    case LINK_CMD_LOG_RAW_INPUT:
        if (!ops->raw_input)
            return LINK_UNAVAILABLE;
        if (n != 1)
            return LINK_BAD_LENGTH;
        if ((p[0] & ~LINK_RAW_ALL & 0x0Fu) || (p[0] >> LINK_RAW_PROBE_SHIFT) > LINK_RAW_PROBE_MAX)
            return LINK_BAD_ARG;
        ops->raw_input(ops->ctx, p[0]);
        LOG_INFO(link, "raw input mode %s", p[0] ? "on" : "off");
        return LINK_OK;

    case LINK_CMD_METRICS_NOW:
        if (!ops->metrics)
            return LINK_UNAVAILABLE;
        if (n)
            return LINK_BAD_LENGTH;
        ops->metrics(ops->ctx);
        return LINK_OK;

    case LINK_CMD_BOOT_LOG:
        if (!ops->boot_log)
            return LINK_UNAVAILABLE;
        if (n)
            return LINK_BAD_LENGTH;
        ops->boot_log(ops->ctx);
        return LINK_OK;

    case LINK_CMD_RESET:
        if (!ops->reset)
            return LINK_UNAVAILABLE;
        if (n != 1)
            return LINK_BAD_LENGTH;
        if (p[0] > LINK_RESET_MAX)
            return LINK_BAD_ARG;
        ops->reset(ops->ctx, p[0], o);
        *len += 1;
        return LINK_OK;

    case LINK_CMD_STOCK:
        if (!ops->stock)
            return LINK_UNAVAILABLE;
        if (!n || n != (p[0] == LINK_STOCK_BACK ? 1u + sizeof LINK_STOCK_MAGIC - 1u : 1u))
            return LINK_BAD_LENGTH;
        if (p[0] > LINK_STOCK_BACK || (p[0] == LINK_STOCK_BACK && memcmp(p + 1, LINK_STOCK_MAGIC, 4)))
            return LINK_BAD_ARG;
        ops->stock(ops->ctx, p[0], o);
        *len += LINK_STOCK_REPLY;
        return LINK_OK;

    case LINK_CMD_INPUT_INJECT:
        if (!ops->inject)
            return LINK_UNAVAILABLE;
        if (!n)
            return LINK_BAD_LENGTH;
        ops->inject(ops->ctx, smclog_port_time_us(), p, n, o);
        *len += 3;
        return LINK_OK;

    case LINK_CMD_CORE_SETUP:
        if (!ops->setup)
            return LINK_UNAVAILABLE;
        ops->setup(ops->ctx, p, n, o); /* empty: the configuration as built in */
        *len += 2;
        return LINK_OK;

    default:
        if (cmd >= LINK_CMD_CFG_FIRST && cmd <= LINK_CMD_CFG_LAST) {
            if (!ops->config)
                return LINK_UNAVAILABLE;
            return ops->config(ops->cfg, cmd, p, n, o, len);
        }
        return LINK_UNKNOWN_CMD;
    }
}

_Static_assert(REPLY_HDR + 4 + LINK_PING_MAX <= LINK_REPLY_MAX, "PING reply");
_Static_assert(REPLY_HDR + 17 + LINK_BUILD_ID_MAX + LINK_VERSION_TEXT_MAX <= LINK_REPLY_MAX, "GET_INFO reply");
_Static_assert(REPLY_HDR + 1 + SMCLOG_MOD_COUNT <= LINK_REPLY_MAX, "LOG_GET_LEVELS reply");
_Static_assert(SMCLOG_FRAME_LEN(LINK_REPLY_MAX) <= 255, "reply_len is a byte");
_Static_assert(LINK_SYSEX_LEN(LINK_REPLY_MAX) <= 255, "reply_len is a byte");

static void command(struct link *l, const uint8_t *rec, size_t n)
{
    uint8_t *out = l->out; /* room for the CRC after LINK_REPLY_MAX */
    uint8_t seq = rec[1], cmd = rec[2];
    size_t len = REPLY_HDR;

    if (l->last_valid && seq == l->last_seq && cmd == l->last_cmd) {
        /* FR-CFG-03: a retry is answered with the same reply and not executed again */
        l->stats.retries++;
        l->reply_pending = true;
        LOG_DEBUG(link, "command %02x seq %u again: the same reply", cmd, seq);
        return;
    }
    out[0] = LINK_KIND_REPLY;
    out[1] = seq;
    out[2] = cmd;
    out[3] = execute(l, cmd, rec + 3, n - 3, out, &len);
    l->stats.commands++;
    if (out[3] == LINK_OK)
        LOG_DEBUG(link, "command %02x seq %u: ok", cmd, seq);
    else
        LOG_WARN(link, "command %02x seq %u (%u bytes): status %u", cmd, seq, (unsigned)(n - 3),
                 out[3]);
    l->reply_len = (uint8_t)(l->sysex ? sysex_frame(out, len, l->reply, sizeof l->reply)
                                      : smclog_frame(out, len, l->reply, sizeof l->reply));
    l->last_valid = true;
    l->last_seq = seq;
    l->last_cmd = cmd;
    l->reply_pending = true;
}

static void frame_in(struct link *l)
{
    uint8_t *rec = l->cmd; /* with its CRC */
    int n;

    if (l->sysex) {
        if (l->rx_len < LINK_SYSEX_HDR || l->rx[0] != LINK_SYSEX_MFR ||
            l->rx[1] != LINK_SYSEX_TAG0 || l->rx[2] != LINK_SYSEX_TAG1) {
            l->stats.foreign++;
            return;
        }
        n = link_unpack7(l->rx + LINK_SYSEX_HDR, l->rx_len - LINK_SYSEX_HDR, rec, sizeof l->cmd);
    } else {
        n = cobs_decode(l->rx, l->rx_len, rec, sizeof l->cmd);
    }
    l->stats.frames++;
    if (n < 3 || smclog_crc16(rec, (size_t)n - 2) != (uint16_t)(rec[n - 2] | rec[n - 1] << 8)) {
        if (l->stats.bad_frames++ == 0)
            LOG_WARN(link, "bad frame (%u bytes), counting the rest", l->rx_len);
        return;
    }
    n -= 2;
    if (rec[0] != LINK_KIND_CMD || n < 3) {
        l->stats.ignored++;
        LOG_DEBUG(link, "record kind %02x (%d bytes) ignored", rec[0], n);
        return;
    }
    command(l, rec, (size_t)n);
}

/* SysEx bytes: F0 opens a message, F7 ends it, a real-time byte is skipped, any other status
 * ends it unfinished; data bytes outside a message are not ours. */
static size_t rx_sysex(struct link *l, const uint8_t *p, size_t n)
{
    size_t i = 0;

    while (i < n && !l->reply_pending) {
        uint8_t b = p[i++];
        if (b < 0x80) {
            if (!l->rx_in || l->rx_skip)
                continue;
            if (l->rx_len == sizeof l->rx) {
                l->rx_skip = true;
                if (l->stats.overflows++ == 0)
                    LOG_WARN(link, "SysEx longer than %u bytes dropped, counting the rest",
                             (unsigned)sizeof l->rx);
                continue;
            }
            l->rx[l->rx_len++] = b;
            continue;
        }
        if (b >= 0xF8)
            continue; /* real-time: allowed anywhere */
        if (l->rx_in && !l->rx_skip) {
            if (b == 0xF7)
                frame_in(l);
            else if (l->stats.bad_frames++ == 0)
                LOG_WARN(link, "SysEx cut short by status %02x, counting the rest", b);
        }
        l->rx_in = b == 0xF0;
        l->rx_skip = false;
        l->rx_len = 0;
    }
    return i;
}

size_t link_rx(struct link *l, const uint8_t *p, size_t n)
{
    size_t i = 0;

    if (l->sysex)
        return rx_sysex(l, p, n);

    while (i < n && !l->reply_pending) {
        uint8_t b = p[i++];
        if (b != 0) {
            if (l->rx_skip)
                continue;
            if (l->rx_len == sizeof l->rx) {
                l->rx_skip = true;
                if (l->stats.overflows++ == 0)
                    LOG_WARN(link, "frame longer than %u bytes dropped, counting the rest",
                             (unsigned)sizeof l->rx);
                continue;
            }
            l->rx[l->rx_len++] = b;
            continue;
        }
        if (!l->rx_skip && l->rx_len)
            frame_in(l);
        l->rx_skip = false;
        l->rx_len = 0;
    }
    return i;
}

size_t link_tx(struct link *l, uint8_t *out, size_t cap)
{
    size_t done = 0, n;

    if (l->reply_pending) {
        if (l->reply_len > cap)
            return 0;
        memcpy(out, l->reply, l->reply_len);
        done = l->reply_len;
        l->reply_pending = false;
        l->stats.replies++;
        /* A SysEx link gives one message per call: BLE MIDI's sender takes one SysEx at a time
         * and refuses bytes after its F7. The EVENT goes on the next call. */
        if (l->sysex)
            return done;
    }
    if (l->header_pending) {
        n = smclog_stream_header(out + done, cap - done);
        if (!n)
            return done;
        done += n;
        l->header_pending = false;
    }
    if (l->events && !l->ev_len)
        event_due(l);
    if (l->ev_len) {
        if (l->ev_len > cap - done)
            return done;
        memcpy(out + done, l->ev, l->ev_len);
        done += l->ev_len;
        l->ev_len = 0;
        l->stats.events++;
    }
    if (l->sysex && done)
        return done; /* one SysEx message per call */
    if (l->sent_on && !l->sx_len)
        sent_due(l);
    if (l->sx_len) {
        if (l->sx_len > cap - done)
            return done;
        memcpy(out + done, l->sx, l->sx_len);
        done += l->sx_len;
        l->sx_len = 0;
        l->stats.sent++;
    }
    if (l->subscribed)
        done += smclog_drain(out + done, cap - done);
    return done;
}
