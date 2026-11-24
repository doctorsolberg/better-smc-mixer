/*
 * The configuration's flash slots on BR25 (flash_br25.h), and sector 0 for going back to M-Vave's
 * firmware (flash_br25_boot_ops, at the end).
 *
 * The SDK's flash driver in cpu.a, as its VM (syscfg) calls it in our build's listing
 * (build/app/app.lst, _vm_area_erase, vm_data_copy):
 *
 * - sfc_erase(cmd, addr): cmd 2 erases the 4 KB sector at addr. ✅ from the listing: the VM erases
 *   its area with cmd 2 and steps the address by 4096 (cmd 3: 256-byte pages, by 256). The SDK's
 *   own headers disagree in their comments (a third-party port calls cmd 2 "64k" in its enum and
 *   "4k" where it uses it); the VM's code decides. It returns true for cmd 1..3 whatever the flash
 *   did: the erase is checked by reading back what is programmed after it.
 * - sfc_write(buf, addr, len) → norflash_write(): refuses (nothing written) past the flash's end or
 *   where sdfile_check_write_protect() says no, splits at 256-byte pages, and returns len whatever
 *   happened. So every program is read back (cfgstore.c does).
 * - sfc_write programs the bytes as given, anywhere below the flash's end: no cipher on this side
 *   (✅ the listing: norflash_write, and sdfile_check_write_protect() returns 0 in our build).
 * - sfc_read(buf, addr, len): ✅ from the listing: at and above a boundary B it copies from the
 *   cache's mapping (flash_addr2cpu_addr: addr - B + the window's base); below B it reads the flash
 *   raw (norflash_origin_read) and then XORs the buffer with the ENC keystream of 0xFFFF started at
 *   the buffer's first byte (doe(0xFFFF, buf, len, 0): the transform the stock read shows on sector
 *   0, docs/image.md). B is our area's start, 0x41000 (⚠️ inferred: the entry 0x1E00120 is flash
 *   0x41120, and the window starts at 0x1E00000). B lives in a variable of cpu.a's; the helpers
 *   that use it (flash_addr2cpu_addr, cpu_addr2flash_addr) are local to cpu.a, so the firmware
 *   cannot log it (2026-11-12: neither links); the read back after every program is the check.
 *   The slots lie below B (0x25000..0x34FFF since 2026-11-12, docs/ble.md, "Flash"), so f_read
 *   XORs the same keystream again: the transform is its own inverse, and what comes back is what
 *   was programmed. Reading through sfc_read keeps the SDK's flash mutex.
 *   Until 2026-11-12 the slots were at 0x5E000..0x6DFFF, above B: ✅ two saves read back there and
 *   a raw read through the key agreed (docs/config.md, "The first run"). ✅ At the new place too
 *   (2026-11-12, controller 21; docs/config.md, "At the new place"). Were the rule above wrong,
 *   every save would fail its read back (store status FAILED) and a start would find no valid
 *   header and run the factory configuration; nothing would be lost.
 * - The driver takes its own mutex and runs the flash commands from RAM (the SDK's code; the VM's
 *   writes run the same way). An erase holds the scan off up to 10 ms (✅ 2026-11-12, docs/config.md,
 *   "The first run"); cfglink saves only while the panel is quiet.
 *
 * Addresses outside the slots and the panel's state (0x25000..0x36FFF) are refused here, before the driver, so no bug above can reach the
 * code, the loader or the VM.
 */
#include "flash_br25.h"
#include "statestore.h"

enum { SFC_SECTOR_ERASE = 2 }; /* 4 KB (above) */

/* cpu.a's, declared here with <stdint.h> types: the SDK's headers typedef bool as u8, which
 * <stdbool.h> (cfgstore.h) does not allow in the same file. Its bool is that u8. */
extern uint8_t sfc_erase(int cmd, uint32_t addr);
extern uint32_t sfc_write(const uint8_t *buf, uint32_t addr, uint32_t len);
extern uint32_t sfc_read(uint8_t *buf, uint32_t addr, uint32_t len);

