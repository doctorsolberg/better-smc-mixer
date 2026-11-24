/* smclog: record encoding, the RAM ring and framing (see smclog.h, docs/logging.md). */
#include "smclog.h"

#include <stdarg.h>
#include <stdbool.h>
#include <string.h>

#include "smc_ram.h"
#include "smclog_port.h"

#if (SMCLOG_RING_SIZE & (SMCLOG_RING_SIZE - 1)) != 0 || SMCLOG_RING_SIZE < 256
#error "SMCLOG_RING_SIZE must be a power of two, at least 256"
#endif
#if SMCLOG_REC_MAX > 255 || SMCLOG_REC_MAX < 32
#error "SMCLOG_REC_MAX must be 32..255"
#endif
#if (SMCLOG_RECENT_SIZE & (SMCLOG_RECENT_SIZE - 1)) != 0 || SMCLOG_RECENT_SIZE < 256 || \
    SMCLOG_RECENT_SIZE > 32768
#error "SMCLOG_RECENT_SIZE must be a power of two, 256..32768"
#endif

#if defined(__APPLE__)
extern const char smclog_section_start_[] __asm("section$start$__DATA$__smclog");
#define SECTION_START smclog_section_start_
#else
extern const char __start_smclog[]; /* GNU ld for orphan sections; smclog.ld on the device */
#define SECTION_START __start_smclog
#endif

#define MASK (SMCLOG_RING_SIZE - 1u)
#define RMASK (SMCLOG_RECENT_SIZE - 1u)
#define POS_MASK 0xFFFFu /* the recent ring's head and tail: 16-bit counts */
#define LOG_HDR 11  /* kind, seq u16, t_us u32, id u16, cause u16 */
#define LOST_LEN 11 /* kind, seq u16, t_us u32, count u32 */
#define BUILD_ID_MAX 32
#define FRAME_LEN(n) SMCLOG_FRAME_LEN(n)

uint8_t smclog_levels[SMCLOG_MOD_COUNT] = {[0 ... SMCLOG_MOD_COUNT - 1] = SMCLOG_LEVEL_DEFAULT};
uint16_t smclog_cause;

/* Zero-initialized, so records written before smclog_init() are kept. */
static struct {
    uint8_t buf[SMCLOG_RING_SIZE];
    uint32_t head, tail; /* free-running byte counts: used = head - tail */
    uint32_t used_max;
    uint32_t records, lost_total;
    uint32_t lost; /* records dropped since the last LOST record */
    uint16_t seq;
    uint16_t cause_next;
} ring;

static struct smclog_recent *recent; /* the flight recorder, once attached */

SMC_RAM_CODE static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

SMC_RAM_CODE static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

void smclog_init(void)
{
    LOG_INFO(smclog, "log start: format %u, ring %u bytes, records up to %u bytes",
             SMCLOG_FORMAT_VERSION, SMCLOG_RING_SIZE, SMCLOG_REC_MAX);
}

void smclog_reset(void)
{
    uint32_t s = smclog_port_lock();
    memset(&ring, 0, sizeof ring);
    memset(smclog_levels, SMCLOG_LEVEL_DEFAULT, sizeof smclog_levels);
    smclog_cause = 0;
    recent = NULL;
    smclog_port_unlock(s);
}

void smclog_set_level(enum smclog_module module, enum smclog_level level)
{
    if ((unsigned)module < SMCLOG_MOD_COUNT)
        smclog_levels[module] = (uint8_t)level;
}

SMC_HOT uint16_t smclog_cause_begin(void)
{
    uint32_t s = smclog_port_lock();
    if (++ring.cause_next == 0)
        ring.cause_next = 1;
    smclog_cause = ring.cause_next;
    smclog_port_unlock(s);
    return smclog_cause;
}

void smclog_get_stats(struct smclog_stats *st)
{
    uint32_t s = smclog_port_lock();
    st->records = ring.records;
    st->lost = ring.lost_total;
    st->used = ring.head - ring.tail;
    st->used_max = ring.used_max;
    smclog_port_unlock(s);
}

/* --- ring (lock held) ------------------------------------------------------------------------ */

SMC_RAM_CODE static uint32_t ring_free(void)
{
    return SMCLOG_RING_SIZE - (ring.head - ring.tail);
}

