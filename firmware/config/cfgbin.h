/*
 * The configuration's binary form (docs/config.md, "Binary form"; SPEC D-4): what the device
 * stores and what the configuration protocol carries.
 *
 * - cfgbin_decode() checks everything before the core can see it: the header and its CRC, every
 *   record, every value against config/schema.json's rules (through cfg_schema.h). With cfg NULL
 *   it only checks (an upload, before it is stored); with cfg it also fills it. On an error cfg's
 *   content is undefined: decode into a configuration not in use, or check first.
 * - cfgbin_encode() writes the canonical form: byte for byte what tools/smcconf writes for the
 *   same configuration.
 *
 * Errors are logged (module cfg, WARN) with where they are. Portable C11, no allocation; the
 * decoder needs about 300 bytes of stack.
 */
#ifndef CFGBIN_H
#define CFGBIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core.h"

#define CFGBIN_HEADER 36
#define CFGBIN_CONTROLS (CORE_PAGES * CORE_STRIPS * 6 + CORE_ROW_BUTTONS) /* per preset */
/* The largest binary form: every control with all its fields and a Shift layer with all its
 * fields (records: global 9 + 2, the LED light's 3 + 2, preset 6 + 2, Normal 16 + 2, Shift 17 + 2
 * bytes; a layer's fields take at most 12 bytes, since n (encoders) and flags (buttons) never apply
 * together). */
#define CFGBIN_MAX (CFGBIN_HEADER + 11 + 5 + CORE_PRESETS * (8 + CFGBIN_CONTROLS * (18 + 19)))

struct cfgbin_meta {
    uint8_t id[16];    /* the configuration's UUID (FR-APP-24); zeros: none */
    uint32_t revision; /* on a running configuration, assigned by the controller */
};

enum cfgbin_code {
    CFGBIN_OK,
    CFGBIN_E_SHORT,   /* shorter than the header */
    CFGBIN_E_MAGIC,   /* not a configuration */
    CFGBIN_E_VERSION, /* another binary version */
    CFGBIN_E_LENGTH,  /* the header's lengths are not the data's */
    CFGBIN_E_CRC,
    CFGBIN_E_RECORD,  /* a record past the end, or its length is not its fields' */
    CFGBIN_E_TAG,     /* an unknown record that may not be skipped */
    CFGBIN_E_TWICE,   /* a second global, LED light or preset record, Normal or Shift layer */
    CFGBIN_E_ORDER,   /* a control before any preset record; a Shift layer apart from its Normal one */
    CFGBIN_E_WHERE,   /* a preset, page or control this device does not have */
    CFGBIN_E_FIELD,   /* an unknown field */
    CFGBIN_E_TYPE,    /* a message type the control does not take */
    CFGBIN_E_VALUE,   /* a value out of range, or set where its field does not apply */
    CFGBIN_E_SHIFT,   /* a navigation button's Shift layer; an override that sends nothing */
    CFGBIN_E_SPACE,   /* encoding: the buffer is too small */
    CFGBIN_CODES
};

struct cfgbin_error {
    uint8_t code;    /* enum cfgbin_code */
    uint8_t preset;  /* 0-based; 255: not a preset's */
    uint8_t page;    /* 0-based; CFG_ROW_PAGE (255): the bottom row; 255 also when no control */
    uint8_t control; /* the binary control id (docs/config.md); 255: none */
    uint8_t field;   /* the field's index in its record; 255: none */
    uint32_t offset; /* the record's offset in the data */
};

/* Returns CFGBIN_OK or the error's code; err (may be NULL) says where. meta may be NULL. */
int cfgbin_decode(const uint8_t *data, size_t len, struct core_config *cfg, struct cfgbin_meta *meta,
                  struct cfgbin_error *err);

/* The canonical binary form into out (at most cap bytes; CFGBIN_MAX always fits). Returns its
 * length, or 0 if cfg does not pass cfgbin_check() or out is too small (err says which). meta may
 * be NULL (no ID, revision 0). */
size_t cfgbin_encode(const struct core_config *cfg, const struct cfgbin_meta *meta, uint8_t *out,
                     size_t cap, struct cfgbin_error *err);

/* An edit of the running configuration (CFG_SET, P-1; docs/config.md, "Edits"): records of the
 * binary form, without a header, all of one preset: at most one global record and one LED light
 * record, at most that preset's record, and its controls' Normal and Shift layers, in the form's
 * order rules. Each
 * setting record replaces those settings, each control named is replaced whole, as a complete
 * form would have it (no Normal layer: none; no Shift layer: the default). Unknown optional
 * records are skipped. With cfg NULL it only checks; with cfg it also applies, in place, record
 * by record: check first (an error leaves cfg half changed). changing (may be NULL) is called
 * with a control before its first record changes it, with NULL before the preset's settings
 * change (the global settings change no note). */
typedef void (*cfgbin_changing_fn)(void *ctx, const struct core_control *old);
int cfgbin_patch(const uint8_t *data, size_t len, unsigned preset, struct core_config *cfg,
                 cfgbin_changing_fn changing, void *ctx, struct cfgbin_error *err);

/* Visit old controls/settings of a checked patch without changing cfg; Normal/Shift records may
 * visit the same control twice. No callback on invalid data. */
int cfgbin_patch_visit(const uint8_t *data, size_t len, unsigned preset, const struct core_config *cfg,
                       cfgbin_changing_fn visit, void *ctx, struct cfgbin_error *err);

/* Checks a configuration in memory by the same rules (the factory one, one built in code). */
int cfgbin_check(const struct core_config *cfg, struct cfgbin_error *err);

const char *cfgbin_error_name(unsigned code);

/* CRC-32 (IEEE 802.3, as zlib's crc32): start with 0. */
uint32_t cfgbin_crc32(uint32_t crc, const uint8_t *p, size_t n);

#endif /* CFGBIN_H */
