/*
 * A fuzzer for everything a host can send the controller (night plan N-2): the command link's
 * frames over the CDC (COBS) and over SysEx, the configuration protocol's commands (PUT, SET,
 * SELECT, SAVE, REVERT, EVENTS, ...) with mutated payloads, raw bytes, MIDI in over USB and BLE,
 * panel events, and power cuts with the flash's slots damaged. The same wiring as the controller
 * (firmware/app/controller.c) and test_cfglink.c: link -> cfglink -> ctl -> core, a model of NOR
 * flash behind cfgstore and statestore.
 *
 * Checked after every step: the configuration the core runs on passes cfgbin_check() and
 * re-encodes; every reply frame the link writes is whole (CDC: one COBS frame with a good CRC;
 * SysEx: F0 ... F7). Built with AddressSanitizer and UndefinedBehaviorSanitizer by `make fuzz`,
 * which also catches out-of-bounds access and undefined behaviour.
 *
 * The parsers on their own get the same mutations: cfgbin_decode(), cfgbin_patch(),
 * blemidi_rx_packet(), usbmidi_unpack(), usbmidi_packet_bytes(), link_unpack7().
 *
 *   fuzz_device [CASES [SEED]]     CASES 2000 by default; SEED from the clock if not given
 *
 * Exit 0 when every case passed; on a failure it prints the seed and the case to replay.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "blemidi.h"
#include "bleq.h"
#include "cfg_schema.h"
#include "cfgbin.h"
#include "cfglink.h"
#include "cfgstore.h"
#include "ctl.h"
#include "link.h"
#include "smclog.h"
#include "smclog_host.h"
#include "statestore.h"
#include "usbmidi.h"

/* evq.h: the platform's lock; one thread here. */
uint32_t input_port_lock(void) { return 0; }
void input_port_unlock(uint32_t state) { (void)state; }

/* --- randomness --------------------------------------------------------------------------------- */

static uint64_t rng;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}
static uint32_t below(uint32_t n) { return n ? rnd() % n : 0; }
static int chance(unsigned pct) { return below(100) < pct; }

static unsigned long long case_no;
static uint64_t case_seed;

#define FAIL(...)                                                                                  \
    do {                                                                                           \
        fprintf(stderr, "fuzz_device: case %llu (replay: fuzz_device 1 %llu): ", case_no,         \
                (unsigned long long)case_seed);                                                    \
        fprintf(stderr, __VA_ARGS__);                                                              \
        fputc('\n', stderr);                                                                       \
        exit(1);                                                                                   \
    } while (0)

/* --- the controller --------------------------------------------------------------------------- */

static uint32_t now_us;
static uint32_t fake_clock(void) { return now_us; }

static struct core_config CFG;
static struct evq Q;
static struct midiq OUT;
static struct bleq BLEQ;
static struct ctl C;
static struct cfglink CL;
static struct link LK;
static uint8_t BUF[CFGBIN_MAX];

static size_t cfg_get(void *ctx, uint8_t *out, size_t cap, struct cfgbin_error *err)
{
    (void)ctx;
    return ctl_encode_config(&C, out, cap, err);
}
static bool cfg_apply(void *ctx, const uint8_t *blob, uint32_t len)
{
    (void)ctx;
    return ctl_request_config(&C, blob, len);
}
static bool cfg_pending(void *ctx)
{
    (void)ctx;
    return ctl_change_pending(&C);
}
static void cfg_active(void *ctx, uint8_t *source, struct cfgbin_meta *meta)
{
    (void)ctx;
    if (ctl_change_pending(&C))
        return;
    *source = C.source;
    *meta = C.meta;
}
static uint32_t cfg_touched(void *ctx)
{
    (void)ctx;
    return C.last_touch_us;
}
static uint32_t cfg_state(void *ctx)
{
    (void)ctx;
    return ctl_state(&C);
}
static bool cfg_edit(void *ctx, unsigned preset, const uint8_t *rec, uint32_t len, uint32_t revision)
{
    (void)ctx;
    return ctl_request_edit(&C, preset, rec, len, revision);
}
static bool cfg_revert(void *ctx, const uint8_t *blob, uint32_t len)
{
    (void)ctx;
    return ctl_request_revert(&C, blob, len);
}
static bool cfg_autosave(void *ctx)
{
    (void)ctx;
    return C.cfg->global.save == CORE_SAVE_AUTO;
}
static void cfg_panel(void *ctx, uint8_t out[CFGLINK_PANEL_BYTES])
{
    (void)ctx;
    ctl_panel_bytes(ctl_panel(&C), out);
}
static bool cfg_select(void *ctx, int preset, int page)
{
    (void)ctx;
    return ctl_request_select(&C, preset, page);
}
static unsigned cfg_sent(void *ctx, uint32_t from, uint8_t (*out)[CFGLINK_SENT_LEN], unsigned max,
                         uint32_t *first)
{
    (void)ctx;
    return ctl_sent_read(&C, from, out, max, first);
}

