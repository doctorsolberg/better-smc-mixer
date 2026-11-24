/*
 * Host tests of the configuration protocol (firmware/link/cfglink.c, docs/link.md,
 * "Configuration"): commands framed and sent through the command link (link.c), the control path
 * (ctl.c) and the core behind it on the factory configuration, wired as the controller wires them.
 * Replies are decoded here from the format in docs/link.md. The autosave (cfglink_poll()) runs on
 * a model of NOR flash (store/cfgstore.c; its power-cut tests are test_cfgstore.c).
 * Run: make -C firmware test
 */
#include <stdio.h>
#include <string.h>

#include "cfg_schema.h"
#include "cfgbin.h"
#include "cfglink.h"
#include "cfgstore.h"
#include "check.h"
#include "ctl.h"
#include "link.h"
#include "smclog.h"
#include "smclog_host.h"

int check_failed;

/* evq.h: the platform's lock; one thread here. */
uint32_t input_port_lock(void) { return 0; }
void input_port_unlock(uint32_t state) { (void)state; }

static uint32_t now_us;
static uint32_t fake_clock(void) { return now_us; }

static struct core_config CFG;
static struct evq Q;
static struct midiq OUT;
static struct ctl C;
static struct cfglink CL;
static struct link LK;
static uint8_t BUF[CFGBIN_MAX];
static uint8_t seq;

/* --- the controller's wiring (firmware/app/controller.c) -------------------------------------- */

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
/* a host of the protocol without the panel (as before version 3) */
static const struct cfglink_ops CFG_OPS_V2 = {.get = cfg_get,
                                              .apply = cfg_apply,
                                              .pending = cfg_pending,
                                              .active = cfg_active,
                                              .touched_at = cfg_touched,
                                              .state = cfg_state,
                                              .edit = cfg_edit,
                                              .revert = cfg_revert,
                                              .autosave = cfg_autosave};
/* a host of protocol 1: uploads only, no live edit, no going back */
static const struct cfglink_ops CFG_OPS_V1 = {.get = cfg_get,
                                              .apply = cfg_apply,
                                              .pending = cfg_pending,
                                              .active = cfg_active,
                                              .touched_at = cfg_touched,
                                              .state = cfg_state};
static const struct link_ops OPS = {.config = cfglink_command, .cfg = &CL, .cfg_poll = cfglink_poll};
static const struct link_ops BARE = {0};
static const struct ctl_ops CTL_OPS = {0};

/* --- NOR flash (as devhost's --flash) ---------------------------------------------------------- */

#define AREA (STATESTORE_END - CFGSTORE_FLASH_AT) /* the slots and the panel's state */
static uint8_t FLASH[AREA];
static int fail_programs; /* programs left to fail */
static struct cfgstore ST;
static struct statestore SS;

static bool fl_read(void *ctx, uint32_t addr, uint8_t *buf, uint32_t len)
{
    (void)ctx;
    memcpy(buf, FLASH + (addr - CFGSTORE_FLASH_AT), len);
    return true;
}

static bool fl_erase(void *ctx, uint32_t addr)
{
    (void)ctx;
    memset(FLASH + (addr - CFGSTORE_FLASH_AT), 0xFF, CFGSTORE_SECTOR);
    return true;
}

static bool fl_program(void *ctx, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    (void)ctx;
    if (fail_programs > 0) {
        fail_programs--;
        return false;
    }
    for (uint32_t i = 0; i < len; i++)
        FLASH[(addr - CFGSTORE_FLASH_AT) + i] &= buf[i];
    return true;
}

static const struct cfgstore_flash FL = {0, fl_read, fl_erase, fl_program};

/* A start as the controller's: the store, the saved configuration if any, the control path. */
static bool boot(const struct link_ops *ops, bool with_store)
{
    struct cfgbin_meta meta;
    smclog_host_set_clock(fake_clock);
    now_us = 1000;
    smclog_reset();
    if (with_store)
        cfgstore_init(&ST, &FL);
    memset(BUF, 0xEE, sizeof BUF);
    cfglink_init(&CL, &CFG_OPS, BUF, sizeof BUF, with_store ? &ST : NULL);
    bool stored = cfglink_load(&CL, &CFG, &meta);
    evq_init(&Q);
    midiq_init(&OUT);
    ctl_init(&C, &CFG, &Q, &OUT, &CTL_OPS);
    if (stored)
        ctl_set_stored(&C, &meta);
    if (with_store) {
        uint32_t w;
        statestore_init(&SS, &FL);
        if (statestore_load(&SS, &w))
            ctl_restore_state(&C, w);
        cfglink_set_state(&CL, &SS);
    }
    link_init(&LK, ops);
    return stored;
}

static void start(const struct link_ops *ops)
{
    smclog_host_set_clock(fake_clock);
    now_us = 1000;
    smclog_reset();
    core_config_factory(&CFG);
    evq_init(&Q);
    midiq_init(&OUT);
    ctl_init(&C, &CFG, &Q, &OUT, &CTL_OPS);
    memset(BUF, 0xEE, sizeof BUF);
    cfglink_init(&CL, &CFG_OPS, BUF, sizeof BUF, NULL);
    link_init(&LK, ops);
}

/* --- frames ----------------------------------------------------------------------------------- */

static unsigned le16(const uint8_t *p) { return (unsigned)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return le16(p) | (uint32_t)le16(p + 2) << 16; }

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t c = 0xFFFF;
    while (n--) {
        c ^= (uint16_t)(*p++ << 8);
        for (int k = 0; k < 8; k++)
            c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}

/* The last reply: kind, seq, cmd, link status, payload. */
static uint8_t R[256];
static size_t RLEN;

/* Sends one command (the next seq, or the last one again) and decodes the one reply. Returns the
 * link status, 0xFF if no single valid reply came. */
static unsigned send(bool again, uint8_t cmd, const uint8_t *p, size_t n)
{
    uint8_t rec[LINK_CMD_MAX + 8], frame[400], out[1024], dec[300];

    if (!again)
        seq++;
    rec[0] = LINK_KIND_CMD;
    rec[1] = seq;
    rec[2] = cmd;
    memcpy(rec + 3, p, n);
    size_t len = smclog_frame(rec, 3 + n, frame, sizeof frame);
    if (link_rx(&LK, frame, len) != len)
        return 0xFF;
    size_t got = link_tx(&LK, out, sizeof out);
    if (!got || out[got - 1] != 0)
        return 0xFF;
    size_t o = 0, j = 0, end = got - 1;
    if (memchr(out, 0, end))
        return 0xFF; /* more than one frame */
    while (j < end) {
        uint8_t code = out[j++];
        if (code == 0 || j + code - 1 > end)
            return 0xFF;
        for (int k = 1; k < code; k++)
            dec[o++] = out[j++];
        if (code < 0xFF && j < end)
            dec[o++] = 0;
    }
    if (o < 6 || crc16(dec, o - 2) != le16(dec + o - 2))
        return 0xFF;
    RLEN = o - 2;
    memcpy(R, dec, RLEN);
    if (R[0] != LINK_KIND_REPLY || R[1] != seq || R[2] != cmd || RLEN > LINK_REPLY_MAX)
        return 0xFF;
    return R[3];
}

