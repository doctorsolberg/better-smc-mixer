/*
 * Host tests of the command link (firmware/link, docs/link.md). Run: make -C firmware test
 *
 *   test_link                  run the tests
 *   test_link --serve SECONDS  a fake device on stdin/stdout (a pseudo-terminal's master side):
 *                              the link with a log record every 100 ms and stand-in variant
 *                              functions, for tools/smclog/test_smclog.py
 *
 * Replies are decoded here independently of link.c, from the format in docs/link.md and
 * docs/logging.md. Commands are framed with smclog_frame(), which test_smclog checks.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "check.h"
#include "link.h"
#include "sxpipe.h"
#include "smclog.h"
#include "smclog_host.h"
#include "smclog_port.h"

int check_failed;

static uint32_t now_us;

static uint32_t fake_clock(void)
{
    return now_us;
}

/* --- the variant's functions ----------------------------------------------------------------- */

static struct {
    int raw_calls, raw_on, metrics_calls, boot_log_calls, inject_calls, setup_calls, reset_calls;
    int reset_how, stock_calls, stock_what;
    uint8_t inject_payload[LINK_CMD_MAX];
    size_t setup_len;
    size_t inject_len;
    uint32_t inject_t;
} var;

static void raw_input(void *ctx, uint8_t which)
{
    (void)ctx;
    var.raw_calls++;
    var.raw_on = which;
    LOG_INFO(test, "raw input %u", which);
}

static void metrics(void *ctx)
{
    (void)ctx;
    var.metrics_calls++;
    LOG_INFO(test, "metrics now");
}

static void boot_log(void *ctx)
{
    (void)ctx;
    var.boot_log_calls++;
    LOG_INFO(test, "boot journal again");
}

/* A stand-in: the debug modes are refused, as with injection off. */
static void reset(void *ctx, uint8_t how, uint8_t out[1])
{
    (void)ctx;
    var.reset_calls++;
    var.reset_how = how;
    out[0] = how == LINK_RESET_SOFT ? LINK_RESET_OK : LINK_RESET_DISABLED;
    LOG_INFO(test, "reset %u", how);
}

/* A stand-in: sector 0 points at our area; going back says it was written in one try. */
static void stock(void *ctx, uint8_t what, uint8_t out[LINK_STOCK_REPLY])
{
    (void)ctx;
    var.stock_calls++;
    var.stock_what = what;
    uint32_t ptr = what == LINK_STOCK_BACK ? 0x1000u : 0x41000u;
    out[0] = 0;
    for (unsigned k = 0; k < 4; k++)
        out[1 + k] = (uint8_t)(ptr >> (8 * k));
    out[5] = what == LINK_STOCK_BACK;
    LOG_INFO(test, "stock %u", what);
}

static void inject(void *ctx, uint32_t t_us, const uint8_t *p, size_t len, uint8_t out[3])
{
    (void)ctx;
    var.inject_calls++;
    var.inject_t = t_us;
    var.inject_len = len;
    memcpy(var.inject_payload, p, len);
    out[0] = 0;                             /* INJECT_OK */
    out[1] = (uint8_t)(len > 1 ? (len - 1) / 5 : 0); /* accepted */
    out[2] = 0;
    LOG_INFO(test, "inject %u bytes", len);
}

/* A stand-in: "bad" anywhere is refused as pair 2. */
static void setup(void *ctx, const uint8_t *p, size_t len, uint8_t out[2])
{
    (void)ctx;
    var.setup_calls++;
    var.setup_len = len;
    bool bad = len >= 3 && memmem(p, len, "bad", 3) != NULL;
    out[0] = bad ? LINK_SETUP_BAD : LINK_SETUP_OK;
    out[1] = bad ? 2 : 0;
    LOG_INFO(test, "setup %u bytes", len);
}

static const struct link_ops full_ops = {.variant = 1,
                                         .version = "1.23.4",
                                         .raw_input = raw_input,
                                         .metrics = metrics,
                                         .boot_log = boot_log,
                                         .reset = reset,
                                         .stock = stock,
                                         .inject = inject,
                                         .setup = setup};
static const struct link_ops bare_ops = {.variant = 0};
static const struct link_ops long_ops = {.version = "10.20.30-a.very.long.tag"};

/* --- frames ---------------------------------------------------------------------------------- */

static unsigned le16(const uint8_t *p) { return (unsigned)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return le16(p) | (uint32_t)le16(p + 2) << 16; }

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

/* A record decoded from the device's output. */
struct rec {
    size_t len; /* without the CRC */
    uint8_t b[256];
};

#define RECS_MAX 64
static struct rec recs[RECS_MAX];
static size_t nrecs;

/* Splits out[0..n) into frames, checks each ends where a delimiter is, decodes them into recs.
 * Returns false on anything that is not a whole, valid frame. */
static bool decode_all(const uint8_t *out, size_t n)
{
    size_t start = 0;
    nrecs = 0;
    for (size_t i = 0; i < n; i++) {
        if (out[i])
            continue;
        struct rec *r = &recs[nrecs];
        uint8_t dec[300];
        size_t o = 0, j = start;
        while (j < i) {
            uint8_t code = out[j++];
            if (code == 0 || j + code - 1 > i)
                return false;
            for (int k = 1; k < code; k++)
                dec[o++] = out[j++];
            if (code < 0xFF && j < i)
                dec[o++] = 0;
        }
        if (o < 3 || crc16(dec, o - 2) != le16(dec + o - 2) || nrecs == RECS_MAX)
            return false;
        r->len = o - 2;
        memcpy(r->b, dec, r->len);
        nrecs++;
        start = i + 1;
    }
    return start == n; /* nothing after the last delimiter */
}

static struct link lk;

static void start(const struct link_ops *ops)
{
    smclog_host_set_clock(fake_clock);
    now_us = 1000;
    smclog_reset();
    memset(&var, 0, sizeof var);
    link_init(&lk, ops);
}

/* A CMD frame into buf; returns its length. */
static size_t cmd_frame(uint8_t *buf, uint8_t seq, uint8_t cmd, const uint8_t *p, size_t n)
{
    uint8_t rec[LINK_CMD_MAX + 64];
    rec[0] = LINK_KIND_CMD;
    rec[1] = seq;
    rec[2] = cmd;
    memcpy(rec + 3, p, n);
    return smclog_frame(rec, 3 + n, buf, 400);
}

static size_t send_cmd(uint8_t seq, uint8_t cmd, const uint8_t *p, size_t n)
{
    uint8_t buf[400];
    size_t len = cmd_frame(buf, seq, cmd, p, n);
    return link_rx(&lk, buf, len);
}

/* All the device has to send, decoded into recs; false if the output is not whole frames. */
static bool recv_all(size_t cap)
{
    static uint8_t out[16384];
    size_t n = link_tx(&lk, out, cap);
    return decode_all(out, n);
}

/* Sends a command and returns its reply (the first record out), NULL if there is none. */
static const struct rec *call(uint8_t seq, uint8_t cmd, const uint8_t *p, size_t n)
{
    send_cmd(seq, cmd, p, n);
    if (!recv_all(16384) || !nrecs || recs[0].b[0] != LINK_KIND_REPLY || recs[0].len < 4)
        return NULL;
    return &recs[0];
}

static size_t count_kind(uint8_t kind)
{
    size_t c = 0;
    for (size_t i = 0; i < nrecs; i++)
        c += recs[i].b[0] == kind;
    return c;
}

