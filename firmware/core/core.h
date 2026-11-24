/*
 * The control core (SPEC D-2, D-3; design in docs/core.md): control events in, MIDI messages and
 * LED states out. Portable C11: no platform headers, no clock, no hardware. Deterministic: the
 * same events give the same output and the same log records.
 *
 *   struct core c;
 *   core_init(&c, &config, &io, ctx);
 *   core_event(&c, &(struct core_event){.t_us = t, .type = CORE_EV_FADER, .index = 2, .value = p});
 *
 * Logging goes through smclog (modules input and engine), in the cause chain of each event.
 */
#ifndef CORE_H
#define CORE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- the panel ----------------------------------------------------------------------------- */

#define CORE_STRIPS 8
#define CORE_PRESETS 4
#define CORE_PAGES 4
#define CORE_BUTTONS 45
#define CORE_STRIP_BUTTONS 32 /* 4 per strip: M, S, R, SELECT */
#define CORE_ROW_BUTTONS 11   /* the bottom row, 32-42 */
#define CORE_LEDS 53          /* 45 under the buttons (same indexes), 8 at the encoders */
#define CORE_FADER_MAX 16383  /* fader positions are 14-bit */

/* Button indexes, as in the stock firmware's table (docs/hardware.md). Strip n (0-7) has
 * M = 4n, S = 4n + 1, R = 4n + 2, SELECT = 4n + 3. */
enum core_button {
    CORE_BTN_M = 0,
    CORE_BTN_S = 1,
    CORE_BTN_R = 2,
    CORE_BTN_SEL = 3,
    CORE_BTN_PLAY = 32,
    CORE_BTN_STOP,
    CORE_BTN_REC,
    CORE_BTN_REW,        /* double arrow ◀◀: Shift + it = preset 1 */
    CORE_BTN_FF,         /* ▶▶: preset 2 */
    CORE_BTN_BANK_LEFT,  /* «: preset 3 */
    CORE_BTN_BANK_RIGHT, /* »: preset 4 */
    CORE_BTN_UP,         /* ▲: Shift + it = page 1 */
    CORE_BTN_DOWN,       /* ▼: page 2 */
    CORE_BTN_LEFT,       /* ◀: page 3 */
    CORE_BTN_RIGHT,      /* ▶: page 4 */
    CORE_BTN_BT,
    CORE_BTN_SHIFT,
};
#define CORE_LED_ENC(n) (CORE_BUTTONS + (n))

/* --- events -------------------------------------------------------------------------------- */

enum core_ev_type {
    CORE_EV_TICK,    /* time passes */
    CORE_EV_BUTTON,  /* index: button; value: 1 press, 0 release */
    CORE_EV_ENCODER, /* index: encoder; value: signed clicks, + clockwise */
    CORE_EV_FADER,   /* index: fader; value: position 0-CORE_FADER_MAX */
    CORE_EV_MIDI_IN, /* index: transport (CORE_FROM_); midi: a complete message */
    CORE_EV_LINK,    /* index: transport (CORE_FROM_BLE); value: 1 a central connected, 0 none */
};
enum core_transport { CORE_FROM_USB, CORE_FROM_BLE, CORE_TRANSPORTS };

#define CORE_EV_INJECTED 0x01u /* flags: put in by INPUT_INJECT (LOG-33), not the scan */

struct core_event {
    uint32_t t_us; /* µs, from any free-running clock; the core uses differences only */
    uint8_t type;  /* enum core_ev_type */
    uint8_t index;
    uint8_t flags;
    uint8_t midi[3];
    int16_t value;
};

/* --- configuration (generated from config/schema.json, SPEC D-4; docs/config.md) ----------- */

#include "core_config.h"

/* The configuration the unit tests and most scenarios use (docs/core.md, "Configuration"). */
void core_config_test(struct core_config *cfg);
/* What a fresh device sends (docs/core.md, "Factory configuration"; SPEC FR-MOD-06). */
void core_config_factory(struct core_config *cfg);

/* --- outputs ------------------------------------------------------------------------------- */

enum core_led_state { CORE_LED_OFF, CORE_LED_ON, CORE_LED_SLOW, CORE_LED_FAST };

