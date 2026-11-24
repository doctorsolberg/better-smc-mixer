/*
 * The queue of MIDI messages for BLE (docs/ble.md, "Last value wins"): the control task puts the
 * core's output in, the BLE sender (the SDK's btstack task) takes it out in notifications. Single
 * producer, single consumer, no lock, like midiq.h, whose units, counters and backpressure it
 * shares; it adds the merge BLE needs (FR-IO-08), since a connection carries far fewer messages
 * than the core can make.
 *
 * Groups: the messages of a unit form groups. A run of messages with the same nonzero mark (the
 * core's out_merge: one continuous control's value, its NRPN selection included) is a merge group;
 * a run of unmarked ones is a plain group. When a merge group comes and an older one of the same
 * shape (the same messages but for their values: status and first data byte each, the status alone for pitch bend) still waits,
 * the older one is marked dead and the new one goes at the end: the newest value wins and no
 * message ever goes ahead of one queued after it. Plain groups (notes, buttons, nulls, relative
 * encoders) are never dropped.
 *
 * NRPN and RPN: the shape includes the parameter's number (the values of CC 98-101). A group that
 * selects its parameter replaces an older one only when no group in between relies on the older
 * one's selection (data entry or half a number with no full selection of its own before it); a
 * group of data entry alone only when no group in between changes the selection. So the receiver
 * ends up with the selection the core believes it has (core.h, param_out), and every value goes to
 * the parameter it went to without the merge.
 *
 * The consumer checks a group's mark when it reaches the group's first message and then sends the
 * whole group: a group the producer marks dead after that goes out whole all the same, before its
 * newer value. So a group is never split, and nothing is lost to the race.
 *
 * Portable C; host tests in firmware/tests/test_bleq.c.
 */
#ifndef BLEQ_H
#define BLEQ_H

#include <stdbool.h>
#include <stdint.h>

#define BLEQ_SIZE 128u /* messages, a power of two; one slot stays empty */

struct bleq_msg {
    uint8_t m[3];  /* a channel message; its length follows from the status */
    uint8_t len;   /* the group's messages (on each of them) */
    uint8_t pos;   /* this message's place in its group, 0 the first */
    uint8_t merge; /* in a merge group */
    uint8_t timed; /* t_us is set */
    uint32_t t_us; /* the input event's time (LOG-06) */
};

struct bleq {
    struct bleq_msg msg[BLEQ_SIZE];
    volatile uint8_t dead[BLEQ_SIZE]; /* on a group's first message: replaced by a newer value */
    volatile uint16_t head, tail;     /* head: written by the producer; tail: by the consumer */
    /* the producer's counters */
    uint32_t pushed, refused, merged; /* merged: messages of older values marked dead */
    uint16_t depth_max, depth_win;
    /* the consumer's */
    uint32_t taken, dropped, skipped; /* skipped: dead messages passed over */
    uint8_t in_group;                 /* messages of the group being sent still to take */
    /* backpressure, as in midiq.h */
    volatile uint8_t room_wanted;
    volatile uint16_t room_mark;
};

void bleq_init(struct bleq *q);

/* The length of a channel message by its status: 2 for program change and channel pressure, else 3. */
unsigned bleq_msg_len(uint8_t status);

/* The producer: n channel messages with their marks (0: plain; the same nonzero mark in a row: one
 * merge group) and the input event's time t_us if timed: all queued, or none (false; all n refused
 * and counted) when they do not all fit. Older values the merge groups replace are marked dead. */
bool bleq_push_unit(struct bleq *q, const uint8_t (*msg)[3], const uint8_t *mark, unsigned n,
                    uint32_t t_us, bool timed);
unsigned bleq_depth(const struct bleq *q); /* dead messages included until the consumer passes them */
unsigned bleq_room(const struct bleq *q);
void bleq_want_room(struct bleq *q, unsigned depth);
unsigned bleq_take_window_max(struct bleq *q);

/* The consumer: the next message to send, dead groups passed over (and counted), or NULL; then
 * pop it once it is in a packet. */
const struct bleq_msg *bleq_peek(struct bleq *q);
void bleq_pop(struct bleq *q);
/* The consumer: true while the message bleq_peek() returned begins a group (a packet may be sent
 * before it rather than split the group). */
bool bleq_at_group_start(const struct bleq *q);
/* The consumer: throws everything queued away (no connection); returns how many. */
unsigned bleq_drop_all(struct bleq *q);
/* The consumer, after taking or dropping messages: true once when the producer asked for room and
 * at most its mark wait now. */
bool bleq_room_due(struct bleq *q);

#endif /* BLEQ_H */
