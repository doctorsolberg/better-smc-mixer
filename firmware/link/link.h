/*
 * The command link (docs/link.md): commands from the host, their replies and the log stream over
 * one byte pipe: the USB CDC, or SysEx over USB MIDI and BLE MIDI (SPEC §7, Q-28).
 *
 * On the CDC both directions use the log's frames (docs/logging.md, "Frames": the record, its
 * CRC-16, COBS, a 0x00 delimiter). Over SysEx the same record and CRC-16 are 7-bit packed in
 * F0 7D 53 4D ... F7 (link_init_sysex; docs/link.md, "Over SysEx"), and there is no log stream.
 * The host sends CMD records; the device answers each with a REPLY record and, once the host
 * subscribes (CDC only), streams its log records on the same pipe.
 *
 *   CMD   (host to device)  0x10, seq u8, cmd u8, payload
 *   REPLY (device to host)  0x11, seq u8, cmd u8, status u8, payload
 *
 * Portable C (no platform headers). The transport calls link_rx() with the bytes it received and
 * link_tx() with the room it has; both from one task. Single consumer of the log ring
 * (smclog_drain()).
 */
#ifndef LINK_H
#define LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "smclog.h"

#define LINK_VERSION 1

#define LINK_KIND_CMD 0x10   /* host to device */
#define LINK_KIND_REPLY 0x11 /* device to host */
#define LINK_KIND_EVENT 0x12 /* device to host, after EVENTS on: count u8, the panel's state */
#define LINK_KIND_SENT 0x13  /* device to host, after EVENTS' bit 1: first u32, lost u8, count u8, entries (P-5) */

enum link_cmd {
    LINK_CMD_PING = 0x01,            /* payload (at most LINK_PING_MAX) -> t_us u32, the payload */
    LINK_CMD_GET_INFO = 0x02,        /* -> docs/link.md */
    LINK_CMD_LOG_SUBSCRIBE = 0x20,   /* -> ok; then a stream header and the log records */
    LINK_CMD_LOG_UNSUBSCRIBE = 0x21, /* -> ok; the stream stops */
    LINK_CMD_LOG_SET_LEVEL = 0x22,   /* module u8 (LINK_ALL_MODULES: all), level u8 -> ok */
    LINK_CMD_LOG_GET_LEVELS = 0x23,  /* -> count u8, a level per module */
    LINK_CMD_LOG_RAW_INPUT = 0x24,   /* which u8 (LINK_RAW_* bits, 0: off) -> ok (LOG-07) */
    LINK_CMD_METRICS_NOW = 0x25,     /* -> ok; the metrics are logged now (LOG-06) */
    LINK_CMD_BOOT_LOG = 0x26,        /* -> ok; the boot journal is logged again (LOG-11) */
    LINK_CMD_RESET = 0x27,           /* how u8 (enum link_reset) -> reset status u8 */
    LINK_CMD_EVENTS = 0x28,          /* on u8 (1 on, 0 off) -> ok; then EVENT records (P-2) */
    LINK_CMD_STOCK = 0x29,           /* what u8 (enum link_stock), "BACK" to write -> status, pointer u32, tries */
    LINK_CMD_INPUT_INJECT = 0x30,    /* inject.h payload -> status, accepted, refused (LOG-33) */
    LINK_CMD_CORE_SETUP = 0x31,      /* settings text -> status, bad pair (LOG-35; docs/link.md) */
    /* The configuration protocol (cfglink.h, docs/link.md, "Configuration"): each reply's payload
     * starts with an enum cfglink_status */
    LINK_CMD_CFG_INFO = 0x40,        /* -> versions, limits, transfer state, the running one's ID */
    LINK_CMD_CFG_GET_BEGIN = 0x41,   /* -> length u32, CRC-32 u32 of a snapshot of the running one */
    LINK_CMD_CFG_GET_CHUNK = 0x42,   /* offset u32, count u8 -> offset u32, the bytes */
    LINK_CMD_CFG_PUT_BEGIN = 0x43,   /* length u32, CRC-32 u32 -> ok */
    LINK_CMD_CFG_PUT_CHUNK = 0x44,   /* offset u32, the bytes -> received u32 */
    LINK_CMD_CFG_PUT_COMMIT = 0x45,  /* -> ok (applied in the next round) or why not */
    LINK_CMD_CFG_ABORT = 0x46,       /* -> ok; the transfer is dropped */
    LINK_CMD_CFG_SET = 0x47,         /* revision u32, preset u8, records -> ok (a live edit, P-1) */
    LINK_CMD_CFG_SAVE = 0x48,        /* -> ok; the running configuration is saved now (Q-26) */
    LINK_CMD_CFG_REVERT = 0x49,      /* -> ok; what the mixer starts with replaces it (Q-26) */
    LINK_CMD_CFG_STATE = 0x4A,       /* -> the panel's state (P-2, P-6; docs/link.md) */
    LINK_CMD_CFG_SELECT = 0x4B,      /* preset u8, page u8 (0xFF keeps) -> ok (P-3) */
    LINK_CMD_CFG_SENT = 0x4C,        /* from u32 -> first u32, count u8, entries (P-5) */
};
#define LINK_CMD_CFG_FIRST LINK_CMD_CFG_INFO
#define LINK_CMD_CFG_LAST 0x4F

