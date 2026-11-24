/*
 * The input event queue (evq.h).
 */
#include "evq.h"

#include <string.h>

#include "smc_ram.h"

void evq_init(struct evq *q)
{
    memset(q, 0, sizeof *q);
    memset(q->fader_slot, -1, sizeof q->fader_slot);
}

void evq_set_merge(struct evq *q, bool on)
{
    q->merge = on;
}

SMC_RAM_CODE unsigned evq_depth(const struct evq *q)
{
    return (uint16_t)(q->head - q->tail) & (EVQ_SIZE - 1u);
}

SMC_RAM_CODE bool evq_push(struct evq *q, const struct core_event *ev)
{
    uint32_t s = input_port_lock();
    q->was_empty = q->head == q->tail;
    if (q->merge) {
        bool fader = ev->type == CORE_EV_FADER && ev->index < CORE_STRIPS &&
                     !(ev->flags & CORE_EV_INJECTED);
        if (fader && q->fader_slot[ev->index] >= 0) {
            q->ev[q->fader_slot[ev->index]].value = ev->value;
            q->ev[q->fader_slot[ev->index]].t_us = ev->t_us; /* the newest position's time */
            q->merged++;
            input_port_unlock(s);
            return true;
        }
        if (!fader)
            memset(q->fader_slot, -1, sizeof q->fader_slot);
    }
    uint16_t next = (uint16_t)((q->head + 1u) & (EVQ_SIZE - 1u));
    bool ok = next != q->tail;
    if (ok) {
        if (q->merge && ev->type == CORE_EV_FADER && ev->index < CORE_STRIPS &&
            !(ev->flags & CORE_EV_INJECTED))
            q->fader_slot[ev->index] = (int8_t)q->head;
        q->ev[q->head] = *ev;
        q->head = next;
        q->pushed++;
        uint16_t depth = (uint16_t)evq_depth(q);
        if (depth > q->depth_max)
            q->depth_max = depth;
        if (depth > q->depth_win)
            q->depth_win = depth;
    } else {
        q->refused++;
    }
    input_port_unlock(s);
    return ok;
}

unsigned evq_take_window_max(struct evq *q)
{
    uint32_t s = input_port_lock();
    unsigned m = q->depth_win;
    q->depth_win = (uint16_t)evq_depth(q);
    input_port_unlock(s);
    return m;
}

SMC_HOT bool evq_pop(struct evq *q, struct core_event *ev)
{
    uint32_t s = input_port_lock();
    bool ok = q->tail != q->head;
    if (ok) {
        *ev = q->ev[q->tail];
        if (ev->type == CORE_EV_FADER && ev->index < CORE_STRIPS &&
            q->fader_slot[ev->index] == (int8_t)q->tail)
            q->fader_slot[ev->index] = -1; /* taken: the next event of that fader queues anew */
        q->tail = (uint16_t)((q->tail + 1u) & (EVQ_SIZE - 1u));
    }
    input_port_unlock(s);
    return ok;
}

SMC_RAM_CODE bool evq_sink(void *ctx, const struct core_event *ev)
{
    return evq_push(ctx, ev);
}
