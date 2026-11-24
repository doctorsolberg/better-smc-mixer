/*
 * Sector 0's boot pointer (bootsel.h). The formats are tools/image/flashimg.py's:
 *
 * - a JLFS entry is 32 bytes: header CRC u16, data CRC u16, offset u32, size u32, attr u8, res u8,
 *   index u16, name[16] (NUL-terminated); the header CRC is CRC-16/XMODEM of bytes 2..31;
 * - sector 0 holds the flash header, then a list of entries, each ENC-ciphered on its own with the
 *   key 0xFFFF; the list ends at the entry whose index is not 0 (16 at most);
 * - M-Vave's area is SFC-ciphered: each 32-byte block XORed with the ENC keystream of
 *   CHIPKEY ^ bits 2..17 of its CPU address (0x1E00000 + flash offset - 0x1000). Its first entry,
 *   app_area_head, gives the entry point (offset), the area's size and the CRC of everything after
 *   that entry; the directory's other entries follow it.
 */
#include "bootsel.h"

#include <stdbool.h>
#include <string.h>

#define ATTR_FILE 0x82u
#define LIST_MAX 16u /* entries after sector 0's header (flashimg.py, sector0_entries) */
#define DIR_MAX 9u   /* a directory's slots (flashimg.py, SLOTS) */

