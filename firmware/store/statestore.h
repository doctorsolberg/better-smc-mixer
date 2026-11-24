/*
 * The panel's state in flash (docs/config.md, "The panel's state"; the owner, 2026-11-12: Bluetooth
 * on or off, the selected preset and page, as they were left, after power-on; SPEC Q-25).
 *
 * The state is one 32-bit word (its packing is the caller's: ctl.h, ctl_state()). It goes into two
 * sectors of 4 KB right after the configuration's slots, as 16-byte records appended one after
 * another: magic "SS", version, 0, a sequence number u32, the word u32, and the CRC-32 of the 12
 * bytes before it. A save programs the next erased record of the sector holding the newest one,
 * reads it back, and is done: no erase. When that sector is full, the save erases the other one
 * and starts it; the full sector stays as it was until the new record is down, so a power cut at
 * any moment leaves the newest complete record readable (test_statestore.c cuts before every
 * operation). One erase per 256 saves, the two sectors in turn.
 *
 * A start takes the valid record with the highest sequence number in either sector. A record
 * that is neither valid nor erased (a program cut short) is passed over; the next save goes after
 * the last record that is not erased, so it never programs over one.
 *
 * Portable C11, no allocation; the flash operations are cfgstore.h's (struct cfgstore_flash).
 */
#ifndef STATESTORE_H
#define STATESTORE_H

#include <stdbool.h>
#include <stdint.h>

#include "cfgstore.h"

/* Right after the configuration's slots (docs/image.md, "The layout") */
#define STATESTORE_FLASH_AT (CFGSTORE_FLASH_AT + CFGSTORE_SLOTS * CFGSTORE_SLOT_SIZE) /* 0x35000 */
#define STATESTORE_SECTORS 2u
#define STATESTORE_REC 16u
#define STATESTORE_PER_SECTOR (CFGSTORE_SECTOR / STATESTORE_REC) /* 256 */
#define STATESTORE_END (STATESTORE_FLASH_AT + STATESTORE_SECTORS * CFGSTORE_SECTOR)
#define STATESTORE_VERSION 1u

struct statestore_stats {
    uint32_t saves;    /* records written and read back */
    uint32_t failures; /* saves that failed (the record before stays) */
    uint32_t erases;   /* sectors erased */
};

struct statestore {
    const struct cfgstore_flash *fl;
    bool have;      /* a valid record was loaded or saved */
    uint32_t word;  /* the newest record's word, when have */
    uint32_t seq;   /* its sequence number; the next save writes seq + 1 */
    uint8_t sector; /* the sector a save appends to */
    uint16_t next;  /* the next record there to program; STATESTORE_PER_SECTOR: full */
    struct statestore_stats stats;
};

void statestore_init(struct statestore *s, const struct cfgstore_flash *fl);

/* At start: reads both sectors; true and *word if a valid record is there. */
bool statestore_load(struct statestore *s, uint32_t *word);

/* Saves word (blocking: one program, and one sector erase every STATESTORE_PER_SECTOR saves).
 * True when it is written and read back; false: the newest record stays what it was (a failed
 * program also moves the next save past the record it spoilt). */
bool statestore_save(struct statestore *s, uint32_t word);

#endif /* STATESTORE_H */