struct core_io {
    /* One complete MIDI message; cause: the log cause ID of the event that produced it. */
    void (*midi)(void *ctx, const uint8_t *msg, unsigned len, uint16_t cause);
    /* A LED changed state (enum core_led_state). */
    void (*led)(void *ctx, unsigned led, unsigned state);
    /* The BT button's long press turned Bluetooth on (1) or off (0); NULL: nobody listens. */
    void (*bluetooth)(void *ctx, unsigned on);
};

/* --- state --------------------------------------------------------------------------------- */

#ifndef CORE_TARGETS
#define CORE_TARGETS 1024 /* target store entries, a power of two */
#endif

struct core_target {
    uint32_t key;   /* core_target_key(), the channel resolved; 0 = empty slot */
    uint16_t value;
    uint8_t known;  /* the value is set */
    uint8_t used;   /* core_reconfigured(): the configuration still addresses it */
};

#define CORE_PARAM_SET 0x8000u  /* param_out, param_in: a parameter is selected */
#define CORE_PARAM_RPN 0x4000u  /* ... and it is an RPN */
#define CORE_PARAM_NULL_NUM 0x3FFFu /* 127/127: the null parameter, data entry goes nowhere */

struct core_gesture {
    uint32_t last_us; /* the last event */
    uint8_t active;
    uint8_t layer;    /* 0 Normal, 1 Shift */
    uint8_t null_ch;  /* paramNull gesture: the channel + 1 whose null parameter is due at its end */
};

struct core_fader {
    struct core_gesture g;
    uint16_t pos;
    uint8_t have_pos;
    uint8_t locked;     /* follows its target: sync_key holds sync_value because of this fader */
    uint8_t waiting;    /* not picked up during this gesture (logged once) */
    uint8_t indication; /* LED at the strip's encoder: CORE_LED_OFF / SLOW (move up) / FAST */
    uint32_t sync_key;
    uint16_t sync_value;
};

struct core_encoder {
    struct core_gesture g;
    int16_t rem; /* CORE_DIV: clicks not yet turned into a step; acc/fine: the step's Q8 fraction */
    int8_t dir;  /* the last click's direction: +1, -1 (0 before the first) */
};

enum core_held { CORE_HELD_NONE, CORE_HELD_ASSIGN, CORE_HELD_NAV, CORE_HELD_OTHER };

struct core_button_state {
    uint8_t down;
    uint8_t held;            /* enum core_held: what the press did */
    uint8_t preset;          /* CORE_HELD_ASSIGN: the preset of the press (its Note Off form) */
    struct core_assign a;    /* CORE_HELD_ASSIGN: the assignment of the press */
};

struct core_stats {
    uint32_t events;
    uint32_t injected;
    uint32_t bad_events; /* index or type out of range */
    uint32_t midi_out;
    uint32_t midi_in;
    uint32_t midi_in_off; /* of midi_in: from BLE while Bluetooth is off, ignored */
    uint32_t targets;    /* store entries in use */
    uint32_t store_full; /* target references in the configuration that did not fit (WARN) */
};

struct core {
    const struct core_config *cfg;
    const struct core_io *io;
    void *ctx;
    uint32_t now_us;

    uint8_t preset, page;
    uint8_t page_of[CORE_PRESETS]; /* the page last used in each preset */

    uint8_t shift;           /* active */
    uint8_t shift_down;      /* the button is held */
    uint8_t shift_used;      /* a control was used while it was held */
    uint8_t shift_press_off; /* hybrid: this press turned a latched Shift off */
    uint8_t shift_press_mode; /* the preset's Shift mode when the button went down */
    uint32_t shift_down_us;

    /* Bluetooth (SPEC §5.5): none after core_init(), until the platform says it has it
     * (core_set_bluetooth()); the BT button's long press turns it on and off. Its LED: off; blinking while no central is connected; lit while one is (CORE_EV_LINK).
     * While it is off the output goes to USB alone (ctl) and MIDI in from BLE is ignored. */
    uint8_t bt_on;
    uint8_t bt_none;      /* no Bluetooth here: off, and the long press does nothing */
    uint8_t bt_link;
    uint8_t bt_long;      /* this press of the BT button has toggled it */
    uint32_t bt_down_us;

    int8_t flash_led; /* blinking after a page or preset switch, -1 none */
    uint32_t flash_us;

