/* The configuration in flash: two slots, the header written last (cfgstore.h). */
#include "cfgstore.h"

#include <string.h>

#include "cfgbin.h" /* cfgbin_crc32() */
#include "smclog.h"

/* The header: magic "SMCS", version u8, 3 reserved (0), generation u32, length u32, the form's
 * CRC-32 u32, 8 reserved (0), the CRC-32 of the 28 bytes before it. */
#define MAGIC "SMCS"
#define H_CRC 28

_Static_assert(CFGSTORE_SLOT_SIZE % CFGSTORE_SECTOR == 0, "slots are whole sectors");
_Static_assert(CFGSTORE_HEADER <= CFGSTORE_PAGE, "the header is in the first page");
_Static_assert(CFGBIN_MAX <= CFGSTORE_MAX, "a slot holds the largest form");

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t base(unsigned slot)
{
    return CFGSTORE_FLASH_AT + slot * CFGSTORE_SLOT_SIZE;
}

static void header(uint8_t h[CFGSTORE_HEADER], uint32_t generation, uint32_t len, uint32_t crc)
{
    memset(h, 0, CFGSTORE_HEADER);
    memcpy(h, MAGIC, 4);
    h[4] = CFGSTORE_VERSION;
    put32(h + 8, generation);
    put32(h + 12, len);
    put32(h + 16, crc);
    put32(h + H_CRC, cfgbin_crc32(0, h, H_CRC));
}

/* A slot's header: true if it checks; its generation, length and the form's CRC. */
static bool read_header(struct cfgstore *s, unsigned slot, uint32_t *gen, uint32_t *len, uint32_t *crc)
{
    uint8_t h[CFGSTORE_HEADER];
    if (!s->fl->read(s->fl->ctx, base(slot), h, sizeof h))
        return false;
    if (memcmp(h, MAGIC, 4) != 0 || h[4] != CFGSTORE_VERSION ||
        get32(h + H_CRC) != cfgbin_crc32(0, h, H_CRC))
        return false;
    *gen = get32(h + 8);
    *len = get32(h + 12);
    *crc = get32(h + 16);
    return *gen != 0 && *len <= CFGSTORE_MAX;
}

/* Whether flash at addr reads as p[0..n). */
static bool same(struct cfgstore *s, uint32_t addr, const uint8_t *p, uint32_t n)
{
    uint8_t b[64];
    while (n) {
        uint32_t k = n < sizeof b ? n : (uint32_t)sizeof b;
        if (!s->fl->read(s->fl->ctx, addr, b, k) || memcmp(b, p, k) != 0)
            return false;
        addr += k;
        p += k;
        n -= k;
    }
    return true;
}

void cfgstore_init(struct cfgstore *s, const struct cfgstore_flash *fl)
{
    memset(s, 0, sizeof *s);
    s->fl = fl;
    s->newest = -1;
}

uint32_t cfgstore_generation(const struct cfgstore *s)
{
    return s->newest >= 0 ? s->slot[s->newest].generation : 0;
}

uint32_t cfgstore_load(struct cfgstore *s, uint8_t *buf, uint32_t cap, uint32_t *generation,
                       bool (*accept)(void *ctx, const uint8_t *blob, uint32_t len), void *ctx)
{
    uint32_t gen[CFGSTORE_SLOTS], len[CFGSTORE_SLOTS], crc[CFGSTORE_SLOTS];
    bool head[CFGSTORE_SLOTS];

    s->newest = -1;
    s->generation = 0;
    for (unsigned i = 0; i < CFGSTORE_SLOTS; i++) {
        s->slot[i].valid = false;
        head[i] = read_header(s, i, &gen[i], &len[i], &crc[i]);
        if (head[i] && gen[i] > s->generation)
            s->generation = gen[i]; /* the next save goes above every header, even a bad form's */
    }
    /* the newer first */
    unsigned order[CFGSTORE_SLOTS] = {0, 1};
    if (head[1] && (!head[0] || gen[1] > gen[0])) {
        order[0] = 1;
        order[1] = 0;
    }
    for (unsigned k = 0; k < CFGSTORE_SLOTS; k++) {
        unsigned i = order[k];
        if (!head[i])
            continue;
        if (len[i] > cap) {
            LOG_WARN(cfg, "flash slot %u: generation %u is %u bytes, more than the %u this build takes",
                     i, gen[i], len[i], cap);
            continue;
        }
        if (!s->fl->read(s->fl->ctx, base(i) + CFGSTORE_HEADER, buf, len[i]) ||
            cfgbin_crc32(0, buf, len[i]) != crc[i]) {
            LOG_WARN(cfg, "flash slot %u: generation %u does not check (CRC)", i, gen[i]);
            continue;
        }
        if (accept && !accept(ctx, buf, len[i])) {
            LOG_WARN(cfg, "flash slot %u: generation %u is not a configuration this firmware takes", i, gen[i]);
            continue;
        }
        s->slot[i].valid = true;
        s->slot[i].generation = gen[i];
        s->slot[i].length = len[i];
        s->newest = (int8_t)i;
        if (generation)
            *generation = gen[i];
        return len[i];
    }
    return 0;
}

