/* The USB-MIDI packet queue (midiq.h). */
#include "midiq.h"

#include <string.h>

/* Keeps the compiler from moving memory accesses across it: the data is written (or read) before
 * the index that hands it over. One core, so nothing else reorders them. */
#define MIDIQ_BARRIER() __asm__ __volatile__("" ::: "memory")

#define NEXT(i) ((uint16_t)(((i) + 1u) & (MIDIQ_SIZE - 1u)))

void midiq_init(struct midiq *q)
{
    memset(q, 0, sizeof *q);
}

unsigned midiq_depth(const struct midiq *q)
{
    return (uint16_t)(q->head - q->tail) & (MIDIQ_SIZE - 1u);
}

unsigned midiq_room(const struct midiq *q)
{
    return MIDIQ_SIZE - 1u - midiq_depth(q);
}

bool midiq_push_unit(struct midiq *q, const uint8_t (*pkt)[4], unsigned n, uint32_t t_us, bool timed)
{
    if (n == 0)
        return true;
    if (n > midiq_room(q)) {
        q->refused += n;
        return false;
    }
    uint16_t head = q->head;
    for (unsigned i = 0; i < n; i++) {
        memcpy(q->pkt[head], pkt[i], 4);
        q->t_us[head] = t_us;
        q->timed[head] = timed;
        head = NEXT(head);
    }
    MIDIQ_BARRIER();
    q->head = head; /* the consumer sees the whole unit at once */
    q->pushed += n;
    uint16_t depth = (uint16_t)midiq_depth(q);
    if (depth > q->depth_max)
        q->depth_max = depth;
    if (depth > q->depth_win)
        q->depth_win = depth;
    return true;
}

bool midiq_push(struct midiq *q, const uint8_t pkt[4])
{
    return midiq_push_unit(q, (const uint8_t(*)[4])pkt, 1, 0, false);
}

bool midiq_push_at(struct midiq *q, const uint8_t pkt[4], uint32_t t_us)
{
    return midiq_push_unit(q, (const uint8_t(*)[4])pkt, 1, t_us, true);
}

void midiq_want_room(struct midiq *q, unsigned depth)
{
    q->room_mark = (uint16_t)depth;
    MIDIQ_BARRIER();
    q->room_wanted = 1;
}

bool midiq_room_due(struct midiq *q)
{
    if (!q->room_wanted || midiq_depth(q) > q->room_mark)
        return false;
    q->room_wanted = 0;
    return true;
}

unsigned midiq_take_window_max(struct midiq *q)
{
    unsigned m = q->depth_win;
    q->depth_win = (uint16_t)midiq_depth(q);
    return m;
}

const uint8_t *midiq_peek(const struct midiq *q)
{
    uint16_t tail = q->tail;
    if (tail == q->head)
        return NULL;
    MIDIQ_BARRIER();
    return q->pkt[tail];
}

bool midiq_peek_time(const struct midiq *q, uint32_t *t_us)
{
    uint16_t tail = q->tail;
    if (tail == q->head)
        return false;
    MIDIQ_BARRIER();
    *t_us = q->t_us[tail];
    return q->timed[tail] != 0;
}

void midiq_pop(struct midiq *q)
{
    uint16_t tail = q->tail;
    if (tail == q->head)
        return;
    MIDIQ_BARRIER();
    q->tail = NEXT(tail);
    q->taken++;
}

unsigned midiq_drop_all(struct midiq *q)
{
    uint16_t head = q->head;
    unsigned n = (uint16_t)(head - q->tail) & (MIDIQ_SIZE - 1u);
    MIDIQ_BARRIER();
    q->tail = head;
    q->dropped += n;
    return n;
}