static unsigned call(uint8_t cmd, const uint8_t *p, size_t n) { return send(false, cmd, p, n); }

/* The configuration status of the last reply (its first payload byte). */
static unsigned cst(void) { return RLEN > 4 ? R[4] : 0xFF; }

/* --- helpers ---------------------------------------------------------------------------------- */

static size_t encode(const struct core_config *cfg, uint32_t revision, uint8_t idbyte, uint8_t *out)
{
    struct cfgbin_meta meta;
    memset(&meta, 0, sizeof meta);
    memset(meta.id, idbyte, sizeof meta.id);
    meta.revision = revision;
    return cfgbin_encode(cfg, &meta, out, CFGBIN_MAX, NULL);
}

static unsigned put_begin(uint32_t len, uint32_t crc)
{
    uint8_t p[8];
    put32(p, len);
    put32(p + 4, crc);
    CHECK_EQ(call(LINK_CMD_CFG_PUT_BEGIN, p, 8), LINK_OK);
    return cst();
}

static unsigned put_chunk(uint32_t at, const uint8_t *b, size_t n)
{
    uint8_t p[4 + CFGLINK_CHUNK + 8];
    put32(p, at);
    memcpy(p + 4, b, n);
    CHECK_EQ(call(LINK_CMD_CFG_PUT_CHUNK, p, 4 + n), LINK_OK);
    return cst();
}

/* A whole upload, chunk by chunk; returns the commit's status. */
static unsigned upload(const uint8_t *blob, size_t len)
{
    CHECK_EQ(put_begin((uint32_t)len, cfgbin_crc32(0, blob, len)), CFGLINK_OK);
    for (size_t at = 0; at < len; at += CFGLINK_CHUNK) {
        size_t n = len - at < CFGLINK_CHUNK ? len - at : CFGLINK_CHUNK;
        CHECK_EQ(put_chunk((uint32_t)at, blob + at, n), CFGLINK_OK);
        CHECK_EQ(le32(R + 5), at + n);
    }
    CHECK_EQ(call(LINK_CMD_CFG_PUT_COMMIT, NULL, 0), LINK_OK);
    return cst();
}

/* A whole download into out; returns its length (0 on an error). */
static size_t download(uint8_t *out)
{
    if (call(LINK_CMD_CFG_GET_BEGIN, NULL, 0) != LINK_OK || cst() != CFGLINK_OK || RLEN != 4 + 9)
        return 0;
    uint32_t len = le32(R + 5), crc = le32(R + 9);
    for (uint32_t at = 0; at < len;) {
        uint8_t p[5];
        put32(p, at);
        p[4] = CFGLINK_CHUNK;
        if (call(LINK_CMD_CFG_GET_CHUNK, p, 5) != LINK_OK || cst() != CFGLINK_OK || le32(R + 5) != at)
            return 0;
        size_t n = RLEN - 9;
        if (n == 0 || n > CFGLINK_CHUNK)
            return 0;
        memcpy(out + at, R + 9, n);
        at += (uint32_t)n;
    }
    return cfgbin_crc32(0, out, len) == crc ? len : 0;
}

static void run(void)
{
    now_us += 10000;
    ctl_run(&C, now_us);
}

/* CFG_INFO: state and source */
static void info(unsigned *state, unsigned *source, uint32_t *revision, uint8_t *id0)
{
    CHECK_EQ(call(LINK_CMD_CFG_INFO, NULL, 0), LINK_OK);
    CHECK_EQ(RLEN, 4 + 37);
    *state = R[4 + 10];
    *source = R[4 + 11];
    *revision = le32(R + 4 + 12);
    *id0 = R[4 + 16];
}

/* --- tests ------------------------------------------------------------------------------------ */

static void test_info(void)
{
    start(&OPS);
    CHECK_EQ(call(LINK_CMD_GET_INFO, NULL, 0), LINK_OK);
    CHECK(le32(R + 7) & LINK_CAP_CONFIG);
    CHECK_EQ(call(LINK_CMD_CFG_INFO, NULL, 0), LINK_OK);
    CHECK_EQ(RLEN, 4 + 37);
    const uint8_t *o = R + 4;
    CHECK_EQ(o[0], CFGLINK_OK);
    CHECK_EQ(o[1], CFGLINK_VERSION);
    CHECK_EQ(o[2], CFG_BINARY_VERSION);
    CHECK_EQ(le16(o + 3), CFG_SCHEMA_VERSION);
    CHECK_EQ(le32(o + 5), CFGBIN_MAX);
    CHECK_EQ(o[9], CFGLINK_CHUNK);
    CHECK_EQ(o[10], CFGLINK_IDLE);
    CHECK_EQ(o[11], CTL_SOURCE_FACTORY);
    CHECK_EQ(le32(o + 12), 0);
    for (int i = 0; i < 16; i++)
        CHECK_EQ(o[16 + i], 0);
    CHECK_EQ(o[32], CFGLINK_STORE_NONE); /* no flash */
    CHECK_EQ(le32(o + 33), 0);

    /* without the protocol: unavailable, and no capability */
    start(&BARE);
    CHECK_EQ(call(LINK_CMD_GET_INFO, NULL, 0), LINK_OK);
    CHECK(!(le32(R + 7) & LINK_CAP_CONFIG));
    CHECK_EQ(call(LINK_CMD_CFG_INFO, NULL, 0), LINK_UNAVAILABLE);
    CHECK_EQ(call(LINK_CMD_CFG_PUT_COMMIT, NULL, 0), LINK_UNAVAILABLE);
    CHECK_EQ(call(LINK_CMD_CFG_LAST, NULL, 0), LINK_UNAVAILABLE);
}