/* --- tests ----------------------------------------------------------------------------------- */

static void test_ping(void)
{
    start(&bare_ops);
    now_us = 123456;
    const uint8_t hello[] = {'h', 'i', 0, 7};
    const struct rec *r = call(5, LINK_CMD_PING, hello, sizeof hello);
    CHECK(r != NULL);
    if (!r)
        return;
    CHECK_EQ(nrecs, 1); /* not subscribed: no log */
    CHECK_EQ(r->b[1], 5);
    CHECK_EQ(r->b[2], LINK_CMD_PING);
    CHECK_EQ(r->b[3], LINK_OK);
    CHECK_EQ(r->len, 4 + 4 + sizeof hello);
    CHECK_EQ(le32(r->b + 4), 123456);
    CHECK(memcmp(r->b + 8, hello, sizeof hello) == 0);

    uint8_t big[LINK_PING_MAX + 1] = {0};
    r = call(6, LINK_CMD_PING, big, sizeof big);
    CHECK(r && r->b[3] == LINK_BAD_LENGTH && r->len == 4);
    r = call(7, LINK_CMD_PING, NULL, 0);
    CHECK(r && r->b[3] == LINK_OK && r->len == 8);
}

static void test_get_info(void)
{
    size_t idlen;
    start(&full_ops);
    smclog_port_build_id(&idlen);
    const struct rec *r = call(1, LINK_CMD_GET_INFO, NULL, 0);
    CHECK(r != NULL);
    if (!r)
        return;
    CHECK_EQ(r->b[3], LINK_OK);
    CHECK_EQ(r->b[4], LINK_VERSION);
    CHECK_EQ(r->b[5], SMCLOG_FORMAT_VERSION);
    CHECK_EQ(r->b[6], 1);
    CHECK_EQ(le32(r->b + 7),
             LINK_CAP_LOG | LINK_CAP_RAW_INPUT | LINK_CAP_INJECT | LINK_CAP_METRICS | LINK_CAP_SETUP |
                 LINK_CAP_BOOT_LOG | LINK_CAP_RESET | LINK_CAP_STOCK);
    CHECK_EQ(le32(r->b + 11), SMCLOG_RING_SIZE);
    CHECK_EQ(le32(r->b + 15), SMCLOG_TIMER_HZ);
    CHECK_EQ(r->b[19], idlen);
    CHECK(idlen == 16 || idlen == 20); /* LC_UUID on macOS, the GNU build ID on Linux */
    CHECK_EQ(r->b[20 + idlen], 6); /* the version's text */
    CHECK(memcmp(r->b + 21 + idlen, "1.23.4", 6) == 0);
    CHECK_EQ(r->len, 21 + idlen + 6);

    start(&bare_ops);
    r = call(1, LINK_CMD_GET_INFO, NULL, 0);
    CHECK(r && le32(r->b + 7) == LINK_CAP_LOG && r->b[6] == 0);
    CHECK(r && r->b[20 + idlen] == 0 && r->len == 21 + idlen); /* no version */
    start(&long_ops);
    r = call(1, LINK_CMD_GET_INFO, NULL, 0); /* cut to LINK_VERSION_TEXT_MAX */
    CHECK(r && r->b[20 + idlen] == LINK_VERSION_TEXT_MAX && r->len == 21 + idlen + LINK_VERSION_TEXT_MAX);
    CHECK(r && memcmp(r->b + 21 + idlen, "10.20.30-a.very.", LINK_VERSION_TEXT_MAX) == 0);
    r = call(2, LINK_CMD_GET_INFO, (const uint8_t *)"x", 1);
    CHECK(r && r->b[3] == LINK_BAD_LENGTH);
}

static void test_stream(void)
{
    start(&bare_ops);
    LOG_INFO(test, "before the subscription");
    CHECK(recv_all(16384) && nrecs == 0); /* nothing until the host subscribes */

    const struct rec *r = call(1, LINK_CMD_LOG_SUBSCRIBE, NULL, 0);
    CHECK(r && r->b[3] == LINK_OK);
    /* the reply, the header, then every record so far: the one before and "log stream on" */
    CHECK(nrecs >= 4);
    CHECK_EQ(recs[1].b[0], SMCLOG_KIND_HEADER);
    CHECK_EQ(count_kind(SMCLOG_KIND_HEADER), 1);
    CHECK_EQ(count_kind(SMCLOG_KIND_LOG), nrecs - 2);

    LOG_INFO(test, "while subscribed");
    CHECK(recv_all(16384) && nrecs == 1 && recs[0].b[0] == SMCLOG_KIND_LOG);
    CHECK(recv_all(16384) && nrecs == 0);

    /* subscribing again starts a new stream: another header */
    r = call(2, LINK_CMD_LOG_SUBSCRIBE, NULL, 0);
    CHECK(r && count_kind(SMCLOG_KIND_HEADER) == 1);

    r = call(3, LINK_CMD_LOG_UNSUBSCRIBE, NULL, 0);
    CHECK(r && r->b[3] == LINK_OK);
    LOG_INFO(test, "after the subscription");
    CHECK(recv_all(16384) && nrecs == 0);
    r = call(4, LINK_CMD_LOG_UNSUBSCRIBE, NULL, 0); /* harmless */
    CHECK(r && r->b[3] == LINK_OK && nrecs == 1);
    r = call(5, LINK_CMD_LOG_SUBSCRIBE, (const uint8_t *)"x", 1);
    CHECK(r && r->b[3] == LINK_BAD_LENGTH && nrecs == 1);
}

static void test_levels(void)
{
    start(&bare_ops);
    const uint8_t set[] = {SMCLOG_MOD_test, SMCLOG_DEBUG};
    const struct rec *r = call(1, LINK_CMD_LOG_SET_LEVEL, set, 2);
    CHECK(r && r->b[3] == LINK_OK);
    CHECK_EQ(smclog_levels[SMCLOG_MOD_test], SMCLOG_DEBUG);
    CHECK_EQ(smclog_levels[SMCLOG_MOD_input], SMCLOG_INFO);

    r = call(2, LINK_CMD_LOG_GET_LEVELS, NULL, 0);
    CHECK(r && r->b[3] == LINK_OK && r->b[4] == SMCLOG_MOD_COUNT && r->len == 5 + SMCLOG_MOD_COUNT);
    CHECK(r && r->b[5 + SMCLOG_MOD_test] == SMCLOG_DEBUG && r->b[5 + SMCLOG_MOD_usb] == SMCLOG_INFO);

    const uint8_t all[] = {LINK_ALL_MODULES, SMCLOG_WARN};
    r = call(3, LINK_CMD_LOG_SET_LEVEL, all, 2);
    CHECK(r && r->b[3] == LINK_OK);
    for (unsigned m = 0; m < SMCLOG_MOD_COUNT; m++)
        CHECK_EQ(smclog_levels[m], SMCLOG_WARN);

    const uint8_t bad_mod[] = {SMCLOG_MOD_COUNT, SMCLOG_INFO}, bad_level[] = {0, SMCLOG_TRACE + 1};
    r = call(4, LINK_CMD_LOG_SET_LEVEL, bad_mod, 2);
    CHECK(r && r->b[3] == LINK_BAD_ARG);
    r = call(5, LINK_CMD_LOG_SET_LEVEL, bad_level, 2);
    CHECK(r && r->b[3] == LINK_BAD_ARG);
    r = call(6, LINK_CMD_LOG_SET_LEVEL, set, 1);
    CHECK(r && r->b[3] == LINK_BAD_LENGTH);
    smclog_reset();
}

