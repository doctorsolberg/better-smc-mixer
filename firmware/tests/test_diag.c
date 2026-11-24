/*
 * Host tests for the diagnostics across a reset (firmware/diag): what a reset keeps, the crash
 * record, the boot journal. A reset is simulated: the log's main ring is lost (smclog_reset()),
 * struct diag_keep is kept. Run: make -C firmware test
 */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "diag.h"
#include "smclog.h"
#include "smclog_host.h"

int check_failed;

static uint32_t T;
static uint32_t clock_now(void) { return T; }

static struct diag_keep keep;
static struct diag_boot boot;
static uint8_t out[1 << 15];

static uint32_t records(void)
{
    struct smclog_stats st;
    smclog_get_stats(&st);
    return st.records;
}

/* The kind byte of each frame drained (a record's first byte: never 0, so right after the COBS
 * code byte); the number of frames. */
static int drain_kinds(uint8_t *kinds, int max)
{
    size_t n = smclog_drain(out, sizeof out), start = 0;
    int count = 0;
    for (size_t i = 0; i < n; i++) {
        if (out[i])
            continue;
        if (count < max)
            kinds[count] = out[start] > 1 ? out[start + 1] : 0;
        count++;
        start = i + 1;
    }
    return count;
}

static void power_on(void)
{
    smclog_host_set_clock(clock_now);
    smclog_reset();
    memset(&keep, 0xA5, sizeof keep); /* what power-on leaves in RAM */
    T = 5000;
}

static void reset(void)
{
    smclog_reset(); /* the main ring is lost, keep is not */
    T = 3000;
}

static void fill_crash(struct diag_crash *c)
{
    memset(c, 0, sizeof *c);
    c->tag = smclog_build_tag();
    c->boot = keep.boots;
    c->t_ms = 4321;
    c->cause = DIAG_CAUSE_EXCEPTION;
    c->frame = 0x10F00;
    for (unsigned i = 0; i < DIAG_REGS; i++)
        c->reg[i] = 0x1000u + i;
    c->reg[DIAG_RETI] = 0x1E01234u;
    c->trace[0] = 0x1E00500u;
    c->stack_at = 0x20000u;
    c->stack[13] = 0xFFFFFFFFu;
    strcpy(c->task, "smc_ctl");
    diag_crash_seal(c);
}

static void test_first_boot(void)
{
    power_on();
    uint32_t before = records();
    diag_boot_begin(&boot, &keep, T, 0x41, "power-on, soft reset", "test");
    CHECK_EQ(boot.boot, 1);
    CHECK(!boot.have_prev);
    CHECK(!boot.have_crash);
    CHECK_EQ(boot.recent, SMCLOG_RECENT_NONE);
    CHECK_EQ(boot.tag, smclog_build_tag());
    CHECK_EQ(records() - before, 4); /* head, previous run, no crash, no recent records */
    CHECK_EQ(keep.magic, DIAG_KEEP_MAGIC);
    CHECK_EQ(keep.boots, 1);
    CHECK_EQ(keep.last_seq, 0);
    CHECK(!diag_crash_valid(&keep.crash));
    CHECK_EQ(keep.recent.magic, SMCLOG_RECENT_MAGIC);
}

