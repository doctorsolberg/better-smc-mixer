/*
 * A queue of USB-MIDI packets (usbmidi.h) from one task to another: the control task puts the
 * core's output in, the USB task takes it out for the host. Single producer, single consumer, no
 * lock: each side writes only its own index, after the data (single core). A full queue refuses a
 * packet and counts it; packets thrown away because no host takes them are counted too (LOG-34:
 * nothing is lost silently). Portable C; host tests in firmware/tests/test_midi.c.
 *
 * A packet may carry the time of the input event it answers (LOG-06: the consumer measures input →
 * USB latency); packets with no such event (a tick's output) carry none.
 *
 * Units: the producer can push several packets as one unit, all or none (the messages of one NRPN
 * value must never be split: docs/core.md, "The MIDI queue under load"); the consumer sees them
 * together. Backpressure: a producer that finds too many packets waiting asks to be told when at
 * most a given number wait (midiq_want_room()); the consumer, after taking packets, asks
 * midiq_room_due() and wakes the producer once when it is so.
 */
#ifndef MIDIQ_H
#define MIDIQ_H

#include <stdbool.h>
#include <stdint.h>

#define MIDIQ_SIZE 64u /* packets, a power of two; one slot stays empty */

struct midiq {
    uint8_t pkt[MIDIQ_SIZE][4];
    uint32_t t_us[MIDIQ_SIZE]; /* the input event's time */
    uint8_t timed[MIDIQ_SIZE]; /* t_us is set */
    volatile uint16_t head, tail; /* head: written by the producer; tail: by the consumer */
    /* the producer's counters */
    uint32_t pushed, refused;
    uint16_t depth_max; /* since start */
    uint16_t depth_win; /* since midiq_take_window_max() */
    /* the consumer's */
    uint32_t taken, dropped;
    /* backpressure: written by the producer (wanted 1, mark), cleared by the consumer (wanted 0) */
    volatile uint8_t room_wanted;
    volatile uint16_t room_mark;
};

void midiq_init(struct midiq *q);

/* The producer: a packet with no input event's time, or with one. */
bool midiq_push(struct midiq *q, const uint8_t pkt[4]);
bool midiq_push_at(struct midiq *q, const uint8_t pkt[4], uint32_t t_us);
/* The producer: n packets as one unit, with the input event's time t_us if timed: all queued, or
 * none (false; all n refused and counted) when they do not all fit. */
bool midiq_push_unit(struct midiq *q, const uint8_t (*pkt)[4], unsigned n, uint32_t t_us, bool timed);
/* The producer: how many packets fit now. */
unsigned midiq_room(const struct midiq *q);
/* The producer: wake me (midiq_room_due()) once at most `depth` packets wait. Check the depth again
 * after calling it: the consumer may have taken them meanwhile. */
void midiq_want_room(struct midiq *q, unsigned depth);
/* The producer: the most waiting since the previous call, then a new window. */
unsigned midiq_take_window_max(struct midiq *q);

/* The consumer: the oldest packet, or NULL; then pop it once it is handed on. */
const uint8_t *midiq_peek(const struct midiq *q);
/* The consumer, after midiq_peek() found a packet: true and its event's time if it has one. */
bool midiq_peek_time(const struct midiq *q, uint32_t *t_us);
void midiq_pop(struct midiq *q);
/* The consumer: throws everything queued away (no host to send it to); returns how many. */
unsigned midiq_drop_all(struct midiq *q);
/* The consumer, after taking or dropping packets: true once when the producer asked for room and at
 * most its mark wait now (then wake it). */
bool midiq_room_due(struct midiq *q);

unsigned midiq_depth(const struct midiq *q);

#endif /* MIDIQ_H */
