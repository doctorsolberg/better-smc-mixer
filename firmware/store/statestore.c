/* The panel's state in flash: records appended in two sectors (statestore.h). */
#include "statestore.h"

#include <string.h>

#include "cfgbin.h" /* cfgbin_crc32() */
#include "smclog.h"

_Static_assert(CFGSTORE_SECTOR % STATESTORE_REC == 0, "records fill a sector");
_Static_assert(CFGSTORE_PAGE % STATESTORE_REC == 0, "a record never crosses a page");
_Static_assert(STATESTORE_PER_SECTOR <= 0xFFFFu, "next fits");

#define R_CRC 12 /* the CRC-32 of the 12 bytes before it */

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t addr_of(unsigned sector, unsigned rec)
{
    return STATESTORE_FLASH_AT + sector * CFGSTORE_SECTOR + rec * STATESTORE_REC;
}

static void record(uint8_t r[STATESTORE_REC], uint32_t seq, uint32_t word)
{
    r[0] = 'S';
    r[1] = 'S';
    r[2] = STATESTORE_VERSION;
    r[3] = 0;
    put32(r + 4, seq);
    put32(r + 8, word);
    put32(r + R_CRC, cfgbin_crc32(0, r, R_CRC));
}

static bool valid(const uint8_t r[STATESTORE_REC])
{
    return r[0] == 'S' && r[1] == 'S' && r[2] == STATESTORE_VERSION && r[3] == 0 &&
           get32(r + R_CRC) == cfgbin_crc32(0, r, R_CRC) && get32(r + 4) != 0;
}

static bool erased(const uint8_t r[STATESTORE_REC])
{
    for (unsigned i = 0; i < STATESTORE_REC; i++)
        if (r[i] != 0xFF)
            return false;
    return true;
}

void statestore_init(struct statestore *s, const struct cfgstore_flash *fl)
{
    memset(s, 0, sizeof *s);
    s->fl = fl;
}

bool statestore_load(struct statestore *s, uint32_t *word)
{
    uint16_t end[STATESTORE_SECTORS]; /* after the last record that is not erased */
    int best = -1;
    uint8_t page[4 * STATESTORE_REC]; /* read a few records at a time: little stack */

    s->have = false;
    s->seq = 0;
    for (unsigned k = 0; k < STATESTORE_SECTORS; k++) {
        end[k] = 0;
        for (unsigned at = 0; at < CFGSTORE_SECTOR; at += sizeof page) {
            if (!s->fl->read(s->fl->ctx, addr_of(k, 0) + at, page, sizeof page)) {
                LOG_WARN(cfg, "state: flash sector %u not readable", k);
                end[k] = STATESTORE_PER_SECTOR; /* never appended to; erased when its turn comes */
                break;
            }
            for (unsigned i = 0; i < sizeof page / STATESTORE_REC; i++) {
                const uint8_t *r = page + i * STATESTORE_REC;
                unsigned n = at / STATESTORE_REC + i;
                if (!erased(r))
                    end[k] = (uint16_t)(n + 1);
                if (valid(r) && get32(r + 4) > s->seq) {
                    s->seq = get32(r + 4);
                    s->word = get32(r + 8);
                    best = (int)k;
                }
            }
        }
    }
    if (best < 0) {
        /* nothing saved: start in the sector with more room (an erase first if both are full) */
        s->sector = end[1] < end[0] ? 1 : 0;
        s->next = end[s->sector];
        LOG_INFO(cfg, "state: none saved");
        return false;
    }
    s->have = true;
    s->sector = (uint8_t)best;
    s->next = end[best];
    if (word)
        *word = s->word;
    LOG_INFO(cfg, "state loaded: %08x (record %u, flash sector %u)", s->word, s->seq, (unsigned)best);
    return true;
}

bool statestore_save(struct statestore *s, uint32_t word)
{
    uint8_t r[STATESTORE_REC], back[STATESTORE_REC];

    if (s->next >= STATESTORE_PER_SECTOR) {
        unsigned other = s->sector ^ 1u;
        if (!s->fl->erase(s->fl->ctx, addr_of(other, 0))) {
            LOG_ERROR(cfg, "state: erasing flash sector %u failed; the state saved before stays", other);
            s->stats.failures++;
            return false;
        }
        s->stats.erases++;
        s->sector = (uint8_t)other;
        s->next = 0;
    }
    uint32_t at = addr_of(s->sector, s->next);
    record(r, s->seq + 1, word);
    s->next++; /* spoilt or not, the next save goes after it */
    if (!s->fl->program(s->fl->ctx, at, r, sizeof r) || !s->fl->read(s->fl->ctx, at, back, sizeof back) ||
        memcmp(back, r, sizeof r) != 0) {
        LOG_ERROR(cfg, "state: writing the record at %06x failed; the state saved before stays", at);
        s->stats.failures++;
        return false;
    }
    s->seq++;
    s->word = word;
    s->have = true;
    s->stats.saves++;
    LOG_INFO(cfg, "state saved: %08x (record %u, flash sector %u)", word, s->seq, s->sector);
    return true;
}