SMC_RAM_CODE static void ring_put(const uint8_t *rec, uint32_t n)
{
    uint32_t off = (ring.head + 1) & MASK, first = SMCLOG_RING_SIZE - off;

    ring.buf[ring.head & MASK] = (uint8_t)n;
    if (first > n)
        first = n;
    memcpy(ring.buf + off, rec, first);
    memcpy(ring.buf, rec + first, n - first);
    ring.head += n + 1;
    if (ring.head - ring.tail > ring.used_max)
        ring.used_max = ring.head - ring.tail;
}

static uint32_t ring_take(uint8_t *rec)
{
    uint32_t n = ring.buf[ring.tail & MASK];
    uint32_t off = (ring.tail + 1) & MASK, first = SMCLOG_RING_SIZE - off;

    if (first > n)
        first = n;
    memcpy(rec, ring.buf + off, first);
    memcpy(rec + first, ring.buf, n - first);
    ring.tail += n + 1;
    return n;
}

SMC_RAM_CODE static void put_lost(uint32_t now)
{
    uint8_t rec[LOST_LEN];

    rec[0] = SMCLOG_KIND_LOST;
    put16(rec + 1, ring.seq++);
    put32(rec + 3, now);
    put32(rec + 7, ring.lost);
    ring_put(rec, LOST_LEN);
    ring.lost = 0;
}

SMC_RAM_CODE static bool ring_room(uint32_t n)
{
    return ring_free() >= n + 1 + (ring.lost ? LOST_LEN + 1 : 0);
}

/* The recent ring (lock held): drops whole entries from the tail until rec fits, then writes it. */
SMC_RAM_CODE static void recent_put(struct smclog_recent *r, const uint8_t *rec, uint32_t n)
{
    uint32_t head = r->pos & POS_MASK, tail = r->pos >> 16;

    for (;;) {
        uint32_t used = (head - tail) & POS_MASK, step;
        if (used + n + 1 <= SMCLOG_RECENT_SIZE)
            break;
        step = r->buf[tail & RMASK] + 1u;
        if (step > used) { /* damaged (a stray write): start again */
            tail = head;
            break;
        }
        tail = (tail + step) & POS_MASK;
    }
    r->pos = head | tail << 16;
    r->buf[head & RMASK] = (uint8_t)n;
    for (uint32_t i = 0; i < n; i++)
        r->buf[(head + 1 + i) & RMASK] = rec[i];
    r->pos = ((head + n + 1) & POS_MASK) | tail << 16;
}

SMC_RAM_CODE static void commit(uint8_t *rec, uint32_t n)
{
    uint32_t s = smclog_port_lock();
    uint32_t now = smclog_port_time_us();
    bool room = ring_room(n);

    if (room && ring.lost)
        put_lost(now);
    put16(rec + 1, ring.seq); /* dropped: the number it would have had */
    put32(rec + 3, now);
    if (room) {
        ring.seq++;
        ring_put(rec, n);
        ring.records++;
    } else {
        ring.lost++;
        ring.lost_total++;
    }
    if (recent)
        recent_put(recent, rec, n);
    smclog_port_unlock(s);
}

/* --- the recent ring across a reset ---------------------------------------------------------- */

uint32_t smclog_build_tag(void)
{
    size_t len = 0;
    const uint8_t *id = smclog_port_build_id(&len);

    if (!id || len < 4)
        return 0;
    return id[0] | (uint32_t)id[1] << 8 | (uint32_t)id[2] << 16 | (uint32_t)id[3] << 24;
}

/* The number of entries in a valid ring, or SMCLOG_RECENT_NONE. */
static int recent_count(const struct smclog_recent *r)
{
    uint32_t head = r->pos & POS_MASK, p = r->pos >> 16;
    int count = 0;

    if (r->magic != SMCLOG_RECENT_MAGIC || ((head - p) & POS_MASK) > SMCLOG_RECENT_SIZE)
        return SMCLOG_RECENT_NONE;
    while (p != head) {
        uint32_t n = r->buf[p & RMASK], kind = r->buf[(p + 1) & RMASK];
        if (n < LOG_HDR || n > SMCLOG_REC_MAX || ((head - p) & POS_MASK) < n + 1 ||
            (kind != SMCLOG_KIND_LOG && kind != SMCLOG_KIND_LOG_TRUNC))
            return SMCLOG_RECENT_NONE;
        p = (p + n + 1) & POS_MASK;
        count++;
    }
    return count;
}

