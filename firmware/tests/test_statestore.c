/*
 * Host tests of the panel's state in flash (firmware/store/statestore.c) on a model of NOR flash,
 * as test_cfgstore.c: an erase sets a sector to 0xFF, a program only clears bits and stays within
 * its page; a power cut can come before any flash operation and halves the one it hits. After every
 * cut the next start must load the newest state saved completely. Run: make -C firmware test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "smclog.h"
#include "smclog_host.h"
#include "statestore.h"

int check_failed;

#define AREA (STATESTORE_SECTORS * CFGSTORE_SECTOR)

static struct {
    uint8_t mem[AREA];
    long ops_left; /* operations until the power cut (-1: none) */
    bool dead;
    bool fail_program; /* the next program fails (no power cut) */
    unsigned bad_addr;
    unsigned erases, programs;
} F;

static bool in_area(uint32_t addr, uint32_t len)
{
    return addr >= STATESTORE_FLASH_AT && addr + len <= STATESTORE_FLASH_AT + AREA;
}

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
    memcpy(buf, F.mem + (addr - STATESTORE_FLASH_AT), len);
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
    uint8_t *p = F.mem + (addr - STATESTORE_FLASH_AT);
    if (half) {
        for (unsigned i = 0; i < CFGSTORE_SECTOR; i++)
            p[i] = (uint8_t)rand();
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
    uint8_t *p = F.mem + (addr - STATESTORE_FLASH_AT);
    for (uint32_t i = 0; i < (half ? len / 2 : len); i++)
        p[i] &= buf[i];
    F.programs++;
    return !half;
}

static const struct cfgstore_flash FL = {0, f_read, f_erase, f_program};

static void blank(void)
{
    memset(&F, 0, sizeof F);
    memset(F.mem, 0xFF, sizeof F.mem);
    F.ops_left = -1;
}

/* A fresh start on the flash as it is: what it loads */
static bool start(struct statestore *s, uint32_t *w)
{
    statestore_init(s, &FL);
    return statestore_load(s, w);
}

static void test_empty(void)
{
    struct statestore s;
    uint32_t w = 77;
    blank();
    CHECK(!start(&s, &w));
    CHECK_EQ(w, 77);
    CHECK(!s.have);
    CHECK_EQ(F.programs, 0);
}

static void test_round_trip(void)
{
    struct statestore s;
    uint32_t w = 0;
    blank();
    start(&s, NULL);
    CHECK(statestore_save(&s, 0x1234));
    CHECK(statestore_save(&s, 0x5678));
    CHECK_EQ(F.erases, 0); /* an erased sector is appended to, never erased first */
    CHECK_EQ(F.programs, 2);
    CHECK(start(&s, &w));
    CHECK_EQ(w, 0x5678);
    CHECK_EQ(s.seq, 2);
    CHECK_EQ(s.next, 2); /* after the last record */
    CHECK(statestore_save(&s, 0x9));
    CHECK(start(&s, &w));
    CHECK_EQ(w, 0x9);
    CHECK_EQ(F.bad_addr, 0);
}

/* Many saves: the sectors take turns, one erase per sector's worth, the newest always loads. */
static void test_wrap(void)
{
    struct statestore s;
    uint32_t w = 0;
    blank();
    start(&s, NULL);
    unsigned n = 3 * STATESTORE_PER_SECTOR + 5;
    for (unsigned i = 1; i <= n; i++) {
        CHECK(statestore_save(&s, i * 7));
        if (i % 97 == 0 || i == n) {
            struct statestore t;
            CHECK(start(&t, &w));
            CHECK_EQ(w, i * 7);
        }
    }
    CHECK_EQ(F.erases, 3); /* sector 1, sector 0, sector 1 */
    CHECK_EQ(s.sector, 1);
    CHECK_EQ(F.bad_addr, 0);
}

/* A power cut before every flash operation of a save, in the middle of a sector and at the switch
 * to the other sector: the next start loads the old state or the new one, never anything else, and
 * a save after it goes through. */
static void cuts_around(unsigned before)
{
    struct statestore s;
    uint32_t w;
    for (unsigned cut = 0; cut <= 2; cut++) {
        blank();
        start(&s, NULL);
        for (unsigned i = 1; i <= before; i++)
            CHECK(statestore_save(&s, i));
        F.ops_left = cut;
        bool ok = statestore_save(&s, 0xABCD);
        F.dead = false;
        F.ops_left = -1;
        CHECK(start(&s, &w));
        if (ok)
            CHECK_EQ(w, 0xABCD);
        else
            CHECK(w == before);
        CHECK(statestore_save(&s, 0xBEEF));
        CHECK(start(&s, &w));
        CHECK_EQ(w, 0xBEEF);
        CHECK_EQ(F.bad_addr, 0);
    }
}

static void test_power_cuts(void)
{
    cuts_around(1);                         /* a program in the middle of a sector */
    cuts_around(STATESTORE_PER_SECTOR);     /* the erase of the other sector, then the program */
    cuts_around(2 * STATESTORE_PER_SECTOR); /* the same, back to sector 0 */
}

/* A program that fails (no power cut): the state before stays, the next save goes past the
 * spoilt record and works. */
static void test_failed_program(void)
{
    struct statestore s;
    uint32_t w;
    blank();
    start(&s, NULL);
    CHECK(statestore_save(&s, 1));
    F.fail_program = true;
    CHECK(!statestore_save(&s, 2));
    CHECK_EQ(s.stats.failures, 1);
    CHECK(start(&s, &w));
    CHECK_EQ(w, 1);
    CHECK(statestore_save(&s, 3));
    CHECK(start(&s, &w));
    CHECK_EQ(w, 3);
}

/* Noise where records should be (neither valid nor erased): passed over, appended after. */
static void test_noise(void)
{
    struct statestore s;
    uint32_t w;
    blank();
    start(&s, NULL);
    CHECK(statestore_save(&s, 5));
    memset(F.mem + 3 * STATESTORE_REC, 0x5A, STATESTORE_REC); /* a record past the end, spoilt */
    CHECK(start(&s, &w));
    CHECK_EQ(w, 5);
    CHECK_EQ(s.next, 4);
    CHECK(statestore_save(&s, 6));
    CHECK(start(&s, &w));
    CHECK_EQ(w, 6);
    /* both sectors full of noise: nothing loads, a save erases one and goes on */
    blank();
    for (unsigned i = 0; i < AREA; i++)
        F.mem[i] = (uint8_t)(i * 31 + 7);
    CHECK(!start(&s, &w));
    CHECK(statestore_save(&s, 8));
    CHECK_EQ(F.erases, 1);
    CHECK(start(&s, &w));
    CHECK_EQ(w, 8);
}

int main(void)
{
    smclog_init();
    RUN(test_empty);
    RUN(test_round_trip);
    RUN(test_wrap);
    RUN(test_power_cuts);
    RUN(test_failed_program);
    RUN(test_noise);
    printf("test_statestore: %s\n", check_failed ? "FAILED" : "all ok");
    return check_failed ? 1 : 0;
}
