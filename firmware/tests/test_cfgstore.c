/*
 * Host tests of the configuration in flash (firmware/store/cfgstore.c) on a model of NOR flash:
 * an erase sets a sector to 0xFF, a program only clears bits and stays within its page. A power cut
 * can come before any flash operation; the one it hits is half done (an erase leaves noise, a
 * program writes half its bytes). After every possible cut the next start must load the newest
 * complete configuration, byte for byte. Run: make -C firmware test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfgstore.h"
#include "check.h"
#include "smclog.h"
#include "smclog_host.h"

int check_failed;

#define AREA (CFGSTORE_SLOTS * CFGSTORE_SLOT_SIZE)

static struct {
    uint8_t mem[AREA];
    long ops_left; /* operations until the power cut (-1: none) */
    bool dead;     /* the power is gone: every operation fails */
    bool fail_program; /* the next program fails (no power cut) */
    unsigned bad_addr; /* a program outside a page, an erase not on a sector, out of the area */
    unsigned erases, programs;
} F;

static bool in_area(uint32_t addr, uint32_t len)
{
    return addr >= CFGSTORE_FLASH_AT && addr + len <= CFGSTORE_FLASH_AT + AREA;
}

/* false when the power is gone; a cut halves this operation */
static bool power(bool *half)
{
    *half = false;
    if (F.dead)
        return false;
    if (F.ops_left == 0) {
        F.dead = true;
        *half = true;
        return true;
    }
    if (F.ops_left > 0)
        F.ops_left--;
    return true;
}

static bool f_read(void *ctx, uint32_t addr, uint8_t *buf, uint32_t len)
{
    (void)ctx;
    if (F.dead || !in_area(addr, len))
        return false;
    memcpy(buf, F.mem + (addr - CFGSTORE_FLASH_AT), len);
    return true;
}

static bool f_erase(void *ctx, uint32_t addr)
{
    bool half;
    (void)ctx;
    if (addr % CFGSTORE_SECTOR || !in_area(addr, CFGSTORE_SECTOR)) {
        F.bad_addr++;
        return false;
    }
    if (!power(&half))
        return false;
    uint8_t *p = F.mem + (addr - CFGSTORE_FLASH_AT);
    if (half) {
        for (unsigned i = 0; i < CFGSTORE_SECTOR; i++)
            p[i] = (uint8_t)rand(); /* an erase cut short: anything */
        return false;
    }
    memset(p, 0xFF, CFGSTORE_SECTOR);
    F.erases++;
    return true;
}

static bool f_program(void *ctx, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    bool half;
    (void)ctx;
    if (!len || addr / CFGSTORE_PAGE != (addr + len - 1) / CFGSTORE_PAGE || !in_area(addr, len)) {
        F.bad_addr++;
        return false;
    }
    if (F.fail_program) {
        F.fail_program = false;
        return false;
    }
    if (!power(&half))
        return false;
    uint8_t *p = F.mem + (addr - CFGSTORE_FLASH_AT);
    for (uint32_t i = 0; i < (half ? len / 2 : len); i++)
        p[i] &= buf[i]; /* NOR: bits only go from 1 to 0 */
    F.programs++;
    return !half;
}

static const struct cfgstore_flash FL = {0, f_read, f_erase, f_program};

static uint8_t A[CFGSTORE_MAX], B[CFGSTORE_MAX], C[CFGSTORE_MAX], BUF[CFGSTORE_MAX];

static void fill(uint8_t *p, uint32_t n, unsigned seed)
{
    srand(seed);
    for (uint32_t i = 0; i < n; i++)
        p[i] = (uint8_t)rand();
}

static void fresh(void)
{
    smclog_reset();
    memset(&F, 0, sizeof F);
    F.ops_left = -1;
    for (unsigned i = 0; i < AREA; i++)
        F.mem[i] = (uint8_t)(i * 7 + 3); /* whatever the flash held before: not a slot */
}

static int save(struct cfgstore *s, const uint8_t *p, uint32_t n)
{
    if (!cfgstore_begin(s, p, n))
        return -1;
    for (int k = 0; k < 10000; k++) {
        int r = cfgstore_step(s);
        if (r != CFGSTORE_MORE)
            return r;
    }
    return -2;
}

/* A start: a new store loads from the flash as it is. */
static uint32_t boot(struct cfgstore *s, uint32_t *gen)
{
    F.dead = false;
    F.ops_left = -1;
    cfgstore_init(s, &FL);
    return cfgstore_load(s, BUF, sizeof BUF, gen, NULL, NULL);
}

static void test_empty_then_saves(void)
{
    struct cfgstore s;
    uint32_t gen = 99;
    fresh();
    fill(A, 6551, 1);
    fill(B, 300, 2);
    fill(C, CFGSTORE_MAX, 3);
    CHECK_EQ(boot(&s, &gen), 0);
    CHECK_EQ(gen, 99); /* untouched */
    CHECK_EQ(cfgstore_generation(&s), 0);

    CHECK_EQ(save(&s, A, 6551), CFGSTORE_DONE);
    CHECK_EQ(s.target, 0);
    CHECK_EQ(F.erases, 2); /* 32 + 6551 bytes: two sectors */
    CHECK_EQ(boot(&s, &gen), 6551);
    CHECK_EQ(gen, 1);
    CHECK(memcmp(BUF, A, 6551) == 0);

    CHECK_EQ(save(&s, B, 300), CFGSTORE_DONE); /* into the other slot */
    CHECK_EQ(s.target, 1);
    CHECK_EQ(boot(&s, &gen), 300);
    CHECK_EQ(gen, 2);
    CHECK(memcmp(BUF, B, 300) == 0);

    CHECK_EQ(save(&s, C, CFGSTORE_MAX), CFGSTORE_DONE); /* the largest, back into slot A */
    CHECK_EQ(s.target, 0);
    CHECK_EQ(cfgstore_generation(&s), 3);
    CHECK_EQ(boot(&s, &gen), CFGSTORE_MAX);
    CHECK_EQ(gen, 3);
    CHECK(memcmp(BUF, C, CFGSTORE_MAX) == 0);
    CHECK_EQ(s.stats.saves, 0); /* a new store */
    CHECK_EQ(F.bad_addr, 0);

    CHECK(!cfgstore_begin(&s, A, 0));
    CHECK(!cfgstore_begin(&s, A, CFGSTORE_MAX + 1));
}