static void test_variant_commands(void)
{
    const uint8_t on = 1, all = LINK_RAW_ALL, four = 4;
    const uint8_t events[] = {2, 0, 7, 1, 0, 0, 0, 7, 0, 0, 0}; /* op 2: press, release */

    start(&bare_ops);
    const struct rec *r = call(1, LINK_CMD_LOG_RAW_INPUT, &on, 1);
    CHECK(r && r->b[3] == LINK_UNAVAILABLE);
    r = call(2, LINK_CMD_METRICS_NOW, NULL, 0);
    CHECK(r && r->b[3] == LINK_UNAVAILABLE);
    r = call(3, LINK_CMD_INPUT_INJECT, events, sizeof events);
    CHECK(r && r->b[3] == LINK_UNAVAILABLE && r->len == 4);
    r = call(4, LINK_CMD_CORE_SETUP, NULL, 0);
    CHECK(r && r->b[3] == LINK_UNAVAILABLE && r->len == 4);
    r = call(5, LINK_CMD_BOOT_LOG, NULL, 0);
    CHECK(r && r->b[3] == LINK_UNAVAILABLE);
    r = call(6, LINK_CMD_RESET, (const uint8_t[]){LINK_RESET_SOFT}, 1);
    CHECK(r && r->b[3] == LINK_UNAVAILABLE && r->len == 4);
    r = call(7, LINK_CMD_STOCK, (const uint8_t[]){LINK_STOCK_CHECK}, 1);
    CHECK(r && r->b[3] == LINK_UNAVAILABLE && r->len == 4);

    start(&full_ops);
    r = call(1, LINK_CMD_LOG_RAW_INPUT, &on, 1);
    CHECK(r && r->b[3] == LINK_OK);
    CHECK_EQ(var.raw_calls, 1);
    CHECK_EQ(var.raw_on, LINK_RAW_MATRIX);
    r = call(2, LINK_CMD_LOG_RAW_INPUT, &all, 1);
    CHECK(r && r->b[3] == LINK_OK);
    CHECK_EQ(var.raw_on, LINK_RAW_MATRIX | LINK_RAW_FADERS);
    r = call(3, LINK_CMD_LOG_RAW_INPUT, &four, 1);
    CHECK(r && r->b[3] == LINK_BAD_ARG);
    r = call(40, LINK_CMD_LOG_RAW_INPUT, (const uint8_t[]){LINK_RAW_MATRIX | 8u << 4}, 1);
    CHECK(r && r->b[3] == LINK_OK); /* the probe on encoder 8 */
    CHECK_EQ(var.raw_on, LINK_RAW_MATRIX | 8u << 4);
    r = call(41, LINK_CMD_LOG_RAW_INPUT, (const uint8_t[]){9u << 4}, 1);
    CHECK(r && r->b[3] == LINK_BAD_ARG); /* no encoder 9 */
    r = call(39, LINK_CMD_LOG_RAW_INPUT, NULL, 0);
    CHECK(r && r->b[3] == LINK_BAD_LENGTH);
    CHECK_EQ(var.raw_calls, 3);

    r = call(4, LINK_CMD_METRICS_NOW, NULL, 0);
    CHECK(r && r->b[3] == LINK_OK);
    CHECK_EQ(var.metrics_calls, 1);
    r = call(42, LINK_CMD_METRICS_NOW, &on, 1);
    CHECK(r && r->b[3] == LINK_BAD_LENGTH); /* it takes nothing */
    CHECK_EQ(var.metrics_calls, 1);
    r = call(40, LINK_CMD_BOOT_LOG, &on, 1);
    CHECK(r && r->b[3] == LINK_BAD_LENGTH);
    r = call(41, LINK_CMD_BOOT_LOG, NULL, 0);
    CHECK(r && r->b[3] == LINK_OK);
    CHECK_EQ(var.boot_log_calls, 1);

    r = call(42, LINK_CMD_RESET, NULL, 0);
    CHECK(r && r->b[3] == LINK_BAD_LENGTH);
    r = call(43, LINK_CMD_RESET, (const uint8_t[]){LINK_RESET_MAX + 1}, 1);
    CHECK(r && r->b[3] == LINK_BAD_ARG);
    CHECK_EQ(var.reset_calls, 0);
    r = call(44, LINK_CMD_RESET, (const uint8_t[]){LINK_RESET_FAULT}, 1);
    CHECK(r && r->b[3] == LINK_OK && r->len == 5 && r->b[4] == LINK_RESET_DISABLED);
    r = call(45, LINK_CMD_RESET, (const uint8_t[]){LINK_RESET_SOFT}, 1);
    CHECK(r && r->b[3] == LINK_OK && r->len == 5 && r->b[4] == LINK_RESET_OK);
    CHECK_EQ(var.reset_calls, 2);
    CHECK_EQ(var.reset_how, LINK_RESET_SOFT);

    /* STOCK: the check takes one byte; going back the magic too, anything else is refused unseen */
    r = call(50, LINK_CMD_STOCK, NULL, 0);
    CHECK(r && r->b[3] == LINK_BAD_LENGTH);
    r = call(51, LINK_CMD_STOCK, (const uint8_t[]){LINK_STOCK_CHECK, 0}, 2);
    CHECK(r && r->b[3] == LINK_BAD_LENGTH);
    r = call(52, LINK_CMD_STOCK, (const uint8_t[]){LINK_STOCK_BACK}, 1);
    CHECK(r && r->b[3] == LINK_BAD_LENGTH);
    r = call(53, LINK_CMD_STOCK, (const uint8_t[]){LINK_STOCK_BACK, 'B', 'A', 'C', 'X'}, 5);
    CHECK(r && r->b[3] == LINK_BAD_ARG);
    r = call(54, LINK_CMD_STOCK, (const uint8_t[]){2}, 1);
    CHECK(r && r->b[3] == LINK_BAD_ARG);
    CHECK_EQ(var.stock_calls, 0);
    r = call(55, LINK_CMD_STOCK, (const uint8_t[]){LINK_STOCK_CHECK}, 1);
    CHECK(r && r->b[3] == LINK_OK && r->len == 4 + LINK_STOCK_REPLY && r->b[4] == 0 && le32(r->b + 5) == 0x41000u);
    r = call(56, LINK_CMD_STOCK, (const uint8_t[]){LINK_STOCK_BACK, 'B', 'A', 'C', 'K'}, 5);
    CHECK(r && r->b[3] == LINK_OK && le32(r->b + 5) == 0x1000u && r->b[9] == 1);
    CHECK_EQ(var.stock_calls, 2);
    CHECK_EQ(var.stock_what, LINK_STOCK_BACK);

    now_us = 777;
    r = call(5, LINK_CMD_INPUT_INJECT, events, sizeof events);
    CHECK(r && r->b[3] == LINK_OK && r->len == 7);
    CHECK(r && r->b[4] == 0 && r->b[5] == 2 && r->b[6] == 0);
    CHECK_EQ(var.inject_calls, 1);
    CHECK_EQ(var.inject_t, 777);
    CHECK_EQ(var.inject_len, sizeof events);
    CHECK(memcmp(var.inject_payload, events, sizeof events) == 0);
    r = call(6, LINK_CMD_INPUT_INJECT, NULL, 0);
    CHECK(r && r->b[3] == LINK_BAD_LENGTH);
    CHECK_EQ(var.inject_calls, 1);

    r = call(7, 0x7F, NULL, 0);
    CHECK(r && r->b[3] == LINK_UNKNOWN_CMD && r->b[2] == 0x7F);

    static const char text[] = "config test note_off 8n";
    r = call(8, LINK_CMD_CORE_SETUP, (const uint8_t *)text, sizeof text - 1);
    CHECK(r && r->b[3] == LINK_OK && r->len == 6 && r->b[4] == LINK_SETUP_OK && r->b[5] == 0);
    CHECK_EQ(var.setup_len, sizeof text - 1);
    r = call(9, LINK_CMD_CORE_SETUP, (const uint8_t *)"a b bad c", 9);
    CHECK(r && r->b[3] == LINK_OK && r->b[4] == LINK_SETUP_BAD && r->b[5] == 2);
    r = call(10, LINK_CMD_CORE_SETUP, NULL, 0); /* empty: the built-in configuration */
    CHECK(r && r->b[3] == LINK_OK && r->b[4] == LINK_SETUP_OK);
    CHECK_EQ(var.setup_calls, 3);
}