static void test_get(void)
{
    static uint8_t want[CFGBIN_MAX], got[CFGBIN_MAX];
    start(&OPS);
    size_t n = encode(&CFG, 0, 0, want);
    CHECK(n > CFGBIN_HEADER);
    CHECK_EQ(download(got), n);
    CHECK(memcmp(got, want, n) == 0);
    CHECK_EQ(CL.stats.gets, 1);

    /* the end: no bytes; past it: bad offset; a short count is honoured */
    uint8_t p[5];
    put32(p, (uint32_t)n);
    p[4] = 10;
    CHECK_EQ(call(LINK_CMD_CFG_GET_CHUNK, p, 5), LINK_OK);
    CHECK_EQ(cst(), CFGLINK_OK);
    CHECK_EQ(RLEN, 4 + 5);
    put32(p, (uint32_t)n + 1);
    CHECK_EQ(call(LINK_CMD_CFG_GET_CHUNK, p, 5), LINK_OK);
    CHECK_EQ(cst(), CFGLINK_BAD_OFFSET);
    put32(p, 3);
    p[4] = 7;
    CHECK_EQ(call(LINK_CMD_CFG_GET_CHUNK, p, 5), LINK_OK);
    CHECK_EQ(RLEN, 4 + 5 + 7);
    CHECK(memcmp(R + 9, want + 3, 7) == 0);
    p[4] = 255; /* more than a chunk: a chunk */
    CHECK_EQ(call(LINK_CMD_CFG_GET_CHUNK, p, 5), LINK_OK);
    CHECK_EQ(RLEN, 4 + 5 + CFGLINK_CHUNK);

    /* no snapshot after an abort */
    CHECK_EQ(call(LINK_CMD_CFG_ABORT, NULL, 0), LINK_OK);
    CHECK_EQ(cst(), CFGLINK_OK);
    CHECK_EQ(call(LINK_CMD_CFG_GET_CHUNK, p, 5), LINK_OK);
    CHECK_EQ(cst(), CFGLINK_NO_TRANSFER);

    /* bad lengths are the link's */
    CHECK_EQ(call(LINK_CMD_CFG_GET_CHUNK, p, 4), LINK_BAD_LENGTH);
    CHECK_EQ(call(LINK_CMD_CFG_GET_BEGIN, p, 1), LINK_BAD_LENGTH);
    CHECK_EQ(call(LINK_CMD_CFG_INFO, p, 1), LINK_BAD_LENGTH);
    CHECK_EQ(call(LINK_CMD_CFG_LAST, NULL, 0), LINK_UNKNOWN_CMD);
}

/* An upload replaces the running configuration between rounds with its ID and the next revision. */
static void test_put_applies(void)
{
    static struct core_config test_cfg;
    static uint8_t blob[CFGBIN_MAX], back[CFGBIN_MAX];
    unsigned state, source;
    uint32_t revision;
    uint8_t id0;

    start(&OPS);
    core_config_test(&test_cfg);
    size_t n = encode(&test_cfg, 42, 0xA5, blob);
    CHECK(n > 2 * CFGLINK_CHUNK);
    CHECK_EQ(upload(blob, n), CFGLINK_OK);
    CHECK_EQ(CL.stats.commits, 1);

    /* not applied yet: the running configuration is the factory one; everything waits */
    info(&state, &source, &revision, &id0);
    CHECK_EQ(state, CFGLINK_APPLYING);
    CHECK_EQ(source, CTL_SOURCE_FACTORY);
    CHECK_EQ(C.stats.loads, 0);
    CHECK_EQ(put_begin((uint32_t)n, 0), CFGLINK_BUSY);
    CHECK_EQ(call(LINK_CMD_CFG_GET_BEGIN, NULL, 0), LINK_OK);
    CHECK_EQ(cst(), CFGLINK_BUSY);
    CHECK_EQ(call(LINK_CMD_CFG_ABORT, NULL, 0), LINK_OK);
    CHECK_EQ(cst(), CFGLINK_BUSY);
    CHECK_EQ(call(LINK_CMD_CFG_PUT_COMMIT, NULL, 0), LINK_OK);
    CHECK_EQ(cst(), CFGLINK_BUSY);
    struct core_settings s;
    core_settings_init(&s, CORE_CONFIG_FACTORY);
    CHECK(!ctl_request_setup(&C, &s));

    run();
    CHECK_EQ(C.stats.loads, 1);
    info(&state, &source, &revision, &id0);
    CHECK_EQ(state, CFGLINK_IDLE);
    CHECK_EQ(source, CTL_SOURCE_RECEIVED);
    CHECK_EQ(revision, 1);
    CHECK_EQ(id0, 0xA5);
    CHECK(memcmp(&CFG, &test_cfg, sizeof CFG) == 0);

    /* the download gives it back with the mixer's revision and a matching CRC */
    CHECK_EQ(download(back), n);
    CHECK_EQ(encode(&test_cfg, 1, 0xA5, blob), n);
    CHECK(memcmp(back, blob, n) == 0);

    /* the core runs on it: M1 on the test configuration is note 24 */
    struct core_event ev;
    memset(&ev, 0, sizeof ev);
    ev.t_us = now_us;
    ev.type = CORE_EV_BUTTON;
    ev.index = 0;
    ev.value = 1;
    CHECK(evq_push(&Q, &ev));
    run();
    const uint8_t *pkt = midiq_peek(&OUT);
    CHECK(pkt && pkt[1] == 0x90 && pkt[2] == 24);

    /* a setup afterwards: a built-in configuration, no ID */
    core_settings_init(&s, CORE_CONFIG_FACTORY);
    CHECK(core_settings_add(&s, "note_off", "8n"));
    CHECK(ctl_request_setup(&C, &s));
    run();
    info(&state, &source, &revision, &id0);
    CHECK_EQ(source, CTL_SOURCE_FACTORY | CTL_SOURCE_SETTINGS);
    CHECK_EQ(revision, 2);
    CHECK_EQ(id0, 0);
}