static const struct cfglink_ops CFG_OPS = {.get = cfg_get,
                                           .apply = cfg_apply,
                                           .pending = cfg_pending,
                                           .active = cfg_active,
                                           .touched_at = cfg_touched,
                                           .state = cfg_state,
                                           .edit = cfg_edit,
                                           .revert = cfg_revert,
                                           .autosave = cfg_autosave,
                                           .panel = cfg_panel,
                                           .select = cfg_select,
                                           .sent = cfg_sent};
static const struct link_ops OPS = {.config = cfglink_command, .cfg = &CL, .cfg_poll = cfglink_poll};
static const struct ctl_ops CTL_OPS = {0};

/* NOR flash: the slots and the panel's state, kept across power cuts. */
#define AREA (STATESTORE_END - CFGSTORE_FLASH_AT)
static uint8_t FLASH[AREA];
static int fail_programs;
static struct cfgstore ST;
static struct statestore SS;

static bool fl_read(void *ctx, uint32_t addr, uint8_t *buf, uint32_t len)
{
    (void)ctx;
    if (addr < CFGSTORE_FLASH_AT || addr - CFGSTORE_FLASH_AT + len > AREA)
        FAIL("flash read outside the area: %#x + %u", (unsigned)addr, (unsigned)len);
    memcpy(buf, FLASH + (addr - CFGSTORE_FLASH_AT), len);
    return true;
}
static bool fl_erase(void *ctx, uint32_t addr)
{
    (void)ctx;
    if (addr < CFGSTORE_FLASH_AT || addr - CFGSTORE_FLASH_AT + CFGSTORE_SECTOR > AREA || addr % CFGSTORE_SECTOR)
        FAIL("flash erase outside the area: %#x", (unsigned)addr);
    memset(FLASH + (addr - CFGSTORE_FLASH_AT), 0xFF, CFGSTORE_SECTOR);
    return true;
}
static bool fl_program(void *ctx, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    (void)ctx;
    if (addr < CFGSTORE_FLASH_AT || addr - CFGSTORE_FLASH_AT + len > AREA)
        FAIL("flash program outside the area: %#x + %u", (unsigned)addr, (unsigned)len);
    if (fail_programs > 0) {
        fail_programs--;
        return false;
    }
    for (uint32_t i = 0; i < len; i++)
        FLASH[(addr - CFGSTORE_FLASH_AT) + i] &= buf[i];
    return true;
}
static const struct cfgstore_flash FL = {0, fl_read, fl_erase, fl_program};

static bool sysex_mode;

/* What the cases reached, summed over the starts: a fuzzer that never gets past the checks tests
 * nothing, so the run prints these and `make fuzz` wants each above zero. */
static struct {
    unsigned long long starts, stored, loads, edits, selects, decoded, patched;
} reach;

static void count(void)
{
    reach.loads += C.stats.loads;
    reach.edits += C.stats.edits;
    reach.selects += C.stats.selects;
}

static void boot(void)
{
    struct cfgbin_meta meta;
    uint32_t w;

    if (reach.starts++)
        count();

    smclog_host_set_clock(fake_clock);
    smclog_reset();
    cfgstore_init(&ST, &FL);
    cfglink_init(&CL, &CFG_OPS, BUF, sizeof BUF, &ST);
    bool stored = cfglink_load(&CL, &CFG, &meta);
    evq_init(&Q);
    midiq_init(&OUT);
    bleq_init(&BLEQ);
    ctl_init(&C, &CFG, &Q, &OUT, &CTL_OPS);
    ctl_set_ble(&C, &BLEQ);
    if (stored) {
        ctl_set_stored(&C, &meta);
        reach.stored++;
    }
    statestore_init(&SS, &FL);
    if (statestore_load(&SS, &w))
        ctl_restore_state(&C, w);
    cfglink_set_state(&CL, &SS);
    if (sysex_mode)
        link_init_sysex(&LK, &OPS);
    else
        link_init(&LK, &OPS);
}

