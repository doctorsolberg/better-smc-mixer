/* USB-MIDI 1.0 event packets (usbmidi.h). */
#include "usbmidi.h"

unsigned usbmidi_channel_len(uint8_t status)
{
    if (status < 0x80 || status >= 0xF0)
        return 0;
    return (status & 0xF0u) == 0xC0 || (status & 0xF0u) == 0xD0 ? 2 : 3;
}

bool usbmidi_pack(unsigned cable, const uint8_t *msg, unsigned len, uint8_t pkt[4])
{
    unsigned want = usbmidi_channel_len(msg[0]);
    if (!want || len != want || cable > 15 || msg[1] > 0x7F || (len == 3 && msg[2] > 0x7F))
        return false;
    pkt[0] = (uint8_t)(cable << 4 | msg[0] >> 4); /* for channel messages the CIN is the status */
    pkt[1] = msg[0];
    pkt[2] = msg[1];
    pkt[3] = len == 3 ? msg[2] : 0;
    return true;
}

enum usbmidi_kind usbmidi_unpack(const uint8_t pkt[4], uint8_t msg[3])
{
    unsigned cin = USBMIDI_CIN(pkt);

    switch (cin) {
    case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD: case 0xE: {
        unsigned len = usbmidi_channel_len(pkt[1]);
        if (!len || (unsigned)pkt[1] >> 4 != cin || pkt[2] > 0x7F || (len == 3 && pkt[3] > 0x7F))
            return USBMIDI_BAD;
        msg[0] = pkt[1];
        msg[1] = pkt[2];
        msg[2] = len == 3 ? pkt[3] : 0;
        return USBMIDI_CHANNEL;
    }
    case 0x4: case 0x6: case 0x7:
        return USBMIDI_SYSEX; /* start or continue; ends with two or three bytes */
    case 0x5:
        return pkt[1] == 0xF7 ? USBMIDI_SYSEX : USBMIDI_SYSTEM; /* SysEx end, or a 1-byte common */
    case 0x2: case 0x3:
        return USBMIDI_SYSTEM;
    case 0xF:
        return pkt[1] >= 0xF8 ? USBMIDI_REALTIME : USBMIDI_BAD;
    default: /* 0x0, 0x1: reserved for future extensions */
        return USBMIDI_BAD;
    }
}

unsigned usbmidi_packet_bytes(const uint8_t pkt[4], uint8_t out[3])
{
    static const uint8_t len[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};
    unsigned n = len[USBMIDI_CIN(pkt)];

    for (unsigned i = 0; i < n; i++)
        out[i] = pkt[1 + i];
    return n;
}

unsigned usbmidi_sysex_pack(unsigned cable, const uint8_t *p, unsigned n, uint8_t pkt[4])
{
    unsigned k = 0;

    while (k < n && k < 3 && p[k] != 0xF7)
        k++;
    if (k < n && k < 3 && p[k] == 0xF7)
        k++; /* the end in this packet: CIN 5, 6 or 7 by its bytes */
    else if (k < 3)
        return 0; /* the end is not here and three bytes are not either */
    else
        k = 0; /* three bytes, the end further on: CIN 4 */
    unsigned cnt = k ? k : 3;
    pkt[0] = (uint8_t)(cable << 4 | (k ? 4u + k : 4u));
    pkt[1] = p[0];
    pkt[2] = cnt > 1 ? p[1] : 0;
    pkt[3] = cnt > 2 ? p[2] : 0;
    return cnt;
}