static void test_retry(void)
{
    const uint8_t events[] = {2, 0, 7, 1, 0, 0};
    start(&full_ops);
    const struct rec *r = call(9, LINK_CMD_INPUT_INJECT, events, sizeof events);
    CHECK(r && r->b[3] == LINK_OK);
    struct rec first = *r;
    r = call(9, LINK_CMD_INPUT_INJECT, events, sizeof events); /* the host did not see the reply */
    CHECK(r && r->len == first.len && memcmp(r->b, first.b, first.len) == 0);
    CHECK_EQ(var.inject_calls, 1); /* not injected twice */
    CHECK_EQ(lk.stats.retries, 1);
    CHECK_EQ(lk.stats.commands, 1);

    r = call(9, LINK_CMD_PING, NULL, 0); /* the same seq with another command is a new command */
    CHECK(r && r->b[2] == LINK_CMD_PING);
    r = call(10, LINK_CMD_INPUT_INJECT, events, sizeof events);
    CHECK_EQ(var.inject_calls, 2);

    link_reset(&lk); /* the port was closed: the seq is forgotten */
    r = call(10, LINK_CMD_INPUT_INJECT, events, sizeof events);
    CHECK(r && r->b[3] == LINK_OK);
    CHECK_EQ(var.inject_calls, 3);
}

static void test_one_reply_at_a_time(void)
{
    uint8_t buf[400];
    start(&bare_ops);
    size_t a = cmd_frame(buf, 1, LINK_CMD_PING, NULL, 0);
    size_t b = cmd_frame(buf + a, 2, LINK_CMD_PING, NULL, 0);
    size_t took = link_rx(&lk, buf, a + b);
    CHECK_EQ(took, a);                       /* stops after the first command */
    CHECK_EQ(link_rx(&lk, buf + took, b), 0); /* its reply is still waiting */
    CHECK(recv_all(16384) && nrecs == 1 && recs[0].b[1] == 1);
    CHECK_EQ(link_rx(&lk, buf + took, b), b);
    CHECK(recv_all(16384) && nrecs == 1 && recs[0].b[1] == 2);
}

static void test_bad_frames(void)
{
    uint8_t buf[400];
    start(&bare_ops);

    const uint8_t junk[] = {0x00, 0x00, 0x05, 0x11, 0x00}; /* empty frames, then bad COBS */
    link_rx(&lk, junk, sizeof junk);
    size_t n = cmd_frame(buf, 1, LINK_CMD_PING, NULL, 0);
    buf[2] ^= 0x40; /* a bad CRC */
    link_rx(&lk, buf, n);
    CHECK_EQ(lk.stats.bad_frames, 2);
    CHECK(recv_all(16384) && nrecs == 0);

    memset(buf, 0x55, 300); /* longer than any command */
    buf[300] = 0;
    CHECK_EQ(link_rx(&lk, buf, 301), 301);
    CHECK_EQ(lk.stats.overflows, 1);

    uint8_t rec[8] = {SMCLOG_KIND_LOG, 1, 2, 3}; /* a good frame that is not a command */
    n = smclog_frame(rec, 4, buf, sizeof buf);
    link_rx(&lk, buf, n);
    rec[0] = LINK_KIND_CMD; /* a command too short to have a cmd byte */
    n = smclog_frame(rec, 2, buf, sizeof buf);
    link_rx(&lk, buf, n);
    CHECK_EQ(lk.stats.ignored, 2);
    CHECK(recv_all(16384) && nrecs == 0);

    const struct rec *r = call(2, LINK_CMD_PING, NULL, 0); /* the link still works */
    CHECK(r && r->b[3] == LINK_OK);
    CHECK_EQ(lk.stats.frames, 5); /* empty frames and the long one are not counted */

    /* a frame cut by a reset is not executed when the rest arrives */
    n = cmd_frame(buf, 3, LINK_CMD_PING, NULL, 0);
    link_rx(&lk, buf, 3);
    link_reset(&lk);
    link_rx(&lk, buf + 3, n - 3);
    CHECK_EQ(lk.stats.bad_frames, 3);
    CHECK(recv_all(16384) && nrecs == 0);
}

static void test_tx_room(void)
{
    start(&bare_ops);
    for (int i = 0; i < 20; i++)
        LOG_INFO(test, "record %d", i);
    send_cmd(1, LINK_CMD_LOG_SUBSCRIBE, NULL, 0);
    uint8_t out[512];
    CHECK_EQ(link_tx(&lk, out, 5), 0); /* the reply does not fit: nothing */
    CHECK_EQ(link_tx(&lk, out, lk.reply_len), lk.reply_len); /* the reply alone */
    CHECK(decode_all(out, lk.reply_len) && nrecs == 1 && recs[0].b[0] == LINK_KIND_REPLY);
    size_t total = 0, frames = 0;
    for (int k = 0; k < 200; k++) { /* small room: whole frames only, each call */
        size_t n = link_tx(&lk, out, SMCLOG_FRAME_MAX);
        if (!n)
            break;
        CHECK(decode_all(out, n));
        total += n;
        frames += nrecs;
    }
    CHECK_EQ(frames, 1 + 20 + 1); /* the header, the records, "log stream on" */
    CHECK(total > 0);
}

static void test_reset_stops_stream(void)
{
    start(&bare_ops);
    send_cmd(1, LINK_CMD_LOG_SUBSCRIBE, NULL, 0);
    link_reset(&lk); /* the reply was not sent: dropped */
    LOG_INFO(test, "after the reset");
    CHECK(recv_all(16384) && nrecs == 0);
    CHECK(!lk.subscribed);
}

/* --- SysEx (docs/link.md, "Over SysEx") ------------------------------------------------------ */

/* The frame of PING seq 7 with the payload 80 FF 00, worked out by hand from the format: the
 * record 10 07 01 80 FF 00, its CRC-16 51 BD, 7-bit packed in groups of 7. */
static const uint8_t golden_ping[] = {0xF0, 0x7D, 0x53, 0x4D, 0x18, 0x10, 0x07, 0x01,
                                      0x00, 0x7F, 0x00, 0x51, 0x00, 0x3D, 0xF7};

/* Packing written here from the format, independently of link_pack7(). */
static size_t sx_pack(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (i % 7 == 0)
            out[o++] = 0;
        out[o - 1 - i % 7] |= (uint8_t)((in[i] >> 7) << (i % 7));
        out[o++] = in[i] & 0x7F;
    }
    return o;
}

