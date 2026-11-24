/*
 * Input injection (SPEC LOG-33, LOG-34): the INPUT_INJECT debug command puts input events into the
 * event queue as if the scan had produced them, flagged CORE_EV_INJECTED.
 *
 * Payload (transport-independent; the USB CDC framing and the SysEx 7-bit packing come with the
 * configuration protocol, SPEC §7; provisional, docs/hwtest.md):
 *
 *   byte 0: op: 0 off, 1 on (for this session), 2 events
 *   op 2:   then 5 bytes per event: type, index, b0, b1, b2 (type: enum core_ev_type)
 *           button: b0 1 press, 0 release
 *           encoder: b0 | b1 << 8, signed clicks, non-zero
 *           fader: b0 | b1 << 8, 0-16383
 *           MIDI in: index is the transport (core_transport), b0 b1 b2 the message
 *
 * A long press is a press, a wait and a release: the host times it. Every event of a command is
 * checked before any is queued; a full queue refuses the rest, counted (never dropped silently).
 * Injection is off at start and does not survive a reboot (LOG-34).
 */
#ifndef INJECT_H
#define INJECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "evq.h"

enum inject_op { INJECT_OP_OFF = 0, INJECT_OP_ON = 1, INJECT_OP_EVENTS = 2 };

enum inject_status {
    INJECT_OK = 0,
    INJECT_DISABLED = 1,   /* events while injection is off */
    INJECT_BAD_FORMAT = 2, /* unknown op, or a length that is not a whole number of events */
    INJECT_BAD_EVENT = 3,  /* an event out of range: nothing was queued */
    INJECT_FULL = 4,       /* the queue refused some: 'accepted' were queued, the rest not */
};

#define INJECT_EVENT_BYTES 5u
#define INJECT_MAX_EVENTS 32u

struct inject_reply {
    uint8_t status; /* enum inject_status */
    uint8_t accepted, refused;
};

struct inject {
    struct evq *q;
    bool on;
    uint32_t accepted, refused, rejected; /* events: queued, refused by a full queue, invalid */
};

void inject_init(struct inject *in, struct evq *q);
struct inject_reply inject_command(struct inject *in, uint32_t t_us, const uint8_t *p, size_t len);

#endif /* INJECT_H */
