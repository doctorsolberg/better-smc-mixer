/*
 * The command link over BLE MIDI (docs/link.md, "Over SysEx"): SysEx bytes between the BLE stack's
 * task and the USB task, which runs the link (one task for every link, so the configuration
 * protocol's calls never race). No lock: one producer and one consumer each way, like midiq.h.
 *
 * - in: the bytes of SysEx messages the BLE stack received (its task puts, the USB task gets).
 *   What does not fit is dropped and counted; the link then sees a message cut short or a bad
 *   CRC, and the host sends its command again.
 * - out: one whole SysEx reply at a time (the USB task puts it when the slot is empty, the BLE
 *   sender takes it).
 * - epoch: the BLE side bumps it when a central connects or leaves; the USB task then resets its
 *   link and drops what waits in `in`. The BLE side drops a reply waiting in `out` itself.
 *
 * Portable C; host tests in firmware/tests/test_link.c.
 */
#ifndef SXPIPE_H
#define SXPIPE_H

#include <stddef.h>
#include <stdint.h>

#include "link.h"

#define SXPIPE_IN_SIZE 512u /* a power of two: two of the largest commands */

struct sxpipe {
    uint8_t in[SXPIPE_IN_SIZE];
    volatile uint16_t in_head, in_tail; /* head: the BLE task's; tail: the USB task's */
    uint32_t in_dropped;                /* the BLE task's: bytes that did not fit */
    uint8_t out[LINK_SYSEX_LEN(LINK_REPLY_MAX)];
    volatile uint16_t out_len; /* set by the USB task, cleared by the BLE task */
    volatile uint32_t epoch;   /* the BLE task's */
};

void sxpipe_init(struct sxpipe *p);

/* The BLE task: received SysEx bytes. Returns how many fit (the rest is counted as dropped). */
size_t sxpipe_put_in(struct sxpipe *p, const uint8_t *b, size_t n);

/* The USB task: up to cap received bytes into out; returns how many. */
size_t sxpipe_get_in(struct sxpipe *p, uint8_t *out, size_t cap);

/* The USB task: drops what waits in `in` (after an epoch change). */
void sxpipe_drop_in(struct sxpipe *p);

/* The USB task: a reply into the empty out slot; 0 (nothing taken) when the slot is full or the
 * reply too long. */
size_t sxpipe_put_out(struct sxpipe *p, const uint8_t *b, size_t n);

/* The BLE task: the waiting reply, or NULL; sxpipe_done_out() when it has been sent (or dropped). */
const uint8_t *sxpipe_peek_out(const struct sxpipe *p, size_t *n);
void sxpipe_done_out(struct sxpipe *p);

#endif /* SXPIPE_H */
