/*
 * Sector 0's boot pointer: which application the uboot starts (docs/image.md, "Sector 0" and
 * "Going back"; SPEC Q-18, the owner's direction of 2026-11-12 and decision of 2026-11-14).
 *
 * The installation leaves M-Vave's application area at 0x1000 untouched and changes one 32-byte
 * entry of sector 0, app_dir_head, from 0x1000 to our area at 0x41000. Going back to M-Vave's
 * firmware is that entry pointed at 0x1000 again: no backup file, no M-Vave update file, no second
 * image. It is also the first half of an update: M-Vave's firmware then starts, and the editor
 * installs the new version through its service SysEx as it installed the first one.
 *
 * bootsel_check() reads sector 0 and M-Vave's area and says whether going back can work; nothing
 * is written. bootsel_to_stock() checks the same, then writes sector 0 with that entry changed and
 * reads it back. Between the erase and the end of the write (about 100 ms) a power loss leaves a
 * mixer that does not start: the recovery key's way (docs/recovery.md) is then the way back.
 *
 * Portable C11, no allocation; the platform gives read, erase and program (struct
 * cfgstore_flash), physical addresses and bytes as stored. On BR25: platform/br25/flash_br25.c
 * (flash_br25_boot_ops); on the host: the tests' model of NOR flash and devhost.
 */
#ifndef BOOTSEL_H
#define BOOTSEL_H

#include <stdint.h>

#include "cfgstore.h"

#define BOOTSEL_SECTOR 0x1000u   /* sector 0: the flash header and its file list */
#define BOOTSEL_PAGE 256u        /* program unit */
#define BOOTSEL_STOCK_AT 0x1000u /* M-Vave's application area (its directory) */
#define BOOTSEL_OURS_AT 0x41000u /* ours (tools/image/installplan.py, SLOT) */
#define BOOTSEL_STOCK_END 0x41000u /* the stock area must end below ours */
#define BOOTSEL_ENTRY 32u        /* a JLFS entry */
#define BOOTSEL_CHIPKEY 0x980Fu  /* the SFC chip key (docs/image.md, "SFC cipher"; ✅ the ROM loader, 2026-11-10) */
#define BOOTSEL_CPU_BASE 0x1E00000u /* where the uboot maps the area it starts */
#define BOOTSEL_TRIES 3u         /* writes of sector 0 before it gives up */

enum bootsel_status {
    BOOTSEL_OK = 0,         /* check: going back can work; to_stock: written and read back */
    BOOTSEL_NO_SECTOR0 = 1, /* sector 0 does not parse: its header, or no app_dir_head */
    BOOTSEL_NOT_OURS = 2,   /* sector 0 does not point at our area: nothing to do */
    BOOTSEL_NO_STOCK = 3,   /* M-Vave's area at 0x1000 does not check (a CRC, its entry point) */
    BOOTSEL_FLASH = 4,      /* a read failed (nothing written), or sector 0 never read back as written */
    BOOTSEL_BUSY = 5,       /* the platform's: the flash is in use (a configuration being saved) */
    BOOTSEL_DISABLED = 6,   /* the platform's: this firmware does not offer it */
};

struct bootsel_info {
    uint32_t pointer;     /* where sector 0 points now */
    uint8_t entry;        /* app_dir_head's index in sector 0's list */
    uint32_t stock_size;  /* M-Vave's area as its directory gives it (app_area_head.size) */
    uint32_t stock_entry; /* its entry point (a CPU address) */
    uint8_t tries;        /* to_stock: writes of sector 0 it took */
};

/* The checks only. sector: 4 KB of scratch, which gets sector 0 as stored. */
int bootsel_check(const struct cfgstore_flash *fl, uint8_t sector[BOOTSEL_SECTOR], struct bootsel_info *out);

/* The checks, then sector 0 with app_dir_head at BOOTSEL_STOCK_AT: erased, written page by page,
 * read back; again up to BOOTSEL_TRIES times. The chip must then be reset. */
int bootsel_to_stock(const struct cfgstore_flash *fl, uint8_t sector[BOOTSEL_SECTOR], struct bootsel_info *out);

/* The pieces, for the tests: sector 0 (as stored) with app_dir_head at dir; false when it does not
 * parse. In place. */
_Bool bootsel_point(uint8_t sector[BOOTSEL_SECTOR], uint32_t dir, struct bootsel_info *out);

/* A status's name for the log. */
const char *bootsel_name(int status);

#endif /* BOOTSEL_H */