/* RESET: how. The device answers first and resets about 100 ms later. FAULT and WATCHDOG are
 * debug functions (off until injection is on, LOG-34, SPEC D-9). */
enum link_reset {
    LINK_RESET_SOFT = 0,     /* a soft reset of the chip */
    LINK_RESET_FAULT = 1,    /* a deliberate CPU exception: the crash record's path (LOG-10) */
    LINK_RESET_WATCHDOG = 2, /* hang with interrupts off until the watchdog resets the chip */
};
#define LINK_RESET_MAX LINK_RESET_WATCHDOG

/* RESET's own status (the reply's payload) */
enum link_reset_status {
    LINK_RESET_OK = 0,       /* the reset follows */
    LINK_RESET_DISABLED = 1, /* a debug mode while injection is off */
};

/* STOCK: going back to M-Vave's firmware (store/bootsel.h; docs/link.md). CHECK reads sector 0 and
 * M-Vave's area and writes nothing; BACK (with the 4 bytes "BACK") points sector 0 at M-Vave's area
 * and, when that read back, resets the chip about 100 ms after the reply. The reply: the status
 * (enum bootsel_status's values), where sector 0 points (u32), the writes of sector 0 it took. */
enum link_stock {
    LINK_STOCK_CHECK = 0,
    LINK_STOCK_BACK = 1,
};
#define LINK_STOCK_MAGIC "BACK"
#define LINK_STOCK_REPLY 6u

/* CORE_SETUP's own status (the reply's first payload byte) */
enum link_setup_status {
    LINK_SETUP_OK = 0,       /* taken: the core restarts on it (log: "core start") */
    LINK_SETUP_DISABLED = 1, /* injection is off: debug commands are off (LOG-34) */
    LINK_SETUP_BAD = 2,      /* a pair not understood; the second byte is its number (from 1) */
    LINK_SETUP_BUSY = 3,     /* the previous setup has not been applied yet */
};

enum link_status {
    LINK_OK = 0,
    LINK_UNKNOWN_CMD = 1,
    LINK_BAD_LENGTH = 2,
    LINK_BAD_ARG = 3,
    LINK_UNAVAILABLE = 4, /* this firmware does not offer the command */
};

/* GET_INFO capabilities */
#define LINK_CAP_LOG (1u << 0)
#define LINK_CAP_RAW_INPUT (1u << 1)
#define LINK_CAP_INJECT (1u << 2)
#define LINK_CAP_METRICS (1u << 3)
#define LINK_CAP_SETUP (1u << 4)
#define LINK_CAP_BOOT_LOG (1u << 5)
#define LINK_CAP_RESET (1u << 6)
#define LINK_CAP_CONFIG (1u << 7) /* the configuration protocol */
#define LINK_CAP_EVENTS (1u << 8) /* EVENTS (the configuration protocol's CFG_STATE behind them) */
#define LINK_CAP_SENT (1u << 9)   /* EVENTS' bit 1, SENT records (CFG_SENT behind them; P-5) */
#define LINK_CAP_STOCK (1u << 10) /* STOCK: going back to M-Vave's firmware */

