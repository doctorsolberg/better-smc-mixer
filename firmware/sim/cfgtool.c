/*
 * The device's configuration codec on the host (firmware/config/cfgbin.c; docs/config.md), for
 * tools/smcconf's tests and by hand:
 *
 *   cfgtool factory|test OUT   the built-in configuration's binary form into OUT
 *   cfgtool check FILE         decode and encode again: "ok N bytes, revision R, canonical" (or
 *                              "not canonical"), exit 0; "error NAME at byte N ...", exit 1
 *   cfgtool batch IN OUT       IN: binary forms, each as a 32-bit little-endian length and its
 *                              bytes; OUT: a line each, "ok HEX" (the canonical form of what was
 *                              decoded) or "error NAME"
 *   cfgtool patch BASE EDITS   BASE: a binary form; EDITS: live edits (CFG_SET, cfgbin_patch()),
 *                              each a preset u8, a length u16 little-endian and the records,
 *                              applied in order; prints "ok HEX" (the canonical form after them,
 *                              with BASE's ID and revision) or "error NAME at edit N, byte B",
 *                              exit 1
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfgbin.h"

static struct core_config cfg;
static uint8_t out[CFGBIN_MAX];

static int usage(void)
{
    fprintf(stderr, "usage: cfgtool factory|test OUT | check FILE | batch IN OUT | patch BASE EDITS\n");
    return 2;
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return NULL;
    }
    size_t cap = 4096, n = 0;
    uint8_t *buf = malloc(cap);
    for (size_t got; buf && (got = fread(buf + n, 1, cap - n, f)) > 0;) {
        n += got;
        if (n == cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
    }
    fclose(f);
    *len = n;
    return buf;
}

static int write_file(const char *path, const uint8_t *p, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(p, 1, n, f) != n || fclose(f)) {
        perror(path);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return usage();
    struct cfgbin_error e;
    struct cfgbin_meta meta;
    if (!strcmp(argv[1], "factory") || !strcmp(argv[1], "test")) {
        if (argv[1][0] == 'f')
            core_config_factory(&cfg);
        else
            core_config_test(&cfg);
        size_t n = cfgbin_encode(&cfg, NULL, out, sizeof out, &e);
        if (!n) {
            fprintf(stderr, "cfgtool: the %s configuration does not encode: %s (preset %u, page %u, control %u, "
                            "field %u)\n", argv[1], cfgbin_error_name(e.code), e.preset, e.page, e.control, e.field);
            return 1;
        }
        return write_file(argv[2], out, n);
    }
    size_t len;
    uint8_t *data = read_file(argv[2], &len);
    if (!data)
        return 1;
    if (!strcmp(argv[1], "check")) {
        int r = cfgbin_decode(data, len, &cfg, &meta, &e);
        if (r) {
            printf("error %s at byte %u (preset %u, page %u, control %u, field %u)\n", cfgbin_error_name((unsigned)r),
                   e.offset, e.preset, e.page, e.control, e.field);
            return 1;
        }
        size_t n = cfgbin_encode(&cfg, &meta, out, sizeof out, &e);
        printf("ok %u bytes, revision %u, %s\n", (unsigned)len, meta.revision,
               n == len && !memcmp(out, data, n) ? "canonical" : "not canonical");
        return 0;
    }
    if (!strcmp(argv[1], "batch") && argc == 4) {
        FILE *f = fopen(argv[3], "w");
        if (!f) {
            perror(argv[3]);
            return 1;
        }
        for (size_t off = 0; off + 4 <= len;) {
            size_t n = (size_t)data[off] | (size_t)data[off + 1] << 8 | (size_t)data[off + 2] << 16 |
                       (size_t)data[off + 3] << 24;
            off += 4;
            if (n > len - off) {
                fprintf(stderr, "cfgtool: batch: a blob past the end\n");
                return 1;
            }
            int r = cfgbin_decode(data + off, n, &cfg, &meta, &e);
            if (r) {
                fprintf(f, "error %s\n", cfgbin_error_name((unsigned)r));
            } else {
                size_t m = cfgbin_encode(&cfg, &meta, out, sizeof out, &e);
                fputs("ok ", f);
                for (size_t i = 0; i < m; i++)
                    fprintf(f, "%02x", out[i]);
                fputc('\n', f);
            }
            off += n;
        }
        return fclose(f) ? 1 : 0;
    }
    if (!strcmp(argv[1], "patch") && argc == 4) {
        int r = cfgbin_decode(data, len, &cfg, &meta, &e);
        if (r) {
            printf("error %s in the base\n", cfgbin_error_name((unsigned)r));
            return 1;
        }
        size_t elen;
        uint8_t *ed = read_file(argv[3], &elen);
        if (!ed)
            return 1;
        unsigned k = 0;
        for (size_t off = 0; off + 3 <= elen; k++) {
            unsigned preset = ed[off], n = ed[off + 1] | (unsigned)ed[off + 2] << 8;
            off += 3;
            if (n > elen - off) {
                fprintf(stderr, "cfgtool: patch: an edit past the end\n");
                return 1;
            }
            r = cfgbin_patch(ed + off, n, preset, NULL, NULL, NULL, &e);
            if (!r)
                r = cfgbin_patch(ed + off, n, preset, &cfg, NULL, NULL, &e);
            if (r) {
                printf("error %s at edit %u, byte %u\n", cfgbin_error_name((unsigned)r), k, e.offset);
                return 1;
            }
            off += n;
        }
        size_t m = cfgbin_encode(&cfg, &meta, out, sizeof out, &e);
        if (!m) {
            printf("error %s when encoded\n", cfgbin_error_name(e.code));
            return 1;
        }
        fputs("ok ", stdout);
        for (size_t i = 0; i < m; i++)
            printf("%02x", out[i]);
        putchar('\n');
        return 0;
    }
    return usage();
}
