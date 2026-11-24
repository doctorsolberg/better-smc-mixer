/*
 * Host unit tests for smclog (built with SMCLOG_RING_SIZE=512, SMCLOG_RECENT_SIZE=256). Run:
 * make -C firmware test
 *
 *   test_smclog            run the tests
 *   test_smclog --emit F   write a fixed log stream to F (decoded by tools/smclog/test_smclog.py
 *                          with the dictionary of this binary)
 *   test_smclog --emit-past F   a stream with records replayed from the recent ring after a
 *                          simulated reset (SMCLOG_KIND_PAST)
 *
 * Frames are decoded here independently of smclog.c, from the format in docs/logging.md.
 */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "smclog.h"
#include "smclog_host.h"
#include "smclog_port.h"

int check_failed;

static uint32_t now_us, tick_us;

static uint32_t fake_clock(void)
{
    uint32_t t = now_us;
    now_us += tick_us;
    return t;
}

static void setup(uint32_t start, uint32_t tick)
{
    smclog_host_set_clock(fake_clock);
    now_us = start;
    tick_us = tick;
    smclog_reset();
}

/* --- frame decoding -------------------------------------------------------------------------- */

struct rec {
    uint8_t kind;
    unsigned seq, id, cause;
    uint32_t t_us, count;
    size_t len;          /* record bytes without the CRC */
    uint8_t b[256];      /* the record */
    const uint8_t *args; /* LOG: after the 11-byte header */
    size_t nargs;
};

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

static long cobs_decode(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    size_t i = 0, o = 0;
    while (i < n) {
        uint8_t code = in[i++];
        if (code == 0)
            return -1;
        for (uint8_t k = 1; k < code; k++) {
            if (i >= n || o >= cap)
                return -1;
            out[o++] = in[i++];
        }
        if (code < 0xFF && i < n) {
            if (o >= cap)
                return -1;
            out[o++] = 0;
        }
    }
    return (long)o;
}

/* Frames in buf -> recs; the number of records, or -1 for a bad or unterminated frame. */
static int parse(const uint8_t *buf, size_t n, struct rec *recs, int max)
{
    int count = 0;
    size_t start = 0;

    for (size_t i = 0; i < n; i++) {
        struct rec *r;
        long m;
        if (buf[i])
            continue;
        if (count == max)
            return -1;
        r = &recs[count++];
        memset(r, 0, sizeof *r);
        m = cobs_decode(buf + start, i - start, r->b, sizeof r->b);
        start = i + 1;
        if (m < 3 || crc16(r->b, (size_t)m - 2) != le16(r->b + m - 2))
            return -1;
        r->len = (size_t)m - 2;
        r->kind = r->b[0];
        uint8_t k = r->kind & (uint8_t)~SMCLOG_KIND_PAST;
        if (k == SMCLOG_KIND_LOG || k == SMCLOG_KIND_LOG_TRUNC) {
            r->seq = le16(r->b + 1);
            r->t_us = le32(r->b + 3);
            r->id = le16(r->b + 7);
            r->cause = le16(r->b + 9);
            r->args = r->b + 11;
            r->nargs = r->len - 11;
        } else if (k == SMCLOG_KIND_LOST) {
            r->seq = le16(r->b + 1);
            r->t_us = le32(r->b + 3);
            r->count = le32(r->b + 7);
        }
    }
    return start == n ? count : -1;
}

static uint8_t out[1 << 16];
static struct rec recs[256];

static int drain_all(void)
{
    return parse(out, smclog_drain(out, sizeof out), recs, 256);
}

static int args_are(const struct rec *r, const uint8_t *want, size_t n)
{
    return r->nargs == n && memcmp(r->args, want, n) == 0;
}

/* --- tests ----------------------------------------------------------------------------------- */