bool cfgstore_begin(struct cfgstore *s, const uint8_t *blob, uint32_t len)
{
    if (len == 0 || len > CFGSTORE_MAX)
        return false;
    s->target = (uint8_t)(s->newest == 0 ? 1 : 0);
    s->blob = blob;
    s->len = len;
    s->crc = cfgbin_crc32(0, blob, len);
    s->pos = 0;
    s->state = CFGSTORE_ERASE;
    s->slot[s->target].valid = false; /* it is about to be erased */
    return true;
}

static int fail(struct cfgstore *s, const char *what)
{
    LOG_ERROR(cfg, "flash slot %u: %s failed at %u; the configuration saved before stays", s->target,
              what, s->pos);
    s->stats.failures++;
    s->state = CFGSTORE_IDLE;
    return CFGSTORE_FAILED;
}

int cfgstore_step(struct cfgstore *s)
{
    uint32_t at = base(s->target);

    switch (s->state) {
    case CFGSTORE_ERASE: {
        uint32_t need = (CFGSTORE_HEADER + s->len + CFGSTORE_SECTOR - 1) / CFGSTORE_SECTOR * CFGSTORE_SECTOR;
        if (!s->fl->erase(s->fl->ctx, at + s->pos))
            return fail(s, "erase");
        s->stats.erases++;
        s->pos += CFGSTORE_SECTOR;
        if (s->pos >= need) {
            s->state = CFGSTORE_PROGRAM;
            s->pos = 0;
        }
        return CFGSTORE_MORE;
    }
    case CFGSTORE_PROGRAM:
        for (unsigned k = 0; k < CFGSTORE_PAGES_PER_STEP && s->pos < s->len; k++) {
            uint32_t addr = at + CFGSTORE_HEADER + s->pos;
            uint32_t n = CFGSTORE_PAGE - addr % CFGSTORE_PAGE; /* up to the page's end */
            if (n > s->len - s->pos)
                n = s->len - s->pos;
            if (!s->fl->program(s->fl->ctx, addr, s->blob + s->pos, n))
                return fail(s, "program");
            if (!same(s, addr, s->blob + s->pos, n))
                return fail(s, "read back");
            s->stats.pages++;
            s->pos += n;
        }
        if (s->pos == s->len)
            s->state = CFGSTORE_COMMIT;
        return CFGSTORE_MORE;
    case CFGSTORE_COMMIT: {
        uint8_t h[CFGSTORE_HEADER];
        header(h, s->generation + 1, s->len, s->crc);
        if (!s->fl->program(s->fl->ctx, at, h, sizeof h))
            return fail(s, "program the header");
        if (!same(s, at, h, sizeof h) || !same(s, at + CFGSTORE_HEADER, s->blob, s->len))
            return fail(s, "read back the header");
        s->generation++;
        s->slot[s->target].valid = true;
        s->slot[s->target].generation = s->generation;
        s->slot[s->target].length = s->len;
        s->newest = (int8_t)s->target;
        s->state = CFGSTORE_IDLE;
        s->stats.saves++;
        LOG_INFO(cfg, "configuration saved: flash slot %u, generation %u, %u bytes", s->target,
                 s->generation, s->len);
        return CFGSTORE_DONE;
    }
    default:
        return CFGSTORE_DONE;
    }
}