/* A CMD record as a SysEx into buf; returns its length. */
static size_t sx_cmd(uint8_t *buf, uint8_t seq, uint8_t cmd, const uint8_t *p, size_t n)
{
    uint8_t rec[LINK_CMD_MAX + 64];
    rec[0] = LINK_KIND_CMD;
    rec[1] = seq;
    rec[2] = cmd;
    memcpy(rec + 3, p, n);
    uint16_t c = crc16(rec, 3 + n);
    rec[3 + n] = (uint8_t)c;
    rec[4 + n] = (uint8_t)(c >> 8);
    buf[0] = 0xF0;
    buf[1] = 0x7D;
    buf[2] = 0x53;
    buf[3] = 0x4D;
    size_t o = 4 + sx_pack(rec, 5 + n, buf + 4);
    buf[o++] = 0xF7;
    return o;
}

/* The device's output as SysEx replies into recs (CRC checked, without it); false if it is not
 * whole SysEx messages of ours. */
static bool sx_decode_all(const uint8_t *out, size_t n)
{
    size_t i = 0;
    nrecs = 0;
    while (i < n) {
        if (n - i < 6 || out[i] != 0xF0 || out[i + 1] != 0x7D || out[i + 2] != 0x53 ||
            out[i + 3] != 0x4D || nrecs == RECS_MAX)
            return false;
        size_t j = i + 4, o = 0, g = 0;
        uint8_t top = 0, dec[300];
        for (; j < n && out[j] != 0xF7; j++, g = (g + 1) % 8) {
            if (out[j] & 0x80)
                return false;
            if (g == 0)
                top = out[j];
            else
                dec[o++] = (uint8_t)(out[j] | ((top >> (g - 1)) & 1) << 7);
        }
        if (j == n || o < 3 || crc16(dec, o - 2) != le16(dec + o - 2))
            return false;
        recs[nrecs].len = o - 2;
        memcpy(recs[nrecs].b, dec, o - 2);
        nrecs++;
        i = j + 1;
    }
    return true;
}

static const struct rec *sx_call(uint8_t seq, uint8_t cmd, const uint8_t *p, size_t n)
{
    uint8_t buf[400];
    static uint8_t out[1024];
    size_t len = sx_cmd(buf, seq, cmd, p, n);
    link_rx(&lk, buf, len);
    size_t got = link_tx(&lk, out, sizeof out);
    if (!sx_decode_all(out, got) || nrecs != 1 || recs[0].b[0] != LINK_KIND_REPLY || recs[0].len < 4)
        return NULL;
    return &recs[0];
}

/* --- events (P-2) ------------------------------------------------------------------------------ */

static uint8_t ev_state[30];
static uint8_t ev_cfg_status; /* CFG_STATE's configuration status: 0 ok */
static unsigned ev_state_calls;

/* what the mixer sent, as ctl keeps it (P-5): entry k is k's low byte in each field */
static bool sn_avail;      /* CFG_SENT answers */
static uint32_t sn_w;      /* the next entry's number */
#define SN_KEPT 63u

static void sn_entry(uint32_t k, uint8_t *e)
{
    e[0] = 0xB0;
    for (unsigned i = 1; i < 7; i++)
        e[i] = (uint8_t)((k + i) & 0x7F);
}

static uint8_t sn_config(const uint8_t *p, size_t n, uint8_t *out, size_t *len)
{
    CHECK_EQ(n, 4);
    if (!sn_avail) {
        out[0] = 8; /* CFGLINK_UNAVAILABLE */
        *len += 1;
        return LINK_OK;
    }
    uint32_t from = le32(p), first = from;
    unsigned c = 0;
    if (from == 0xFFFFFFFFu || (int32_t)(sn_w - from) < 0) {
        first = sn_w;
    } else {
        if (sn_w - from > SN_KEPT)
            first = sn_w - SN_KEPT;
        c = sn_w - first < 16 ? sn_w - first : 16;
    }
    out[0] = 0;
    out[1] = (uint8_t)first;
    out[2] = (uint8_t)(first >> 8);
    out[3] = (uint8_t)(first >> 16);
    out[4] = (uint8_t)(first >> 24);
    out[5] = (uint8_t)c;
    for (unsigned i = 0; i < c; i++)
        sn_entry(first + i, out + 6 + 7 * i);
    *len += 6 + 7 * c;
    return LINK_OK;
}

static uint8_t ev_config(void *cfg, uint8_t cmd, const uint8_t *p, size_t n, uint8_t *out, size_t *len)
{
    (void)cfg;
    if (cmd == LINK_CMD_CFG_SENT)
        return sn_config(p, n, out, len);
    if (cmd != LINK_CMD_CFG_STATE)
        return LINK_UNKNOWN_CMD;
    CHECK_EQ(n, 0);
    ev_state_calls++;
    out[0] = ev_cfg_status;
    if (ev_cfg_status) {
        *len += 1;
        return LINK_OK;
    }
    memcpy(out + 1, ev_state, sizeof ev_state);
    *len += 1 + sizeof ev_state;
    return LINK_OK;
}

static const struct link_ops ev_ops = {.variant = 0, .config = ev_config};

/* The EVENT among recs: its index, -1 if there is none. */
static int find_event(void)
{
    for (size_t i = 0; i < nrecs; i++)
        if (recs[i].b[0] == LINK_KIND_EVENT)
            return (int)i;
    return -1;
}

