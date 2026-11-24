/*
 * The factory configuration (docs/core.md, "Factory configuration"; SPEC FR-MOD-06): what a fresh
 * device sends. Layout C, decided by the owner on 2026-11-08: a preset is an instrument, its pages
 * are groups of that instrument's parameters.
 *
 *   preset k (1-4): MIDI channel k, the preset's channel, which every assignment follows (channel
 *           preset: changing the preset's channel moves them all); its 4 pages x (8 faders + 8
 *           encoders) are 64 different CCs
 *   faders: absolute CC, pickup (the global default); encoders: absolute CC, acc2, starting at 64,
 *           Shift = the same target in fine mode (the encoder's default Shift layer)
 *   buttons: on the preset's channel too, as keys (decided 2026-11-08): Note On velocity 127 on
 *           press, released on release (momentary), the same on every page. The strip buttons are
 *           32 chromatic keys from note 48: SELECT 1-8 = 48-55, R = 56-63, S = 64-71, M = 72-79;
 *           the bottom row goes on with notes 80-90. Toggle or momentary, and a note or a CC with
 *           its values, are each button's own settings in the preset.
 *   Shift: hold; notes released as Note On with velocity 0, as the stock firmware does
 */
#include <string.h>

#include "core.h"

#define KEY_BASE 48 /* the lowest key: SELECT 1 */

/*
 * CC numbers by page: 8 faders, then 8 encoders. Only controllers with no fixed meaning (3, 9,
 * 14-15, 20-31, 85-87, 89-90, 102-119), general purpose (16-19, 80-83), effect control (12-13),
 * sound controllers (70-79) and effect depths (91-95). Never bank select (0, 32), modulation (1),
 * breath, foot, portamento (2, 4, 5, 65, 84), data entry (6, 38, 96, 97), volume (7), balance (8),
 * pan (10), expression (11), the LSBs (32-63), sustain and the other pedals (64-69), the velocity
 * prefix (88), (N)RPN (98-101) or channel mode (120-127). These are all 64 such numbers.
 */
static const uint8_t factory_cc[CORE_PAGES][2][CORE_STRIPS] = {
    {{20, 21, 22, 23, 24, 25, 26, 27}, {102, 103, 104, 105, 106, 107, 108, 109}},
    {{12, 13, 14, 15, 16, 17, 18, 19}, {110, 111, 112, 113, 114, 115, 116, 117}},
    {{70, 71, 72, 73, 74, 75, 76, 77}, {78, 79, 80, 81, 82, 83, 85, 86}},
    {{87, 89, 90, 91, 92, 93, 94, 95}, {3, 9, 28, 29, 30, 31, 118, 119}},
};

/* An assignment on the preset's channel, its range 0-127. */
static struct core_assign assign(uint8_t msg, unsigned num, uint8_t mode, unsigned start)
{
    struct core_assign a;
    memset(&a, 0, sizeof a);
    a.msg = msg;
    a.channel = CORE_CHANNEL_PRESET;
    a.number = (uint16_t)num;
    a.mode = mode;
    a.lo = 0;
    a.hi = 127;
    a.start = (uint16_t)start;
    return a;
}

void core_config_factory(struct core_config *cfg)
{
    static const uint8_t key_row[4] = {3, 2, 1, 0}; /* M, S, R, SELECT: SELECT has the lowest keys */

    memset(cfg, 0, sizeof *cfg);
    cfg->global.layer_mode = CORE_LAYER_GESTURE;
    cfg->global.pickup = CORE_PICKUP;
    cfg->global.gesture_ms = 300;
    cfg->global.hybrid_ms = 300;
    cfg->global.enc_accel = 100;
    cfg->global.fine14 = CORE_FINE14_RANGE;
    cfg->global.save = CORE_SAVE_MANUAL;

    for (unsigned k = 0; k < CORE_PRESETS; k++) {
        struct core_preset *p = &cfg->preset[k];
        p->channel = (uint8_t)k;
        p->shift_mode = CORE_SHIFT_HOLD;
        p->note_off = CORE_NOTE_OFF_9N;
        for (unsigned pg = 0; pg < CORE_PAGES; pg++)
            for (unsigned n = 0; n < CORE_STRIPS; n++) {
                struct core_strip *s = &p->page[pg][n];
                s->fader.normal = assign(CORE_MSG_CC, factory_cc[pg][0][n], CORE_PICKUP_GLOBAL, 0);
                s->encoder.normal = assign(CORE_MSG_CC, factory_cc[pg][1][n], CORE_ACC2, 64);
                s->encoder.shift = assign(CORE_MSG_CC, factory_cc[pg][1][n], CORE_FINE, 64);
                s->encoder.shift_kind = CORE_SHIFT_OVERRIDE;
                for (unsigned b = 0; b < 4; b++)
                    s->button[b].normal = assign(CORE_MSG_NOTE, KEY_BASE + 8 * key_row[b] + n, CORE_MOMENTARY, 0);
            }
        for (unsigned b = 0; b < CORE_ROW_BUTTONS; b++)
            p->row[b].normal = assign(CORE_MSG_NOTE, KEY_BASE + 32 + b, CORE_MOMENTARY, 0);
    }
}