static void test_levels(void)
{
    setup(1000, 10);
    LOG_DEBUG(test, "hidden by default");
    LOG_INFO(test, "shown");
    smclog_set_level(SMCLOG_MOD_test, SMCLOG_DEBUG);
    LOG_DEBUG(test, "shown at debug");
    smclog_set_level(SMCLOG_MOD_test, SMCLOG_OFF);
    LOG_ERROR(test, "module off");
    LOG_ERROR(input, "other module");
    CHECK_EQ(drain_all(), 3);
    CHECK_EQ(recs[0].seq, 0);
    CHECK_EQ(recs[1].seq, 1);
    CHECK_EQ(recs[2].seq, 2);
    CHECK_EQ(recs[0].t_us, 1000);
    CHECK_EQ(recs[1].t_us, 1010);
    CHECK(recs[0].id != recs[1].id && recs[1].id != recs[2].id);
    CHECK_EQ(recs[0].nargs, 0);
}

static void test_same_site_same_id(void)
{
    setup(0, 1);
    for (int i = 0; i < 3; i++)
        LOG_INFO(test, "loop %d", i);
    LOG_INFO(test, "loop %d", 3);
    CHECK_EQ(drain_all(), 4);
    CHECK_EQ(recs[0].id, recs[2].id);
    CHECK(recs[0].id != recs[3].id);
}

static void test_types_word(void)
{
    char c = 'x', buf[4] = "ab";
    uint8_t u8 = 1;
    size_t sz = 1;
    void *ptr = &c;
    float f = 1;
    double d = 2;
    const uint32_t wide = sizeof(void *) > sizeof(int) ? SMCLOG_T_I64 : SMCLOG_T_INT;

    CHECK_EQ(SMCLOG_TYPES_(), 0);
    CHECK_EQ(SMCLOG_TYPES_(c, u8, 5, 5u), 4);
    CHECK_EQ(SMCLOG_TYPES_(1LL, f, d, "s"), 4 | 1 << 4 | 2 << 6 | 2 << 8 | 3 << 10);
    CHECK_EQ(SMCLOG_TYPES_(ptr, sz), 2 | wide << 4 | wide << 6);
    CHECK_EQ(SMCLOG_TYPES_(buf), 1 | 3 << 4);
    CHECK_EQ(SMCLOG_TYPES_(1, 2, 3, 4, 5, 6, 7, 8), 8);
}

static void test_ints(void)
{
    static const uint8_t want[] = {0x00, 0x02, 0x01, 0x7E, 0x80, 0x01,
                                   0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x01};
    setup(0, 1);
    LOG_INFO(test, "%d %d %d %d %d %d %u", 0, 1, -1, 63, 64, INT32_MIN, UINT32_MAX);
    CHECK_EQ(drain_all(), 1);
    CHECK_EQ(recs[0].kind, SMCLOG_KIND_LOG);
    CHECK(args_are(&recs[0], want, sizeof want));
}

static void test_i64(void)
{
    static const uint8_t want[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x40,
                                   0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01,
                                   0x01};
    setup(0, 1);
    LOG_INFO(test, "%lld %lld %llu", 1LL << 40, (long long)INT64_MIN, (unsigned long long)UINT64_MAX);
    CHECK_EQ(drain_all(), 1);
    CHECK(args_are(&recs[0], want, sizeof want));
}

static void test_floats(void)
{
    static const uint8_t want[] = {0x00, 0x00, 0xC0, 0x3F, 0x00, 0x00, 0x80, 0xBE};
    setup(0, 1);
    LOG_INFO(test, "%f %f", 1.5f, -0.25);
    CHECK_EQ(drain_all(), 1);
    CHECK(args_are(&recs[0], want, sizeof want));
}