static void test_events(void)
{
    uint8_t on = 1, off = 0, bad = 4;
    const struct rec *r;

    /* no configuration protocol: no events */
    start(&bare_ops);
    r = call(1, LINK_CMD_GET_INFO, NULL, 0);
    CHECK(r && (le32(r->b + 7) & LINK_CAP_EVENTS) == 0);
    r = call(2, LINK_CMD_EVENTS, &on, 1);
    CHECK(r && r->b[3] == LINK_UNAVAILABLE);

    start(&ev_ops);
    for (unsigned i = 0; i < sizeof ev_state; i++)
        ev_state[i] = (uint8_t)i;
    ev_cfg_status = 0;
    r = call(1, LINK_CMD_GET_INFO, NULL, 0);
    CHECK(r && (le32(r->b + 7) & LINK_CAP_EVENTS) != 0);
    r = call(2, LINK_CMD_EVENTS, NULL, 0);
    CHECK(r && r->b[3] == LINK_BAD_LENGTH);
    r = call(3, LINK_CMD_EVENTS, &bad, 1);
    CHECK(r && r->b[3] == LINK_BAD_ARG && !lk.events);
    /* a firmware whose configuration protocol has no state: unavailable */
    ev_cfg_status = 8;
    r = call(4, LINK_CMD_EVENTS, &on, 1);
    CHECK(r && r->b[3] == LINK_UNAVAILABLE && !lk.events);
    ev_cfg_status = 0;

    /* on: the reply, then the first EVENT at once with the whole state */
    r = call(5, LINK_CMD_EVENTS, &on, 1);
    CHECK(r && r->b[3] == LINK_OK && nrecs == 2);
    int e = find_event();
    CHECK(e == 1);
    if (e >= 0) {
        CHECK_EQ(recs[e].len, 2 + sizeof ev_state);
        CHECK_EQ(recs[e].b[1], 0);
        CHECK(memcmp(recs[e].b + 2, ev_state, sizeof ev_state) == 0);
    }
    /* unchanged: nothing */
    now_us += 100000;
    CHECK(recv_all(16384) && nrecs == 0);
    /* changed within the gap: nothing yet; after it, one EVENT with the newest state */
    ev_state[0] = 3;
    now_us += 1000;
    CHECK(recv_all(16384) && nrecs == 1); /* the gap counts from the last EVENT, long past */
    e = find_event();
    CHECK(e == 0 && recs[0].b[1] == 1 && recs[0].b[2] == 3);
    ev_state[4] = 9;
    now_us += LINK_EVENT_GAP_US - 1;
    CHECK(recv_all(16384) && nrecs == 0);
    ev_state[4] = 10; /* a newer change before it goes: only the newest is sent */
    now_us += 1;
    CHECK(recv_all(16384) && nrecs == 1 && recs[0].b[1] == 2 && recs[0].b[6] == 10);
    CHECK_EQ(lk.stats.events, 3);

    /* no room: it waits whole, then goes */
    ev_state[4] = 11;
    now_us += LINK_EVENT_GAP_US;
    CHECK(recv_all(10) && nrecs == 0);
    CHECK(recv_all(16384) && nrecs == 1 && recs[0].b[1] == 3 && recs[0].b[6] == 11);

    /* a reply goes before an EVENT that is due */
    ev_state[4] = 12;
    now_us += LINK_EVENT_GAP_US;
    r = call(6, LINK_CMD_PING, NULL, 0);
    CHECK(r && nrecs == 2 && recs[1].b[0] == LINK_KIND_EVENT && recs[1].b[1] == 4);

    /* off: none */
    r = call(7, LINK_CMD_EVENTS, &off, 1);
    CHECK(r && r->b[3] == LINK_OK && nrecs == 1 && !lk.events);
    ev_state[4] = 13;
    now_us += LINK_EVENT_GAP_US;
    CHECK(recv_all(16384) && nrecs == 0);

    /* on again: the count starts over; a reset stops them */
    r = call(8, LINK_CMD_EVENTS, &on, 1);
    CHECK(r && nrecs == 2 && recs[1].b[1] == 0 && recs[1].b[6] == 13);
    link_reset(&lk);
    CHECK(!lk.events);
    ev_state[4] = 14;
    now_us += LINK_EVENT_GAP_US;
    CHECK(recv_all(16384) && nrecs == 0);

    /* over SysEx: the same records, packed */
    start(&ev_ops);
    link_init_sysex(&lk, &ev_ops);
    uint8_t buf[400];
    static uint8_t out[1024];
    size_t len = sx_cmd(buf, 9, LINK_CMD_EVENTS, &on, 1);
    link_rx(&lk, buf, len);
    /* one SysEx message per call (BLE MIDI's sender takes one at a time): the reply, then the
     * EVENT */
    size_t got = link_tx(&lk, out, sizeof out);
    CHECK_EQ(got, LINK_SYSEX_LEN(4));
    CHECK(sx_decode_all(out, got) && nrecs == 1 && recs[0].b[0] == LINK_KIND_REPLY);
    got = link_tx(&lk, out, sizeof out);
    CHECK_EQ(got, LINK_SYSEX_LEN(2 + sizeof ev_state));
    CHECK(sx_decode_all(out, got) && nrecs == 1 && recs[0].b[0] == LINK_KIND_EVENT);
    CHECK(recs[0].len == 2 + sizeof ev_state && memcmp(recs[0].b + 2, ev_state, sizeof ev_state) == 0);
    CHECK_EQ(link_tx(&lk, out, sizeof out), 0);
    /* a reply while an EVENT is due: still one message a call, the reply first */
    ev_state[4] = 15;
    now_us += LINK_EVENT_GAP_US;
    len = sx_cmd(buf, 10, LINK_CMD_PING, NULL, 0);
    link_rx(&lk, buf, len);
    got = link_tx(&lk, out, sizeof out);
    CHECK(sx_decode_all(out, got) && nrecs == 1 && recs[0].b[0] == LINK_KIND_REPLY);
    got = link_tx(&lk, out, sizeof out);
    CHECK(sx_decode_all(out, got) && nrecs == 1 && recs[0].b[0] == LINK_KIND_EVENT && recs[0].b[6] == 15);
}

/* A SENT record's fields: first, lost, count; its entries checked against sn_entry(). */
static bool sent_rec(const struct rec *r, uint32_t first, unsigned lost, unsigned count)
{
    uint8_t e[7];
    if (r->b[0] != LINK_KIND_SENT || r->len != 7u + 7u * count || le32(r->b + 1) != first ||
        r->b[5] != lost || r->b[6] != count)
        return false;
    for (unsigned i = 0; i < count; i++) {
        sn_entry(first + i, e);
        if (memcmp(r->b + 7 + 7 * i, e, 7) != 0)
            return false;
    }
    return true;
}

/* What the mixer sent (P-5): EVENTS' bit 1, SENT records. */
static void test_sent(void)
{
    uint8_t sent = 2, both = 3, off = 0;
    const struct rec *r;

    start(&ev_ops);
    for (unsigned i = 0; i < sizeof ev_state; i++)
        ev_state[i] = (uint8_t)i;
    ev_cfg_status = 0;
    r = call(1, LINK_CMD_GET_INFO, NULL, 0);
    CHECK(r && (le32(r->b + 7) & LINK_CAP_SENT) != 0);
    /* a configuration protocol without CFG_SENT: unavailable */
    sn_avail = false;
    r = call(2, LINK_CMD_EVENTS, &sent, 1);
    CHECK(r && r->b[3] == LINK_UNAVAILABLE && !lk.sent_on);

    /* on: from the next entry; nothing until one comes */
    sn_avail = true;
    sn_w = 5;
    r = call(3, LINK_CMD_EVENTS, &sent, 1);
    CHECK(r && r->b[3] == LINK_OK && nrecs == 1 && lk.sent_on && !lk.events);
    now_us += 100000;
    CHECK(recv_all(16384) && nrecs == 0);
    sn_w = 8;
    CHECK(recv_all(16384) && nrecs == 1 && sent_rec(&recs[0], 5, 0, 3));
    /* within the gap: nothing yet; after it, the next */
    sn_w = 9;
    now_us += LINK_SENT_GAP_US - 1;
    CHECK(recv_all(16384) && nrecs == 0);
    now_us += 1;
    CHECK(recv_all(16384) && nrecs == 1 && sent_rec(&recs[0], 8, 0, 1));
    /* 16 at most a record: the rest after the gap */
    sn_w = 29;
    now_us += LINK_SENT_GAP_US;
    CHECK(recv_all(16384) && nrecs == 1 && sent_rec(&recs[0], 9, 0, 16));
    now_us += LINK_SENT_GAP_US;
    CHECK(recv_all(16384) && nrecs == 1 && sent_rec(&recs[0], 25, 0, 4));
    /* fallen behind: the oldest skipped and counted */
    sn_w = 29 + 100;
    now_us += LINK_SENT_GAP_US;
    CHECK(recv_all(16384) && nrecs == 1 && sent_rec(&recs[0], 129 - 63, 129 - 63 - 29, 16));
    sn_w = 129 + 400; /* more than 255 lost: 255 */
    now_us += LINK_SENT_GAP_US;
    CHECK(recv_all(16384) && nrecs == 1 && sent_rec(&recs[0], 529 - 63, 255, 16));
    CHECK_EQ(lk.stats.sent, 6);
    /* no room: it waits whole */
    now_us += LINK_SENT_GAP_US;
    CHECK(recv_all(20) && nrecs == 0);
    CHECK(recv_all(16384) && nrecs == 1 && sent_rec(&recs[0], 529 - 63 + 16, 0, 16));

    /* both: the reply, the first EVENT, a SENT, in that order */
    sn_w = 600;
    now_us += LINK_SENT_GAP_US;
    r = call(4, LINK_CMD_EVENTS, &both, 1);
    CHECK(r && nrecs == 3 && recs[1].b[0] == LINK_KIND_EVENT && recs[2].b[0] == LINK_KIND_SENT);
    CHECK(lk.events && lk.sent_on);
    /* off, then a reset stops them too */
    r = call(5, LINK_CMD_EVENTS, &off, 1);
    CHECK(r && nrecs == 1 && !lk.events && !lk.sent_on);
    sn_w = 610;
    now_us += LINK_SENT_GAP_US;
    CHECK(recv_all(16384) && nrecs == 0);
    r = call(6, LINK_CMD_EVENTS, &sent, 1);
    CHECK(r && nrecs == 1);
    link_reset(&lk);
    CHECK(!lk.sent_on);
    sn_w = 620;
    CHECK(recv_all(16384) && nrecs == 0);

    /* over SysEx: one message per call, the reply, the EVENT, then the SENT */
    start(&ev_ops);
    link_init_sysex(&lk, &ev_ops);
    uint8_t buf[400];
    static uint8_t out[1024];
    sn_w = 700;
    size_t len = sx_cmd(buf, 9, LINK_CMD_EVENTS, &both, 1);
    link_rx(&lk, buf, len);
    sn_w = 702;
    size_t got = link_tx(&lk, out, sizeof out);
    CHECK(sx_decode_all(out, got) && nrecs == 1 && recs[0].b[0] == LINK_KIND_REPLY);
    got = link_tx(&lk, out, sizeof out);
    CHECK(sx_decode_all(out, got) && nrecs == 1 && recs[0].b[0] == LINK_KIND_EVENT);
    got = link_tx(&lk, out, sizeof out);
    CHECK(sx_decode_all(out, got) && nrecs == 1 && sent_rec(&recs[0], 700, 0, 2));
    CHECK_EQ(got, LINK_SYSEX_LEN(7 + 2 * 7));
    CHECK_EQ(link_tx(&lk, out, sizeof out), 0);
    sn_avail = false;
}