/* --- checks ----------------------------------------------------------------------------------- */

static uint8_t ENC[CFGBIN_MAX];

static void check_running(const char *after)
{
    struct cfgbin_error err;
    int code = cfgbin_check(C.cfg, &err);
    if (code != CFGBIN_OK)
        FAIL("after %s the running configuration fails its check: %s (preset %u page %u control %u field %u)",
             after, cfgbin_error_name((unsigned)code), err.preset, err.page, err.control, err.field);
    if (!cfgbin_encode(C.cfg, NULL, ENC, sizeof ENC, &err))
        FAIL("after %s the running configuration does not encode: %s", after, cfgbin_error_name(err.code));
}

static uint16_t crc16(const uint8_t *p, size_t n) { return smclog_crc16(p, n); }

/* Every frame the link wrote: CDC frames end with 0 and decode with a good CRC; SysEx frames are
 * F0 7D 53 4D, 7-bit bytes, F7. */
static void check_out(const uint8_t *out, size_t n)
{
    size_t i = 0;
    while (i < n) {
        if (sysex_mode) {
            if (out[i] != 0xF0)
                FAIL("SysEx out: %02x where F0 should start a frame", out[i]);
            size_t j = i + 1;
            while (j < n && out[j] < 0x80)
                j++;
            if (j == n || out[j] != 0xF7)
                FAIL("SysEx out: a frame not ended by F7");
            if (j - i < 1 + LINK_SYSEX_HDR || out[i + 1] != LINK_SYSEX_MFR)
                FAIL("SysEx out: a frame without our header");
            i = j + 1;
            continue;
        }
        const uint8_t *z = memchr(out + i, 0, n - i);
        if (!z)
            FAIL("CDC out: bytes after the last frame's end");
        size_t end = (size_t)(z - out), o = 0, j = i;
        static uint8_t dec[2048];
        while (j < end) {
            uint8_t code = out[j++];
            if (j + code - 1 > end)
                FAIL("CDC out: a COBS code past the frame's end");
            for (int k = 1; k < code; k++)
                dec[o++] = out[j++];
            if (code < 0xFF && j < end)
                dec[o++] = 0;
        }
        if (o < 3 || crc16(dec, o - 2) != (uint16_t)(dec[o - 2] | dec[o - 1] << 8))
            FAIL("CDC out: a frame with a bad CRC (%zu bytes)", o);
        i = end + 1;
    }
}

static uint8_t OUTBUF[8192];

static void drain(void)
{
    for (int k = 0; k < 64; k++) {
        size_t got = link_tx(&LK, OUTBUF, sizeof OUTBUF);
        if (!got)
            return;
        check_out(OUTBUF, got);
    }
}

/* Bytes into the link, draining its replies whenever it stops to answer. */
static void feed(const uint8_t *p, size_t n)
{
    size_t at = 0;
    for (int stuck = 0; at < n && stuck < 4;) {
        size_t took = link_rx(&LK, p + at, n - at);
        at += took;
        stuck = took ? 0 : stuck + 1;
        drain();
    }
}

static void run(void)
{
    now_us += 1000 + below(chance(10) ? 4000000 : 20000);
    ctl_run(&C, now_us);
    cfglink_poll(&CL, now_us);
    check_running("a round");
    if (C.stats.midi_bad)
        FAIL("the core sent %u messages that are not channel messages", (unsigned)C.stats.midi_bad);
    if (chance(90)) {
        while (midiq_peek(&OUT))
            midiq_pop(&OUT);
        while (bleq_peek(&BLEQ))
            bleq_pop(&BLEQ);
    }
    drain();
}

/* --- inputs ----------------------------------------------------------------------------------- */

static uint8_t seq;