/* smclog_f32_bits() against the compiler's conversion: edge cases and many random doubles. */
static uint32_t cast_bits(double d)
{
    float f = (float)d;
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static void f32_case(uint64_t bits)
{
    double d;
    memcpy(&d, &bits, 8);
    uint32_t got = smclog_f32_bits((uint32_t)(bits >> 32), (uint32_t)bits);
    if (d != d) { /* NaN: any quiet NaN with the sign kept */
        CHECK((got & 0x7FC00000u) == 0x7FC00000u && (got >> 31) == (uint32_t)(bits >> 63));
        return;
    }
    if (got != cast_bits(d)) {
        CHECK_EQ(got, cast_bits(d));
        fprintf(stderr, "  double bits %016llx\n", (unsigned long long)bits);
    }
}

static void test_f32_bits(void)
{
    static const double edge[] = {0.0, -0.0, 1.0, -1.5, 0.1, 3.4028234663852886e38, 3.4028235677973366e38,
                                  1e39, -1e39, 1.1754943508222875e-38, 1.401298464324817e-45,
                                  7.006492321624085e-46, 7.006492321624087e-46, 2.1019476964872256e-45,
                                  1e-50, 5e-324, 1.0000000596046448, 1.0000001788139343};
    for (unsigned i = 0; i < sizeof edge / sizeof edge[0]; i++) {
        uint64_t b;
        memcpy(&b, &edge[i], 8);
        f32_case(b);
    }
    f32_case(0x7FF0000000000000ull); /* infinity */
    f32_case(0xFFF8000000000001ull); /* NaN */
    uint64_t x = 0x9E3779B97F4A7C15ull;
    for (unsigned i = 0; i < 200000; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        /* exponents around the float's range, where rounding and subnormals happen */
        uint64_t e = 1023 - 160 + (x >> 40) % 320;
        f32_case((x & 0x800FFFFFFFFFFFFFull) | e << 52);
        if (i % 4 == 0)
            f32_case(x); /* anything at all */
    }
}

static void test_strings(void)
{
    static const uint8_t want[] = {3, 'a', 'b', 'c', 0, 6, '(', 'n', 'u', 'l', 'l', ')'};
    char longs[201];
    const size_t room = SMCLOG_REC_MAX - 11 - 1;

    memset(longs, 'z', 200);
    longs[200] = 0;
    setup(0, 1);
    LOG_INFO(test, "%s %s %s", "abc", "", (const char *)NULL);
    LOG_INFO(test, "%s", longs);
    LOG_INFO(test, "%s %d", longs, 5);
    CHECK_EQ(drain_all(), 3);
    CHECK(args_are(&recs[0], want, sizeof want));
    CHECK_EQ(recs[1].kind, SMCLOG_KIND_LOG);
    CHECK_EQ(recs[1].args[0], 0x80 | room); /* cut */
    CHECK_EQ(recs[1].nargs, 1 + room);
    CHECK_EQ(recs[2].kind, SMCLOG_KIND_LOG_TRUNC); /* the int did not fit */
    CHECK_EQ(recs[2].nargs, 1 + room);
}

static void test_overflow_lost_at_drain(void)
{
    struct smclog_stats st;
    int n, stored;

    setup(0, 1);
    for (int i = 0; i < 100; i++)
        LOG_INFO(test, "fill %d", i);
    smclog_get_stats(&st);
    CHECK(st.records > 10 && st.records < 100);
    CHECK_EQ(st.records + st.lost, 100);
    CHECK(st.used_max <= 512);
    stored = (int)st.records;
    n = drain_all();
    CHECK_EQ(n, stored + 1);
    for (int i = 0; i < stored; i++)
        CHECK_EQ(recs[i].seq, i);
    CHECK_EQ(recs[stored].kind, SMCLOG_KIND_LOST);
    CHECK_EQ(recs[stored].count, 100 - stored);
    CHECK_EQ(recs[stored].seq, stored);
    LOG_INFO(test, "after");
    CHECK_EQ(drain_all(), 1); /* the drops were reported already */
    CHECK_EQ(recs[0].kind, SMCLOG_KIND_LOG);
    CHECK_EQ(recs[0].seq, stored + 1);
}

static void test_overflow_lost_before_record(void)
{
    struct smclog_stats st;
    int n;

    setup(0, 1);
    do {
        LOG_INFO(test, "fill");
        smclog_get_stats(&st);
    } while (st.lost < 3);
    n = parse(out, smclog_drain(out, 60), recs, 256); /* make some room, leave records */
    CHECK(n >= 2);
    LOG_INFO(test, "next");
    n = drain_all();
    CHECK(n >= 3);
    CHECK_EQ(recs[n - 2].kind, SMCLOG_KIND_LOST);
    CHECK_EQ(recs[n - 2].count, 3);
    CHECK_EQ(recs[n - 1].kind, SMCLOG_KIND_LOG);
    CHECK_EQ(recs[n - 1].seq, recs[n - 2].seq + 1);
}

static void test_drain_cap(void)
{
    size_t got;
    int total = 0;

    setup(0, 1);
    for (int i = 0; i < 10; i++)
        LOG_INFO(test, "rec %d", i);
    CHECK_EQ(smclog_drain(out, 5), 0); /* smaller than a frame: no progress, nothing lost */
    while ((got = smclog_drain(out, 40)) > 0) {
        int n = parse(out, got, recs, 256);
        CHECK(got <= 40);
        CHECK(n > 0);
        for (int i = 0; i < n; i++)
            CHECK_EQ(recs[i].seq, total + i);
        total += n;
    }
    CHECK_EQ(total, 10);
}

static void test_wrap(void)
{
    char s[64];
    int bad = 0;

    memset(s, 'q', sizeof s);
    setup(0, 1);
    for (unsigned i = 0; i < 3000; i++) {
        unsigned len = i % 50;
        s[len] = 0;
        LOG_INFO(test, "%s %u", s, i);
        s[len] = 'q';
        if (drain_all() != 1 || recs[0].seq != (i & 0xFFFF) || recs[0].args[0] != len ||
            recs[0].nargs != 1 + len + (i * 2 < 128 ? 1 : i * 2 < 16384 ? 2 : 3))
            bad++;
    }
    CHECK_EQ(bad, 0);
}

static void test_cause(void)
{
    setup(0, 1);
    CHECK_EQ(smclog_cause_begin(), 1);
    LOG_INFO(input, "event");
    LOG_INFO(engine, "decision");
    smclog_cause_end();
    LOG_INFO(engine, "no cause");
    LOGC_INFO(42, engine, "explicit");
    CHECK_EQ(smclog_cause_begin(), 2);
    smclog_cause_end();
    CHECK_EQ(drain_all(), 4);
    CHECK_EQ(recs[0].cause, 1);
    CHECK_EQ(recs[1].cause, 1);
    CHECK_EQ(recs[2].cause, 0);
    CHECK_EQ(recs[3].cause, 42);
}

static void test_header(void)
{
    struct rec *r = &recs[0];
    size_t n;

    setup(777, 1);
    CHECK_EQ(smclog_stream_header(out, 10), 0);
    n = smclog_stream_header(out, sizeof out);
    CHECK_EQ(parse(out, n, recs, 1), 1);
    CHECK_EQ(r->kind, SMCLOG_KIND_HEADER);
    CHECK_EQ(r->b[1], SMCLOG_FORMAT_VERSION);
    CHECK_EQ(le32(r->b + 2), 1000000);
    CHECK_EQ(le32(r->b + 6), 777);
    CHECK_EQ(le32(r->b + 10), 0);
    CHECK_EQ(le32(r->b + 14), SMCLOG_RING_SIZE);
    {
        size_t idlen;
        smclog_port_build_id(&idlen); /* LC_UUID on macOS: 16; the GNU build ID on Linux: 20 */
        CHECK(idlen == 16 || idlen == 20);
        CHECK_EQ(r->b[18], idlen);
        CHECK_EQ(r->len, 19 + idlen);
    }
}

/* --- the recent ring ------------------------------------------------------------------------ */

static struct smclog_recent kept, copy;

/* The zigzag varint of the first argument (records of "... %d" with one int). */
static int first_int(const struct rec *r)
{
    uint32_t v = 0;
    for (unsigned i = 0, shift = 0; i < r->nargs; i++, shift += 7) {
        v |= (uint32_t)(r->args[i] & 0x7F) << shift;
        if (!(r->args[i] & 0x80))
            break;
    }
    return (int)(v >> 1) ^ -(int)(v & 1);
}

/* A reset: the main ring is lost, what the recent ring held is kept (in copy). */
static void reset_keeping(uint32_t start)
{
    memcpy(&copy, &kept, sizeof copy);
    setup(start, 1);
}

static void test_recent_replay(void)
{
    struct rec before[3];
    struct smclog_stats st;

    setup(1000, 10);
    memset(&kept, 0x5A, sizeof kept); /* RAM as power-on leaves it */
    CHECK_EQ(smclog_recent_attach(&kept), SMCLOG_RECENT_NONE);
    CHECK_EQ(kept.magic, SMCLOG_RECENT_MAGIC);
    CHECK_EQ(kept.tag, smclog_build_tag());
    CHECK(smclog_build_tag() != 0); /* LC_UUID */
    CHECK_EQ(drain_all(), 0);       /* nothing replayed */
    LOG_INFO(test, "one %d", 1);
    LOG_INFO(test, "two %d", 2);
    LOG_WARN(test, "three %s", "x");
    CHECK_EQ(drain_all(), 3);
    memcpy(before, recs, sizeof before);

    reset_keeping(50);
    CHECK_EQ(smclog_recent_attach(&copy), 3);
    LOG_INFO(test, "new");
    CHECK_EQ(drain_all(), 4);
    for (int i = 0; i < 3; i++) {
        CHECK_EQ(recs[i].kind, SMCLOG_KIND_LOG | SMCLOG_KIND_PAST);
        CHECK_EQ(recs[i].len, before[i].len);
        CHECK(memcmp(recs[i].b + 1, before[i].b + 1, before[i].len - 1) == 0); /* seq, time, args */
        CHECK_EQ(recs[i].seq, (unsigned)i);
    }
    CHECK_EQ(recs[3].kind, SMCLOG_KIND_LOG);
    CHECK_EQ(recs[3].seq, 0); /* replayed records take no sequence number */
    smclog_get_stats(&st);
    CHECK_EQ(st.records, 4);

    /* the ring started again with this run: only "new" */
    memcpy(&kept, &copy, sizeof kept);
    reset_keeping(0);
    CHECK_EQ(smclog_recent_attach(&copy), 1);
    CHECK_EQ(drain_all(), 1);

    /* detached: nothing more is mirrored */
    CHECK_EQ(smclog_recent_attach(NULL), 0);
    LOG_INFO(test, "not kept");
    memcpy(&kept, &copy, sizeof kept);
    reset_keeping(0);
    CHECK_EQ(smclog_recent_attach(&copy), 0);
}

static void test_recent_keeps_the_latest(void)
{
    struct smclog_stats st;
    int n;

    setup(0, 1);
    memset(&kept, 0, sizeof kept);
    smclog_recent_attach(&kept);
    for (int i = 0; i < 200; i++)
        LOG_INFO(test, "fill %d", i); /* the main ring (512 bytes) drops most of them */
    smclog_get_stats(&st);
    CHECK(st.lost > 100);

    reset_keeping(0);
    n = smclog_recent_attach(&copy);
    CHECK(n > 10 && n < 200); /* SMCLOG_RECENT_SIZE (256 here) of 13- and 14-byte entries */
    CHECK_EQ(drain_all(), n);
    for (int i = 0; i < n; i++)
        CHECK_EQ(first_int(&recs[i]), 200 - n + i); /* the latest, oldest first, dropped ones too */
}

static void test_recent_full_main_ring(void)
{
    struct smclog_stats st;
    int n;

    setup(0, 1);
    memset(&kept, 0, sizeof kept);
    smclog_recent_attach(&kept);
    for (int i = 0; i < 5; i++)
        LOG_INFO(test, "fill %d", i);
    reset_keeping(0);
    for (int i = 0; i < 100; i++)
        LOG_INFO(test, "fill %d", i); /* fills the main ring before the replay */
    smclog_get_stats(&st);
    uint32_t lost = st.lost;
    n = smclog_recent_attach(&copy);
    CHECK_EQ(n, 5);
    smclog_get_stats(&st);
    CHECK_EQ(st.lost, lost + 5);
}

static void test_recent_damaged(void)
{
    setup(0, 1);
    memset(&kept, 0, sizeof kept);
    smclog_recent_attach(&kept);
    LOG_INFO(test, "one %d", 1);
    LOG_INFO(test, "two %d", 2);
    LOG_INFO(test, "three %d", 3);

    reset_keeping(0);
    copy.buf[copy.pos >> 16 & (SMCLOG_RECENT_SIZE - 1)] = 3; /* an entry too short */
    CHECK_EQ(smclog_recent_attach(&copy), SMCLOG_RECENT_NONE);
    CHECK_EQ(drain_all(), 0);
    CHECK_EQ(copy.pos, 0); /* started again */

    reset_keeping(0);
    copy.pos += 1; /* the head inside an entry */
    CHECK_EQ(smclog_recent_attach(&copy), SMCLOG_RECENT_NONE);

    reset_keeping(0);
    copy.pos = (copy.pos & 0xFFFFu) | (uint32_t)(copy.pos - 2000u) << 16; /* more than the ring */
    CHECK_EQ(smclog_recent_attach(&copy), SMCLOG_RECENT_NONE);

    reset_keeping(0);
    copy.buf[1] = SMCLOG_KIND_LOST; /* not a LOG record */
    CHECK_EQ(smclog_recent_attach(&copy), SMCLOG_RECENT_NONE);

    reset_keeping(0);
    copy.magic ^= 1;
    CHECK_EQ(smclog_recent_attach(&copy), SMCLOG_RECENT_NONE);

    reset_keeping(0);
    copy.tag ^= 1; /* another build */
    CHECK_EQ(smclog_recent_attach(&copy), SMCLOG_RECENT_OTHER);
    CHECK_EQ(drain_all(), 0);
    CHECK_EQ(copy.tag, smclog_build_tag());

    /* a reset between moving the tail and writing: the entries left are still read */
    reset_keeping(0);
    uint32_t tail = copy.pos >> 16, head = copy.pos & 0xFFFFu;
    tail += copy.buf[tail & (SMCLOG_RECENT_SIZE - 1)] + 1u;
    copy.pos = head | tail << 16;
    copy.buf[head & (SMCLOG_RECENT_SIZE - 1)] = 0xEE; /* half-written */
    CHECK_EQ(smclog_recent_attach(&copy), 2);
    CHECK_EQ(drain_all(), 2);
    CHECK_EQ(first_int(&recs[0]), 2);
}

static void test_recent_wraps(void)
{
    /* entries across the end of the buffer, many times over */
    setup(0, 1);
    memset(&kept, 0, sizeof kept);
    smclog_recent_attach(&kept);
    for (int round = 0; round < 5; round++) {
        for (int i = 0; i < 37 + round * 11; i++)
            LOG_INFO(test, "a somewhat longer record to wrap differently %d %s", i, "padding");
        reset_keeping(0);
        int n = smclog_recent_attach(&copy);
        CHECK(n > 5);
        CHECK_EQ(drain_all(), n);
        CHECK_EQ(first_int(&recs[n - 1]), 37 + round * 11 - 1);
        memcpy(&kept, &copy, sizeof kept); /* go on from the restarted ring */
        smclog_recent_attach(&kept);
        drain_all();
    }
}

/* --- stream for the decoder test ------------------------------------------------------------- */

static int emit(const char *path)
{
    static uint8_t buf[1 << 15];
    size_t n;
    FILE *f;

    setup(5000, 100);
    smclog_set_level(SMCLOG_MOD_test, SMCLOG_TRACE);
    n = smclog_stream_header(buf, sizeof buf);
    smclog_init();
    LOG_INFO(test, "hello");
    LOG_WARN(test, "ints %d %u %x %X %o %c", -5, 4000000000u, 0xBEEFu, 0xBEEFu, 8, 'A');
    LOG_ERROR(test, "i64 %lld %llu %llx", (long long)INT64_MIN, (unsigned long long)UINT64_MAX,
              0x123456789ALL);
    LOG_DEBUG(test, "float %.2f %g", 1.5f, -0.25);
    LOG_TRACE(test, "str '%s' '%s' [%5s] [%-4s]", "abc", "", "ab", "cd");
    LOG_INFO(test, "percent 100%% of %u, hex %08x", 3u, 0xABCu);
    LOG_INFO(test, "ptr %p", (void *)0x1234);
    LOG_INFO(test, "eight %d %d %d %d %d %d %d %d", 1, 2, 3, 4, 5, 6, 7, 8);
    smclog_cause_begin();
    LOG_INFO(input, "fader %u = %u", 3, 100);
    smclog_cause_end();
    LOGC_INFO(7, engine, "explicit cause");
    n += smclog_drain(buf + n, sizeof buf - n);
    for (int i = 0; i < 60; i++)
        LOG_DEBUG(test, "fill %d", i);
    n += smclog_drain(buf + n, sizeof buf - n);
    LOG_INFO(test, "after loss");
    n += smclog_drain(buf + n, sizeof buf - n);
    f = fopen(path, "wb");
    if (!f || fwrite(buf, 1, n, f) != n || fclose(f) != 0) {
        perror(path);
        return 1;
    }
    return 0;
}

/* Records of a run, a reset, the next run's start with them replayed. */
static int emit_past(const char *path)
{
    static uint8_t buf[1 << 14];
    size_t n;
    FILE *f;

    setup(900000, 1000);
    memset(&kept, 0, sizeof kept);
    smclog_recent_attach(&kept);
    LOG_INFO(test, "before reset %d", 1);
    LOG_WARN(test, "before reset %d", 2);
    LOG_INFO(test, "before reset %d", 3);
    reset_keeping(20);
    smclog_set_level(SMCLOG_MOD_test, SMCLOG_TRACE);
    n = smclog_stream_header(buf, sizeof buf);
    smclog_init();
    smclog_recent_attach(&copy);
    LOG_INFO(test, "after reset");
    n += smclog_drain(buf + n, sizeof buf - n);
    f = fopen(path, "wb");
    if (!f || fwrite(buf, 1, n, f) != n || fclose(f) != 0) {
        perror(path);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--emit") == 0)
        return emit(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--emit-past") == 0)
        return emit_past(argv[2]);
    RUN(test_levels);
    RUN(test_same_site_same_id);
    RUN(test_types_word);
    RUN(test_ints);
    RUN(test_i64);
    RUN(test_floats);
    RUN(test_f32_bits);
    RUN(test_strings);
    RUN(test_overflow_lost_at_drain);
    RUN(test_overflow_lost_before_record);
    RUN(test_drain_cap);
    RUN(test_wrap);
    RUN(test_cause);
    RUN(test_header);
    RUN(test_recent_replay);
    RUN(test_recent_keeps_the_latest);
    RUN(test_recent_full_main_ring);
    RUN(test_recent_damaged);
    RUN(test_recent_wraps);
    printf("%s\n", check_failed ? "FAILED" : "all passed");
    return check_failed != 0;
}