static void test_sysex_pack(void)
{
    uint8_t in[200], packed[240], back[200];
    for (size_t n = 0; n <= sizeof in; n++) {
        for (size_t i = 0; i < n; i++)
            in[i] = (uint8_t)(i * 37 + n * 11 + (i & 1) * 0x80);
        uint8_t ref[240] = {0};
        size_t m = link_pack7(in, n, packed);
        CHECK_EQ(m, LINK_PACKED_LEN(n));
        CHECK_EQ(sx_pack(in, n, ref), m);
        CHECK(memcmp(ref, packed, m) == 0);
        CHECK_EQ(link_unpack7(packed, m, back, sizeof back), (int)n);
        CHECK(memcmp(back, in, n) == 0);
        for (size_t i = 0; i < m; i++)
            CHECK(packed[i] < 0x80);
    }
    const uint8_t lone[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x00}; /* 8 + a header */
    CHECK_EQ(link_unpack7(lone, sizeof lone, back, sizeof back), -1);
    const uint8_t high[] = {0x00, 0x81};
    CHECK_EQ(link_unpack7(high, sizeof high, back, sizeof back), -1);
    const uint8_t two[] = {0x00, 0x01, 0x02};
    CHECK_EQ(link_unpack7(two, sizeof two, back, 1), -1); /* does not fit */
}

static void test_sysex_frame(void)
{
    uint8_t buf[64];
    const uint8_t pay[] = {0x80, 0xFF, 0x00};
    size_t n = sx_cmd(buf, 7, LINK_CMD_PING, pay, sizeof pay);
    CHECK_EQ(n, sizeof golden_ping);
    CHECK(memcmp(buf, golden_ping, sizeof golden_ping) == 0);

    start(&bare_ops);
    link_init_sysex(&lk, &bare_ops);
    now_us = 0x01020304;
    CHECK_EQ(link_rx(&lk, golden_ping, sizeof golden_ping), sizeof golden_ping);
    static uint8_t out[512];
    size_t got = link_tx(&lk, out, sizeof out);
    CHECK(sx_decode_all(out, got) && nrecs == 1);
    CHECK(got <= LINK_SYSEX_LEN(LINK_REPLY_MAX));
    const struct rec *r = &recs[0];
    CHECK(r->len == 4 + 4 + 3 && r->b[0] == LINK_KIND_REPLY && r->b[1] == 7 && r->b[2] == LINK_CMD_PING);
    CHECK(r->b[3] == LINK_OK && le32(r->b + 4) == 0x01020304 && memcmp(r->b + 8, pay, 3) == 0);
}

static void test_sxpipe(void)
{
    static struct sxpipe p;
    uint8_t b[700], got[700];
    size_t n;
    for (size_t i = 0; i < sizeof b; i++)
        b[i] = (uint8_t)i;
    sxpipe_init(&p);
    CHECK_EQ(sxpipe_get_in(&p, got, sizeof got), 0);
    CHECK_EQ(sxpipe_put_in(&p, b, 300), 300);
    CHECK_EQ(sxpipe_get_in(&p, got, 100), 100);
    CHECK_EQ(sxpipe_put_in(&p, b + 300, 400), SXPIPE_IN_SIZE - 1 - 200); /* full: the rest dropped */
    CHECK_EQ(p.in_dropped, 400 - (SXPIPE_IN_SIZE - 1 - 200));
    n = sxpipe_get_in(&p, got + 100, sizeof got - 100);
    CHECK_EQ(n, SXPIPE_IN_SIZE - 1);
    CHECK(memcmp(got, b, 100 + n) == 0); /* in order across the wrap */
    CHECK_EQ(sxpipe_put_in(&p, b, 5), 5);
    sxpipe_drop_in(&p);
    CHECK_EQ(sxpipe_get_in(&p, got, sizeof got), 0);

    const uint8_t *o = sxpipe_peek_out(&p, &n);
    CHECK(o == NULL && n == 0);
    CHECK_EQ(sxpipe_put_out(&p, b, 10), 10);
    CHECK_EQ(sxpipe_put_out(&p, b, 10), 0); /* one at a time */
    CHECK_EQ(sxpipe_put_out(&p, b, 0), 0);
    o = sxpipe_peek_out(&p, &n);
    CHECK(o && n == 10 && memcmp(o, b, 10) == 0);
    sxpipe_done_out(&p);
    CHECK_EQ(sxpipe_put_out(&p, b, sizeof p.out + 1), 0);
    CHECK_EQ(sxpipe_put_out(&p, b, sizeof p.out), sizeof p.out);
}