/* A command record framed for the link's mode, with a good CRC. */
static size_t frame(uint8_t cmd, const uint8_t *p, size_t n, uint8_t *out, size_t cap)
{
    uint8_t rec[LINK_CMD_MAX + 16];
    if (n > LINK_CMD_MAX + 4)
        n = LINK_CMD_MAX + 4; /* a little past the limit too */
    rec[0] = chance(97) ? LINK_KIND_CMD : (uint8_t)rnd();
    rec[1] = chance(90) ? ++seq : (uint8_t)rnd();
    rec[2] = cmd;
    memcpy(rec + 3, p, n);
    n += 3;
    if (!sysex_mode)
        return smclog_frame(rec, n, out, cap);
    uint16_t crc = crc16(rec, n);
    rec[n] = (uint8_t)crc;
    rec[n + 1] = (uint8_t)(crc >> 8);
    if (chance(2))
        rec[n] ^= 1; /* a bad CRC now and then */
    size_t o = 0;
    out[o++] = 0xF0;
    out[o++] = LINK_SYSEX_MFR;
    out[o++] = LINK_SYSEX_TAG0;
    out[o++] = LINK_SYSEX_TAG1;
    o += link_pack7(rec, n + 2, out + o);
    out[o++] = 0xF7;
    return o;
}

static void send(uint8_t cmd, const uint8_t *p, size_t n)
{
    static uint8_t f[1024];
    size_t len = frame(cmd, p, n, f, sizeof f);
    if (chance(3) && len > 2)
        len = 1 + below((uint32_t)len); /* cut short */
    feed(f, len);
}

static void mutate(uint8_t *p, size_t *n, size_t cap)
{
    unsigned k = 1 + below(chance(20) ? 16 : 3);
    while (k--) {
        switch (below(7)) {
        case 0:
            if (*n)
                p[below((uint32_t)*n)] ^= (uint8_t)(1u << below(8));
            break;
        case 1:
            if (*n)
                p[below((uint32_t)*n)] = (uint8_t)rnd();
            break;
        case 2: /* an interesting value */
            if (*n) {
                static const uint8_t v[] = {0, 1, 0x7F, 0x80, 0xFE, 0xFF, 15, 16, 3, 4};
                p[below((uint32_t)*n)] = v[below(sizeof v)];
            }
            break;
        case 3: /* insert */
            if (*n < cap) {
                size_t at = below((uint32_t)*n + 1);
                memmove(p + at + 1, p + at, *n - at);
                p[at] = (uint8_t)rnd();
                (*n)++;
            }
            break;
        case 4: /* delete */
            if (*n) {
                size_t at = below((uint32_t)*n);
                memmove(p + at, p + at + 1, *n - at - 1);
                (*n)--;
            }
            break;
        case 5: /* truncate */
            if (*n)
                *n = below((uint32_t)*n);
            break;
        default: /* copy a slice over another place */
            if (*n > 4) {
                size_t len = 1 + below((uint32_t)(*n / 2)), from = below((uint32_t)(*n - len)),
                       to = below((uint32_t)(*n - len));
                memmove(p + to, p + from, len);
            }
            break;
        }
    }
}

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

/* Seeds: the factory and test configurations in their binary form. */
static uint8_t SEED[2][CFGBIN_MAX];
static size_t SEED_LEN[2];

static size_t pick_config(uint8_t *out, size_t cap)
{
    unsigned s = below(2);
    size_t n = SEED_LEN[s];
    memcpy(out, SEED[s], n);
    if (chance(70)) {
        mutate(out, &n, cap);
        if (chance(60) && n >= CFGBIN_HEADER) { /* keep the CRC right, so the decoder looks deeper */
            uint32_t crc = cfgbin_crc32(0, out + CFGBIN_HEADER, n - CFGBIN_HEADER);
            (void)crc; /* where the header keeps it is the decoder's business: see fix_crc() */
        }
    }
    return n;
}

/* The header's lengths and CRC made right again after a mutation, as an editor with a bug would
 * still send them (docs/config.md, "Binary form": the CRC covers what follows the header). */
