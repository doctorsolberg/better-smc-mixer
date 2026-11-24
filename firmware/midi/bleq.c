/* The queue of MIDI messages for BLE (bleq.h). */
#include "bleq.h"

#include <string.h>

/* As in midiq.c: the data is written (or read) before the index that hands it over; one core. */
#define BLEQ_BARRIER() __asm__ __volatile__("" ::: "memory")

#define MASK (BLEQ_SIZE - 1u)
#define AT(i) ((uint16_t)((i) & MASK))

void bleq_init(struct bleq *q)
{
    memset(q, 0, sizeof *q);
}

unsigned bleq_msg_len(uint8_t status)
{
    unsigned hi = status & 0xF0u;
    return hi == 0xC0u || hi == 0xD0u ? 2u : 3u;
}

unsigned bleq_depth(const struct bleq *q)
{
    return AT(q->head - q->tail);
}

unsigned bleq_room(const struct bleq *q)
{
    return BLEQ_SIZE - 1u - bleq_depth(q);
}

/* The parameter CCs on channel ch: CC 6, 38 (data entry), 96, 97 (increment, decrement) touch the
 * selected parameter; 98-101 (its number) select one. */
static bool is_cc(const uint8_t m[3], unsigned ch)
{
    return (m[0] & 0xF0u) == 0xB0u && (m[0] & 0x0Fu) == ch;
}

static bool touches_param(const uint8_t m[3], unsigned ch)
{
    return is_cc(m, ch) && (m[1] == 6 || m[1] == 38 || (m[1] >= 96 && m[1] <= 101));
}

static bool selects(const uint8_t m[3], unsigned ch)
{
    return is_cc(m, ch) && m[1] >= 98 && m[1] <= 101;
}

/* What a group does to channel ch's parameter selection: SEL_NONE leaves it alone, SEL_FULL selects
 * one (both halves of an NRPN or an RPN number), SEL_RELIES uses or changes part of the one before
 * it (data entry, increments, half a number). */
enum { SEL_NONE, SEL_FULL, SEL_RELIES };

static unsigned sel_kind(const struct bleq *q, uint16_t first, unsigned len, unsigned ch)
{
    bool touch = false, n99 = false, n98 = false, n101 = false, n100 = false;
    for (unsigned j = 0; j < len; j++) {
        const uint8_t *m = q->msg[AT(first + j)].m;
        if (!touches_param(m, ch))
            continue;
        touch = true;
        n99 = n99 || m[1] == 99;
        n98 = n98 || m[1] == 98;
        n101 = n101 || m[1] == 101;
        n100 = n100 || m[1] == 100;
    }
    if (!touch)
        return SEL_NONE;
    return (n99 && n98) || (n101 && n100) ? SEL_FULL : SEL_RELIES;
}

static bool group_selects(const struct bleq *q, uint16_t first, unsigned len, unsigned ch)
{
    for (unsigned j = 0; j < len; j++)
        if (selects(q->msg[AT(first + j)].m, ch))
            return true;
    return false;
}

/* The same messages but for their values: status and first data byte each (the status alone for
 * pitch bend and channel pressure), and the parameter's number (the value of CC 98-101) too. */
static bool same_shape(const struct bleq *q, uint16_t first, const uint8_t (*g)[3], unsigned k)
{
    for (unsigned j = 0; j < k; j++) {
        const uint8_t *m = q->msg[AT(first + j)].m;
        bool keyed = bleq_msg_len(m[0]) == 3 && (m[0] & 0xF0u) != 0xE0u; /* pitch bend: all value */
        if (m[0] != g[j][0] || (keyed && m[1] != g[j][1]))
            return false;
        if (selects(m, m[0] & 0x0Fu) && m[2] != g[j][2])
            return false;
    }
    return true;
}

/* A merge group g of k messages comes: the newest waiting group of its shape is marked dead, when
 * that leaves the parameter selection on g's channel as it was:
 * - g selects its parameter itself: no group between relies on the older one's selection (a group
 *   that selects fully, later, covers the ones after it);
 * - g only touches the selected parameter: no group between changes the selection;
 * - g does not touch it: nothing to keep.
 * Only groups that begin at or after the tail read here: one the consumer has begun goes out. */