static uint16_t crc16(const uint8_t *p, uint32_t n, uint16_t crc)
{
    while (n--) {
        crc ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}

/* XORs p[0..n) with the ENC keystream of key, skipping its first skip bytes. */
static void enc(uint8_t *p, uint32_t n, uint16_t key, uint32_t skip)
{
    for (uint32_t i = 0; i < skip + n; i++) {
        if (i >= skip)
            p[i - skip] ^= (uint8_t)key;
        key = (key & 0x8000u) ? (uint16_t)((key << 1) ^ 0x1021u) : (uint16_t)(key << 1);
    }
}

/* The SFC (de)cipher of p[0..n), which lies at flash offset off of M-Vave's area. */
static void sfc(uint8_t *p, uint32_t n, uint32_t off)
{
    uint32_t pos = 0;
    while (pos < n) {
        uint32_t a = off + pos, start = a % BOOTSEL_ENTRY;
        uint32_t len = BOOTSEL_ENTRY - start < n - pos ? BOOTSEL_ENTRY - start : n - pos;
        uint32_t cpu = BOOTSEL_CPU_BASE + (a - start) - BOOTSEL_STOCK_AT;
        enc(p + pos, len, (uint16_t)(BOOTSEL_CHIPKEY ^ ((cpu >> 2) & 0xFFFFu)), start);
        pos += len;
    }
}

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

struct entry {
    uint16_t data_crc;
    uint32_t offset, size;
    uint8_t attr;
    uint16_t index;
    char name[17];
};

/* A plain entry; false when its header CRC does not match. */
static bool entry_parse(const uint8_t b[BOOTSEL_ENTRY], struct entry *e)
{
    if (crc16(b + 2, BOOTSEL_ENTRY - 2, 0) != u16(b))
        return false;
    e->data_crc = u16(b + 2);
    e->offset = u32(b + 4);
    e->size = u32(b + 8);
    e->attr = b[12];
    e->index = u16(b + 14);
    memcpy(e->name, b + 16, 16);
    e->name[16] = 0;
    char *nul = memchr(e->name, 0, 17);
    if (nul)
        *nul = 0;
    return true;
}

/* Sector 0's entry i (0: the flash header), deciphered, into b. */
static bool s0_entry(const uint8_t *sector, unsigned i, uint8_t b[BOOTSEL_ENTRY], struct entry *e)
{
    memcpy(b, sector + i * BOOTSEL_ENTRY, BOOTSEL_ENTRY);
    enc(b, BOOTSEL_ENTRY, 0xFFFFu, 0);
    return entry_parse(b, e);
}

/* app_dir_head's index in sector 0's list, 0 when the sector does not parse or has none. */
static unsigned find_dir_head(const uint8_t *sector, uint32_t *pointer)
{
    uint8_t b[BOOTSEL_ENTRY];
    struct entry e;
    if (!s0_entry(sector, 0, b, &e))
        return 0;
    unsigned found = 0;
    for (unsigned i = 1; i <= LIST_MAX; i++) {
        if (!s0_entry(sector, i, b, &e))
            return 0;
        if (!found && !strcmp(e.name, "app_dir_head")) {
            found = i;
            *pointer = e.offset;
        }
        if (e.index)
            return found;
    }
    return 0; /* no last entry */
}

bool bootsel_point(uint8_t sector[BOOTSEL_SECTOR], uint32_t dir, struct bootsel_info *out)
{
    uint32_t pointer = 0;
    unsigned i = find_dir_head(sector, &pointer);
    if (!i)
        return false;
    uint8_t b[BOOTSEL_ENTRY];
    struct entry e;
    s0_entry(sector, i, b, &e);
    for (unsigned k = 0; k < 4; k++)
        b[4 + k] = (uint8_t)(dir >> (8 * k));
    uint16_t c = crc16(b + 2, BOOTSEL_ENTRY - 2, 0);
    b[0] = (uint8_t)c;
    b[1] = (uint8_t)(c >> 8);
    enc(b, BOOTSEL_ENTRY, 0xFFFFu, 0);
    memcpy(sector + i * BOOTSEL_ENTRY, b, BOOTSEL_ENTRY);
    if (out) {
        out->entry = (uint8_t)i;
        out->pointer = dir;
    }
    return true;
}

/* M-Vave's area as the uboot would start it: app_area_head, the CRC of the area after it, the
 * directory's entries, the entry point inside one of its files. buf: 4 KB of scratch. */
static int check_stock(const struct cfgstore_flash *fl, uint8_t *buf, struct bootsel_info *out)
{
    const uint32_t at = BOOTSEL_STOCK_AT;
    struct entry head, e;
    if (!fl->read(fl->ctx, at, buf, BOOTSEL_SECTOR))
        return BOOTSEL_FLASH;
    sfc(buf, BOOTSEL_SECTOR, at);
    if (!entry_parse(buf, &head) || strcmp(head.name, "app_area_head") || head.size < 2 * BOOTSEL_ENTRY
        || head.size > BOOTSEL_STOCK_END - at || (head.offset & ~0xFFFFu) != BOOTSEL_CPU_BASE)
        return BOOTSEL_NO_STOCK;
    /* the directory: the entries after the head, up to the one with an index (in the first
     * sector); the entry point must land in one of its files */
    uint32_t rel = head.offset - BOOTSEL_CPU_BASE;
    bool in_file = false;
    for (unsigned n = 1;; n++) {
        if (n == DIR_MAX || (n + 1) * BOOTSEL_ENTRY > head.size || !entry_parse(buf + n * BOOTSEL_ENTRY, &e))
            return BOOTSEL_NO_STOCK;
        if (e.attr == ATTR_FILE && e.offset <= rel && rel - e.offset < e.size && e.offset + e.size <= head.size)
            in_file = true;
        if (e.index)
            break;
    }
    if (!in_file)
        return BOOTSEL_NO_STOCK;
    /* the CRC of everything after the head, a sector at a time */
    uint16_t crc = 0;
    for (uint32_t off = 0; off < head.size; off += BOOTSEL_SECTOR) {
        uint32_t len = head.size - off < BOOTSEL_SECTOR ? head.size - off : BOOTSEL_SECTOR;
        if (off) {
            if (!fl->read(fl->ctx, at + off, buf, len))
                return BOOTSEL_FLASH;
            sfc(buf, len, at + off);
        }
        uint32_t skip = off ? 0 : BOOTSEL_ENTRY;
        crc = crc16(buf + skip, len - skip, crc);
    }
    if (crc != head.data_crc)
        return BOOTSEL_NO_STOCK;
    out->stock_size = head.size;
    out->stock_entry = head.offset;
    return BOOTSEL_OK;
}

int bootsel_check(const struct cfgstore_flash *fl, uint8_t sector[BOOTSEL_SECTOR], struct bootsel_info *out)
{
    memset(out, 0, sizeof *out);
    int st = check_stock(fl, sector, out);
    if (st != BOOTSEL_OK)
        return st;
    if (!fl->read(fl->ctx, 0, sector, BOOTSEL_SECTOR))
        return BOOTSEL_FLASH;
    unsigned i = find_dir_head(sector, &out->pointer);
    if (!i)
        return BOOTSEL_NO_SECTOR0;
    out->entry = (uint8_t)i;
    return out->pointer == BOOTSEL_OURS_AT ? BOOTSEL_OK : BOOTSEL_NOT_OURS;
}

/* Whether sector 0 reads back as sector, in small pieces (no second 4 KB buffer). */
static bool reads_back(const struct cfgstore_flash *fl, const uint8_t *sector)
{
    uint8_t b[64];
    for (uint32_t off = 0; off < BOOTSEL_SECTOR; off += sizeof b)
        if (!fl->read(fl->ctx, off, b, sizeof b) || memcmp(b, sector + off, sizeof b))
            return false;
    return true;
}

int bootsel_to_stock(const struct cfgstore_flash *fl, uint8_t sector[BOOTSEL_SECTOR], struct bootsel_info *out)
{
    int st = bootsel_check(fl, sector, out);
    if (st != BOOTSEL_OK)
        return st;
    struct bootsel_info now = *out;
    uint32_t pointer = 0;
    if (!bootsel_point(sector, BOOTSEL_STOCK_AT, &now) || find_dir_head(sector, &pointer) != now.entry
        || pointer != BOOTSEL_STOCK_AT)
        return BOOTSEL_NO_SECTOR0;
    for (unsigned t = 1; t <= BOOTSEL_TRIES; t++) {
        out->tries = (uint8_t)t;
        if (!fl->erase(fl->ctx, 0))
            continue;
        for (uint32_t off = 0; off < BOOTSEL_SECTOR; off += BOOTSEL_PAGE) {
            bool blank = true;
            for (uint32_t k = 0; k < BOOTSEL_PAGE && blank; k++)
                blank = sector[off + k] == 0xFF;
            if (!blank)
                fl->program(fl->ctx, off, sector + off, BOOTSEL_PAGE);
        }
        if (reads_back(fl, sector)) {
            out->pointer = BOOTSEL_STOCK_AT;
            return BOOTSEL_OK;
        }
    }
    return BOOTSEL_FLASH;
}

const char *bootsel_name(int status)
{
    static const char *const names[] = {"ok", "sector 0 does not parse", "sector 0 does not point at ours",
                                        "M-Vave's area does not check", "flash", "busy", "not offered"};
    return status >= 0 && status <= BOOTSEL_DISABLED ? names[status] : "?";
}
