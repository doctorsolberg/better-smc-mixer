/*
 * smclog: tokenized device logging (SPEC §8.1).
 *
 *   LOG_INFO(input, "fader %u = %u", idx, value);
 *   LOGC_DEBUG(cause, engine, "page %u", page);     explicit cause ID
 *
 * A record holds no text: the format string, file, line, level and module of every call site go
 * into the "smclog" section, which is not loaded on the device (see smclog.ld). The record holds
 * the call site's ID (its offset in that section / 4), a sequence number, a timestamp in µs, a
 * cause ID and the arguments. tools/smclog/smclog.py builds the dictionary from the ELF (or the
 * host Mach-O binary) and decodes records with it; the build ID ties the two together.
 *
 * Arguments: up to 8; integers up to 64 bits, pointers, float/double (sent as float) and strings
 * (char *, sent truncated to fit the record). Their types are found at compile time (_Generic).
 * The format must be a string literal; length modifiers (l, ll, z, ...) are accepted and ignored.
 *
 * Records go into a RAM ring. A low-priority task drains it with smclog_drain() into frames for a
 * transport (USB CDC, SysEx). When the ring is full the record is dropped and counted; the next
 * record that fits is preceded by a LOST record with the count (LOG-03).
 *
 * smclog_write() may be called from tasks and interrupts (the ring is guarded by
 * smclog_port_lock()). The current cause (smclog_cause) belongs to the code that was interrupted,
 * so interrupt handlers should use the LOGC_ forms.
 */
#ifndef SMCLOG_H
#define SMCLOG_H

#include <stddef.h>
#include <stdint.h>

#include "smclog_modules.h"