int smclog_recent_attach(struct smclog_recent *r)
{
    uint8_t rec[SMCLOG_REC_MAX];
    uint32_t tag = smclog_build_tag();
    uint32_t s = smclog_port_lock();
    int found = 0;

    recent = NULL;
    if (r) {
        found = recent_count(r);
        if (found > 0 && r->tag != tag)
            found = SMCLOG_RECENT_OTHER;
        if (found > 0) {
            uint32_t now = smclog_port_time_us(), head = r->pos & POS_MASK, p = r->pos >> 16;
            while (p != head) {
                uint32_t n = r->buf[p & RMASK];
                for (uint32_t i = 0; i < n; i++)
                    rec[i] = r->buf[(p + 1 + i) & RMASK];
                rec[0] |= SMCLOG_KIND_PAST;
                if (ring_room(n)) {
                    if (ring.lost)
                        put_lost(now);
                    ring_put(rec, n);
                    ring.records++;
                } else {
                    ring.lost++;
                    ring.lost_total++;
                }
                p = (p + n + 1) & POS_MASK;
            }
        }
        r->pos = 0;
        r->tag = tag;
        r->magic = SMCLOG_RECENT_MAGIC;
        recent = r;
    }
    smclog_port_unlock(s);
    return found;
}

/* --- record encoding ------------------------------------------------------------------------- */

/* Each returns the position after the value, or NULL when it does not fit before end. */
SMC_RAM_CODE static uint8_t *put_varint32(uint8_t *p, const uint8_t *end, uint32_t v)
{
    do {
        if (p == end)
            return NULL;
        *p++ = (uint8_t)((v & 0x7Fu) | (v > 0x7Fu ? 0x80u : 0u));
        v >>= 7;
    } while (v);
    return p;
}

SMC_RAM_CODE static uint8_t *put_varint64(uint8_t *p, const uint8_t *end, uint64_t v)
{
    while (v > 0xFFFFFFFFu) {
        if (p == end)
            return NULL;
        *p++ = (uint8_t)((v & 0x7Fu) | 0x80u);
        v >>= 7;
    }
    return put_varint32(p, end, (uint32_t)v);
}

/* In RAM, as the code that may use it (smc_ram.h): a literal would stay in flash. */
SMC_RAM_DATA static const char null_str[] = "(null)";

SMC_RAM_CODE static uint8_t *put_str(uint8_t *p, const uint8_t *end, const char *s)
{
    size_t room, n = 0;

    if (p == end)
        return NULL;
    if (!s)
        s = null_str;
    room = (size_t)(end - p - 1);
    if (room > 127)
        room = 127;
    while (n < room && s[n])
        n++;
    *p = (uint8_t)(n | (s[n] ? 0x80u : 0u)); /* bit 7: the string was cut */
    memcpy(p + 1, s, n);
    return p + 1 + n;
}

/* The bits of the float nearest to a double (ties to even), in integer arithmetic: the device's
 * soft-float conversion lives in flash, and smclog_write() must run from RAM (smc_ram.h). */
SMC_RAM_CODE uint32_t smclog_f32_bits(uint32_t hi, uint32_t lo)
{
    uint32_t sign = hi & 0x80000000u;
    int exp = (int)((hi >> 20) & 0x7FFu);
    if (exp == 0x7FF) /* infinity, or a NaN (made quiet) */
        return sign | 0x7F800000u | ((hi & 0xFFFFFu) || lo ? 0x400000u : 0u);
    if (exp == 0)
        return sign; /* zero, or a double subnormal: far below the float's range */
    int e = exp - 1023 + 127;
    if (e >= 0xFF)
        return sign | 0x7F800000u;
    uint32_t r = 0x800000u | (hi & 0xFFFFFu) << 3 | lo >> 29; /* 24 bits with the leading 1 */
    bool guard = (lo >> 28) & 1u, sticky = (lo & 0x0FFFFFFFu) != 0;
    if (e <= 0) { /* a float subnormal: s more bits go */
        unsigned s = (unsigned)(1 - e);
        if (s > 24)
            return sign;
        sticky = sticky || guard || (r & ((1u << (s - 1)) - 1u));
        guard = (r >> (s - 1)) & 1u;
        r >>= s;
        e = 0;
    }
    if (guard && (sticky || (r & 1u)))
        r++; /* may carry into the exponent: still the right bits */
    return sign | (((uint32_t)e << 23) + r - (e ? 0x800000u : 0u));
}