static void kill_older(struct bleq *q, const uint8_t (*g)[3], unsigned k)
{
    unsigned ch = g[0][0] & 0x0Fu;
    bool touch = false, n99 = false, n98 = false, n101 = false, n100 = false;
    for (unsigned j = 0; j < k; j++) {
        if (!touches_param(g[j], ch))
            continue;
        touch = true;
        n99 = n99 || g[j][1] == 99;
        n98 = n98 || g[j][1] == 98;
        n101 = n101 || g[j][1] == 101;
        n100 = n100 || g[j][1] == 100;
    }
    bool full = (n99 && n98) || (n101 && n100);
    bool relies = false; /* a group after the one looked at uses its selection */
    uint16_t end = q->head;
    unsigned waiting = AT(end - q->tail);
    while (waiting) {
        const struct bleq_msg *e = &q->msg[AT(end - 1u)]; /* the last message of a group */
        unsigned n = e->pos + 1u;
        if (n > waiting)
            return; /* it began before the tail: being sent */
        uint16_t first = AT(end - n);
        waiting -= n;
        end = first;
        if (q->dead[first])
            continue;
        if (q->msg[first].merge && e->len == k && same_shape(q, first, g, k) && !relies) {
            q->dead[first] = 1;
            q->merged += k;
            return;
        }
        if (!touch)
            continue;
        if (full) {
            unsigned kind = sel_kind(q, first, n, ch);
            if (kind == SEL_FULL)
                relies = false;
            else if (kind == SEL_RELIES)
                relies = true;
        } else if (group_selects(q, first, n, ch)) {
            return;
        }
    }
}

bool bleq_push_unit(struct bleq *q, const uint8_t (*msg)[3], const uint8_t *mark, unsigned n,
                    uint32_t t_us, bool timed)
{
    if (n == 0)
        return true;
    if (n > bleq_room(q)) {
        q->refused += n;
        return false;
    }
    uint16_t head = q->head;
    for (unsigned i = 0; i < n;) {
        unsigned k = 1;
        while (i + k < n && mark[i + k] == mark[i] && k < 255u)
            k++;
        bool merge = mark[i] != 0;
        if (merge)
            kill_older(q, msg + i, k);
        for (unsigned j = 0; j < k; j++) {
            struct bleq_msg *e = &q->msg[head];
            memcpy(e->m, msg[i + j], 3);
            e->len = (uint8_t)k;
            e->pos = (uint8_t)j;
            e->merge = merge;
            e->timed = timed;
            e->t_us = t_us;
            q->dead[head] = 0;
            head = AT(head + 1u);
        }
        i += k;
    }
    BLEQ_BARRIER();
    q->head = head; /* the consumer sees the whole unit at once */
    q->pushed += n;
    uint16_t depth = (uint16_t)bleq_depth(q);
    if (depth > q->depth_max)
        q->depth_max = depth;
    if (depth > q->depth_win)
        q->depth_win = depth;
    return true;
}

void bleq_want_room(struct bleq *q, unsigned depth)
{
    q->room_mark = (uint16_t)depth;
    BLEQ_BARRIER();
    q->room_wanted = 1;
}

bool bleq_room_due(struct bleq *q)
{
    if (!q->room_wanted || bleq_depth(q) > q->room_mark)
        return false;
    q->room_wanted = 0;
    return true;
}

unsigned bleq_take_window_max(struct bleq *q)
{
    unsigned m = q->depth_win;
    q->depth_win = (uint16_t)bleq_depth(q);
    return m;
}

const struct bleq_msg *bleq_peek(struct bleq *q)
{
    for (;;) {
        uint16_t tail = q->tail;
        if (tail == q->head)
            return NULL;
        BLEQ_BARRIER();
        const struct bleq_msg *e = &q->msg[tail];
        if (q->in_group == 0 && q->dead[tail]) { /* a group's first message: replaced */
            unsigned len = e->len;
            BLEQ_BARRIER();
            q->tail = AT(tail + len);
            q->skipped += len;
            continue;
        }
        return e;
    }
}

bool bleq_at_group_start(const struct bleq *q)
{
    return q->in_group == 0;
}

void bleq_pop(struct bleq *q)
{
    uint16_t tail = q->tail;
    if (tail == q->head)
        return;
    BLEQ_BARRIER();
    const struct bleq_msg *e = &q->msg[tail];
    q->in_group = q->in_group ? (uint8_t)(q->in_group - 1u) : (uint8_t)(e->len - 1u);
    BLEQ_BARRIER();
    q->tail = AT(tail + 1u);
    q->taken++;
}

unsigned bleq_drop_all(struct bleq *q)
{
    uint16_t head = q->head;
    unsigned n = AT(head - q->tail);
    BLEQ_BARRIER();
    q->tail = head;
    q->in_group = 0;
    q->dropped += n;
    return n;
}