#define LINK_CMD_MAX 192   /* largest CMD record: kind to the end of the payload */
/* SysEx framing (Q-28): F0, the manufacturer 0x7D, the tag "SM", the body 7-bit packed, F7. */
#define LINK_SYSEX_MFR 0x7Du
#define LINK_SYSEX_TAG0 0x53u
#define LINK_SYSEX_TAG1 0x4Du
#define LINK_SYSEX_HDR 3u /* 7D 53 4D, after the F0 */
#define LINK_PACKED_LEN(n) ((n) + ((n) + 6u) / 7u) /* n bytes 7-bit packed */
/* A record of n bytes as a whole SysEx frame: F0, the header, record and CRC packed, F7. */
#define LINK_SYSEX_LEN(n) (1u + LINK_SYSEX_HDR + LINK_PACKED_LEN((n) + 2u) + 1u)
#define LINK_MAX2(a, b) ((a) > (b) ? (a) : (b))
#define LINK_REPLY_MAX 192 /* largest REPLY record (a configuration chunk) */
#define LINK_VERSION_TEXT_MAX 16 /* GET_INFO: the longest version text it carries */
#define LINK_PING_MAX 32   /* PING payload */
#define LINK_ALL_MODULES 0xFF
#define LINK_EVENT_STATE_MAX 32  /* the panel's state an EVENT carries (cfglink.h, CFGLINK_STATE_LEN) */
#define LINK_EVENT_MAX (2u + LINK_EVENT_STATE_MAX) /* an EVENT record: kind, count, the state */
#define LINK_EVENT_GAP_US 40000u /* at most one EVENT per 40 ms per link */
#define LINK_EVENTS_PANEL 1u     /* EVENTS' what: the panel's EVENTs */
#define LINK_EVENTS_SENT 2u      /* EVENTS' what: SENT records */
#define LINK_SENT_ENTRY 7u       /* a SENT entry (cfglink.h, CFGLINK_SENT_LEN) */
#define LINK_SENT_ENTRIES 16u    /* the most in one record (cfglink.h, CFGLINK_SENT_MAX) */
#define LINK_SENT_MAX (7u + LINK_SENT_ENTRIES * LINK_SENT_ENTRY) /* kind, first, lost, count, entries */
#define LINK_SENT_GAP_US 20000u  /* at most one SENT per 20 ms per link */
#define LINK_BUILD_ID_MAX 32

/* LOG_RAW_INPUT: which raw inputs are logged (scan.h keeps the same bits). */
enum {
    LINK_RAW_MATRIX = 1u << 0, /* the button matrix and the encoder lines */
    LINK_RAW_FADERS = 1u << 1, /* the faders' ADC values */
    LINK_RAW_ALL = LINK_RAW_MATRIX | LINK_RAW_FADERS,
    LINK_RAW_PROBE_SHIFT = 4, /* bits 4-7: 1-8 probes that encoder (a diagnostic, scan.h) */
    LINK_RAW_PROBE_MAX = 8,
};

/* What the firmware variant offers. A NULL function makes its command answer LINK_UNAVAILABLE.
 * They are called from link_rx(), in the transport's task. */
struct link_ops {
    void *ctx;
    uint8_t variant; /* GET_INFO: 0 the application, 1 the hardware test */
    const char *version; /* GET_INFO: the product's version (app_version.h); NULL: none */
    void (*raw_input)(void *ctx, uint8_t which); /* LINK_RAW_* bits */
    void (*metrics)(void *ctx);
    void (*boot_log)(void *ctx); /* BOOT_LOG: the boot journal again (diag.h) */
    /* RESET (how checked: at most LINK_RESET_MAX): schedules the reset, which must wait until the
     * reply has reached the host; out: enum link_reset_status */
    void (*reset)(void *ctx, uint8_t how, uint8_t out[1]);
    /* STOCK (what checked, the magic too): the status, the pointer, the tries into out
     * (LINK_STOCK_REPLY bytes); after a BACK that holds, schedules the reset as RESET does */
    void (*stock)(void *ctx, uint8_t what, uint8_t out[LINK_STOCK_REPLY]);
    /* out: inject_reply's status, accepted, refused */
    void (*inject)(void *ctx, uint32_t t_us, const uint8_t *p, size_t len, uint8_t out[3]);
    /* CORE_SETUP: the core on a built-in configuration with settings (core_settings.h, text);
     * out: enum link_setup_status, the number of the pair not understood */
    void (*setup)(void *ctx, const uint8_t *p, size_t len, uint8_t out[2]);
    /* CFG_* (LINK_CMD_CFG_FIRST..LAST), the configuration protocol: cfglink_command() with its
     * struct cfglink as cfg. Writes the reply's payload into out (at most LINK_REPLY_MAX - 4 bytes),
     * adds its length to *len and returns the link status. */
    uint8_t (*config)(void *cfg, uint8_t cmd, const uint8_t *p, size_t n, uint8_t *out, size_t *len);
    void *cfg;
    /* Called by the transport's task every round, with or without a host: cfglink_poll() (the
     * configuration's autosave); NULL: none. Not called by link.c. */
    void (*cfg_poll)(void *cfg, uint32_t now_us);
};