static void test_sysex_link(void)
{
    start(&full_ops);
    link_init_sysex(&lk, &full_ops);

    /* GET_INFO: no log capability over SysEx */
    const struct rec *r = sx_call(1, LINK_CMD_GET_INFO, NULL, 0);
    CHECK(r && r->b[3] == LINK_OK);
    if (r)
        CHECK_EQ(le32(r->b + 7) & LINK_CAP_LOG, 0);
    r = sx_call(2, LINK_CMD_LOG_SUBSCRIBE, NULL, 0);
    CHECK(r && r->b[3] == LINK_UNAVAILABLE);
    CHECK(!lk.subscribed);

    /* a big PING and an INJECT of the largest payload */
    uint8_t big[LINK_CMD_MAX - 3];
    for (size_t i = 0; i < sizeof big; i++)
        big[i] = (uint8_t)(0xFF - i);
    r = sx_call(3, LINK_CMD_PING, big, LINK_PING_MAX);
    CHECK(r && r->b[3] == LINK_OK && r->len == 8 + LINK_PING_MAX && memcmp(r->b + 8, big, LINK_PING_MAX) == 0);
    r = sx_call(4, LINK_CMD_INPUT_INJECT, big, sizeof big);
    CHECK(r && r->b[3] == LINK_OK && var.inject_len == sizeof big);
    CHECK(memcmp(var.inject_payload, big, sizeof big) == 0);

    /* a retry: the same reply, not executed again */
    r = sx_call(4, LINK_CMD_INPUT_INJECT, big, sizeof big);
    CHECK(r && var.inject_calls == 1 && lk.stats.retries == 1);

    /* foreign SysEx, real-time bytes inside ours, other MIDI around it */
    uint8_t mix[600], cmd[400];
    size_t m = 0, c = sx_cmd(cmd, 9, LINK_CMD_PING, NULL, 0);
    const uint8_t other[] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7, 0xF0, 0x7D, 0x53, 0x4E, 0x00, 0xF7,
                             0x90, 0x40, 0x7F, 0xB0, 0x07};
    memcpy(mix, other, sizeof other);
    m = sizeof other;
    for (size_t i = 0; i < c; i++) {
        mix[m++] = cmd[i];
        if (i == 1 || i == c - 2)
            mix[m++] = 0xF8; /* clock */
    }
    uint32_t foreign = lk.stats.foreign;
    CHECK_EQ(link_rx(&lk, mix, m), m);
    CHECK_EQ(lk.stats.foreign, foreign + 2);
    static uint8_t out[1024];
    size_t got = link_tx(&lk, out, sizeof out);
    CHECK(sx_decode_all(out, got) && nrecs == 1 && recs[0].b[1] == 9 && recs[0].b[3] == LINK_OK);

    /* cut short by a status, a bad CRC, a bad packing: no reply, counted */
    uint32_t bad = lk.stats.bad_frames;
    c = sx_cmd(cmd, 10, LINK_CMD_PING, NULL, 0);
    cmd[c - 1] = 0x90; /* a Note On instead of the F7 */
    link_rx(&lk, cmd, c);
    c = sx_cmd(cmd, 11, LINK_CMD_PING, NULL, 0);
    cmd[5] ^= 0x01;
    link_rx(&lk, cmd, c);
    const uint8_t lone[] = {0xF0, 0x7D, 0x53, 0x4D, 0x00, 0x10, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF7};
    link_rx(&lk, lone, sizeof lone);
    CHECK_EQ(link_tx(&lk, out, sizeof out), 0);
    CHECK_EQ(lk.stats.bad_frames, bad + 3);

    /* too long: dropped up to its F7, counted; the next one is answered */
    uint8_t lng[400] = {0xF0, 0x7D, 0x53, 0x4D};
    memset(lng + 4, 0x11, sizeof lng - 5);
    lng[sizeof lng - 1] = 0xF7;
    uint32_t over = lk.stats.overflows;
    CHECK_EQ(link_rx(&lk, lng, sizeof lng), sizeof lng);
    CHECK_EQ(lk.stats.overflows, over + 1);
    r = sx_call(12, LINK_CMD_PING, NULL, 0);
    CHECK(r && r->b[1] == 12);

    /* one reply at a time: bytes after a command wait until its reply is taken */
    uint8_t two[200];
    size_t a = sx_cmd(two, 13, LINK_CMD_PING, NULL, 0);
    size_t b = sx_cmd(two + a, 14, LINK_CMD_PING, NULL, 0);
    CHECK_EQ(link_rx(&lk, two, a + b), a);
    CHECK_EQ(link_tx(&lk, out, 10), 0); /* no room: the reply is not split */
    got = link_tx(&lk, out, sizeof out);
    CHECK(sx_decode_all(out, got) && nrecs == 1 && recs[0].b[1] == 13);
    CHECK_EQ(link_rx(&lk, two + a, b), b);
    got = link_tx(&lk, out, sizeof out);
    CHECK(sx_decode_all(out, got) && nrecs == 1 && recs[0].b[1] == 14);
}

/* --- the fake device ------------------------------------------------------------------------- */

static uint32_t serve_ticks;

static void serve_metrics(void *ctx)
{
    (void)ctx;
    LOG_INFO(test, "metrics: %u ticks", serve_ticks);
}

static const struct link_ops serve_ops = {
    .variant = 1, .version = "1.23.4", .raw_input = raw_input, .metrics = serve_metrics, .inject = inject,
    .setup = setup};

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* stdout shares the master's non-blocking open file with stdin: wait for room, up to a second. */
static bool write_all(const uint8_t *p, size_t n)
{
    uint64_t give_up = mono_ms() + 1000;
    while (n) {
        ssize_t r = write(1, p, n);
        if (r > 0) {
            p += r;
            n -= (size_t)r;
            continue;
        }
        if (r < 0 && errno != EAGAIN)
            return false;
        if (mono_ms() > give_up)
            return false;
        struct pollfd pfd = {.fd = 1, .events = POLLOUT};
        poll(&pfd, 1, 100);
    }
    return true;
}

static int serve(double seconds)
{
    uint8_t in[256], out[1024];
    size_t in_len = 0, in_pos = 0;
    uint64_t end = mono_ms() + (uint64_t)(seconds * 1000), next_tick = mono_ms();

    smclog_init();
    link_init(&lk, &serve_ops);
    fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
    while (mono_ms() < end) {
        struct pollfd pfd = {.fd = 0, .events = POLLIN};
        if (poll(&pfd, 1, 10) > 0 && (pfd.revents & (POLLHUP | POLLERR)) && !(pfd.revents & POLLIN))
            break;
        if (mono_ms() >= next_tick) {
            LOG_INFO(test, "tick %u", serve_ticks++);
            next_tick += 100;
        }
        for (;;) {
            if (in_pos == in_len) {
                ssize_t r = read(0, in, sizeof in);
                if (r <= 0)
                    break;
                in_len = (size_t)r;
                in_pos = 0;
            }
            size_t used = link_rx(&lk, in + in_pos, in_len - in_pos);
            in_pos += used;
            size_t n = link_tx(&lk, out, sizeof out);
            if (n && !write_all(out, n))
                return 1;
            if (!used && !n)
                break;
        }
        size_t n = link_tx(&lk, out, sizeof out);
        if (n && !write_all(out, n))
            return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--serve") == 0)
        return serve(atof(argv[2]));

    RUN(test_ping);
    RUN(test_get_info);
    RUN(test_stream);
    RUN(test_levels);
    RUN(test_variant_commands);
    RUN(test_retry);
    RUN(test_one_reply_at_a_time);
    RUN(test_bad_frames);
    RUN(test_tx_room);
    RUN(test_reset_stops_stream);
    RUN(test_sysex_pack);
    RUN(test_sysex_frame);
    RUN(test_sysex_link);
    RUN(test_sxpipe);
    RUN(test_events);
    RUN(test_sent);
    return check_failed ? 1 : 0;
}