static void fix_header(uint8_t *b, size_t n)
{
    struct cfgbin_error err;
    if (n < CFGBIN_HEADER)
        return;
    /* Try the layout the decoder expects: find the CRC field by asking it. */
    for (size_t at = 4; at + 4 <= CFGBIN_HEADER; at++) {
        uint8_t save[4];
        memcpy(save, b + at, 4);
        put32(b + at, cfgbin_crc32(0, b + CFGBIN_HEADER, n - CFGBIN_HEADER));
        int code = cfgbin_decode(b, n, NULL, NULL, &err);
        if (code != CFGBIN_E_CRC)
            return;
        memcpy(b + at, save, 4);
    }
}

/* SET records: some of a preset's records from a seed, mutated. */
static size_t pick_records(unsigned *preset, uint8_t *out, size_t cap)
{
    unsigned s = below(2);
    const uint8_t *b = SEED[s];
    size_t n = SEED_LEN[s], at = CFGBIN_HEADER, o = 0;
    unsigned want = below(CORE_PRESETS), cur = 0xFF;
    *preset = want;
    while (at + 2 <= n) {
        unsigned tag = b[at], len = b[at + 1];
        if (at + 2 + len > n)
            break;
        if (tag == CFG_TAG_PRESET)
            cur = b[at + 2];
        int take = (tag == CFG_TAG_GLOBAL && chance(20)) || (tag == CFG_TAG_PRESET && cur == want && chance(30)) ||
                   ((tag == CFG_TAG_ASSIGN || tag == CFG_TAG_SHIFT) && cur == want && chance(3));
        if (take && o + 2 + len <= cap) {
            memcpy(out + o, b + at, 2 + len);
            o += 2 + len;
        }
        at += 2 + len;
    }
    if (chance(60))
        mutate(out, &o, cap);
    if (chance(5))
        *preset = below(256);
    return o;
}

static uint8_t BLOB[CFGBIN_MAX + 64];

static void upload(void)
{
    uint8_t p[4 + CFGLINK_CHUNK + 8];
    size_t n = pick_config(BLOB, CFGBIN_MAX + 32);
    if (chance(70))
        fix_header(BLOB, n);
    uint32_t len = chance(95) ? (uint32_t)n : rnd();
    put32(p, len);
    put32(p + 4, chance(95) ? cfgbin_crc32(0, BLOB, n) : rnd());
    send(LINK_CMD_CFG_PUT_BEGIN, p, 8);
    for (size_t at = 0; at < n; at += CFGLINK_CHUNK) {
        size_t k = n - at < CFGLINK_CHUNK ? n - at : CFGLINK_CHUNK;
        if (chance(1))
            k = below(CFGLINK_CHUNK + 8);
        put32(p, chance(98) ? (uint32_t)at : rnd());
        memcpy(p + 4, BLOB + at, k <= n - at ? k : n - at);
        send(LINK_CMD_CFG_PUT_CHUNK, p, 4 + (k <= n - at ? k : n - at));
        if (chance(1))
            run();
    }
    if (chance(95))
        send(chance(97) ? LINK_CMD_CFG_PUT_COMMIT : LINK_CMD_CFG_ABORT, NULL, 0);
}

static void set_edit(void)
{
    uint8_t p[LINK_CMD_MAX + 8];
    unsigned preset;
    size_t n = pick_records(&preset, p + 5, LINK_CMD_MAX - 8);
    put32(p, chance(80) ? C.meta.revision + 1 : rnd());
    p[4] = (uint8_t)preset;
    send(LINK_CMD_CFG_SET, p, 5 + n);
}

static void random_command(void)
{
    static const uint8_t cmds[] = {LINK_CMD_PING,         LINK_CMD_GET_INFO,     LINK_CMD_EVENTS,
                                   LINK_CMD_CFG_INFO,     LINK_CMD_CFG_GET_BEGIN, LINK_CMD_CFG_GET_CHUNK,
                                   LINK_CMD_CFG_ABORT,    LINK_CMD_CFG_SAVE,     LINK_CMD_CFG_REVERT,
                                   LINK_CMD_CFG_STATE,    LINK_CMD_CFG_SELECT,   LINK_CMD_CFG_SENT,
                                   LINK_CMD_LOG_SET_LEVEL, LINK_CMD_LOG_GET_LEVELS, LINK_CMD_INPUT_INJECT,
                                   LINK_CMD_CORE_SETUP,   LINK_CMD_LOG_RAW_INPUT};
    uint8_t p[LINK_CMD_MAX + 8];
    size_t n = below(chance(80) ? 10 : LINK_CMD_MAX + 4);
    for (size_t i = 0; i < n; i++)
        p[i] = chance(50) ? (uint8_t)below(5) : (uint8_t)rnd();
    uint8_t cmd = chance(95) ? cmds[below(sizeof cmds)] : (uint8_t)rnd();
    if (cmd == LINK_CMD_CFG_SELECT && chance(70)) {
        n = 2;
        p[0] = chance(80) ? (uint8_t)below(CORE_PRESETS) : (uint8_t)(chance(50) ? 0xFF : rnd());
        p[1] = chance(80) ? (uint8_t)below(CORE_PAGES) : (uint8_t)(chance(50) ? 0xFF : rnd());
    }
    send(cmd, p, n);
}

