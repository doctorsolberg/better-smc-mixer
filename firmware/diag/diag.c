/* Diagnostics across a reset (diag.h). */
#include "diag.h"

#include <string.h>

/* A hook between the steps of diag_boot_begin(), for the boot-mark build only
 * (platform/br25/bootmark_br25.h: -DDIAG_STEP_HOOK=bootmark_step); nothing elsewhere. */
#ifdef DIAG_STEP_HOOK
void DIAG_STEP_HOOK(unsigned char step);
#define DIAG_STEP(n) DIAG_STEP_HOOK(n)
#else
#define DIAG_STEP(n) ((void)0)
#endif

static uint32_t keep_check(const struct diag_keep *k)
{
    return diag_check(k, offsetof(struct diag_keep, check));
}

static void keep_seal(struct diag_keep *k)
{
    k->magic = DIAG_KEEP_MAGIC;
    k->check = keep_check(k);
}

bool diag_crash_valid(const struct diag_crash *c)
{
    return c->magic == DIAG_CRASH_MAGIC && c->check == diag_check(c, offsetof(struct diag_crash, check));
}

void diag_keep_heartbeat(struct diag_keep *k, uint32_t seq, uint32_t up_ms)
{
    k->last_seq = seq;
    k->last_up_ms = up_ms;
    keep_seal(k);
}

/* --- records ---------------------------------------------------------------------------------- */

static void log_head(const struct diag_boot *b)
{
    LOG_INFO(boot, "boot %u since the RAM was lost: reset cause 0x%x (%s); firmware %s, build tag %08x",
             b->boot, b->reset_raw, b->reset, b->version, b->tag);
}

static void log_prev(const struct diag_boot *b)
{
    if (!b->have_prev)
        LOG_INFO(boot, "previous run: nothing kept (power-on, or the RAM was not kept)");
    else if (!b->prev_seq)
        LOG_INFO(boot, "previous run: ended before its first heartbeat");
    else
        LOG_INFO(boot, "previous run: last heartbeat %u, %u ms after it started", b->prev_seq,
                 b->prev_up_ms);
}

static void log_crash(const struct diag_boot *b)
{
    const struct diag_crash *c = &b->crash;
    const uint32_t *r = c->reg, *s = c->stack;

    if (!b->have_crash) {
        LOG_INFO(boot, "previous run: no crash record");
        return;
    }
    LOG_ERROR(boot, "crash in boot %u at about %u ms: cause %u, task %s, frame at 0x%08x; build tag %08x",
              c->boot, c->t_ms, c->cause, c->task, c->frame, c->tag);
    LOG_ERROR(boot, "crash: RETI 0x%08x RETE 0x%08x RETX 0x%08x RETS 0x%08x PSR 0x%08x ICFG 0x%08x "
              "USP 0x%08x SSP 0x%08x", r[DIAG_RETI], r[DIAG_RETE], r[DIAG_RETX], r[DIAG_RETS],
              r[DIAG_PSR], r[DIAG_ICFG], r[DIAG_USP], r[DIAG_SSP]);
    LOG_ERROR(boot, "crash: EMU_MSG 0x%08x DEBUG_MSG 0x%08x PRP_MMU_MSG 0x%08x PRP_WR_LIMIT_MSG 0x%08x; "
              "trace 0x%08x 0x%08x 0x%08x 0x%08x", r[DIAG_EMU_MSG], r[DIAG_DEBUG_MSG],
              r[DIAG_PRP_MMU_MSG], r[DIAG_PRP_WR_LIMIT_MSG], c->trace[0], c->trace[1], c->trace[2],
              c->trace[3]);
    LOG_ERROR(boot, "crash: R0-R7 %08x %08x %08x %08x %08x %08x %08x %08x", r[0], r[1], r[2], r[3],
              r[4], r[5], r[6], r[7]);
    LOG_ERROR(boot, "crash: R8-R15 %08x %08x %08x %08x %08x %08x %08x %08x", r[8], r[9], r[10],
              r[11], r[12], r[13], r[14], r[15]);
    if (c->stack_at) {
        LOG_ERROR(boot, "crash: stack at 0x%08x: %08x %08x %08x %08x %08x %08x %08x", c->stack_at,
                  s[0], s[1], s[2], s[3], s[4], s[5], s[6]);
        LOG_ERROR(boot, "crash: stack at 0x%08x: %08x %08x %08x %08x %08x %08x %08x",
                  c->stack_at + 28u, s[7], s[8], s[9], s[10], s[11], s[12], s[13]);
    }
}