#ifdef __cplusplus
extern "C" {
#endif

enum smclog_level {
    SMCLOG_OFF = 0,
    SMCLOG_ERROR = 1,
    SMCLOG_WARN = 2,
    SMCLOG_INFO = 3,
    SMCLOG_DEBUG = 4,
    SMCLOG_TRACE = 5,
};

/* Highest level compiled in; calls above it generate no code (LOG-08). */
#ifndef SMCLOG_LEVEL_MAX
#define SMCLOG_LEVEL_MAX SMCLOG_TRACE
#endif
/* Run-time level every module starts with. */
#ifndef SMCLOG_LEVEL_DEFAULT
#define SMCLOG_LEVEL_DEFAULT SMCLOG_INFO
#endif
/* Ring size in bytes, a power of two (LOG-03: 8-16 KB on the device). */
#ifndef SMCLOG_RING_SIZE
#define SMCLOG_RING_SIZE 8192
#endif
/* Largest encoded record; arguments that do not fit are cut (record kind LOG_TRUNC). */
#ifndef SMCLOG_REC_MAX
#define SMCLOG_REC_MAX 96
#endif

#define SMCLOG_MOD_ENUM_(name) SMCLOG_MOD_##name,
enum smclog_module { SMCLOG_MODULES(SMCLOG_MOD_ENUM_) SMCLOG_MOD_COUNT };

/* Wire format, version 1 (docs/logging.md). All fields little-endian. */
#define SMCLOG_FORMAT_VERSION 1
#define SMCLOG_KIND_LOG 0x01       /* seq u16, t_us u32, id u16, cause u16, args */
#define SMCLOG_KIND_LOST 0x02      /* seq u16, t_us u32, count u32 */
#define SMCLOG_KIND_HEADER 0x03    /* version u8, timer_hz u32, t_us u32, lost total u32,
                                      ring size u32, build ID length u8, build ID */
#define SMCLOG_KIND_LOG_TRUNC 0x04 /* as LOG, some arguments missing */
#define SMCLOG_KIND_PAST 0x80      /* flag: a record of the run before the last reset, replayed from
                                      the recent ring (below); otherwise as kind & 0x7F, with that
                                      run's seq and t_us */

#define SMCLOG_T_INT 0u /* zigzag varint of an int */
#define SMCLOG_T_I64 1u /* zigzag varint of a long long */
#define SMCLOG_T_F32 2u /* 4-byte float */
#define SMCLOG_T_STR 3u /* length byte (bit 7: cut), bytes */

/* Call site description in the "smclog" section; followed by the module name, file and format
 * strings, each NUL-terminated. */
#define SMCLOG_META_MAGIC 0x01A54C53u /* "SL", 0xA5, version 1 */
struct smclog_meta {
    uint32_t magic;
    uint16_t size; /* header and strings */
    uint16_t line;
    uint8_t level;
    uint8_t module;
    uint16_t reserved;
    uint32_t types; /* arg count in bits 0-3, then 2 bits per arg (SMCLOG_T_) */
};

#if defined(__APPLE__)
#define SMCLOG_SECTION_ __attribute__((section("__DATA,__smclog"), used, aligned(4)))
#else
#define SMCLOG_SECTION_ __attribute__((section("smclog"), used, aligned(4)))
#endif

#ifndef SMCLOG_FILE
#define SMCLOG_FILE __FILE__
#endif

extern uint8_t smclog_levels[SMCLOG_MOD_COUNT];
extern uint16_t smclog_cause;

/* Writes the "log start" record. Records written earlier (the ring is zero-initialized and
 * ready) are kept. */
void smclog_init(void);
/* Empties the ring and resets levels, counters and the cause (tests, simulator). */
void smclog_reset(void);
void smclog_set_level(enum smclog_module module, enum smclog_level level);
void smclog_write(const struct smclog_meta *meta, uint16_t cause, uint32_t types, ...);

/* A float argument's 4 bytes: the bits of the float nearest to the double whose high and low
 * words are hi and lo (ties to even), as a cast would give, in integer arithmetic. */
uint32_t smclog_f32_bits(uint32_t hi, uint32_t lo);

/* Fill out with whole frames (COBS, CRC-16, 0x00 delimiter); returns the bytes written. A record
 * that does not fit stays in the ring. Single consumer. */
size_t smclog_drain(uint8_t *out, size_t cap);
/* One HEADER frame for the start of a stream (LOG-12); 0 if cap is too small. */
size_t smclog_stream_header(uint8_t *out, size_t cap);
/* Bytes a record of n bytes takes as a frame: the CRC, the COBS overhead and the delimiter. */
#define SMCLOG_FRAME_LEN(n) ((n) + 2u + ((n) + 2u) / 254u + 2u)

/* Largest frame smclog_drain() produces: a buffer of this size always makes progress. */
#define SMCLOG_FRAME_MAX SMCLOG_FRAME_LEN(SMCLOG_REC_MAX)

/* The frame format for other records on a log stream (the command link, docs/link.md): frames
 * rec[0..n) into out. rec must have 2 spare bytes after n (the CRC is put there). Returns the
 * frame's length, or 0 when it does not fit in cap. */
size_t smclog_frame(uint8_t *rec, size_t n, uint8_t *out, size_t cap);

/* CRC-16/CCITT-FALSE, the frames' check (docs/logging.md, "Frames"). */
uint16_t smclog_crc16(const uint8_t *p, size_t n);

struct smclog_stats {
    uint32_t records;    /* records written */
    uint32_t lost;       /* records dropped */
    uint32_t used;       /* ring bytes in use */
    uint32_t used_max;   /* high-water mark */
};
void smclog_get_stats(struct smclog_stats *stats);

/* --- the recent records: a flight recorder across a reset (LOG-10, LOG-11) ------------------- */

/* Every record written also goes into this ring, the oldest overwritten, whether or not the main
 * ring had room for it (a dropped record carries the seq it would have had). The platform keeps
 * the ring in RAM that a reset does not clear, so after a crash, a watchdog or any other reset the
 * latest records of the previous run are still there. At start, smclog_recent_attach() copies them
 * into the log as SMCLOG_KIND_PAST records and starts the ring again (docs/logging.md, "Boot
 * journal").
 *
 * Entries: length u8, then the record as in the stream (LOG or LOG_TRUNC). pos holds head (bits
 * 0-15) and tail (bits 16-31), free-running byte counts, in one word: a reset never sees one moved
 * without the other. A write moves the tail past the entries it overwrites first, then writes,
 * then moves the head; a reset in between loses at most the record being written. */
#ifndef SMCLOG_RECENT_SIZE
#define SMCLOG_RECENT_SIZE 1024
#endif
#define SMCLOG_RECENT_MAGIC 0x52534C53u /* "SLSR" */
struct smclog_recent {
    uint32_t magic;
    uint32_t tag; /* smclog_build_tag() of the build that wrote it */
    uint32_t pos;
    uint8_t buf[SMCLOG_RECENT_SIZE];
};
#define SMCLOG_RECENT_NONE (-1)  /* not a valid ring: power-on, RAM lost or overwritten */
#define SMCLOG_RECENT_OTHER (-2) /* written by another build: its records would not decode */

/* Copies the records r holds, oldest first, into the log as SMCLOG_KIND_PAST records (counted as
 * records, or as lost when the ring is full), then starts r again and mirrors every new record into
 * it. Returns how many it held, or SMCLOG_RECENT_NONE / SMCLOG_RECENT_OTHER (nothing copied). NULL
 * stops the mirroring (so does smclog_reset()). */
int smclog_recent_attach(struct smclog_recent *r);

/* The build ID's first 4 bytes, little-endian (0 without one): ties what RAM keeps across a reset
 * to the build that wrote it. */
uint32_t smclog_build_tag(void);

/* Cause chains (LOG-04): an input event takes a new cause ID; records written until
 * smclog_cause_end() refer to it. */
uint16_t smclog_cause_begin(void);
static inline void smclog_cause_end(void) { smclog_cause = 0; }

/* --- call site macros ------------------------------------------------------------------------ */

#define SMCLOG_T_(x) _Generic((x), \
    char *: SMCLOG_T_STR, const char *: SMCLOG_T_STR, \
    float: SMCLOG_T_F32, double: SMCLOG_T_F32, \
    default: (sizeof(x) <= sizeof(int) ? SMCLOG_T_INT : SMCLOG_T_I64))
