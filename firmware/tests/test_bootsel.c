/*
 * Tests of store/bootsel.c, sector 0's boot pointer back at M-Vave's area, on flashes from
 * tools/image's model (tests/bootsel_vec.py: a synthetic stock flash, and the same after the
 * installation plan). Going back must give M-Vave's sector 0 byte for byte, the installation's
 * rollback R1 (tools/image/installplan.py).
 *
 *   test_bootsel [VECTORS]   (default: the file the build made, BOOTSEL_VEC)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bootsel.h"
#include "check.h"

#define FLASH 0x80000u

int check_failed;

static uint8_t stock[FLASH], installed[FLASH];

/* NOR flash: an erase sets 0xFF, programming only clears bits; faults on demand. */
struct nor {
    uint8_t mem[FLASH];
    int erases, programs, reads;
    int protect;       /* write protection: erases and programs that do nothing (the driver says yes) */
    int fail_program;  /* programs that do nothing */
    int fail_read_at;  /* the read with this number fails (1-based), 0: none */
};

static bool n_read(void *ctx, uint32_t addr, uint8_t *buf, uint32_t len)
{
    struct nor *n = ctx;
    if (++n->reads == n->fail_read_at || addr + len > FLASH)
        return false;
    memcpy(buf, n->mem + addr, len);
    return true;
}

static bool n_erase(void *ctx, uint32_t addr)
{
    struct nor *n = ctx;
    n->erases++;
    if (addr % BOOTSEL_SECTOR)
        return false;
    if (n->protect)
        return true; /* as the SDK's driver: it says yes whatever the flash did */
    memset(n->mem + addr, 0xFF, BOOTSEL_SECTOR);
    return true;
}

static bool n_program(void *ctx, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    struct nor *n = ctx;
    n->programs++;
    if (n->protect)
        return true;
    if (n->fail_program) {
        n->fail_program--;
        return true;
    }
    for (uint32_t i = 0; i < len; i++)
        n->mem[addr + i] &= buf[i];
    return true;
}

static struct nor nor;
static const struct cfgstore_flash ops = {&nor, n_read, n_erase, n_program};
static uint8_t scratch[BOOTSEL_SECTOR];

static void load(const uint8_t *from)
{
    memset(&nor, 0, sizeof nor);
    memcpy(nor.mem, from, FLASH);
}

static void test_check(void)
{
    struct bootsel_info i;
    load(installed);
    CHECK(bootsel_check(&ops, scratch, &i) == BOOTSEL_OK);
    CHECK(i.pointer == BOOTSEL_OURS_AT);
    CHECK(i.entry == 3); /* uboot.boot, ..., app_dir_head: as flashimg.py's sector 0 lists them */
    CHECK(i.stock_entry == 0x1E00120u);
    CHECK(i.stock_size > 5000);
    CHECK(nor.erases == 0 && nor.programs == 0);
    CHECK(!memcmp(nor.mem, installed, FLASH));
    /* on M-Vave's flash itself: nothing of ours to leave */
    load(stock);
    CHECK(bootsel_check(&ops, scratch, &i) == BOOTSEL_NOT_OURS);
    CHECK(i.pointer == BOOTSEL_STOCK_AT);
}

static void test_to_stock(void)
{
    struct bootsel_info i;
    load(installed);
    CHECK(bootsel_to_stock(&ops, scratch, &i) == BOOTSEL_OK);
    CHECK(i.pointer == BOOTSEL_STOCK_AT && i.tries == 1);
    CHECK(nor.erases == 1);
    /* sector 0 is M-Vave's again, byte for byte (the rollback's R1); nothing else changed */
    CHECK(!memcmp(nor.mem, stock, BOOTSEL_SECTOR));
    CHECK(!memcmp(nor.mem + BOOTSEL_SECTOR, installed + BOOTSEL_SECTOR, FLASH - BOOTSEL_SECTOR));
    /* and the way forward is the installation's sector 0 again */
    memcpy(scratch, nor.mem, BOOTSEL_SECTOR);
    CHECK(bootsel_point(scratch, BOOTSEL_OURS_AT, &i) && i.entry == 3);
    CHECK(!memcmp(scratch, installed, BOOTSEL_SECTOR));
    /* again: nothing to do, nothing written */
    int erases = nor.erases;
    CHECK(bootsel_to_stock(&ops, scratch, &i) == BOOTSEL_NOT_OURS);
    CHECK(nor.erases == erases);
}