    /* NRPN and RPN (docs/core.md, "NRPN and RPN"): the parameter selected on each channel, by our
     * output and by each transport's sender: CORE_PARAM_SET | CORE_PARAM_RPN (RPN) | number; 0 none */
    uint16_t param_out[16];
    uint16_t param_in[CORE_TRANSPORTS][16];

    /* While io.midi runs: nonzero for the messages of one continuous control's value (a fader's or
     * an absolute encoder's CC, 14-bit CC, NRPN, RPN, pitch bend, channel pressure), a new number
     * for each value; 0 for every other message. A slow output may drop a waiting value when a
     * newer one of the same target comes (FR-IO-08; docs/ble.md, "Last value wins"). */
    uint8_t out_merge;
    uint8_t out_group; /* the last number given */

    /* Notes our output left on (a Note On with velocity, no Note Off since), a bit per channel and
     * note number, and the Note Off form each takes (set: 8n, the form of the preset that sent
     * the Note On): core_notes_off() */
    uint8_t note_on[16][16];
    uint8_t note_8n[16][16];

    struct core_fader fader[CORE_STRIPS];
    struct core_encoder encoder[CORE_STRIPS];
    struct core_button_state button[CORE_BUTTONS];
    uint8_t led[CORE_LEDS];
    uint8_t leds_dirty;

    struct core_stats stats;
    struct core_target store[CORE_TARGETS];
};

/* --- API ----------------------------------------------------------------------------------- */

/* Starts the core: fills the target store with every target the configuration addresses (no
 * values yet), selects preset 1 page 1 and reports every LED once. */
void core_init(struct core *c, const struct core_config *cfg, const struct core_io *io, void *ctx);

/* One input event. If the caller has begun a log cause (smclog_cause != 0) the core's records go
 * into it; otherwise the core begins and ends its own. */
void core_event(struct core *c, const struct core_event *ev);

/* Sends a Note Off for every note our output left on, each in the form of the preset that sent
 * it, and forgets them; returns how many. For a configuration about to be replaced (ctl, a
 * received configuration; the owner's decision of 2026-11-12): core_init() forgets the notes and
 * the presses, so a toggle left on, or a key held through the change, would keep sounding. */
unsigned core_notes_off(struct core *c);

/* At most max releases. Unsent notes stay tracked for the next call. */
unsigned core_notes_off_limit(struct core *c, unsigned max);

/* The configuration changed in place between two events (a live edit, P-1; docs/core.md, "Live
 * edits"): the core keeps its state (the preset and page, Shift, the targets' values, the held
 * buttons, the faders' pickup), drops the targets the configuration no longer addresses, adds the
 * new ones (no value yet) and reports the LEDs that change. */
void core_reconfigured(struct core *c);

/* Before control ctl of preset `preset` changes in place: a Note Off for each note its layers left
 * on (as core_notes_off() does for them), and a held button of it releases nothing later; returns
 * how many. */
unsigned core_release_control(struct core *c, unsigned preset, const struct core_control *ctl);

/* Sets the Bluetooth state without a press and without io.bluetooth: on 1, off 0, or -1 for a
 * platform without Bluetooth (off, and the long press does nothing); link: a central is connected.
 * For the platform's state at start and for keeping it over a core_init() (a new configuration). */
void core_set_bluetooth(struct core *c, int on, bool link);

/* Selects a preset and a page (0-based; -1 keeps the current one, a new preset alone goes to its
 * last page). For restoring at power-on and for remote selection. */
void core_select(struct core *c, int preset, int page);

/* An assignment's MIDI channel (0-15) in preset `preset`: its own, or the preset's for
 * CORE_CHANNEL_PRESET. */
unsigned core_channel(const struct core_config *cfg, unsigned preset, const struct core_assign *a);
/* Target identity (D-5) on a channel (0-15, resolved): 0 for assignments without one (none,
 * relative). */
uint32_t core_target_key(const struct core_assign *a, unsigned channel);
/* The target's value; false if the target is not stored or has no value yet. */
bool core_target_get(const struct core *c, uint32_t key, uint16_t *value);

/* Names for the simulator, logs and scenarios: "m1", "play", "enc3", "fader8", "led_enc1"... */
const char *core_button_name(unsigned button);
const char *core_led_name(unsigned led);
/* Index of a name, or -1. kind: 'b' button, 'e' encoder, 'f' fader, 'l' LED. */
int core_find_name(char kind, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* CORE_H */
