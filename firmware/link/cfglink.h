/*
 * The configuration protocol over the command link (docs/link.md, "Configuration"; SPEC §7,
 * FR-CFG-02, FR-CFG-03, FR-CFG-05, FR-CFG-07): the host reads the running configuration and
 * replaces it, as its binary form (config/cfgbin.h, docs/config.md, "Binary form"), in chunks.
 *
 * - Download: CFG_GET_BEGIN takes a snapshot of the running configuration (its canonical binary
 *   form, encoded now) and gives its length and CRC-32; CFG_GET_CHUNK reads it at any offset.
 * - Upload: CFG_PUT_BEGIN announces the length and the CRC-32; CFG_PUT_CHUNK sends the bytes in
 *   order; CFG_PUT_COMMIT checks the length, the CRC-32 and then the whole form with
 *   cfgbin_decode(), and only then asks for it to be applied (ops->apply): the control task
 *   decodes it and restarts the core on it between two rounds, as a setup (ctl.h). Until the
 *   commit the running configuration is untouched; an interrupted upload changes nothing.
 * - One transfer at a time, in one buffer: a BEGIN drops the transfer under way. After a commit
 *   the buffer holds the configuration until the control task has taken it (state APPLYING):
 *   meanwhile a BEGIN is BUSY.
 * - The blob is opaque here except for the check before it is applied.
 *
 * - Live edits (P-1): CFG_SET carries records of the binary form for one preset
 *   (cfgbin_patch()), checked here and applied by the control task without a restart (ops->edit).
 *   It touches neither the buffer nor the transfer under way.
 *
 * Storage (SPEC Q-22: two slots of our own; Q-26: saved by hand by default): with a struct
 * cfgstore (store/cfgstore.h), cfglink_load() takes the newest saved configuration at start. A
 * commit or an edit leaves the running configuration unsaved; CFG_SAVE saves it now, and when its
 * global setting `save` is "auto" (ops->autosave) cfglink_poll() saves it by itself once nothing
 * has changed and no control has been touched for CFGLINK_QUIET_US. A save is encoded into the
 * transfer buffer and written step by step (a step per poll, paused while the panel is in use).
 * Meanwhile the state is SAVING; a BEGIN or an edit cancels the save (the slot saved before
 * stays). CFG_REVERT puts back what the mixer starts with: the saved configuration, or the
 * factory one. A configuration from CORE_SETUP (debug) is never saved. Without a store: RAM
 * only.
 *
 * The panel's state (store/statestore.h; Bluetooth, the preset and the page, as ops->state() gives
 * it): with cfglink_set_state(), cfglink_poll() saves it when it differs from the one saved and no
 * control has been touched for CFGLINK_QUIET_US, never while a configuration is being saved (the
 * flash is reached from this task alone).
 *
 * Portable C11, no allocation. Called from the link's task (link_ops.config).
 */
#ifndef CFGLINK_H
#define CFGLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cfgbin.h"
#include "cfgstore.h"
#include "statestore.h"

#define CFGLINK_VERSION 4  /* the configuration protocol's version (CFG_INFO); 2: SET, SAVE, REVERT;
                              3: STATE, SELECT and the link's events; 4: SENT (P-5) */
#define CFGLINK_CHUNK 128  /* the most bytes in one chunk, both ways */

/* The transfer buffer on the device and in devhost (cfglink_init()'s cap; CFG_INFO reports it): a
 * build option (CFG_BUF=n in firmware/app/Makefile). 16 KB takes the factory and the test
 * configurations (7.2 and 8.4 KB, 2026-11-12) with room to spare and leaves the heap 14 KB more
 * than the largest form possible would (CFGBIN_MAX, 30 KB; SPEC §13.1). A larger upload is
 * refused (SIZE). */
#ifndef CFGLINK_BUF
#define CFGLINK_BUF 16384
#endif
/* Autosave: this long after the last commit and the last touch of a control */
#define CFGLINK_QUIET_US 3000000u
#define CFGLINK_SAVE_TRIES 3 /* failed saves in a row before it gives up until the next commit */

/* ops->active()'s source of a configuration received over this protocol: the only one saved
 * (ctl.h's CTL_SOURCE_RECEIVED; the firmware checks they agree) */
#define CFGLINK_SOURCE_RECEIVED 2u
#define CFGLINK_SOURCE_FACTORY 0u
#define CFGLINK_SOURCE_STORED 3u