/* Faults in an upload leave the running configuration as it was. */
static void test_put_refused(void)
{
    static uint8_t blob[CFGBIN_MAX], bad[CFGBIN_MAX], back[CFGBIN_MAX], before[CFGBIN_MAX];
    static struct core_config test_cfg;
    start(&OPS);
    size_t nb = encode(&CFG, 0, 0, before);
    core_config_test(&test_cfg);
    size_t n = encode(&test_cfg, 3, 1, blob);

    /* sizes */
    CHECK_EQ(put_begin(CFGBIN_MAX + 1, 0), CFGLINK_SIZE);
    CHECK_EQ(put_begin(CFGBIN_HEADER - 1, 0), CFGLINK_SIZE);
    CHECK_EQ(put_chunk(0, blob, 8), CFGLINK_NO_TRANSFER);
    CHECK_EQ(call(LINK_CMD_CFG_PUT_COMMIT, NULL, 0), LINK_OK);
    CHECK_EQ(cst(), CFGLINK_NO_TRANSFER);

    /* order: a chunk out of place says where the upload stands; a retry is not taken twice */
    uint32_t crc = cfgbin_crc32(0, blob, n);
    CHECK_EQ(put_begin((uint32_t)n, crc), CFGLINK_OK);
    CHECK_EQ(put_chunk(0, blob, CFGLINK_CHUNK), CFGLINK_OK);
    uint8_t p[4 + CFGLINK_CHUNK + 1] = {0}; /* one more: the "too long" chunk below */
    put32(p, 0);
    memcpy(p + 4, blob, CFGLINK_CHUNK);
    CHECK_EQ(send(true, LINK_CMD_CFG_PUT_CHUNK, p, sizeof p), LINK_OK); /* the same seq: a retry */
    CHECK_EQ(cst(), CFGLINK_OK);
    CHECK_EQ(le32(R + 5), CFGLINK_CHUNK);
    CHECK_EQ(put_chunk(0, blob, CFGLINK_CHUNK), CFGLINK_BAD_OFFSET); /* a new seq: out of place */
    CHECK_EQ(le32(R + 5), CFGLINK_CHUNK);
    CHECK_EQ(put_chunk(2 * CFGLINK_CHUNK, blob, 1), CFGLINK_BAD_OFFSET);
    CHECK_EQ(call(LINK_CMD_CFG_PUT_COMMIT, NULL, 0), LINK_OK);
    CHECK_EQ(cst(), CFGLINK_INCOMPLETE);
    put32(p, CFGLINK_CHUNK);
    CHECK_EQ(call(LINK_CMD_CFG_PUT_CHUNK, p, 4 + CFGLINK_CHUNK + 1), LINK_BAD_LENGTH); /* too long */
    CHECK_EQ(call(LINK_CMD_CFG_PUT_CHUNK, p, 4), LINK_BAD_LENGTH);                      /* no bytes */
    CHECK_EQ(call(LINK_CMD_CFG_PUT_BEGIN, p, 7), LINK_BAD_LENGTH);

    /* past the announced length */
    CHECK_EQ(put_begin(CFGBIN_HEADER, crc), CFGLINK_OK);
    CHECK_EQ(put_chunk(0, blob, CFGBIN_HEADER + 1), CFGLINK_SIZE);
    CHECK_EQ(le32(R + 5), 0);

    /* a wrong CRC-32: refused, and the upload is over */
    CHECK_EQ(put_begin((uint32_t)n, crc ^ 1), CFGLINK_OK);
    for (size_t at = 0; at < n; at += CFGLINK_CHUNK)
        CHECK_EQ(put_chunk((uint32_t)at, blob + at, n - at < CFGLINK_CHUNK ? n - at : CFGLINK_CHUNK),
                 CFGLINK_OK);
    CHECK_EQ(call(LINK_CMD_CFG_PUT_COMMIT, NULL, 0), LINK_OK);
    CHECK_EQ(cst(), CFGLINK_CRC);
    CHECK_EQ(call(LINK_CMD_CFG_PUT_COMMIT, NULL, 0), LINK_OK);
    CHECK_EQ(cst(), CFGLINK_NO_TRANSFER);

    /* not a valid configuration: cfgbin's error comes back */
    memcpy(bad, blob, n);
    bad[0] = 'X'; /* the magic */
    CHECK_EQ(upload(bad, n), CFGLINK_INVALID);
    CHECK_EQ(RLEN, 4 + 1 + 9);
    CHECK_EQ(R[5], CFGBIN_E_MAGIC);
    memcpy(bad, blob, n);
    bad[n - 1] ^= 0x40; /* a value: the blob's own CRC no longer holds */
    CHECK_EQ(upload(bad, n), CFGLINK_INVALID);
    CHECK_EQ(R[5], CFGBIN_E_CRC);
    CHECK_EQ(CL.stats.refused, 4);
    CHECK_EQ(CL.stats.commits, 0);

    /* a new BEGIN drops the upload under way */
    CHECK_EQ(put_begin((uint32_t)n, crc), CFGLINK_OK);
    CHECK_EQ(put_chunk(0, blob, CFGLINK_CHUNK), CFGLINK_OK);
    CHECK_EQ(download(back), nb);
    CHECK_EQ(put_chunk(CFGLINK_CHUNK, blob + CFGLINK_CHUNK, CFGLINK_CHUNK), CFGLINK_NO_TRANSFER);

    /* nothing reached the core */
    run();
    CHECK_EQ(C.stats.loads, 0);
    CHECK_EQ(download(back), nb);
    CHECK(memcmp(back, before, nb) == 0);
}

/* A commit while a setup waits is BUSY and keeps the upload: committed again later. */
static void test_commit_while_setup(void)
{
    static uint8_t blob[CFGBIN_MAX];
    static struct core_config test_cfg;
    struct core_settings s;
    start(&OPS);
    core_config_test(&test_cfg);
    size_t n = encode(&test_cfg, 9, 2, blob);
    core_settings_init(&s, CORE_CONFIG_FACTORY);
    CHECK(ctl_request_setup(&C, &s));
    CHECK_EQ(upload(blob, n), CFGLINK_BUSY);
    CHECK_EQ(call(LINK_CMD_CFG_GET_BEGIN, NULL, 0), LINK_OK); /* the setup may rewrite it: busy */
    CHECK_EQ(cst(), CFGLINK_BUSY);
    run(); /* the setup */
    CHECK_EQ(C.stats.setups, 1);
    /* GET_BEGIN above did not drop the upload: it was busy before it began */
    CHECK_EQ(call(LINK_CMD_CFG_PUT_COMMIT, NULL, 0), LINK_OK);
    CHECK_EQ(cst(), CFGLINK_OK);
    run();
    CHECK_EQ(C.stats.loads, 1);
    CHECK_EQ(C.meta.revision, 2);
}

/* --- autosave ---------------------------------------------------------------------------------- */

/* The store's status and saved generation from CFG_INFO */
static unsigned store_status(uint32_t *gen)
{
    CHECK_EQ(call(LINK_CMD_CFG_INFO, NULL, 0), LINK_OK);
    if (gen)
        *gen = le32(R + 4 + 33);
    return R[4 + 32];
}

/* The USB task's rounds for ms milliseconds: a poll every 10 ms; the control task's too. */
static void wait_ms(unsigned ms)
{
    for (unsigned t = 0; t < ms; t += 10) {
        run();
        cfglink_poll(&CL, now_us);
    }
}

static void touch(void)
{
    struct core_event ev;
    memset(&ev, 0, sizeof ev);
    ev.t_us = now_us;
    ev.type = CORE_EV_ENCODER;
    ev.index = 0;
    ev.value = 1;
    CHECK(evq_push(&Q, &ev));
    run();
}

/* The test configuration, saving by itself (Q-26: the setting `save`). */
static void auto_cfg(struct core_config *cfg)
{
    core_config_test(cfg);
    cfg->global.save = CORE_SAVE_AUTO;
}

static void received(uint32_t revision, uint8_t idbyte, uint8_t *blob, size_t *n)
{
    static struct core_config test_cfg;
    auto_cfg(&test_cfg);
    *n = encode(&test_cfg, revision, idbyte, blob);
    CHECK_EQ(upload(blob, *n), CFGLINK_OK);
    run();
    CHECK_EQ(C.source, CTL_SOURCE_RECEIVED);
}