struct link_stats {
    uint32_t frames;     /* frames received */
    uint32_t bad_frames; /* bad COBS or CRC, or too short */
    uint32_t overflows;  /* frames longer than a command can be: dropped */
    uint32_t ignored;    /* good frames that are not commands */
    uint32_t commands;   /* commands executed */
    uint32_t retries;    /* the last command again (same seq and cmd): answered, not executed */
    uint32_t replies;    /* replies handed to the transport */
    uint32_t foreign;    /* SysEx: messages that are not ours (another manufacturer or tag) */
    uint32_t events;     /* EVENT records handed to the transport */
    uint32_t sent;       /* SENT records handed to the transport */
};

struct link {
    const struct link_ops *ops;
    bool sysex; /* SysEx framing (link_init_sysex), else the CDC's COBS frames */
    /* the frame being received: COBS-encoded, or a SysEx's bytes after its F0 */
    uint8_t rx[LINK_MAX2(SMCLOG_FRAME_LEN(LINK_CMD_MAX),
                         LINK_SYSEX_HDR + LINK_PACKED_LEN(LINK_CMD_MAX + 2u))];
    uint16_t rx_len;
    bool rx_skip; /* the frame is too long: dropping it up to the next delimiter */
    bool rx_in;   /* SysEx: inside one (an F0 came) */
    bool last_valid;
    uint8_t last_seq, last_cmd;
    /* the last reply, framed, kept for a retry (its length fits reply_len: checked in link.c) */
    uint8_t reply[LINK_MAX2(SMCLOG_FRAME_LEN(LINK_REPLY_MAX), LINK_SYSEX_LEN(LINK_REPLY_MAX))];
    uint8_t reply_len;
    bool reply_pending; /* not handed to the transport yet */
    bool subscribed, header_pending;
    /* events (EVENTS on): the last state sent, when, and the next EVENT framed until it is out */
    bool events, ev_sent;
    uint8_t ev_count;
    uint8_t ev_state_len;
    uint32_t ev_at;
    uint8_t ev_state[LINK_EVENT_STATE_MAX];
    uint8_t ev[LINK_MAX2(SMCLOG_FRAME_LEN(LINK_EVENT_MAX), LINK_SYSEX_LEN(LINK_EVENT_MAX))];
    uint8_t ev_len;
    /* what the mixer sent (EVENTS' bit 1): the next entry's number, when the last record went,
     * and the next record framed until it is out */
    bool sent_on, sent_any;
    uint32_t sent_next, sent_at;
    uint8_t sx[LINK_MAX2(SMCLOG_FRAME_LEN(LINK_SENT_MAX), LINK_SYSEX_LEN(LINK_SENT_MAX))];
    uint8_t sx_len;
    struct link_stats stats;
    /* scratch, kept out of the transport task's stack: the command decoded, the reply built */
    uint8_t cmd[LINK_CMD_MAX + 2];
    uint8_t out[LINK_REPLY_MAX + 2];
};

void link_init(struct link *l, const struct link_ops *ops);

/* The same link with SysEx framing (docs/link.md, "Over SysEx"): link_rx() takes the SysEx bytes
 * the transport received (other MIDI may be left out; a real-time byte inside a SysEx is skipped,
 * any other status ends it unfinished), link_tx() gives one whole SysEx message per call (a reply
 * or an EVENT: BLE MIDI's sender takes one at a time). Messages without our
 * header are counted (foreign) and ignored. LOG_SUBSCRIBE answers LINK_UNAVAILABLE. */
void link_init_sysex(struct link *l, const struct link_ops *ops);

/* 7-bit packing (Q-28): each group of up to 7 bytes becomes a byte of their top bits (bit i:
 * byte i's) and the bytes with their top bits cleared. Returns LINK_PACKED_LEN(n). */
size_t link_pack7(const uint8_t *in, size_t n, uint8_t *out);

/* The reverse; -1 when a byte has its top bit set, a group is a lone header byte, or the result
 * does not fit in cap. */
int link_unpack7(const uint8_t *in, size_t n, uint8_t *out, size_t cap);

/* Bytes from the host. Returns how many were taken: fewer than n when a command's reply has not
 * been sent yet (one reply at a time). The caller keeps the rest and offers it again after
 * link_tx(). */
size_t link_rx(struct link *l, const uint8_t *p, size_t n);

/* Bytes for the host, at most cap: the pending reply, then the stream header after a subscribe,
 * then an EVENT when one is due (EVENTS on: the panel's state changed, at most one per
 * LINK_EVENT_GAP_US), then log frames while subscribed. Frames are never split: a frame that does
 * not fit waits. */
size_t link_tx(struct link *l, uint8_t *out, size_t cap);

/* The pipe went away (the port was closed, a bus reset): the stream and the events stop, a
 * partial frame and the pending reply are dropped, and the last seq is forgotten. */
void link_reset(struct link *l);

#endif /* LINK_H */