/* the slots, then the panel's state right after them (store/statestore.h) */
#define AREA_END STATESTORE_END
_Static_assert(STATESTORE_FLASH_AT == CFGSTORE_FLASH_AT + CFGSTORE_SLOTS * CFGSTORE_SLOT_SIZE,
               "the state's sectors follow the slots");

static bool inside(uint32_t addr, uint32_t len)
{
    return addr >= CFGSTORE_FLASH_AT && addr < AREA_END && len <= AREA_END - addr;
}

/* Where sfc_read() stops reading raw through the keystream (above): our area's start. */
#define SFC_RAW_BELOW 0x41000u
_Static_assert(AREA_END <= SFC_RAW_BELOW, "the slots are read as lying below the area");

/* XORs buf with the ENC keystream of 0xFFFF from its first byte: the low byte of a 16-bit LFSR,
 * polynomial 0x1021 (tools/image/flashimg.py, keystream(); it starts ff df 9f). */
static void enc_ffff(uint8_t *buf, uint32_t len)
{
    uint16_t k = 0xFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        buf[i] ^= (uint8_t)k;
        k = (k & 0x8000u) ? (uint16_t)((k << 1) ^ 0x1021u) : (uint16_t)(k << 1);
    }
}

static bool f_read(void *ctx, uint32_t addr, uint8_t *buf, uint32_t len)
{
    (void)ctx;
    if (!inside(addr, len))
        return false;
    sfc_read(buf, addr, len);
    enc_ffff(buf, len); /* undoes sfc_read's doe(0xFFFF) below SFC_RAW_BELOW */
    return true;
}

static bool f_erase(void *ctx, uint32_t addr)
{
    (void)ctx;
    if (addr % CFGSTORE_SECTOR || !inside(addr, CFGSTORE_SECTOR))
        return false;
    return sfc_erase(SFC_SECTOR_ERASE, addr) != 0;
}

static bool f_program(void *ctx, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    (void)ctx;
    if (!len || !inside(addr, len) || addr / CFGSTORE_PAGE != (addr + len - 1) / CFGSTORE_PAGE)
        return false;
    sfc_write(buf, addr, len);
    return true;
}

static const struct cfgstore_flash ops = {0, f_read, f_erase, f_program};

const struct cfgstore_flash *flash_br25_ops(void)
{
    return &ops;
}

/* Sector 0's boot pointer (store/bootsel.h; docs/image.md, "Going back"). Reads below our area
 * take the same raw path as the slots' (above), so sector 0 and M-Vave's area come back as stored.
 * Erase and program reach sector 0 alone; sfc_write stores the bytes as given (no cipher), as the
 * stock firmware's write did when sector 0 was rewritten on 2026-11-10 (docs/bringup.md, "Step 2b").
 * ✅ On the mixer (controller 36, 2026-11-14, STOCK back): sector 0 erased, written at the first
 * try and read back; the uboot then started M-Vave's firmware (docs/image.md, "Going back"). */
static bool b_read(void *ctx, uint32_t addr, uint8_t *buf, uint32_t len)
{
    (void)ctx;
    if (addr >= SFC_RAW_BELOW || len > SFC_RAW_BELOW - addr)
        return false;
    sfc_read(buf, addr, len);
    enc_ffff(buf, len);
    return true;
}

static bool b_erase(void *ctx, uint32_t addr)
{
    (void)ctx;
    if (addr != 0)
        return false;
    return sfc_erase(SFC_SECTOR_ERASE, 0) != 0;
}

static bool b_program(void *ctx, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    (void)ctx;
    if (!len || addr + len > CFGSTORE_SECTOR || addr / CFGSTORE_PAGE != (addr + len - 1) / CFGSTORE_PAGE)
        return false;
    sfc_write(buf, addr, len);
    return true;
}

static const struct cfgstore_flash boot_ops = {0, b_read, b_erase, b_program};

const struct cfgstore_flash *flash_br25_boot_ops(void)
{
    return &boot_ops;
}