static void raw_bytes(void)
{
    uint8_t p[512];
    size_t n = 1 + below(sizeof p);
    for (size_t i = 0; i < n; i++)
        p[i] = chance(20) ? (uint8_t)(sysex_mode ? (chance(50) ? 0xF0 : 0xF7) : 0) : (uint8_t)rnd();
    feed(p, n);
}

static void panel_event(void)
{
    struct core_event ev;
    memset(&ev, 0, sizeof ev);
    ev.t_us = now_us;
    ev.type = (uint8_t)below(6);
    switch (ev.type) {
    case CORE_EV_BUTTON:
        ev.index = (uint8_t)below(chance(98) ? CORE_BUTTONS : 256);
        ev.value = (int16_t)below(2);
        break;
    case CORE_EV_ENCODER:
        ev.index = (uint8_t)below(chance(98) ? CORE_STRIPS : 256);
        ev.value = (int16_t)(chance(90) ? (int)below(9) - 4 : (int16_t)rnd());
        break;
    case CORE_EV_FADER:
        ev.index = (uint8_t)below(chance(98) ? CORE_STRIPS : 256);
        ev.value = (int16_t)(chance(95) ? below(CORE_FADER_MAX + 1) : rnd());
        break;
    case CORE_EV_MIDI_IN: {
        uint8_t pkt[4], msg[3];
        ev.index = (uint8_t)below(CORE_TRANSPORTS);
        for (int i = 0; i < 4; i++)
            pkt[i] = (uint8_t)rnd();
        if (chance(80)) { /* a message the USB parser accepts */
            if (usbmidi_unpack(pkt, msg) == 0)
                return;
            memcpy(ev.midi, msg, 3);
        } else {
            ev.midi[0] = (uint8_t)(0x80 | rnd());
            ev.midi[1] = (uint8_t)(rnd() & 0x7F);
            ev.midi[2] = (uint8_t)(rnd() & 0x7F);
        }
        break;
    }
    case CORE_EV_LINK:
        ev.index = CORE_FROM_BLE;
        ev.value = (int16_t)below(2);
        break;
    default:
        break;
    }
    evq_push(&Q, &ev);
}

/* Time with nobody touching anything: the autosave, SAVE's steps and the panel state's save run
 * to their end. */
static void quiet(void)
{
    for (int k = 0; k < 40; k++) {
        now_us += 250000;
        ctl_run(&C, now_us);
        cfglink_poll(&CL, now_us);
        drain();
    }
    check_running("a quiet time");
}

static void power_cut(void)
{
    if (chance(30)) { /* damage the flash a little, as a cut during a write could */
        unsigned k = 1 + below(8);
        while (k--)
            FLASH[below(AREA)] &= (uint8_t)rnd();
    }
    if (chance(10))
        fail_programs = (int)below(4);
    sysex_mode = chance(50);
    boot();
    check_running("a start");
}

/* --- the parsers on their own ------------------------------------------------------------------ */

static struct core_config SCRATCH;

