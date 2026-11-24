/*
 * BLE-MIDI packets (the MIDI Association's "Specification for MIDI over Bluetooth Low Energy",
 * Apple's format; docs/ble.md, "Design"). Portable C (no platform headers, no allocation); host
 * tests in firmware/tests/test_blemidi.c.
 *
 * A packet is one ATT write or notification:
 *
 *   header     1 0 t12..t7          the timestamp's high 6 bits (13-bit milliseconds)
 *   timestamp  1 t6..t0             before each message; its low 7 bits
 *   message    status, data...      or data only: running status
 *
 * A timestamp whose low 7 bits are below the previous one's in the same packet has its high bits
 * one more than the header's (the receiver counts the wrap). A message whose status and timestamp
 * are the previous message's may leave both out (running status). A real-time byte may come
 * anywhere, a timestamp before it, also inside a SysEx. A SysEx runs over several packets: its F0
 * and its F7 each have a timestamp before them; a packet that continues one has the header and
 * then the data at once.
 *
 * Sending (one packet at a time; the caller sends it and begins the next):
 *
 *   struct blemidi_tx tx;
 *   blemidi_tx_init(&tx);
 *   blemidi_tx_begin(&tx, buf, mtu - 3);
 *   while (have a message && blemidi_tx_add(&tx, now_ms, msg, len)) take it;
 *   if (blemidi_tx_len(&tx)) notify(buf, blemidi_tx_len(&tx));
 *
 * Receiving:
 *
 *   struct blemidi_rx rx;          blemidi_rx_init(&rx);
 *   blemidi_rx_packet(&rx, pkt, len, &sink);    // per ATT write; sink.msg per message
 */
#ifndef BLEMIDI_H
#define BLEMIDI_H

#include <stdbool.h>
#include <stdint.h>

#define BLEMIDI_TS_MASK 0x1FFFu /* timestamps are 13-bit milliseconds */

/* The length of a message by its status byte, the status included: channel messages 2 or 3,
 * System Common F1 2, F2 3, F3 2, F6 1, real-time F8..FF 1; 0 for a data byte, F0, F7 and the
 * undefined F4 and F5. */
unsigned blemidi_msg_len(uint8_t status);

struct blemidi_tx {
    uint8_t *buf;
    unsigned cap, len;
    uint16_t last_ts; /* the timestamp of the packet's last message */
    uint8_t running;  /* the status a following channel message may leave out; 0: none */
    bool sysex;       /* a SysEx was started and its F7 is not written yet */
};

void blemidi_tx_init(struct blemidi_tx *t);

/* Starts a packet in buf (cap bytes: the ATT MTU less 3). An unfinished SysEx continues in it. */
void blemidi_tx_begin(struct blemidi_tx *t, uint8_t *buf, unsigned cap);

/* Adds a whole message (a channel message, System Common or real-time; not SysEx) with its
 * timestamp (ms, the low 13 bits are used). False, and the packet unchanged, when it does not fit,
 * when its timestamp cannot follow the packet's last one (earlier, or more than one wrap of the
 * low 7 bits ahead), when it is not a valid message, or when a SysEx is unfinished and the
 * message is not real-time. A false for a valid message means: send this packet, begin another. */
bool blemidi_tx_add(struct blemidi_tx *t, uint16_t ts, const uint8_t *msg, unsigned len);

/* Adds SysEx bytes: the message from F0 to F7, or the part of it not yet taken. Returns how many
 * bytes it took: all of them, or fewer when the packet is full (send it, begin another and call
 * again with the rest). 0 on an empty packet means the bytes are not a SysEx (no F0 first, a byte
 * above 0x7F inside, or anything after the F7). */
unsigned blemidi_tx_sysex(struct blemidi_tx *t, uint16_t ts, const uint8_t *data, unsigned len);

/* The packet's length so far: 0 while nothing has been added. */
unsigned blemidi_tx_len(const struct blemidi_tx *t);

/* Where a decoded message goes. msg: a whole message other than SysEx, with its timestamp.
 * sysex: SysEx bytes as they come, F0 and F7 included; a SysEx cut short by another status ends
 * without F7 (the decoder counts it as bad). */
struct blemidi_sink {
    void (*msg)(void *ctx, uint16_t ts, const uint8_t *msg, unsigned len);
    void (*sysex)(void *ctx, const uint8_t *bytes, unsigned len);
    void *ctx;
};

struct blemidi_rx {
    uint8_t running;  /* the running status; 0: none */
    uint8_t msg[3];
    uint8_t have;     /* bytes of msg so far, the status included */
    uint8_t want;     /* the length msg needs; 0: no message under way */
    uint16_t ts;      /* the last timestamp */
    bool sysex;       /* inside a SysEx */
    /* Counters for the log (LOG-34). */
    uint32_t packets, messages, sysex_bytes, bad;
};

void blemidi_rx_init(struct blemidi_rx *r);

/* Decodes one packet into the sink (either callback may be NULL). Returns the faults found in it
 * (also added to r->bad): a packet too short or without a header, a data byte with no status for
 * it (or before the packet's first timestamp), a message cut short, a timestamp with no message
 * after it, an F7 outside a SysEx, a SysEx cut short, an undefined status (F4, F5). Decoding goes
 * on after a fault at the next byte that makes sense. Running status carries over from packet to
 * packet; a message never does (only a SysEx). */
unsigned blemidi_rx_packet(struct blemidi_rx *r, const uint8_t *pkt, unsigned len, const struct blemidi_sink *s);

#endif /* BLEMIDI_H */