/* A received configuration is saved after the quiet pause and is there at the next start. */
static void test_autosave(void)
{
    static uint8_t blob[CFGBIN_MAX], back[CFGBIN_MAX];
    static struct core_config test_cfg;
    unsigned state, source;
    uint32_t revision, gen;
    uint8_t id0;
    size_t n;

    memset(FLASH, 0xFF, sizeof FLASH);
    CHECK(!boot(&OPS, true)); /* nothing saved: the factory configuration */
    CHECK_EQ(C.source, CTL_SOURCE_FACTORY);
    CHECK_EQ(store_status(&gen), CFGLINK_STORE_CLEAN);
    CHECK_EQ(gen, 0);
    wait_ms(5000);
    CHECK_EQ(ST.stats.erases, 0); /* nothing to save */

    received(42, 0xA5, blob, &n);
    CHECK_EQ(store_status(NULL), CFGLINK_STORE_WAITING);
    wait_ms(2500);
    CHECK_EQ(ST.stats.erases, 0); /* not before 3 s */
    touch();                      /* the panel in use: 3 s from here */
    wait_ms(2500);
    CHECK_EQ(ST.stats.erases, 0);
    CHECK_EQ(CL.state, CFGLINK_IDLE);
    wait_ms(520);
    CHECK_EQ(CL.state, CFGLINK_SAVING);
    CHECK_EQ(store_status(NULL), CFGLINK_STORE_WRITING);
    info(&state, &source, &revision, &id0);
    CHECK_EQ(state, CFGLINK_SAVING);

    /* touched while writing: it pauses */
    touch();
    uint32_t pages = ST.stats.pages, erases = ST.stats.erases;
    wait_ms(2000);
    CHECK_EQ(ST.stats.pages, pages);
    CHECK_EQ(ST.stats.erases, erases);
    wait_ms(2000);
    CHECK_EQ(CL.state, CFGLINK_IDLE);
    CHECK_EQ(store_status(&gen), CFGLINK_STORE_CLEAN);
    CHECK_EQ(gen, 1);
    CHECK_EQ(CL.stats.saved, 1);
    wait_ms(5000);
    CHECK_EQ(CL.stats.saved, 1); /* once */

    /* the next start: the saved one, with its ID and revision */
    CHECK(boot(&OPS, true));
    auto_cfg(&test_cfg);
    CHECK(memcmp(&CFG, &test_cfg, sizeof CFG) == 0);
    info(&state, &source, &revision, &id0);
    CHECK_EQ(source, CTL_SOURCE_STORED);
    CHECK_EQ(revision, 1);
    CHECK_EQ(id0, 0xA5);
    CHECK_EQ(store_status(&gen), CFGLINK_STORE_CLEAN);
    CHECK_EQ(gen, 1);
    CHECK_EQ(download(back), n);
    CHECK_EQ(encode(&test_cfg, 1, 0xA5, blob), n);
    CHECK(memcmp(back, blob, n) == 0);

    /* a newer one goes to the other slot; the start takes the newer */
    received(43, 0xA6, blob, &n);
    wait_ms(4000);
    CHECK_EQ(store_status(&gen), CFGLINK_STORE_CLEAN);
    CHECK_EQ(gen, 2);
    CHECK_EQ(ST.target, 1);
    CHECK(boot(&OPS, true));
    CHECK_EQ(C.meta.revision, 2);

    /* without a store: the factory configuration, nothing saved */
    CHECK(!boot(&OPS, false));
    CHECK_EQ(C.source, CTL_SOURCE_FACTORY);
}

/* A transfer while saving cancels the save; it starts over after the next pause. A setup before the
 * save: nothing is saved. */
static void test_save_interrupted(void)
{
    static uint8_t blob[CFGBIN_MAX], back[CFGBIN_MAX];
    struct core_settings s;
    uint32_t gen;
    size_t n;

    memset(FLASH, 0xFF, sizeof FLASH);
    boot(&OPS, true);
    received(7, 1, blob, &n);
    wait_ms(3010);
    CHECK_EQ(CL.state, CFGLINK_SAVING);
    CHECK(download(back) == n); /* cancels the save */
    CHECK_EQ(store_status(&gen), CFGLINK_STORE_WAITING);
    CHECK_EQ(gen, 0);
    CHECK_EQ(call(LINK_CMD_CFG_ABORT, NULL, 0), LINK_OK);
    wait_ms(4000);
    CHECK_EQ(store_status(&gen), CFGLINK_STORE_CLEAN);
    CHECK_EQ(gen, 1);

    /* a setup replaces it before the save: not saved */
    received(8, 2, blob, &n);
    core_settings_init(&s, CORE_CONFIG_FACTORY);
    CHECK(ctl_request_setup(&C, &s));
    wait_ms(4000);
    CHECK_EQ(store_status(&gen), CFGLINK_STORE_CLEAN);
    CHECK_EQ(gen, 1);
    CHECK(boot(&OPS, true));
    CHECK_EQ(C.meta.revision, 1);

    /* an abandoned download snapshot does not hold the save off */
    received(9, 3, blob, &n);
    CHECK_EQ(call(LINK_CMD_CFG_GET_BEGIN, NULL, 0), LINK_OK);
    CHECK_EQ(CL.state, CFGLINK_GET);
    wait_ms(4000);
    CHECK_EQ(store_status(&gen), CFGLINK_STORE_CLEAN);
    CHECK_EQ(gen, 2);
}

/* Failed saves: tried again after a pause, given up after CFGLINK_SAVE_TRIES; the saved one stays. */
static void test_save_fails(void)
{
    static uint8_t blob[CFGBIN_MAX];
    uint32_t gen;
    size_t n;

    memset(FLASH, 0xFF, sizeof FLASH);
    boot(&OPS, true);
    received(1, 1, blob, &n);
    wait_ms(4000);
    CHECK_EQ(store_status(&gen), CFGLINK_STORE_CLEAN);

    received(2, 2, blob, &n);
    fail_programs = 1;
    wait_ms(4000);
    CHECK_EQ(CL.stats.save_failed, 1);
    CHECK_EQ(store_status(&gen), CFGLINK_STORE_WAITING); /* again later */
    wait_ms(4000);
    CHECK_EQ(store_status(&gen), CFGLINK_STORE_CLEAN);
    CHECK_EQ(gen, 2);

    received(3, 3, blob, &n);
    fail_programs = 1000;
    wait_ms(20000);
    CHECK_EQ(CL.stats.save_failed, 1 + CFGLINK_SAVE_TRIES);
    CHECK_EQ(store_status(&gen), CFGLINK_STORE_FAILED);
    CHECK_EQ(gen, 2);
    CHECK(!CL.dirty);
    fail_programs = 0;
    CHECK(boot(&OPS, true));
    CHECK_EQ(C.meta.revision, 2);
}

/* CFG_SET: revision, preset, records. The configuration status. */
static unsigned set_cmd(uint32_t revision, unsigned preset, const uint8_t *rec, size_t n)
{
    uint8_t p[LINK_CMD_MAX];
    put32(p, revision);
    p[4] = (uint8_t)preset;
    memcpy(p + 5, rec, n);
    CHECK_EQ(call(LINK_CMD_CFG_SET, p, 5 + n), LINK_OK);
    return cst();
}

static void event(uint8_t type, uint8_t index, int16_t value)
{
    struct core_event ev;
    memset(&ev, 0, sizeof ev);
    ev.t_us = now_us;
    ev.type = type;
    ev.index = index;
    ev.value = value;
    CHECK(evq_push(&Q, &ev));
    run();
}

/* The next packet the core queued, or NULL. */
static const uint8_t *next_packet(void)
{
    static uint8_t pkt[4];
    const uint8_t *p = midiq_peek(&OUT);
    if (!p)
        return NULL;
    memcpy(pkt, p, 4);
    midiq_pop(&OUT);
    return pkt;
}