#define SMCLOG_TA_(x, i) ((uint32_t)SMCLOG_T_(x) << (4 + 2 * (i)))

#define SMCLOG_NARGS_(...) SMCLOG_NARGS2_(_, ##__VA_ARGS__, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define SMCLOG_NARGS2_(_0, _1, _2, _3, _4, _5, _6, _7, _8, n, ...) n
#define SMCLOG_CAT_(a, b) SMCLOG_CAT2_(a, b)
#define SMCLOG_CAT2_(a, b) a##b
#define SMCLOG_TYPES_(...) SMCLOG_CAT_(SMCLOG_TYPES_, SMCLOG_NARGS_(__VA_ARGS__))(__VA_ARGS__)
#define SMCLOG_TYPES_0() 0u
#define SMCLOG_TYPES_1(a) (1u | SMCLOG_TA_(a, 0))
#define SMCLOG_TYPES_2(a, b) (2u | SMCLOG_TA_(a, 0) | SMCLOG_TA_(b, 1))
#define SMCLOG_TYPES_3(a, b, c) (3u | SMCLOG_TA_(a, 0) | SMCLOG_TA_(b, 1) | SMCLOG_TA_(c, 2))
#define SMCLOG_TYPES_4(a, b, c, d) \
    (4u | SMCLOG_TA_(a, 0) | SMCLOG_TA_(b, 1) | SMCLOG_TA_(c, 2) | SMCLOG_TA_(d, 3))
#define SMCLOG_TYPES_5(a, b, c, d, e) \
    (5u | SMCLOG_TA_(a, 0) | SMCLOG_TA_(b, 1) | SMCLOG_TA_(c, 2) | SMCLOG_TA_(d, 3) | \
     SMCLOG_TA_(e, 4))
