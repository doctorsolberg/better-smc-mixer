/*
 * USB-MIDI 1.0 event packets (USB Device Class Definition for MIDI Devices 1.0, §4): four bytes,
 * the cable number and the Code Index Number (CIN) in byte 0, then up to three MIDI bytes.
 * Portable C (no platform headers); host tests in firmware/tests/test_midi.c.
 *
 *   uint8_t pkt[4];
 *   usbmidi_pack(USBMIDI_CABLE_PERFORMANCE, msg, len, pkt);   // a channel message to the host
 *   switch (usbmidi_unpack(pkt, msg)) { case USBMIDI_CHANNEL: ... }   // a packet from the host
 */
#ifndef USBMIDI_H
#define USBMIDI_H

#include <stdbool.h>
#include <stdint.h>

/* The device's two ports (SPEC FR-IO-01): controller traffic, and the configuration protocol. */
#define USBMIDI_CABLE_PERFORMANCE 0u
#define USBMIDI_CABLE_CONFIG 1u
#define USBMIDI_CABLES 2u

#define USBMIDI_CABLE(pkt) ((unsigned)(pkt)[0] >> 4)
#define USBMIDI_CIN(pkt) ((unsigned)(pkt)[0] & 0x0Fu)

/* Packs a channel message (status 0x80-0xEF, 2 or 3 bytes as its type has) for `cable`; false
 * for anything else, and pkt is then untouched. */
bool usbmidi_pack(unsigned cable, const uint8_t *msg, unsigned len, uint8_t pkt[4]);

enum usbmidi_kind {
    USBMIDI_CHANNEL,  /* a channel message, in msg */
    USBMIDI_SYSEX,    /* part of a System Exclusive message (CIN 4-7) */
    USBMIDI_SYSTEM,   /* System Common (CIN 2, 3, 5 without an end of SysEx) */
    USBMIDI_REALTIME, /* a single byte (CIN 15): clock, start, stop... */
    USBMIDI_BAD,      /* a reserved CIN, or bytes that do not match it */
};

/* What a packet carries. For USBMIDI_CHANNEL the message goes into msg (3 bytes; a 2-byte message
 * has msg[2] = 0); msg is untouched otherwise. */
enum usbmidi_kind usbmidi_unpack(const uint8_t pkt[4], uint8_t msg[3]);

/* The length of a channel message by its status byte: 2 for Program Change and Channel
 * Aftertouch, 3 for the rest, 0 for a byte that is not a channel status. */
unsigned usbmidi_channel_len(uint8_t status);

/* The MIDI bytes a packet carries, by its CIN, into out; returns how many (0 for CIN 0 and 1).
 * Bytes are copied as they are: a SysEx's data, its F0 and F7, a channel message, real-time. */
unsigned usbmidi_packet_bytes(const uint8_t pkt[4], uint8_t out[3]);

/* The next packet of a SysEx being sent for `cable`: the first bytes of p[0..n) (the part of the
 * message not yet sent, F0 first, F7 last). CIN 4 with three bytes while the end is further on,
 * CIN 5, 6 or 7 with the bytes up to the F7. Returns how many bytes it took: 0 when n is 0, or
 * when fewer than three bytes are left and none is the F7. */
unsigned usbmidi_sysex_pack(unsigned cable, const uint8_t *p, unsigned n, uint8_t pkt[4]);

#endif /* USBMIDI_H */
