/*
 * The test configuration (docs/core.md, "Configuration"): what the unit tests and the scenarios
 * run against. It is not the factory configuration.
 *
 *   bank (preset k, page p): MIDI channel 4k + p + 1
 *   fader n: CC 20 + n, pickup (global default)
 *   encoder n: CC 102 + n, acc1, starting at 64; Shift: the same target in fine mode
 *   M n: note 24 + n toggle; S n: note 32 + n toggle; R n: note 40 + n momentary;
 *   SELECT n: note 48 + n momentary
 *   bottom row: notes 84-94 momentary on the preset's first channel (4k + 1)
 *
 *   bank 16 (preset 4, page 4, channel 16) is the 14-bit bank, the range 0-16383 everywhere:
 *   faders 1-6: NRPN 256 + n (MSB 2, LSB n); fader 7: RPN 1; fader 8: CC 14-bit 27 (20 + n);
 *   encoder n: NRPN 384 + n (MSB 3, LSB n), acc1, starting at 8192; Shift: fine
 */
#include <string.h>

#include "core.h"

static struct core_assign assign(uint8_t msg, unsigned ch, unsigned num, uint8_t mode, unsigned lo,
                                 unsigned hi, unsigned start)
{
    struct core_assign a;
    memset(&a, 0, sizeof a);
    a.msg = msg;
    a.channel = (uint8_t)ch;
    a.number = (uint16_t)num;
    a.mode = mode;
    a.lo = (uint16_t)lo;
    a.hi = (uint16_t)hi;
    a.start = (uint16_t)start;
    return a;
}

void core_config_test(struct core_config *cfg)
{
    static const uint8_t button_note[4] = {24, 32, 40, 48};
    static const uint8_t button_mode[4] = {CORE_TOGGLE, CORE_TOGGLE, CORE_MOMENTARY, CORE_MOMENTARY};

    memset(cfg, 0, sizeof *cfg);
    cfg->global.layer_mode = CORE_LAYER_GESTURE;
    cfg->global.pickup = CORE_PICKUP;
    cfg->global.gesture_ms = 300;
    cfg->global.hybrid_ms = 300;
    cfg->global.enc_accel = 100;
    cfg->global.fine14 = CORE_FINE14_RANGE;
    cfg->global.save = CORE_SAVE_MANUAL;
    cfg->global.led_level = 10; /* full */
    cfg->global.led_unlit = 0;  /* dark */

    for (unsigned k = 0; k < CORE_PRESETS; k++) {
        struct core_preset *p = &cfg->preset[k];
        p->shift_mode = CORE_SHIFT_HOLD; /* the default (FR-SH-01) */
        for (unsigned pg = 0; pg < CORE_PAGES; pg++) {
            unsigned ch = 4 * k + pg;
            for (unsigned n = 0; n < CORE_STRIPS; n++) {
                struct core_strip *s = &p->page[pg][n];
                s->fader.normal = assign(CORE_MSG_CC, ch, 20 + n, CORE_PICKUP_GLOBAL, 0, 127, 0);
                s->encoder.normal = assign(CORE_MSG_CC, ch, 102 + n, CORE_ACC1, 0, 127, 64);
                s->encoder.shift = assign(CORE_MSG_CC, ch, 102 + n, CORE_FINE, 0, 127, 64);
                s->encoder.shift_kind = CORE_SHIFT_OVERRIDE;
                for (unsigned b = 0; b < 4; b++)
                    s->button[b].normal = assign(CORE_MSG_NOTE, ch, button_note[b] + n,
                                                 button_mode[b], 0, 127, 0);
                if (k == CORE_PRESETS - 1 && pg == CORE_PAGES - 1) { /* the 14-bit bank */
                    uint8_t msg = n < 6 ? CORE_MSG_NRPN : n == 6 ? CORE_MSG_RPN : CORE_MSG_CC14;
                    unsigned num = n < 6 ? 256 + n : n == 6 ? 1 : 20 + n;
                    s->fader.normal = assign(msg, ch, num, CORE_PICKUP_GLOBAL, 0, 16383, 0);
                    s->encoder.normal = assign(CORE_MSG_NRPN, ch, 384 + n, CORE_ACC1, 0, 16383, 8192);
                    s->encoder.shift = assign(CORE_MSG_NRPN, ch, 384 + n, CORE_FINE, 0, 16383, 8192);
                }
            }
        }
        for (unsigned b = 0; b < CORE_ROW_BUTTONS; b++)
            p->row[b].normal = assign(CORE_MSG_NOTE, 4 * k, 84 + b, CORE_MOMENTARY, 0, 127, 0);
    }
}