static void log_recent(const struct diag_boot *b)
{
    if (b->recent == SMCLOG_RECENT_NONE)
        LOG_INFO(boot, "previous run: no recent log records (power-on, or the RAM was not kept)");
    else if (b->recent == SMCLOG_RECENT_OTHER)
        LOG_WARN(boot, "previous run: its recent log records are from another build, not replayed");
    else
        LOG_INFO(boot, "previous run: its latest %d log records replayed before this journal",
                 b->recent);
}

static void log_mark(const struct diag_boot *b, unsigned i)
{
    LOG_INFO(boot, "init %s: %u us (done at %u us)", b->mark[i].what, b->mark[i].took_us,
             b->mark[i].at_us);
}

static void log_done(const struct diag_boot *b)
{
    LOG_INFO(boot, "boot done: %u us after the journal started (at %u us), %u parts timed",
             b->done_us - b->start_us, b->start_us, b->marks);
}

/* --- the journal ------------------------------------------------------------------------------ */

void diag_boot_begin(struct diag_boot *b, struct diag_keep *k, uint32_t now_us, uint32_t reset_raw,
                     const char *reset, const char *version)
{
    memset(b, 0, sizeof *b);
    b->reset_raw = reset_raw;
    b->reset = reset;
    b->version = version;
    b->tag = smclog_build_tag();
    b->start_us = b->last_us = now_us;
    DIAG_STEP(0x70);

    /* the records first: everything this run logs from here on is mirrored */
    b->recent = smclog_recent_attach(&k->recent);
    DIAG_STEP(0x71);

    if (k->magic == DIAG_KEEP_MAGIC && k->check == keep_check(k)) {
        b->have_prev = true;
        b->boot = k->boots + 1;
        b->prev_seq = k->last_seq;
        b->prev_up_ms = k->last_up_ms;
    } else {
        b->boot = 1;
    }
    DIAG_STEP(0x72);
    if (diag_crash_valid(&k->crash)) {
        b->have_crash = true;
        b->crash = k->crash;
    }
    DIAG_STEP(0x73);
    memset(&k->crash, 0, sizeof k->crash); /* taken: the next boot must not report it again */
    k->boots = b->boot;
    k->last_seq = 0;
    k->last_up_ms = 0;
    keep_seal(k);
    DIAG_STEP(0x74);

    log_head(b);
    DIAG_STEP(0x75);
    log_prev(b);
    DIAG_STEP(0x76);
    log_crash(b);
    DIAG_STEP(0x77);
    log_recent(b);
}

void diag_boot_mark(struct diag_boot *b, const char *what, uint32_t now_us)
{
    unsigned i = b->marks < DIAG_MARKS ? b->marks : DIAG_MARKS - 1; /* full: the last one is reused */
    b->mark[i].what = what;
    b->mark[i].at_us = now_us;
    b->mark[i].took_us = now_us - b->last_us;
    b->last_us = now_us;
    if (b->marks < DIAG_MARKS)
        b->marks++;
    log_mark(b, i);
}

void diag_boot_done(struct diag_boot *b, uint32_t now_us)
{
    b->done_us = now_us;
    b->done = true;
    log_done(b);
}

void diag_boot_log(const struct diag_boot *b)
{
    LOG_INFO(boot, "boot journal, again on request:");
    log_head(b);
    log_prev(b);
    log_crash(b);
    log_recent(b);
    for (unsigned i = 0; i < b->marks; i++)
        log_mark(b, i);
    if (b->done)
        log_done(b);
    else
        LOG_INFO(boot, "boot not done yet");
}
