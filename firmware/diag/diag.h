/*
 * Diagnostics across a reset: the boot journal (SPEC LOG-11) and the RAM part of the black box
 * (LOG-10: the crash record, the latest records). docs/logging.md, "Boot journal". Portable C11, no
 * allocation; host tests in firmware/tests/test_diag.c.
 *
 * struct diag_keep lives in RAM that a reset does not clear (on BR25 a section of its own, outside
 * .bss and the heap: firmware/platform/br25/diag_br25.c). It holds:
 *
 *   - how many boots since that RAM was last lost, and the previous run's last heartbeat;
 *   - a crash record, written by the platform's exception handler just before it resets the chip;
 *   - the recent log records (smclog.h, struct smclog_recent), written with every record.
 *
 * Each part has a magic and a check, so RAM that power-on left random, or that something before us
 * (the loader) overwrote, reads as "none", never as garbage. Nothing here touches flash (C-6).
 *
 * At start, diag_boot_begin() takes what the previous run left and writes the journal's first
 * records (module "boot"); diag_boot_mark() logs how long each part of the start took;
 * diag_boot_done() ends the journal. diag_boot_log() writes the journal again on request (the
 * link's BOOT_LOG command), from the copy in struct diag_boot.
 */
#ifndef DIAG_H
#define DIAG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "smclog.h"

/* --- the crash record ------------------------------------------------------------------------- */

/* Registers, in the order of the BR25 exception frame (the SDK's exception_irq_handler pushes
 * r0-r15 and, above them, RETI RETE RETX RETS PSR ICFG USP SSP), then debug registers read by the
 * handler. */
enum diag_reg {
    DIAG_R0 = 0,       /* .. DIAG_R0 + 15: r0-r15 */
    DIAG_RETI = 16,    /* return addresses: interrupt, exception, emulation, subroutine */
    DIAG_RETE,
    DIAG_RETX,
    DIAG_RETS,
    DIAG_PSR,
    DIAG_ICFG,
    DIAG_USP,          /* the task stack pointer */
    DIAG_SSP,          /* the interrupt stack pointer */
    DIAG_EMU_MSG,      /* q32DSP EMU_MSG: what the CPU's checks caught */
    DIAG_DEBUG_MSG,    /* JL_DEBUG MSG: memory protection */
    DIAG_PRP_MMU_MSG,  /* JL_DEBUG PRP_MMU_MSG */
    DIAG_PRP_WR_LIMIT_MSG, /* JL_DEBUG PRP_WR_LIMIT_MSG */
    DIAG_REGS
};
#define DIAG_FRAME_REGS 24 /* r0-r15 .. SSP: what the frame holds */
#define DIAG_TRACE 4       /* ETM_PC0-3: the CPU's trace of recent program counters */
#define DIAG_STACK 14      /* words from the task stack (USP) */
#define DIAG_TASK 16       /* the current task's name, NUL-terminated */

#define DIAG_CRASH_MAGIC 0x52434D53u /* "SMCR" */

/* Causes (struct diag_crash.cause): on BR25 the interrupt index the exception came in on. */
#define DIAG_CAUSE_EMU_EXCEPTION 0 /* IRQ_EMUEXCPT_IDX */
#define DIAG_CAUSE_EXCEPTION 1     /* IRQ_EXCEPTION_IDX */
#define DIAG_CAUSE_STALL 2         /* BR25, SMC_STALL_CHECK: the idle task did not run for seconds */
#define DIAG_CAUSE_HOST_SIGNAL 9   /* the host stand-in (firmware/sim/devhost.c) */

struct diag_crash {
    uint32_t magic;
    uint32_t tag;   /* smclog_build_tag() of the build it happened in */
    uint32_t boot;  /* the boot it happened in */
    uint32_t t_ms;  /* about when: the platform's millisecond clock */
    uint32_t cause; /* DIAG_CAUSE_ */
    uint32_t frame; /* where the handler found the frame (its stack pointer) */
    uint32_t reg[DIAG_REGS];
    uint32_t trace[DIAG_TRACE];
    uint32_t stack_at; /* address of stack[0]; 0: not copied (USP outside RAM) */
    uint32_t stack[DIAG_STACK];
    char task[DIAG_TASK];
    uint32_t check; /* diag_check() of everything above */
};

/* FNV-1a over bytes: a check, not protection against tampering. Inline, so the exception handler
 * that seals a record runs no code outside its own section (on BR25 it runs from RAM). */
static inline __attribute__((always_inline)) uint32_t diag_check(const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    uint32_t c = 0x811C9DC5u;
    while (n--)
        c = (c ^ *b++) * 0x01000193u;
    return c;
}

/* The last step of writing a crash record: the magic and the check. */
static inline __attribute__((always_inline)) void diag_crash_seal(struct diag_crash *c)
{
    c->magic = DIAG_CRASH_MAGIC;
    c->check = diag_check(c, offsetof(struct diag_crash, check));
}

bool diag_crash_valid(const struct diag_crash *c);

/* --- what a reset keeps ----------------------------------------------------------------------- */

#define DIAG_KEEP_MAGIC 0x504B4D53u /* "SMKP" */

struct diag_keep {
    uint32_t magic;
    uint32_t boots;      /* this run's boot number since the RAM was lost (1: the first) */
    uint32_t last_seq;   /* the run's latest heartbeat (metrics.h), 0 before the first */
    uint32_t last_up_ms; /* the run's uptime then */
    uint32_t check;      /* diag_check() of the four words above */
    struct diag_crash crash;
    struct smclog_recent recent;
};

/* Each heartbeat: what the next boot reports about this run. */
void diag_keep_heartbeat(struct diag_keep *k, uint32_t seq, uint32_t up_ms);

/* --- the boot journal ------------------------------------------------------------------------- */

#define DIAG_MARKS 12

struct diag_boot {
    uint32_t boot;      /* since the RAM was lost (1: the first, or the RAM was not kept) */
    uint32_t reset_raw; /* the platform's reset cause bits (docs/logging.md) */
    const char *reset;  /* their names */
    const char *version;
    uint32_t tag;       /* smclog_build_tag() */
    bool have_prev;
    uint32_t prev_seq, prev_up_ms;
    int recent;         /* smclog_recent_attach(): records replayed, or SMCLOG_RECENT_NONE/OTHER */
    bool have_crash;
    struct diag_crash crash;
    uint32_t start_us, last_us, done_us;
    bool done;
    unsigned marks;
    struct {
        const char *what; /* a string literal */
        uint32_t at_us, took_us;
    } mark[DIAG_MARKS];
};

/* First thing at start, before other records (they are mirrored into the recent ring from here):
 * takes the previous run's records and crash record from k, starts k for this run and logs the
 * journal's first records. reset_raw/reset: the platform's reset cause; version: the firmware's. */
void diag_boot_begin(struct diag_boot *b, struct diag_keep *k, uint32_t now_us, uint32_t reset_raw,
                     const char *reset, const char *version);
/* A part of the start is done (what: a string literal): logs its time since the previous mark. */
void diag_boot_mark(struct diag_boot *b, const char *what, uint32_t now_us);
/* The start is done. */
void diag_boot_done(struct diag_boot *b, uint32_t now_us);
/* The journal again (without the replayed records, which are logged once). */
void diag_boot_log(const struct diag_boot *b);

#endif /* DIAG_H */