SMC_RAM_CODE void smclog_write(const struct smclog_meta *meta, uint16_t cause, uint32_t types, ...)
{
    uint8_t rec[SMCLOG_REC_MAX];
    uint8_t *p = rec + LOG_HDR;
    const uint8_t *end = rec + sizeof rec;
    unsigned count = types & 0xFu;
    va_list ap;

    rec[0] = SMCLOG_KIND_LOG;
    put16(rec + 7, (uint32_t)(((uintptr_t)meta - (uintptr_t)SECTION_START) >> 2));
    put16(rec + 9, cause);
    va_start(ap, types);
    for (unsigned i = 0; i < count; i++) {
        uint8_t *q = NULL;
        switch ((types >> (4 + 2 * i)) & 3u) {
        case SMCLOG_T_INT: {
            uint32_t u = (uint32_t)va_arg(ap, int);
            q = put_varint32(p, end, (u << 1) ^ (0u - (u >> 31))); /* zigzag */
            break;
        }
        case SMCLOG_T_I64: {
            uint64_t u = (uint64_t)va_arg(ap, long long);
            q = put_varint64(p, end, (u << 1) ^ (0u - (u >> 63)));
            break;
        }
        case SMCLOG_T_F32: {
            double d = va_arg(ap, double);
            uint32_t w[2]; /* little-endian on the device and the hosts */
            memcpy(w, &d, sizeof w);
            if (end - p >= 4) {
                put32(p, smclog_f32_bits(w[1], w[0]));
                q = p + 4;
            }
            break;
        }
        default:
            q = put_str(p, end, va_arg(ap, const char *));
            break;
        }
        if (!q) {
            rec[0] = SMCLOG_KIND_LOG_TRUNC;
            break;
        }
        p = q;
    }
    va_end(ap);
    commit(rec, (uint32_t)(p - rec));
}

/* --- framing --------------------------------------------------------------------------------- */

uint16_t smclog_crc16(const uint8_t *p, size_t n)
{
    uint16_t c = 0xFFFF;

    while (n--) {
        c ^= (uint16_t)(*p++ << 8);
        for (int k = 0; k < 8; k++)
            c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}

static size_t cobs_encode(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t code_at = 0, o = 1;
    uint8_t code = 1;

    for (size_t i = 0; i < n; i++) {
        if (in[i] == 0) {
            out[code_at] = code;
            code_at = o++;
            code = 1;
        } else {
            out[o++] = in[i];
            if (++code == 0xFF) {
                out[code_at] = code;
                code_at = o++;
                code = 1;
            }
        }
    }
    out[code_at] = code;
    return o;
}

/* rec must have 2 spare bytes after n for the CRC. */
static size_t frame(uint8_t *rec, size_t n, uint8_t *out)
{
    size_t m;

    put16(rec + n, smclog_crc16(rec, n));
    m = cobs_encode(rec, n + 2, out);
    out[m++] = 0;
    return m;
}

size_t smclog_frame(uint8_t *rec, size_t n, uint8_t *out, size_t cap)
{
    return FRAME_LEN(n) > cap ? 0 : frame(rec, n, out);
}

size_t smclog_drain(uint8_t *out, size_t cap)
{
    uint8_t rec[SMCLOG_REC_MAX + 2];
    size_t done = 0;

    for (;;) {
        uint32_t n, s = smclog_port_lock();
        if (ring.head == ring.tail) {
            if (!ring.lost) {
                smclog_port_unlock(s);
                break;
            }
            put_lost(smclog_port_time_us()); /* drops with nothing written after them */
        }
        if (FRAME_LEN(ring.buf[ring.tail & MASK]) > cap - done) {
            smclog_port_unlock(s);
            break;
        }
        n = ring_take(rec);
        smclog_port_unlock(s);
        done += frame(rec, n, out + done);
    }
    return done;
}

size_t smclog_stream_header(uint8_t *out, size_t cap)
{
    uint8_t rec[19 + BUILD_ID_MAX + 2];
    size_t idlen = 0;
    const uint8_t *id = smclog_port_build_id(&idlen);
    uint32_t s;

    if (!id)
        idlen = 0;
    if (idlen > BUILD_ID_MAX)
        idlen = BUILD_ID_MAX;
    if (FRAME_LEN(19 + idlen) > cap)
        return 0;
    rec[0] = SMCLOG_KIND_HEADER;
    rec[1] = SMCLOG_FORMAT_VERSION;
    put32(rec + 2, SMCLOG_TIMER_HZ);
    s = smclog_port_lock();
    put32(rec + 6, smclog_port_time_us());
    put32(rec + 10, ring.lost_total);
    smclog_port_unlock(s);
    put32(rec + 14, SMCLOG_RING_SIZE);
    rec[18] = (uint8_t)idlen;
    if (idlen)
        memcpy(rec + 19, id, idlen);
    return frame(rec, 19 + idlen, out);
}