/* A live edit (P-1): applied between rounds without a restart, the core's state kept. */
static void test_edit(void)
{
    unsigned state, source;
    uint32_t revision;
    uint8_t id0;
    uint16_t v0, v;

    start(&OPS);
    core_select(&C.core, 0, 1); /* preset 1 page 2 */
    event(CORE_EV_FADER, 0, 8192);
    const struct core_assign *fa = &CFG.preset[0].page[1][0].fader.normal;
    uint32_t key = core_target_key(fa, core_channel(&CFG, 0, fa));
    CHECK(core_target_get(&C.core, key, &v0));
    while (next_packet())
        ;

    /* encoder 1 of page 2 (control 8) to CC 77 */
    const uint8_t rec[] = {3, 7, 1, 8, 0x05, 0x00, 1, 77, 0};
    CHECK_EQ(set_cmd(5, 0, rec, sizeof rec), CFGLINK_OK);
    CHECK(ctl_change_pending(&C));
    info(&state, &source, &revision, &id0);
    CHECK_EQ(state, CFGLINK_APPLYING);
    CHECK_EQ(call(LINK_CMD_CFG_STATE, NULL, 0), LINK_OK);
    CHECK_EQ(cst(), CFGLINK_BUSY); /* no transient false identity in an EVENT */
    CHECK_EQ(set_cmd(6, 0, rec, sizeof rec), CFGLINK_BUSY); /* the first waits for the control task */
    CHECK_EQ(call(LINK_CMD_CFG_GET_BEGIN, NULL, 0), LINK_OK);
    CHECK_EQ(cst(), CFGLINK_BUSY);
    run();
    CHECK_EQ(C.stats.edits, 1);
    CHECK_EQ(C.stats.loads, 0);
    CHECK_EQ(CL.stats.edits, 1);
    CHECK_EQ(CFG.preset[0].page[1][0].encoder.normal.number, 77);
    CHECK_EQ(C.core.page, 1); /* no restart: the page, the fader's target value stay */
    CHECK(core_target_get(&C.core, key, &v));
    CHECK_EQ(v, v0);
    CHECK(!next_packet()); /* nothing sent */
    info(&state, &source, &revision, &id0);
    CHECK_EQ(state, CFGLINK_IDLE);
    CHECK_EQ(source, CTL_SOURCE_RECEIVED); /* the factory configuration, changed */
    CHECK_EQ(revision, 1);
    event(CORE_EV_ENCODER, 0, 1);
    const uint8_t *pkt = next_packet();
    CHECK(pkt && (pkt[1] & 0xF0) == 0xB0 && pkt[2] == 77);

    /* refused: preset 5, a broken record; too short; nothing applied */
    CHECK_EQ(set_cmd(7, 4, rec, sizeof rec), CFGLINK_INVALID);
    CHECK_EQ(R[5], CFGBIN_E_WHERE);
    CHECK_EQ(set_cmd(7, 0, rec, sizeof rec - 1), CFGLINK_INVALID);
    CHECK_EQ(R[5], CFGBIN_E_RECORD);
    CHECK_EQ(call(LINK_CMD_CFG_SET, rec, 6), LINK_BAD_LENGTH);
    CHECK(!ctl_change_pending(&C));
    CHECK_EQ(CL.stats.refused, 2);
    CHECK_EQ(C.meta.revision, 1);

    /* a note held while its button changes: its Note Off goes at the edit, the release sends nothing */
    const struct core_assign *m1 = &CFG.preset[0].page[1][0].button[CORE_BTN_M].normal;
    CHECK_EQ(m1->msg, CORE_MSG_NOTE);
    unsigned note = m1->number;
    event(CORE_EV_BUTTON, 0, 1);
    pkt = next_packet();
    CHECK(pkt && (pkt[1] & 0xF0) == 0x90 && pkt[2] == note && pkt[3] > 0);
    const uint8_t to_cc[] = {3, 7, 1, 16, 0x05, 0x00, 1, 20, 0}; /* M1 of page 2: CC 20 */
    CHECK_EQ(set_cmd(5, 0, to_cc, sizeof to_cc), CFGLINK_OK); /* the same client revision */
    run();
    CHECK_EQ(C.meta.revision, 2);
    pkt = next_packet();
    CHECK(pkt && pkt[2] == note && ((pkt[1] & 0xF0) == 0x80 || pkt[3] == 0));
    CHECK(!next_packet());
    event(CORE_EV_BUTTON, 0, 0);
    CHECK(!next_packet());
    CHECK_EQ(C.stats.loads, 0);

    /* a setup's configuration stays a setup's when edited (never saved) */
    struct core_settings s;
    core_settings_init(&s, CORE_CONFIG_TEST);
    CHECK(ctl_request_setup(&C, &s));
    run();
    CHECK_EQ(set_cmd(9, 0, rec, sizeof rec), CFGLINK_OK);
    run();
    CHECK_EQ(C.source, CTL_SOURCE_TEST);
    CHECK_EQ(C.meta.revision, 4);
}

static unsigned simple(uint8_t cmd)
{
    CHECK_EQ(call(cmd, NULL, 0), LINK_OK);
    return cst();
}

/* Q-26: saved by hand by default (SAVE), by itself with the setting `save` "auto"; REVERT puts back
 * what the mixer starts with. */