static void test_faults(void)
{
    struct bootsel_info i;
    /* M-Vave's area damaged: a byte of its application, a byte of its directory */
    static const uint32_t spots[] = {0x1000 + 0x200, 0x1000 + 0x30, 0x1000 + 0x5};
    for (unsigned k = 0; k < sizeof spots / sizeof spots[0]; k++) {
        load(installed);
        nor.mem[spots[k]] ^= 0x10;
        CHECK(bootsel_to_stock(&ops, scratch, &i) == BOOTSEL_NO_STOCK);
        CHECK(nor.erases == 0 && nor.programs == 0);
    }
    /* sector 0 damaged: its header, an entry */
    static const uint32_t s0[] = {0x3, 0x45};
    for (unsigned k = 0; k < 2; k++) {
        load(installed);
        nor.mem[s0[k]] ^= 0x01;
        CHECK(bootsel_to_stock(&ops, scratch, &i) == BOOTSEL_NO_SECTOR0);
        CHECK(nor.erases == 0);
    }
    /* a read that fails: nothing written */
    for (int r = 1; r <= 3; r++) {
        load(installed);
        nor.fail_read_at = r;
        CHECK(bootsel_to_stock(&ops, scratch, &i) == BOOTSEL_FLASH);
        CHECK(nor.erases == 0);
    }
    /* write-protected: three tries, sector 0 as it was */
    load(installed);
    nor.protect = 1;
    CHECK(bootsel_to_stock(&ops, scratch, &i) == BOOTSEL_FLASH);
    CHECK(i.tries == BOOTSEL_TRIES && nor.erases == (int)BOOTSEL_TRIES);
    CHECK(!memcmp(nor.mem, installed, BOOTSEL_SECTOR));
    /* a page that did not take once: the second try writes it */
    load(installed);
    nor.fail_program = 1;
    CHECK(bootsel_to_stock(&ops, scratch, &i) == BOOTSEL_OK);
    CHECK(i.tries == 2);
    CHECK(!memcmp(nor.mem, stock, BOOTSEL_SECTOR));
    /* every page fails every time: no good sector 0, said so */
    load(installed);
    nor.fail_program = 999;
    CHECK(bootsel_to_stock(&ops, scratch, &i) == BOOTSEL_FLASH);
    /* a sector 0 without app_dir_head */
    memcpy(scratch, stock, BOOTSEL_SECTOR);
    memset(scratch + 32, 0xFF, BOOTSEL_SECTOR - 32);
    CHECK(!bootsel_point(scratch, BOOTSEL_STOCK_AT, NULL));
    CHECK(!strcmp(bootsel_name(BOOTSEL_NO_STOCK), "M-Vave's area does not check"));
    CHECK(!strcmp(bootsel_name(42), "?"));
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argc > 1 ? argv[1] : BOOTSEL_VEC, "rb");
    if (!f || fread(stock, 1, FLASH, f) != FLASH || fread(installed, 1, FLASH, f) != FLASH) {
        fprintf(stderr, "test_bootsel: cannot read the vectors (tests/bootsel_vec.py)\n");
        return 2;
    }
    fclose(f);
    RUN(test_check);
    RUN(test_to_stock);
    RUN(test_faults);
    printf("test_bootsel: %s\n", check_failed ? "FAILED" : "all ok");
    return check_failed ? 1 : 0;
}