static void parsers(void)
{
    uint8_t b[CFGBIN_MAX + 64];
    size_t n = pick_config(b, sizeof b - 8);
    if (chance(50))
        fix_header(b, n);
    if (cfgbin_decode(b, n, &SCRATCH, NULL, NULL) == CFGBIN_OK) {
        struct cfgbin_error err;
        reach.decoded++;
        if (cfgbin_check(&SCRATCH, &err) != CFGBIN_OK)
            FAIL("a decoded configuration fails cfgbin_check: %s", cfgbin_error_name(err.code));
    }
    unsigned preset;
    n = pick_records(&preset, b, 1024);
    core_config_factory(&SCRATCH);
    if (cfgbin_patch(b, n, preset, NULL, NULL, NULL, NULL) == CFGBIN_OK) {
        struct cfgbin_error err;
        reach.patched++;
        if (cfgbin_patch(b, n, preset, &SCRATCH, NULL, NULL, &err) != CFGBIN_OK)
            FAIL("a SET that checks fails to apply: %s", cfgbin_error_name(err.code));
        if (cfgbin_check(&SCRATCH, &err) != CFGBIN_OK)
            FAIL("a SET that checks leaves a configuration that does not: %s", cfgbin_error_name(err.code));
    }

    /* BLE-MIDI packets */
    struct blemidi_rx r;
    blemidi_rx_init(&r);
    for (int k = below(4) + 1; k; k--) {
        n = below(chance(90) ? 40 : 300);
        for (size_t i = 0; i < n; i++)
            b[i] = chance(30) ? (uint8_t)(0x80 | rnd()) : (uint8_t)(rnd() & 0x7F);
        static const struct blemidi_sink none = {0};
        blemidi_rx_packet(&r, b, (unsigned)n, &none);
    }

    /* USB-MIDI packets */
    for (int k = 0; k < 16; k++) {
        uint8_t pkt[4], msg[3], bytes[3];
        for (int i = 0; i < 4; i++)
            pkt[i] = (uint8_t)rnd();
        usbmidi_unpack(pkt, msg);
        usbmidi_packet_bytes(pkt, bytes);
    }

    /* 7-bit unpacking */
    n = below(300);
    for (size_t i = 0; i < n; i++)
        b[i] = (uint8_t)(chance(95) ? rnd() & 0x7F : rnd());
    uint8_t out[300];
    link_unpack7(b, n, out, below(sizeof out));
}

/* --- main ------------------------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    unsigned long long cases = argc > 1 ? strtoull(argv[1], NULL, 10) : 2000;
    uint64_t seed = argc > 2 ? strtoull(argv[2], NULL, 10) : (uint64_t)time(NULL);

    core_config_factory(&SCRATCH);
    SEED_LEN[0] = cfgbin_encode(&SCRATCH, NULL, SEED[0], CFGBIN_MAX, NULL);
    core_config_test(&SCRATCH);
    SEED_LEN[1] = cfgbin_encode(&SCRATCH, NULL, SEED[1], CFGBIN_MAX, NULL);
    if (!SEED_LEN[0] || !SEED_LEN[1]) {
        fprintf(stderr, "fuzz_device: the seed configurations do not encode\n");
        return 1;
    }

    memset(FLASH, 0xFF, sizeof FLASH);
    for (case_no = 0; case_no < cases; case_no++) {
        case_seed = seed + case_no;
        rng = case_seed * 0x9E3779B97F4A7C15ull | 1;
        if (case_no == 0 || chance(15))
            power_cut();
        for (unsigned steps = 4 + below(40); steps; steps--) {
            unsigned what = below(100);
            if (what < 15)
                upload();
            else if (what < 35)
                set_edit();
            else if (what < 55)
                random_command();
            else if (what < 62)
                raw_bytes();
            else if (what < 85)
                panel_event();
            else if (what < 88)
                parsers();
            else if (what < 91)
                quiet();
            else
                run();
        }
        run();
    }
    count();
    printf("fuzz_device: %llu cases from seed %llu passed; reached: %llu starts (%llu from a stored "
           "configuration), %llu configurations loaded, %llu live edits, %llu selections, %llu mutated "
           "configurations and %llu SETs that decode\n",
           cases, (unsigned long long)seed, reach.starts, reach.stored, reach.loads, reach.edits,
           reach.selects, reach.decoded, reach.patched);
    if (cases >= 1000 && !(reach.stored && reach.loads && reach.edits && reach.selects && reach.decoded && reach.patched)) {
        fprintf(stderr, "fuzz_device: a path was never reached: the fuzzer needs fixing\n");
        return 1;
    }
    return 0;
}
