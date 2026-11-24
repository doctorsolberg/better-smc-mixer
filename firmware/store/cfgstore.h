/*
 * The configuration in flash (docs/config.md, "Storage"; SPEC FR-CFG-04, NFR-05, Q-22 decided
 * 2026-11-11: two slots of our own, saved automatically after a quiet pause).
 *
 * Two slots, A and B, each 32 KB (the largest binary form and a header). A slot holds a header and
 * the configuration's binary form (opaque here). A save writes the slot that does not hold the
 * newest configuration:
 *
 *   1. erase the sectors it needs (4 KB each);
 *   2. program the binary form after the header, a page at a time, each page read back;
 *   3. program the header last: it carries a generation one above the newest, the form's length
 *      and CRC-32, and its own CRC-32, so it is the commit mark;
 *   4. read the header back.
 *
 * A start takes the slot with the highest generation whose header and form check (and that the
 * caller accepts: cfgbin_decode()), else the other one, else none (the factory configuration).
 * A power loss at any moment leaves the newest complete configuration readable: until step 3 ends
 * the slot being written has no valid header, and the other slot is not touched.
 *
 * Saving goes step by step (cfgstore_step(): one sector erase or a few pages), so the caller can
 * stop between steps while the panel is in use and go on later.
 *
 * Portable C11, no allocation; the platform gives read, erase and program (struct cfgstore_flash).
 * On BR25: platform/br25/flash_br25.c; on the host: a model of NOR flash (tests, devhost).
 */
#ifndef CFGSTORE_H
#define CFGSTORE_H

#include <stdbool.h>
#include <stdint.h>

/* The layout (docs/image.md, "The layout"; tools/image/installplan.py plans around it): below our
 * application area, in the free part of the stock VM's range (docs/ble.md, "Flash"; SPEC Q-23). */
#define CFGSTORE_FLASH_AT 0x25000u /* slot A; slot B right after it */
#define CFGSTORE_SLOT_SIZE 0x8000u /* 32 KB */
#define CFGSTORE_SLOTS 2u
#define CFGSTORE_SECTOR 0x1000u /* erase unit */
#define CFGSTORE_PAGE 256u      /* program unit */
#define CFGSTORE_HEADER 32u
#define CFGSTORE_MAX (CFGSTORE_SLOT_SIZE - CFGSTORE_HEADER) /* the largest form a slot holds */
#define CFGSTORE_VERSION 1u
#define CFGSTORE_PAGES_PER_STEP 4u /* pages programmed (and read back) in one cfgstore_step() */

/* The platform's flash: physical addresses, true on success. */
struct cfgstore_flash {
    void *ctx;
    bool (*read)(void *ctx, uint32_t addr, uint8_t *buf, uint32_t len);
    bool (*erase)(void *ctx, uint32_t addr); /* the sector at addr (CFGSTORE_SECTOR-aligned) */
    bool (*program)(void *ctx, uint32_t addr, const uint8_t *buf, uint32_t len); /* within a page */
};

enum cfgstore_state {
    CFGSTORE_IDLE,
    CFGSTORE_ERASE,   /* erasing the target slot's sectors */
    CFGSTORE_PROGRAM, /* programming the form */
    CFGSTORE_COMMIT,  /* programming the header */
};

enum cfgstore_result {
    CFGSTORE_MORE,   /* cfgstore_step(): call again */
    CFGSTORE_DONE,   /* saved and read back */
    CFGSTORE_FAILED, /* a flash operation or a read back failed: the other slot is as it was */
};

struct cfgstore_slot {
    bool valid;          /* its header and form checked at the last look */
    uint32_t generation; /* when valid */
    uint32_t length;
};

struct cfgstore_stats {
    uint32_t saves;    /* completed */
    uint32_t failures; /* failed saves */
    uint32_t erases;   /* sectors erased */
    uint32_t pages;    /* pages programmed */
};

struct cfgstore {
    const struct cfgstore_flash *fl;
    struct cfgstore_slot slot[CFGSTORE_SLOTS];
    int8_t newest; /* the slot of the newest valid configuration (the one loaded), -1: none */
    /* the save under way */
    uint8_t state;  /* enum cfgstore_state */
    uint8_t target; /* the slot written */
    const uint8_t *blob;
    uint32_t len, crc;
    uint32_t generation; /* the highest in a header that checked, or written; a save writes one above */
    uint32_t pos; /* ERASE: the next sector's offset in the slot; PROGRAM: the next byte of the form */
    struct cfgstore_stats stats;
};

void cfgstore_init(struct cfgstore *s, const struct cfgstore_flash *fl);

/* At start: the newest configuration whose header and form check and that accept() takes (NULL:
 * any), into buf (cap bytes). Returns its length, 0 if there is none; *generation (may be NULL)
 * gets its generation. Also learns both slots' state for the saves to come. */
uint32_t cfgstore_load(struct cfgstore *s, uint8_t *buf, uint32_t cap, uint32_t *generation,
                       bool (*accept)(void *ctx, const uint8_t *blob, uint32_t len), void *ctx);

/* Starts a save of blob[0..len) (at most CFGSTORE_MAX; it must stay unchanged until the save ends)
 * into the slot not holding the newest configuration. False if len is out of range. A save under
 * way is dropped (its slot has no valid header yet). */
bool cfgstore_begin(struct cfgstore *s, const uint8_t *blob, uint32_t len);

/* One step of the save: one sector erased, or up to CFGSTORE_PAGES_PER_STEP pages programmed and
 * read back, or the header. CFGSTORE_DONE when it is saved; afterwards that slot is the newest. */
int cfgstore_step(struct cfgstore *s);

/* Drops the save under way: its slot has no valid header, the newest one stays. */
static inline void cfgstore_cancel(struct cfgstore *s) { s->state = CFGSTORE_IDLE; }

/* Whether a save is under way. */
static inline bool cfgstore_busy(const struct cfgstore *s) { return s->state != CFGSTORE_IDLE; }

/* The generation of the newest saved configuration, 0 if none. */
uint32_t cfgstore_generation(const struct cfgstore *s);

#endif /* CFGSTORE_H */
