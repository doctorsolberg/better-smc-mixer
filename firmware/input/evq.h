/*
 * The input event queue: the scan layer (interrupt) and input injection (task) put core events
 * in, the main loop takes them out for the core. A full queue refuses an event and counts it;
 * nothing is dropped silently (LOG-34).
 *
 * Merging (evq_set_merge(), the controller): a fader event from the panel updates that fader's
 * event still waiting in the queue instead of taking a slot, so the queue holds at most one per
 * fader and the core takes the newest position (docs/core.md, "Fader events merged"). The waiting
 * event keeps its place and takes the newest position's stamp: what the core sends is that
 * position, so its latency counts from there (the older ones are never sent). Any other event, and
 * an injected one, ends merging for every fader event queued before it, so the core never sees a
 * position moved across a button, an encoder step or MIDI in.
 */
#ifndef EVQ_H
#define EVQ_H

#include <stdbool.h>
#include <stdint.h>

#include "core.h"

#define EVQ_SIZE 64u /* a power of two; one slot stays empty */

struct evq {
    struct core_event ev[EVQ_SIZE];
    volatile uint16_t head, tail; /* head: next to write; tail: next to read */
    uint32_t pushed, refused;
    uint16_t depth_max; /* high-water mark (LOG-06) */
    uint16_t depth_win; /* the same since evq_take_window_max() */
    bool merge;                       /* evq_set_merge() */
    bool was_empty;                   /* the last evq_push() found the queue empty */
    int8_t fader_slot[CORE_STRIPS];   /* the slot of a fader's mergeable waiting event, or -1 */
    uint32_t merged;                  /* fader events merged into a waiting one */
};

/* Provided by the platform: keep out every other producer (on the device: mask the scan and USB
 * interrupts). The host tests implement them as no-ops. */
uint32_t input_port_lock(void);
void input_port_unlock(uint32_t state);

void evq_init(struct evq *q);
void evq_set_merge(struct evq *q, bool on); /* before the producers start */
bool evq_push(struct evq *q, const struct core_event *ev); /* any context */
bool evq_pop(struct evq *q, struct core_event *ev);        /* the consumer only */
unsigned evq_depth(const struct evq *q);
/* Any context: the most waiting since the previous call, then a new window. */
unsigned evq_take_window_max(struct evq *q);

/* scan_sink adapter: ctx is the queue. */
bool evq_sink(void *ctx, const struct core_event *ev);

#endif /* EVQ_H */