/* The panel's state (CFG_STATE's reply after its status, and every EVENT; docs/link.md, "The
 * panel's state"): CFGLINK_PANEL_BYTES from ops->panel(), storage u8, revision u32, ID[16] */
#define CFGLINK_PANEL_BYTES 9u
#define CFGLINK_STATE_LEN (CFGLINK_PANEL_BYTES + 1u + 4u + 16u)

/* What the mixer sent (CFG_SENT, SENT records; docs/link.md, "What the mixer sent"): entries of
 * CFGLINK_SENT_LEN bytes (the message, 3 bytes; preset, page, control, layer), at most
 * CFGLINK_SENT_MAX in a reply. */
#define CFGLINK_SENT_LEN 7u
#define CFGLINK_SENT_MAX 16u
#define CFGLINK_SENT_NOW 0xFFFFFFFFu /* CFG_SENT's from: none, and the next number */

#define CFGLINK_BUF_BYTES (CFGLINK_BUF < CFGBIN_MAX ? CFGLINK_BUF : CFGBIN_MAX)

/* The first byte of every CFG_* reply's payload */
enum cfglink_status {
    CFGLINK_OK = 0,
    CFGLINK_BUSY = 1,        /* a configuration change waits for the control task: ask again */
    CFGLINK_NO_TRANSFER = 2, /* a chunk or a commit without its BEGIN (or after an ABORT, a BEGIN) */
    CFGLINK_BAD_OFFSET = 3,  /* not where the transfer stands (the reply says where) */
    CFGLINK_SIZE = 4,        /* more than the device takes, less than a header, or past the length */
    CFGLINK_INCOMPLETE = 5,  /* a commit before every byte arrived */
    CFGLINK_CRC = 6,         /* the bytes are not what PUT_BEGIN's CRC-32 says */
    CFGLINK_INVALID = 7,     /* cfgbin refused it (encoding, for a GET): its error follows */
    CFGLINK_UNAVAILABLE = 8, /* SAVE: no flash storage, or the running configuration is a setup's */
};

/* The transfer's state (CFG_INFO) */
enum cfglink_state {
    CFGLINK_IDLE = 0,
    CFGLINK_GET = 1,      /* a snapshot is there to read */
    CFGLINK_PUT = 2,      /* an upload is under way */
    CFGLINK_APPLYING = 3, /* committed; the control task has not taken it yet */
    CFGLINK_SAVING = 4,   /* the buffer is being written to flash */
};

/* Storage (CFG_INFO) */
enum cfglink_store {
    CFGLINK_STORE_NONE = 0,    /* no flash storage in this firmware */
    CFGLINK_STORE_CLEAN = 1,   /* nothing waits to be saved */
    CFGLINK_STORE_WAITING = 2, /* a received configuration waits for the quiet pause */
    CFGLINK_STORE_WRITING = 3, /* being written */
    CFGLINK_STORE_FAILED = 4,  /* the last save failed (the one saved before stays) */
    CFGLINK_STORE_UNSAVED = 5, /* the running configuration is not the saved one: waits for SAVE */
};