static void test_reset_after_a_crash(void)
{
    uint8_t kinds[64];

    power_on();
    diag_boot_begin(&boot, &keep, T, 1, "power-on", "test");
    LOG_INFO(test, "working");
    diag_keep_heartbeat(&keep, 7, 70000);
    fill_crash(&keep.crash); /* the exception handler, then the chip resets */
    struct diag_crash written = keep.crash;

    reset();
    uint32_t before = records();
    diag_boot_begin(&boot, &keep, T, 0x40, "soft reset", "test");
    CHECK_EQ(boot.boot, 2);
    CHECK(boot.have_prev);
    CHECK_EQ(boot.prev_seq, 7);
    CHECK_EQ(boot.prev_up_ms, 70000);
    CHECK(boot.have_crash);
    CHECK(memcmp(&boot.crash, &written, sizeof written) == 0);
    CHECK(!diag_crash_valid(&keep.crash)); /* taken */
    CHECK_EQ(boot.recent, 5);              /* the first journal (4 records) and "working" */
    /* 5 replayed, head, previous run, crash (7 records with the stack), recent */
    CHECK_EQ(records() - before, 5 + 1 + 1 + 7 + 1);
    int n = drain_kinds(kinds, 64);
    CHECK_EQ(n, 15);
    for (int i = 0; i < 5; i++)
        CHECK_EQ(kinds[i], SMCLOG_KIND_LOG | SMCLOG_KIND_PAST);
    for (int i = 5; i < n; i++)
        CHECK_EQ(kinds[i], SMCLOG_KIND_LOG);
    CHECK_EQ(keep.boots, 2);

    /* the next reset without a crash: no crash record, the previous run ended early */
    reset();
    diag_boot_begin(&boot, &keep, T, 0x04, "watchdog", "test");
    CHECK_EQ(boot.boot, 3);
    CHECK(boot.have_prev);
    CHECK_EQ(boot.prev_seq, 0);
    CHECK(!boot.have_crash);
    CHECK_EQ(boot.recent, 1 + 1 + 7 + 1); /* the second journal: replayed records are not kept again */
}

static void test_damaged(void)
{
    power_on();
    diag_boot_begin(&boot, &keep, T, 1, "power-on", "test");
    fill_crash(&keep.crash);
    keep.crash.reg[DIAG_R0 + 3] ^= 0x100; /* a bit changed after it was sealed */
    keep.last_up_ms++;                    /* written after the check */
    reset();
    diag_boot_begin(&boot, &keep, T, 0x40, "soft reset", "test");
    CHECK_EQ(boot.boot, 1);
    CHECK(!boot.have_prev);
    CHECK(!boot.have_crash);
    CHECK(boot.recent > 0); /* the recent ring has a check of its own */

    /* a crash record of another build is still reported (with its tag) */
    fill_crash(&keep.crash);
    keep.crash.tag ^= 0xFF;
    diag_crash_seal(&keep.crash);
    reset();
    diag_boot_begin(&boot, &keep, T, 0x40, "soft reset", "test");
    CHECK(boot.have_crash);
    CHECK(boot.crash.tag != boot.tag);
}

static void test_marks_and_again(void)
{
    power_on();
    diag_boot_begin(&boot, &keep, T, 1, "power-on", "test");
    T = 5600;
    diag_boot_mark(&boot, "log", T);
    T = 9000;
    diag_boot_mark(&boot, "core", T);
    CHECK_EQ(boot.marks, 2);
    CHECK_EQ(boot.mark[0].took_us, 600);
    CHECK_EQ(boot.mark[1].took_us, 3400);
    CHECK_EQ(boot.mark[1].at_us, 9000);
    uint32_t before = records();
    diag_boot_log(&boot); /* again: + 2 marks + "not done yet" */
    CHECK_EQ(records() - before, 1 + 4 + 2 + 1);
    T = 9500;
    diag_boot_done(&boot, T);
    CHECK(boot.done);
    CHECK_EQ(boot.done_us - boot.start_us, 4500);
    for (unsigned i = 0; i < DIAG_MARKS + 3; i++)
        diag_boot_mark(&boot, "more", T + i);
    CHECK_EQ(boot.marks, DIAG_MARKS);
    CHECK_EQ(boot.mark[DIAG_MARKS - 1].at_us, T + DIAG_MARKS + 2); /* the last one reused */
    before = records();
    diag_boot_log(&boot);
    CHECK_EQ(records() - before, 1 + 4 + DIAG_MARKS + 1);
}

static void test_check(void)
{
    CHECK_EQ(diag_check("", 0), 0x811C9DC5u);
    CHECK_EQ(diag_check("a", 1), 0xE40C292Cu); /* FNV-1a test vectors */
    CHECK_EQ(diag_check("foobar", 6), 0xBF9CF968u);
}

int main(void)
{
    RUN(test_check);
    RUN(test_first_boot);
    RUN(test_reset_after_a_crash);
    RUN(test_damaged);
    RUN(test_marks_and_again);
    printf("test_diag: %s\n", check_failed ? "FAILED" : "all ok");
    return check_failed ? 1 : 0;
}