static void test_save_revert(void)
{
    static uint8_t blob[CFGBIN_MAX];
    static struct core_config test_cfg, saved;
    uint32_t gen;

    /* no storage: nothing to save; REVERT is the factory configuration */
    start(&OPS);
    CHECK_EQ(simple(LINK_CMD_CFG_SAVE), CFGLINK_UNAVAILABLE);
    core_config_test(&test_cfg);
    size_t n = encode(&test_cfg, 3, 1, blob);
    CHECK_EQ(upload(blob, n), CFGLINK_OK);
    run();
    CHECK_EQ(simple(LINK_CMD_CFG_REVERT), CFGLINK_OK);
    run();
    CHECK_EQ(C.source, CTL_SOURCE_FACTORY);
    CHECK_EQ(C.stats.loads, 2);
    CHECK_EQ(C.meta.revision, 2);

    /* manual (the default): an upload waits for SAVE */
    memset(FLASH, 0xFF, sizeof FLASH);
    CHECK(!boot(&OPS, true));
    CHECK_EQ(simple(LINK_CMD_CFG_SAVE), CFGLINK_OK); /* the factory one: nothing to save */
    CHECK_EQ(store_status(&gen), CFGLINK_STORE_CLEAN);
    CHECK_EQ(C.meta.revision, 0);
    CHECK_EQ(simple(LINK_CMD_CFG_REVERT), CFGLINK_OK); /* nothing saved: the factory one again */
    run();
    CHECK_EQ(C.source, CTL_SOURCE_FACTORY);
    n = encode(&test_cfg, 10, 0xB1, blob);
    CHECK_EQ(upload(blob, n), CFGLINK_OK);
    run();
    CHECK_EQ(store_status(&gen), CFGLINK_STORE_UNSAVED);
    wait_ms(6000);
    CHECK_EQ(ST.stats.erases, 0); /* not by itself */
    CHECK_EQ(store_status(&gen), CFGLINK_STORE_UNSAVED);
    CHECK_EQ(simple(LINK_CMD_CFG_SAVE), CFGLINK_OK);
    CHECK_EQ(store_status(&gen), CFGLINK_STORE_UNSAVED); /* the next poll begins it */
    wait_ms(10);
    CHECK_EQ(CL.state, CFGLINK_SAVING); /* at once: no quiet pause */
    CHECK_EQ(store_status(&gen), CFGLINK_STORE_WRITING);
    CHECK_EQ(simple(LINK_CMD_CFG_SAVE), CFGLINK_OK); /* under way: nothing more */
    wait_ms(1000);
    CHECK_EQ(store_status(&gen), CFGLINK_STORE_CLEAN);
    CHECK_EQ(gen, 1);
    CHECK_EQ(CL.stats.saved, 1);

    CHECK_EQ(C.meta.revision, 2); /* revert + upload; SAVE did not change it */

    /* an edit: unsaved; REVERT gives the saved one back with its ID and a new revision */
    const uint8_t rec[] = {3, 7, 0, 8, 0x05, 0x00, 1, 77, 0};
    CHECK_EQ(set_cmd(11, 0, rec, sizeof rec), CFGLINK_OK);
    run();
    CHECK_EQ(store_status(&gen), CFGLINK_STORE_UNSAVED);
    CHECK_EQ(CFG.preset[0].page[0][0].encoder.normal.number, 77);
    saved = test_cfg;
    CHECK(memcmp(&CFG, &saved, sizeof CFG) != 0);
    CHECK_EQ(simple(LINK_CMD_CFG_REVERT), CFGLINK_OK);
    CHECK_EQ(store_status(&gen), CFGLINK_STORE_CLEAN);
    CHECK_EQ(set_cmd(12, 0, rec, sizeof rec), CFGLINK_BUSY); /* until the control task has it */
    run();
    CHECK(memcmp(&CFG, &saved, sizeof CFG) == 0);
    CHECK_EQ(C.source, CTL_SOURCE_STORED);
    CHECK_EQ(C.meta.revision, 4);
    CHECK_EQ(C.meta.id[0], 0xB1);
    wait_ms(5000);
    CHECK_EQ(CL.stats.saved, 1);
    CHECK(boot(&OPS, true));
    CHECK_EQ(C.meta.revision, 2); /* REVERT does not rewrite flash; boot loads its saved revision */

    /* the setting "auto", by an edit of the global settings: saved by itself after the pause */
    const uint8_t autosave[] = {1, 2, 0x40, CORE_SAVE_AUTO};
    CHECK_EQ(set_cmd(13, 0, autosave, sizeof autosave), CFGLINK_OK);
    run();
    CHECK_EQ(CFG.global.save, CORE_SAVE_AUTO);
    CHECK_EQ(store_status(&gen), CFGLINK_STORE_WAITING);
    wait_ms(2000);
    CHECK_EQ(CL.stats.saved, 0); /* since the boot above */
    wait_ms(2000);
    CHECK_EQ(store_status(&gen), CFGLINK_STORE_CLEAN);
    CHECK_EQ(gen, 2);
    /* back to manual: unsaved until SAVE (the flash still says auto) */
    const uint8_t manual[] = {1, 1, 0x00};
    CHECK_EQ(set_cmd(14, 0, manual, sizeof manual), CFGLINK_OK);
    run();
    CHECK_EQ(store_status(&gen), CFGLINK_STORE_UNSAVED);
    wait_ms(5000);
    CHECK_EQ(gen, 2);
    CHECK_EQ(simple(LINK_CMD_CFG_SAVE), CFGLINK_OK);
    wait_ms(1000);
    CHECK_EQ(store_status(&gen), CFGLINK_STORE_CLEAN);
    CHECK_EQ(gen, 3);
    CHECK(boot(&OPS, true));
    CHECK_EQ(CFG.global.save, CORE_SAVE_MANUAL);
    CHECK_EQ(C.meta.revision, 4);

    /* a failed SAVE is tried again at once, then FAILED; SAVE again starts over */
    CHECK_EQ(set_cmd(15, 0, rec, sizeof rec), CFGLINK_OK);
    run();
    fail_programs = 1000;
    CHECK_EQ(simple(LINK_CMD_CFG_SAVE), CFGLINK_OK);
    wait_ms(2000);
    CHECK_EQ(store_status(&gen), CFGLINK_STORE_FAILED);
    CHECK_EQ(CL.stats.save_failed, CFGLINK_SAVE_TRIES);
    fail_programs = 0;
    CHECK_EQ(simple(LINK_CMD_CFG_SAVE), CFGLINK_OK);
    wait_ms(1000);
    CHECK_EQ(store_status(&gen), CFGLINK_STORE_CLEAN);
    CHECK_EQ(gen, 4);

    /* a setup's configuration is never saved */
    struct core_settings s;
    core_settings_init(&s, CORE_CONFIG_TEST);
    CHECK(ctl_request_setup(&C, &s));
    run();
    CHECK_EQ(simple(LINK_CMD_CFG_SAVE), CFGLINK_UNAVAILABLE);
}

/* The panel's state: saved once it changed and the panel is quiet, restored at the next start. */
/* A host without live edits or REVERT says so to a client that asks for them (coverage, N-12). */
static void test_without_edit(void)
{
    const uint8_t rec[] = {3, 7, 0, 8, 0x05, 0x00, 1, 77, 0};

    start(&OPS);
    cfglink_init(&CL, &CFG_OPS_V1, BUF, sizeof BUF, NULL);
    CHECK_EQ(set_cmd(1, 0, rec, sizeof rec), CFGLINK_UNAVAILABLE);
    CHECK_EQ(simple(LINK_CMD_CFG_REVERT), CFGLINK_UNAVAILABLE);
    CHECK_EQ(CFG.preset[0].page[0][0].encoder.normal.number, 102); /* the factory one, unchanged */
    CHECK_EQ(C.stats.loads, 0);
}

static void test_state(void)
{
    memset(FLASH, 0xFF, sizeof FLASH);
    boot(&OPS, true);
    CHECK_EQ(C.core.preset, 0);
    touch();
    core_select(&C.core, 2, 1); /* preset 3 page 2 */
    wait_ms(1000);
    CHECK_EQ(CL.stats.state_saved, 0); /* touched a second ago */
    wait_ms(2500);
    CHECK_EQ(CL.stats.state_saved, 1);
    wait_ms(5000);
    CHECK_EQ(CL.stats.state_saved, 1); /* nothing new */
    /* the next start: as left */
    boot(&OPS, true);
    CHECK_EQ(C.core.preset, 2);
    CHECK_EQ(C.core.page, 1);
    wait_ms(4000);
    CHECK_EQ(CL.stats.state_saved, 0); /* what was loaded is not saved again */
    /* a failed write: tried again after the next change, the state before stays meanwhile */
    touch();
    core_select(&C.core, 0, 3);
    fail_programs = CFGLINK_SAVE_TRIES;
    wait_ms(4000);
    CHECK_EQ(CL.stats.state_failed, CFGLINK_SAVE_TRIES);
    CHECK_EQ(CL.stats.state_saved, 0);
    boot(&OPS, true);
    CHECK_EQ(C.core.preset, 2);
    core_select(&C.core, 0, 3);
    wait_ms(4000);
    CHECK_EQ(CL.stats.state_saved, 1);
    boot(&OPS, true);
    CHECK_EQ(C.core.preset, 0);
    CHECK_EQ(C.core.page, 3);
}