/* What the firmware provides (on the device and in devhost: the control path, ctl.h) */
struct cfglink_ops {
    void *ctx;
    /* The running configuration's binary form into out (at most cap bytes): its length; 0 while a
     * change waits (err->code CFGBIN_OK) or if it does not encode (err says why). */
    size_t (*get)(void *ctx, uint8_t *out, size_t cap, struct cfgbin_error *err);
    /* Apply the checked blob between rounds; it stays unchanged until pending() is false. False:
     * a change waits already. */
    bool (*apply)(void *ctx, const uint8_t *blob, uint32_t len);
    /* Whether a change (an applied configuration, a setup) still waits for the control task. */
    bool (*pending)(void *ctx);
    /* The running configuration: where it came from (enum ctl_source with its flag) and its ID
     * and revision. */
    void (*active)(void *ctx, uint8_t *source, struct cfgbin_meta *meta);
    /* The log clock's time (µs) of the last touch of a control, 0 if none yet. */
    uint32_t (*touched_at)(void *ctx);
    /* The panel's state to keep (ctl.h, ctl_state()); NULL: none kept. */
    uint32_t (*state)(void *ctx);
    /* A live edit (CFG_SET; ctl_request_edit()): checked records for a preset, copied. False: a
     * change waits already. NULL: no live edits. */
    bool (*edit)(void *ctx, unsigned preset, const uint8_t *rec, uint32_t len, uint32_t revision);
    /* CFG_REVERT (ctl_request_revert()): the saved configuration blob, checked, or NULL for the
     * factory one; blob stays unchanged until pending() is false. False: a change waits. */
    bool (*revert)(void *ctx, const uint8_t *blob, uint32_t len);
    /* Whether the running configuration saves by itself (its global setting `save`, Q-26); NULL:
     * always. */
    bool (*autosave)(void *ctx);
    /* The panel as a client follows it (ctl_panel_bytes()): the first CFGLINK_PANEL_BYTES bytes of
     * the state. NULL: no CFG_STATE (unavailable), and so no events. */
    void (*panel)(void *ctx, uint8_t out[CFGLINK_PANEL_BYTES]);
    /* CFG_SELECT (ctl_request_select()): preset and page, 0-based, -1 keeps; false: out of range.
     * NULL: unavailable. */
    bool (*select)(void *ctx, int preset, int page);
    /* CFG_SENT (ctl_sent_read()): at most max entries from number from on (CFGLINK_SENT_NOW: none),
     * the first one's number in *first. NULL: unavailable. */
    unsigned (*sent)(void *ctx, uint32_t from, uint8_t (*out)[CFGLINK_SENT_LEN], unsigned max,
                     uint32_t *first);
};

struct cfglink_stats {
    uint32_t gets;     /* snapshots taken */
    uint32_t puts;     /* uploads begun */
    uint32_t commits;  /* uploads handed over to be applied */
    uint32_t refused;  /* commits and edits refused: incomplete, CRC, invalid */
    uint32_t edits;    /* live edits handed over */
    uint32_t reverts;
    uint32_t selects;
    uint32_t saved;    /* saves to flash completed */
    uint32_t save_failed;
    uint32_t state_saved;  /* the panel's state written */
    uint32_t state_failed;
};

struct cfglink {
    const struct cfglink_ops *ops;
    uint8_t *buf; /* the transfer buffer: CFGBIN_MAX takes every configuration */
    uint32_t cap;
    uint8_t state; /* enum cfglink_state */
    uint32_t len;  /* the snapshot's or the announced length */
    uint32_t crc;  /* its CRC-32 (cfgbin_crc32) */
    uint32_t got;  /* PUT: bytes received, in order */
    struct cfgstore *store; /* NULL: RAM only */
    bool dirty;             /* a received configuration is not saved yet */
    bool save_now;          /* SAVE asked for a save: no quiet pause, the setting aside */
    bool manual;            /* the save under way is SAVE's */
    bool heard;             /* a transfer command came since the last poll (not INFO, ABORT) */
    uint32_t quiet_from;    /* the poll's time of the last one (log clock, µs) */
    uint8_t store_status;   /* enum cfglink_store */
    uint8_t save_tries;     /* failed saves in a row */
    struct statestore *state_store; /* NULL: the panel's state is not kept */
    uint32_t state_saved;   /* the state in flash (or the one at start, when none is) */
    uint32_t state_tried;   /* the state the failed saves were for */
    uint8_t state_tries;    /* failed saves of it in a row */
    struct cfglink_stats stats;
};

/* store may be NULL (no flash storage). */
void cfglink_init(struct cfglink *c, const struct cfglink_ops *ops, uint8_t *buf, uint32_t cap,
                  struct cfgstore *store);

/* At start, before the control path runs: the newest saved configuration that cfgbin takes,
 * decoded into cfg with its ID and revision. False if there is none (cfg is then the factory
 * configuration, written here). Uses the transfer buffer. */
bool cfglink_load(struct cfglink *c, struct core_config *cfg, struct cfgbin_meta *meta);

/* After cfglink_init() and once the state is restored (ctl_restore_state()): keep the panel's
 * state in s (loaded already, statestore_load()); what ops->state() gives now counts as saved. */
void cfglink_set_state(struct cfglink *c, struct statestore *s);

/* link_ops.cfg_poll, from the link's task every round (also with no host): the autosave. */
void cfglink_poll(void *cfg, uint32_t now_us);

/* link_ops.config: one CFG_* command (cfg is the struct cfglink). */
uint8_t cfglink_command(void *cfg, uint8_t cmd, const uint8_t *p, size_t n, uint8_t *out, size_t *len);

#endif /* CFGLINK_H */