/* A power cut before every flash operation of a save: the next start has the old configuration,
 * or the new one once its header is down, never anything else. */
static void test_power_cut_anywhere(void)
{
    struct cfgstore s;
    uint32_t gen;
    unsigned total;
    fresh();
    fill(A, 5000, 4);
    fill(B, 9000, 5);
    CHECK_EQ(boot(&s, &gen), 0);
    CHECK_EQ(save(&s, A, 5000), CFGSTORE_DONE);
    /* how many operations a save of B takes */
    static uint8_t keep[AREA];
    memcpy(keep, F.mem, AREA);
    F.erases = F.programs = 0;
    CHECK_EQ(boot(&s, &gen), 5000);
    CHECK_EQ(save(&s, B, 9000), CFGSTORE_DONE);
    total = F.erases + F.programs;
    CHECK(total > 10);
    unsigned old = 0, new = 0;
    for (unsigned cut = 0; cut <= total; cut++) {
        memcpy(F.mem, keep, AREA);
        CHECK_EQ(boot(&s, &gen), 5000);
        F.ops_left = cut;
        int r = save(&s, B, 9000);
        CHECK(cut == total ? r == CFGSTORE_DONE : r == CFGSTORE_FAILED);
        uint32_t n = boot(&s, &gen);
        if (n == 5000 && gen == 1 && memcmp(BUF, A, 5000) == 0) {
            old++;
        } else if (n == 9000 && gen == 2 && memcmp(BUF, B, 9000) == 0) {
            new++;
        } else {
            check_failed++;
            fprintf(stderr, "test_cfgstore: cut before operation %u: loaded %u bytes, generation %u\n", cut,
                    n, gen);
        }
        /* and a save after the cut goes through */
        CHECK_EQ(save(&s, A, 5000), CFGSTORE_DONE);
        CHECK_EQ(boot(&s, &gen), 5000);
    }
    CHECK_EQ(old, total);  /* every cut before the header's program completes keeps the old one */
    CHECK_EQ(new, 1);      /* no cut */
    CHECK_EQ(F.bad_addr, 0);
}

/* A newest slot that does not check, or that the firmware does not take: the older one. */
static bool reject_b(void *ctx, const uint8_t *blob, uint32_t len)
{
    (void)ctx;
    return !(len == 700 && memcmp(blob, B, 700) == 0);
}

static void test_fallback(void)
{
    struct cfgstore s;
    uint32_t gen;
    fresh();
    fill(A, 600, 6);
    fill(B, 700, 7);
    boot(&s, &gen);
    CHECK_EQ(save(&s, A, 600), CFGSTORE_DONE);
    CHECK_EQ(save(&s, B, 700), CFGSTORE_DONE);

    cfgstore_init(&s, &FL);
    CHECK_EQ(cfgstore_load(&s, BUF, sizeof BUF, &gen, reject_b, NULL), 600);
    CHECK_EQ(gen, 1);
    /* the next save goes above generation 2 and into slot B, the one not loaded */
    CHECK_EQ(save(&s, A, 600), CFGSTORE_DONE);
    CHECK_EQ(s.target, 1);
    CHECK_EQ(cfgstore_generation(&s), 3);

    F.mem[CFGSTORE_SLOT_SIZE + CFGSTORE_HEADER + 10] ^= 0x01; /* slot B's form: a bit flipped */
    CHECK_EQ(boot(&s, &gen), 600);
    CHECK_EQ(gen, 1); /* slot A */
    CHECK_EQ(s.newest, 0);

    /* a form larger than this build takes is passed over */
    cfgstore_init(&s, &FL);
    CHECK_EQ(cfgstore_load(&s, BUF, 599, &gen, NULL, NULL), 0);
}

/* A failed program: FAILED, the saved configuration as it was, the next save goes through. */
static void test_failure(void)
{
    struct cfgstore s;
    uint32_t gen;
    fresh();
    fill(A, 1000, 8);
    fill(B, 2000, 9);
    boot(&s, &gen);
    CHECK_EQ(save(&s, A, 1000), CFGSTORE_DONE);
    F.fail_program = true;
    CHECK_EQ(save(&s, B, 2000), CFGSTORE_FAILED);
    CHECK_EQ(s.stats.failures, 1);
    CHECK(!cfgstore_busy(&s));
    CHECK_EQ(cfgstore_generation(&s), 1);
    CHECK_EQ(boot(&s, &gen), 1000);
    CHECK(memcmp(BUF, A, 1000) == 0);
    CHECK_EQ(save(&s, B, 2000), CFGSTORE_DONE);
    CHECK_EQ(boot(&s, &gen), 2000);
    CHECK_EQ(gen, 2);
}

int main(void)
{
    smclog_init();
    RUN(test_empty_then_saves);
    RUN(test_power_cut_anywhere);
    RUN(test_fallback);
    RUN(test_failure);
    printf("test_cfgstore: %s\n", check_failed ? "FAILED" : "all ok");
    return check_failed ? 1 : 0;
}
