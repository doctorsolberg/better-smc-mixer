/*
 * Core settings by name (docs/core.md, "Scenario files", the "set" step): a built-in
 * configuration and a few of its settings changed, for scenarios. The simulator's "set" command
 * and the device's CORE_SETUP debug command (docs/link.md) both use this, so a scenario means the
 * same in both places.
 *
 *   config      factory | test            the configuration; only before any other setting
 *   layer_mode  gesture | immediate       global
 *   pickup      jump | pickup | scale     global
 *   gesture_ms  1-60000                   global
 *   hybrid_ms   1-60000                   global
 *   enc_accel   50-200                    global: the encoder speed scale, percent
 *   fine14      range | lsb               global: the fine step of encoders on 14-bit targets
 *   shift_mode  hold | latch | hybrid     per preset: NAME for all, NAME_p1 ... NAME_p4 for one
 *   note_off    9n | 8n                   per preset, the same way
 *   channel     1-16                      per preset, the same way: the preset's channel, which
 *                                         assignments with channel preset follow
 *   param_null  off | gesture             per preset, the same way: the null parameter after
 *                                         an NRPN or RPN gesture
 *
 * Portable C11; no allocation. A struct core_settings is small (it does not hold a configuration),
 * so a transport task can fill it and the control task apply it.
 */
#ifndef CORE_SETTINGS_H
#define CORE_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core.h"

enum core_config_id { CORE_CONFIG_FACTORY, CORE_CONFIG_TEST };

#define CORE_SETTINGS_MAX 16
#define CORE_SETTINGS_TEXT_MAX 180 /* CORE_SETUP's text, within a command (LINK_CMD_MAX) */

struct core_settings {
    uint8_t config; /* enum core_config_id */
    uint8_t count;
    struct {
        uint8_t what;    /* internal: which setting */
        uint8_t presets; /* per-preset settings: a bit per preset */
        uint16_t value;
    } item[CORE_SETTINGS_MAX];
};

/* No setting changed, on the given configuration. */
void core_settings_init(struct core_settings *s, unsigned config);

/* One setting. False if the name or value is not known, "config" comes after another setting, or
 * there are more than CORE_SETTINGS_MAX; s is then unchanged. A later value of the same setting
 * wins (they are applied in order). */
bool core_settings_add(struct core_settings *s, const char *name, const char *value);

/* "NAME VALUE" pairs separated by spaces, tabs or newlines (at most CORE_SETTINGS_TEXT_MAX bytes,
 * no NUL needed), added to s in order. Returns 0, or the number (from 1) of the first pair not
 * taken; the pairs before it stay added. */
unsigned core_settings_parse(struct core_settings *s, const char *text, size_t len);

/* Writes the configuration with the settings into cfg. */
void core_settings_apply(const struct core_settings *s, struct core_config *cfg);

#endif /* CORE_SETTINGS_H */