/* CFG_STATE and CFG_SELECT (P-2, P-3, P-6), and the link's EVENTs on them */
static void test_panel(void)
{
    static const uint8_t at_start[CFGLINK_PANEL_BYTES] = {0, 0, 0, 2, 0, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t blob[CFGBIN_MAX];

    start(&OPS);
    CHECK_EQ(call(LINK_CMD_CFG_STATE, NULL, 0), LINK_OK);
    CHECK_EQ(cst(), CFGLINK_OK);
    CHECK_EQ(RLEN, 5 + CFGLINK_STATE_LEN);
    CHECK(memcmp(R + 5, at_start, sizeof at_start) == 0);
    CHECK_EQ(R[5 + 9], CFGLINK_STORE_NONE);
    CHECK_EQ(le32(R + 5 + 10), 0);
    CHECK_EQ(R[5 + 14], 0);
    CHECK_EQ(call(LINK_CMD_CFG_STATE, (const uint8_t *)"x", 1), LINK_BAD_LENGTH);

    /* SELECT: applied by the control task in its next round */
    const uint8_t sel[2] = {2, 1}, keep[2] = {0xFF, 0xFF}, bad[2] = {4, 0}, bad_page[2] = {0, 4};
    CHECK_EQ(call(LINK_CMD_CFG_SELECT, sel, 2), LINK_OK);
    CHECK_EQ(cst(), CFGLINK_OK);
    CHECK_EQ(RLEN, 5);
    CHECK_EQ(call(LINK_CMD_CFG_STATE, NULL, 0), LINK_OK);
    CHECK(R[5] == 0 && R[6] == 0);
    run();
    CHECK_EQ(call(LINK_CMD_CFG_STATE, NULL, 0), LINK_OK);
    CHECK(R[5] == 2 && R[6] == 1);
    CHECK_EQ(call(LINK_CMD_CFG_SELECT, keep, 2), LINK_OK);
    CHECK_EQ(call(LINK_CMD_CFG_SELECT, bad, 2), LINK_BAD_ARG);
    CHECK_EQ(call(LINK_CMD_CFG_SELECT, bad_page, 2), LINK_BAD_ARG);
    CHECK_EQ(call(LINK_CMD_CFG_SELECT, sel, 1), LINK_BAD_LENGTH);
    CHECK_EQ(CL.stats.selects, 2);
    CHECK(!CL.heard); /* not transfer commands: the autosave does not wait for them */

    /* the running configuration's revision and ID (P-6) */
    size_t n = encode(&CFG, 9, 0x5A, blob);
    CHECK_EQ(upload(blob, n), CFGLINK_OK);
    run();
    CHECK_EQ(call(LINK_CMD_CFG_STATE, NULL, 0), LINK_OK);
    CHECK_EQ(le32(R + 5 + 10), 1);
    CHECK(R[5 + 14] == 0x5A && R[5 + 29] == 0x5A);
    CHECK(R[5] == 0 && R[6] == 0); /* a new configuration restarts the core: preset 1 page 1 */

    /* Another client changes the configuration with the same ID and current revision. */
    static struct core_config other;
    core_config_factory(&other);
    other.preset[0].channel = 4;
    n = encode(&other, 1, 0x5A, blob);
    CHECK_EQ(upload(blob, n), CFGLINK_OK);
    run();
    CHECK_EQ(call(LINK_CMD_CFG_STATE, NULL, 0), LINK_OK);
    CHECK_EQ(le32(R + 5 + 10), 2);
    CHECK(R[5 + 14] == 0x5A && R[5 + 29] == 0x5A);

    /* a touch: counted, with where it was */
    struct core_event ev = {.t_us = now_us, .type = CORE_EV_ENCODER, .index = 3, .value = 1};
    CHECK(evq_push(&Q, &ev));
    run();
    CHECK_EQ(call(LINK_CMD_CFG_STATE, NULL, 0), LINK_OK);
    CHECK(R[5 + 4] == 1 && R[5 + 5] == 0 && R[5 + 6] == 0 && R[5 + 7] == 11 && R[5 + 8] == 0);

    /* SENT (P-5): what that touch sent, with its source; "now"; nothing past the newest */
    uint8_t from[4] = {0, 0, 0, 0};
    CL.heard = false; /* the upload above was heard */
    CHECK_EQ(call(LINK_CMD_CFG_SENT, from, 4), LINK_OK);
    CHECK_EQ(cst(), CFGLINK_OK);
    CHECK(le32(R + 5) == 0 && R[9] >= 1 && RLEN == 10u + R[9] * CFGLINK_SENT_LEN);
    CHECK((R[10] & 0xF0) == 0xB0 && R[13] == 0 && R[14] == 0 && R[15] == 11 && R[16] == 0);
    uint32_t sent_n = R[9];
    memset(from, 0xFF, 4);
    CHECK_EQ(call(LINK_CMD_CFG_SENT, from, 4), LINK_OK);
    CHECK(le32(R + 5) == sent_n && R[9] == 0 && RLEN == 10);
    CHECK_EQ(call(LINK_CMD_CFG_SENT, from, 3), LINK_BAD_LENGTH);
    CHECK(!CL.heard); /* not a transfer command either */

    /* without the panel: STATE and SELECT unavailable, and EVENTS too */
    cfglink_init(&CL, &CFG_OPS_V2, BUF, sizeof BUF, NULL);
    CHECK_EQ(call(LINK_CMD_CFG_STATE, NULL, 0), LINK_OK);
    CHECK_EQ(cst(), CFGLINK_UNAVAILABLE);
    CHECK_EQ(call(LINK_CMD_CFG_SELECT, sel, 2), LINK_OK);
    CHECK_EQ(cst(), CFGLINK_UNAVAILABLE);
    CHECK_EQ(call(LINK_CMD_CFG_SENT, from, 4), LINK_OK);
    CHECK_EQ(cst(), CFGLINK_UNAVAILABLE);
    const uint8_t on = 1, sent = 2;
    CHECK_EQ(call(LINK_CMD_EVENTS, &on, 1), LINK_UNAVAILABLE);
    CHECK_EQ(call(LINK_CMD_EVENTS, &sent, 1), LINK_UNAVAILABLE);
}

int main(void)
{
    RUN(test_info);
    RUN(test_get);
    RUN(test_put_applies);
    RUN(test_put_refused);
    RUN(test_commit_while_setup);
    RUN(test_autosave);
    RUN(test_save_interrupted);
    RUN(test_save_fails);
    RUN(test_without_edit);
    RUN(test_state);
    RUN(test_edit);
    RUN(test_save_revert);
    RUN(test_panel);
    printf("test_cfglink: %s\n", check_failed ? "FAILED" : "all ok");
    return check_failed ? 1 : 0;
}
