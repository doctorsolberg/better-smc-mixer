/* BLE-MIDI packets (blemidi.h). */
#include "blemidi.h"

#include <stddef.h>

unsigned blemidi_msg_len(uint8_t status)
{
    if (status < 0x80)
        return 0;
    if (status < 0xF0)
        return (status & 0xF0u) == 0xC0 || (status & 0xF0u) == 0xD0 ? 2 : 3;
    if (status >= 0xF8)
        return 1;
    if (status == 0xF1 || status == 0xF3)
        return 2;
    if (status == 0xF2)
        return 3;
    if (status == 0xF6)
        return 1;
    return 0; /* F0, F7: SysEx; F4, F5: undefined */
}

/* --- sending --- */

void blemidi_tx_init(struct blemidi_tx *t)
{
    t->buf = NULL;
    t->cap = t->len = 0;
    t->last_ts = 0;
    t->running = 0;
    t->sysex = false;
}

void blemidi_tx_begin(struct blemidi_tx *t, uint8_t *buf, unsigned cap)
{
    t->buf = buf;
    t->cap = cap;
    t->len = 0;
    t->running = 0; /* each packet starts with a status: a receiver may have lost the last one */
}

unsigned blemidi_tx_len(const struct blemidi_tx *t)
{
    return t->len;
}

/* Whether ts can follow last in one packet: the same high bits and a low part not below, or the
 * high bits one more and the low part below (the receiver counts one wrap). */
static bool ts_follows(uint16_t last, uint16_t ts)
{
    unsigned lh = last >> 7, ll = last & 0x7Fu, h = ts >> 7, l = ts & 0x7Fu;
    if (h == lh)
        return l >= ll;
    return h == ((lh + 1) & 0x3Fu) && l < ll;
}

/* Room for n more bytes after the header (written now if the packet is empty), the timestamp
 * allowed. Writes the header; the caller writes the rest. */
static bool open_for(struct blemidi_tx *t, uint16_t ts, unsigned n)
{
    if (!t->buf)
        return false;
    if (t->len == 0) {
        if (n + 1 > t->cap)
            return false;
        t->buf[t->len++] = (uint8_t)(0x80u | (ts >> 7));
        return true;
    }
    return ts_follows(t->last_ts, ts) && n <= t->cap - t->len;
}

bool blemidi_tx_add(struct blemidi_tx *t, uint16_t ts, const uint8_t *msg, unsigned len)
{
    ts &= BLEMIDI_TS_MASK;
    if (!len || blemidi_msg_len(msg[0]) != len)
        return false;
    for (unsigned i = 1; i < len; i++)
        if (msg[i] > 0x7F)
            return false;
    bool realtime = msg[0] >= 0xF8, channel = msg[0] < 0xF0;
    if (t->sysex && !realtime)
        return false;

    /* Running status: the status and the timestamp left out, the data bytes only. */
    bool run = channel && t->len && msg[0] == t->running && ts == t->last_ts;
    unsigned need = run ? len - 1 : len + 1;
    if (!open_for(t, ts, need))
        return false;
    if (run) {
        for (unsigned i = 1; i < len; i++)
            t->buf[t->len++] = msg[i];
    } else {
        t->buf[t->len++] = (uint8_t)(0x80u | (ts & 0x7Fu));
        for (unsigned i = 0; i < len; i++)
            t->buf[t->len++] = msg[i];
    }
    t->last_ts = ts;
    /* Only right after a channel message may the next one leave its status out (the encoder's own
     * rule: a receiver keeps running status over real-time bytes, not every one may). */
    t->running = channel ? msg[0] : 0;
    return true;
}

unsigned blemidi_tx_sysex(struct blemidi_tx *t, uint16_t ts, const uint8_t *data, unsigned len)
{
    ts &= BLEMIDI_TS_MASK;
    unsigned i = 0;
    if (!t->buf || !len)
        return 0;
    if (!t->sysex) {
        if (data[0] != 0xF0 || !open_for(t, ts, 2)) /* the timestamp and F0 */
            return 0;
        t->buf[t->len++] = (uint8_t)(0x80u | (ts & 0x7Fu));
        t->buf[t->len++] = 0xF0;
        t->last_ts = ts;
        t->running = 0;
        t->sysex = true;
        i = 1;
    } else if (t->len == 0) {
        /* A packet that continues the SysEx: the header, then the data at once. */
        if (data[0] < 0x80) {
            if (!open_for(t, ts, 1))
                return 0;
            t->last_ts = ts;
        }
    }
    while (i < len) {
        uint8_t b = data[i];
        if (b < 0x80) {
            if (t->len == 0 || t->len >= t->cap)
                break;
            t->buf[t->len++] = b;
            i++;
            continue;
        }
        if (b != 0xF7 || i + 1 != len)
            break; /* not a SysEx byte, or bytes after the end */
        if (!open_for(t, ts, 2))
            break;
        t->buf[t->len++] = (uint8_t)(0x80u | (ts & 0x7Fu));
        t->buf[t->len++] = 0xF7;
        t->last_ts = ts;
        t->sysex = false;
        i++;
    }
    return i;
}