#define SMCLOG_TYPES_6(a, b, c, d, e, f) \
    (6u | SMCLOG_TA_(a, 0) | SMCLOG_TA_(b, 1) | SMCLOG_TA_(c, 2) | SMCLOG_TA_(d, 3) | \
     SMCLOG_TA_(e, 4) | SMCLOG_TA_(f, 5))
#define SMCLOG_TYPES_7(a, b, c, d, e, f, g) \
    (7u | SMCLOG_TA_(a, 0) | SMCLOG_TA_(b, 1) | SMCLOG_TA_(c, 2) | SMCLOG_TA_(d, 3) | \
     SMCLOG_TA_(e, 4) | SMCLOG_TA_(f, 5) | SMCLOG_TA_(g, 6))
#define SMCLOG_TYPES_8(a, b, c, d, e, f, g, h) \
    (8u | SMCLOG_TA_(a, 0) | SMCLOG_TA_(b, 1) | SMCLOG_TA_(c, 2) | SMCLOG_TA_(d, 3) | \
     SMCLOG_TA_(e, 4) | SMCLOG_TA_(f, 5) | SMCLOG_TA_(g, 6) | SMCLOG_TA_(h, 7))

#define SMCLOG_(lvl, mod, cause, fmt, ...)                                                     \
    do {                                                                                       \
        if ((lvl) <= SMCLOG_LEVEL_MAX && (lvl) <= smclog_levels[SMCLOG_MOD_##mod]) {           \
            static const struct {                                                              \
                struct smclog_meta h;                                                          \
                char module[sizeof(#mod)];                                                     \
                char file[sizeof(SMCLOG_FILE)];                                                \
                char format[sizeof(fmt)];                                                      \
            } smclog_site_ SMCLOG_SECTION_ = {                                                 \
                {SMCLOG_META_MAGIC,                                                            \
                 (uint16_t)(sizeof(struct smclog_meta) + sizeof(#mod) + sizeof(SMCLOG_FILE) +  \
                            sizeof(fmt)),                                                      \
                 (uint16_t)__LINE__, (lvl), SMCLOG_MOD_##mod, 0, SMCLOG_TYPES_(__VA_ARGS__)},  \
                #mod, SMCLOG_FILE, fmt};                                                       \
            smclog_write(&smclog_site_.h, (cause), SMCLOG_TYPES_(__VA_ARGS__), ##__VA_ARGS__); \
        }                                                                                      \
    } while (0)

#define LOG_ERROR(mod, fmt, ...) SMCLOG_(SMCLOG_ERROR, mod, smclog_cause, fmt, ##__VA_ARGS__)
#define LOG_WARN(mod, fmt, ...) SMCLOG_(SMCLOG_WARN, mod, smclog_cause, fmt, ##__VA_ARGS__)
#define LOG_INFO(mod, fmt, ...) SMCLOG_(SMCLOG_INFO, mod, smclog_cause, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(mod, fmt, ...) SMCLOG_(SMCLOG_DEBUG, mod, smclog_cause, fmt, ##__VA_ARGS__)
#define LOG_TRACE(mod, fmt, ...) SMCLOG_(SMCLOG_TRACE, mod, smclog_cause, fmt, ##__VA_ARGS__)

#define LOGC_ERROR(cause, mod, fmt, ...) SMCLOG_(SMCLOG_ERROR, mod, cause, fmt, ##__VA_ARGS__)
#define LOGC_WARN(cause, mod, fmt, ...) SMCLOG_(SMCLOG_WARN, mod, cause, fmt, ##__VA_ARGS__)
#define LOGC_INFO(cause, mod, fmt, ...) SMCLOG_(SMCLOG_INFO, mod, cause, fmt, ##__VA_ARGS__)
#define LOGC_DEBUG(cause, mod, fmt, ...) SMCLOG_(SMCLOG_DEBUG, mod, cause, fmt, ##__VA_ARGS__)
#define LOGC_TRACE(cause, mod, fmt, ...) SMCLOG_(SMCLOG_TRACE, mod, cause, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* SMCLOG_H */