/* --- receiving --- */

void blemidi_rx_init(struct blemidi_rx *r)
{
    r->running = 0;
    r->have = r->want = 0;
    r->ts = 0;
    r->sysex = false;
    r->packets = r->messages = r->sysex_bytes = r->bad = 0;
}

static void emit_msg(struct blemidi_rx *r, const struct blemidi_sink *s, const uint8_t *m, unsigned len)
{
    r->messages++;
    if (s && s->msg)
        s->msg(s->ctx, r->ts, m, len);
}

static void emit_sysex(struct blemidi_rx *r, const struct blemidi_sink *s, const uint8_t *b, unsigned len)
{
    if (!len)
        return;
    r->sysex_bytes += len;
    if (s && s->sysex)
        s->sysex(s->ctx, b, len);
}

unsigned blemidi_rx_packet(struct blemidi_rx *r, const uint8_t *pkt, unsigned len, const struct blemidi_sink *s)
{
    unsigned bad = 0;
    r->packets++;
    if (len < 2 || (pkt[0] & 0xC0u) != 0x80u) {
        r->bad++;
        return 1;
    }
    unsigned high = pkt[0] & 0x3Fu, last_low = 0;
    bool seen_ts = false, after_ts = false;
    unsigned run = 0; /* the start of a run of SysEx bytes still to pass on; 0: none */

    for (unsigned i = 1; i < len; i++) {
        uint8_t b = pkt[i];
        if (b < 0x80) { /* a data byte */
            if (r->sysex) {
                if (!run)
                    run = i;
                after_ts = false;
                continue;
            }
            if (!seen_ts) { /* the packet's first message needs its timestamp */
                bad++;
                continue;
            }
            after_ts = false;
            if (!r->want) { /* a new message under the running status */
                if (!r->running) {
                    bad++;
                    continue;
                }
                r->msg[0] = r->running;
                r->have = 1;
                r->want = (uint8_t)blemidi_msg_len(r->running);
            }
            r->msg[r->have++] = b;
            if (r->have == r->want) {
                emit_msg(r, s, r->msg, r->want);
                r->want = 0;
            }
            continue;
        }
        if (!after_ts) { /* a timestamp */
            if (run) {
                emit_sysex(r, s, pkt + run, i - run);
                run = 0;
            }
            if (r->want) { /* the message before it is cut short */
                bad++;
                r->want = 0;
            }
            unsigned low = b & 0x7Fu;
            if (seen_ts && low < last_low)
                high = (high + 1) & 0x3Fu;
            last_low = low;
            seen_ts = after_ts = true;
            r->ts = (uint16_t)(high << 7 | low);
            continue;
        }
        after_ts = false; /* a status after its timestamp */
        if (r->sysex) {
            if (b == 0xF7) {
                emit_sysex(r, s, pkt + i, 1);
                r->sysex = false;
                continue;
            }
            if (b >= 0xF8) { /* real-time inside the SysEx */
                emit_msg(r, s, &b, 1);
                continue;
            }
            bad++; /* cut short by another status */
            r->sysex = false;
        }
        if (b == 0xF0) {
            r->sysex = true;
            r->running = 0;
            run = i;
            continue;
        }
        unsigned n = blemidi_msg_len(b);
        if (!n) { /* F7 outside a SysEx, F4, F5 */
            bad++;
            r->running = 0;
            continue;
        }
        if (b >= 0xF8) { /* real-time: running status stays */
            emit_msg(r, s, &b, 1);
            continue;
        }
        r->running = b < 0xF0 ? b : 0; /* System Common cancels it */
        r->msg[0] = b;
        r->have = 1;
        r->want = (uint8_t)n;
        if (n == 1) {
            emit_msg(r, s, r->msg, 1);
            r->want = 0;
        }
    }
    if (run)
        emit_sysex(r, s, pkt + run, len - run);
    if (r->want) { /* a message does not continue into the next packet */
        bad++;
        r->want = 0;
    }
    if (after_ts && !r->sysex)
        bad++; /* a timestamp with nothing after it */
    r->bad += bad;
    return bad;
}
