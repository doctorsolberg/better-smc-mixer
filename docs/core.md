# The control core and its simulator

The core turns control input into MIDI: pages, presets, Shift, pickup, encoder modes, buttons and
the target store (SPEC §4.2 "Control engine", §5). It is portable C11 with no platform headers
(SPEC D-2) and a deterministic state machine (D-3): the same events give the same MIDI, the same
LED states and the same log records. The same code runs on the device and in the simulator on the
host, where scenarios and recorded sessions are replayed (LOG-35, LOG-40…42).

Code: `firmware/core/`. Tests: `firmware/tests/test_core.c` and the scenarios in
`firmware/tests/scenarios/` (both run by `make -C firmware test`). Simulator: `firmware/sim/` (the
C program) and `tools/smcsim/` (the scenario runner).

What is built so far is the skeleton: the structure, the behaviour below, the simulator and the
scenario format. The full feature set is stage 3 (what is left: [the end of this file](#not-in-the-skeleton)).

**Evidence status.** The controller has run on the mixer since 2026-11-11; the dated scenario,
manual-control, acceleration and NRPN results below remain valid for those builds. A code review
of 2026-11-15 found output-unit overflow (R01) and live-edit note/target cleanup (R02); both were
fixed with the other review corrections (R01–R12, 2026-11-16..19) and verified on the mixer. On
2026-11-23, on a retail mixer, all 19 core scenarios (`smcsim device`, including
`note-cleanup-target` for R02 and `live-edit`) agreed with the simulator, and with all 128 strip
buttons holding notes, a configuration replacement released every one of them: 128 distinct
Note Off on the host, none missing (R01 under load).

## Place in the firmware

```
 board support          input layer             core                     MIDI router, board
 matrix, ADC, encoders → debounce, quadrature, → core_event()  ──MIDI──→  USB / BLE queues
                         fader filter            │  (one event   ──LED───→  LED matrix
 MIDI router (in)  ─────────────────────────────→│   at a time)  ──log───→  smclog ring
 INPUT_INJECT (LOG-33) ─────────────────────────→│
 timer ── tick ─────────────────────────────────→┘
```

The core never reads hardware, time or flash. Everything it knows arrives as an event; everything
it does leaves through two callbacks (MIDI, LED) and the log. It does not queue output: the MIDI
router owns the queues and the "last value wins" rule (FR-IO-08).

## Inputs: events

`struct core_event`: a timestamp in µs (the device's µs timer; it may wrap, the core only takes
differences), a type, an index and a value.

| Type | Index | Value |
|---|---|---|
| `CORE_EV_BUTTON` | button 0–44 | 1 press, 0 release |
| `CORE_EV_ENCODER` | encoder 0–7 | signed clicks since the last event (+ clockwise) |
| `CORE_EV_FADER` | fader 0–7 | position 0–16383, after the input layer's filter and end calibration |
| `CORE_EV_MIDI_IN` | transport (0 USB, 1 BLE) | the message in `midi[3]` (complete, no running status) |
| `CORE_EV_LINK` | transport (1 BLE only) | 1 a central connected, 0 none (the BT LED) |
| `CORE_EV_TICK` | — | — : time passes (gesture ends, LED flashes) |

A flag marks injected events (LOG-33); the core logs them as injected and otherwise treats them
like real ones.

Fader positions are 14-bit whatever the ADC gives (≈10 bit, NFR-04), so 7-bit and 14-bit targets
are mapped the same way.

**Control indexes** follow the stock firmware's table (`docs/hardware.md`, "Input matrix"):

| Index | Control |
|---|---|
| buttons 0–31 | strip n (0–7): `4n` M, `4n+1` S, `4n+2` R, `4n+3` □ |
| buttons 32–42 | bottom row: Play, Stop, Rec, Rew ◀◀, FF ▶▶, Bank «, Bank », ▲, ▼, ◀, ▶ |
| button 43, 44 | BT, Shift |
| encoders 0–7, faders 0–7 | strips 1–8 |
| LEDs 0–44 | under the buttons, same indexes |
| LEDs 45–52 | at the encoders |

Names for the simulator and the logs come from one table in the core (`core_names.c`): `m1`, `s1`,
`r1`, `sel1` … `sel8`, `play`, `stop`, `rec`, `rew`, `ff`, `bank_left`, `bank_right`, `up`, `down`,
`left`, `right`, `bt`, `shift`; encoders `enc1`…`enc8`, faders `fader1`…`fader8`; LEDs take the
button names, and `enc1`…`enc8` for the encoder LEDs.

## Outputs

- **MIDI**: `io->midi(ctx, bytes, len, cause)`. One call per message, in order. The cause ID is the
  log cause of the event that produced it (LOG-04), so the router can log "sent over USB" in the
  same chain. A 14-bit value is several messages (CC 14-bit 2, NRPN and RPN 2 or 4: [NRPN and
  RPN](#nrpn-and-rpn)) with one cause ID; the router must keep them together and in order.
- **LEDs**: `io->led(ctx, led, state)` when a LED's state changes. States: off, on, blink slow,
  blink fast. Board support renders the blink phase; the core only decides the state.
- **Bluetooth**: `io->bluetooth(ctx, on)` when the BT button's long press turns it on or off
  ("Bluetooth" below); NULL if nobody listens.
- **Log**: smclog records in modules `input` (events) and `engine` (decisions), all in the cause
  chain of their event (the core begins a cause per event unless its caller already has one):

  | Module, level | Records |
  |---|---|
  | `input` INFO | `button N down`, `button N up` (N: index 0–44), `link T = 0/1` (T: 1 BLE) |
  | `input` DEBUG | `encoder N +k`, `fader N = position`, `midi in T: b0 b1 b2` (N: strip 1–8; T: 0 USB, 1 BLE) |
  | `engine` INFO | state transitions (LOG-05): `core start` and `presets 1-4: shift modes …, note off forms …`, `shift 0/1`, `shift latched`, `preset P page G`, `fader N picked up …`, `fader N waits …`, `bluetooth on/off`; WARN `target store full` |
  | `engine` DEBUG | `out b0 b1 b2` per message sent, gesture starts and ends with their layer, encoder steps, targets changed by incoming MIDI, MIDI from BLE ignored while Bluetooth is off |
  | `engine` TRACE | LED changes |

  Injected input adds ` (injected)`. `smcsim replay` and `smcsim device` read these records back
  (`tools/smcsim/corelog.py`, the one place that parses them; a test checks that `core.c` still
  holds the format strings it lists), so their wording is
  part of the interface.

## State

| State | Kept where | Notes |
|---|---|---|
| preset, page | core | the page is remembered per preset (Appendix A) |
| Shift | core | hold / latch / hybrid (FR-SH-01) |
| target values | the target store | D-5, below |
| per fader | position, previous position, gesture layer and start, pickup indication | |
| per encoder | last click time, divider remainder (or the step's Q8 fraction), last direction, gesture layer | |
| per button | the assignment it was pressed with | the release goes to the same target, even after a page or Shift change |
| NRPN/RPN selection per channel | core | the parameter our output last selected (`param_out`), and each transport's sender's (`param_in`): [NRPN and RPN](#nrpn-and-rpn) |
| LED flashes | core | the selected page/preset button blinks for 1 s (FR-PG-03) |

Persisting preset and page across power-off (FR-MOD-02) is the storage task's job: it reads them
from the core and restores them with `core_select()`.

### The target store (D-5)

A target is the identity of a MIDI parameter: message type, channel and number (CC 7, note 60 on
channel 2, Pitch Bend on channel 5); for an assignment on the preset's channel, the channel is the
preset's (`core_channel()`, `core_target_key()`). The store keeps one value per target, in the target's own
units (0–127, 0–16383 for 14-bit types, velocity for notes). All controls, layers and pages that
address the same target see one value; incoming MIDI writes into it (FR-IO-06).

- A fixed open-addressing hash table, `CORE_TARGETS` entries (1024 by default, 8 bytes each).
- `core_init()` fills it with every target the configuration addresses, without values; incoming
  MIDI only updates those, so a synth sending everything cannot fill it. Its capacity is checked
  once, at start: targets beyond 7/8 of the table (kept free for short probes) are not stored, with
  a WARN record and a counter; their controls still send but keep no value.
- A target that was never written has no value: an encoder starts from the assignment's `start`
  (by default the middle of `lo`..`hi`), a fader takes over on its first move.
- CC 7-bit and CC 14-bit with the same number are different targets (different units). NRPN and
  RPN targets are keyed by their 14-bit parameter number.

## Behaviour

### Layers and Shift

Every control has a Normal and a Shift layer (FR-MOD-03). The Shift layer of an assignment is one
of `inherit` (the Normal layer), `override` (its own assignment: any type, channel, number and
mode the control's kind allows), `block` (nothing). Defaults: faders and buttons `inherit`,
encoders `override` with their Normal assignment in mode `fine` (FR-SH-02).

Shift modes (FR-SH-01), set **per preset**; `hold` unless the preset asks for another:

- `hold`: active while the button is down;
- `latch`: each press toggles it;
- `hybrid`: a short press (under 300 ms, `hybrid_ms`, global) with no other control used latches;
  a longer hold, or using a control while holding, makes it temporary.

The mode of the preset in use governs. Changing presets with Shift active:

- into a `hold` preset, a latched Shift is dropped ("shift latch dropped: preset N holds");
- between two latching presets, a latched Shift stays latched;
- a press that began in a `hold` preset, or is released in one, turns Shift off.

So a `hold` preset never keeps Shift on after the button is released.

The layer of a fader or encoder is fixed at the start of a gesture (`gesture` mode, the default,
FR-SH-05): a gesture starts with the first event after a rest of 300 ms or more and keeps its layer
until it ends. In `immediate` mode the layer follows Shift at once. A button's layer is fixed at
its press.

### Navigation (fixed, FR-SH-02)

With Shift active, the arrows select pages and the double arrows select presets, in every preset:

| Button | ▲ | ▼ | ◀ | ▶ | ◀◀ | ▶▶ | « | » |
|---|---|---|---|---|---|---|---|---|
| Shift + | page 1 | page 2 | page 3 | page 4 | preset 1 | preset 2 | preset 3 | preset 4 |

A preset switch goes to the page last used in that preset. Target values are kept (FR-PG-02).
While Shift is active, the current page's arrow and the current preset's double arrow are lit; a
switch makes the selected button blink fast for 1 s (FR-PG-03). The Shift LED is on while Shift is
active (FR-SH-06). Without Shift these eight buttons are ordinary controls.

### Faders (FR-CTL-03)

The position maps linearly onto the assignment's range `lo…hi` (`lo > hi` is an inverted fader).
Pickup modes:

- `jump`: every move sends.
- `pickup` (default): the fader sends once it is synchronized with the target. It is synchronized
  when its value comes within one step of the target's value (1/128 of the range for ranges of
  1024 values and more) or crosses it during a move, or when the target has no value yet. It stays synchronized while the target's value is the one it sent last; anything else
  changing the target (incoming MIDI, another control, another layer or page addressing a
  different target) breaks the synchronization, and the fader waits again.
- `scale`: the output moves from the target's value towards the end of the range the fader moves
  to, proportionally, and meets the fader there or on the way; then it is synchronized.

A value that equals the last value sent is not sent again (NFR-04: nothing at rest). On a 14-bit target
(CC 14-bit, Pitch Bend, NRPN, RPN) on the full range 0–16383 the position is the value: every
position the scan reports goes out, one step of the fader's 14-bit position is one step of the
value, and the core merges or skips none (the scan's hysteresis keeps a fader at rest quiet,
[hwtest.md](hwtest.md), "Faders"). While a fader
that is not synchronized moves, the LED at its strip's encoder blinks: slow — move up, fast — move
down (FR-PG-04); it goes off when the fader is picked up or its gesture ends.

### Encoders (FR-CTL-04)

An absolute encoder changes its target's value by clicks × step, clamped to the range (the target
store keeps it, so pages and layers cause no jumps). Turning clockwise moves towards `hi`.

| Mode | Step per click |
|---|---|
| `acc0`…`acc3` | the fine step (1 for 7-bit targets, the range / 2048 for 14-bit ones) plus 1.5 for each acceleration level above the first at this speed ([Encoder acceleration](#encoder-acceleration)). `acc0` never accelerates; the first click of a gesture is the fine step |
| `fine` | the fine step, without acceleration |
| `div N` | one value step per N clicks; the remainder resets with a new gesture |
| `step N` | N |

`N` (1–127) is required in `div` and `step`. Relative encoders (CC relative: 2's complement,
offset 64, sign-magnitude) send the step and store nothing. `fine` is the default Shift layer.

### Buttons (FR-CTL-06, FR-CTL-07)

Each button has its own mode and message in the preset (in each page for the strip buttons, once
for the bottom row): a note, a CC, or any other type of the table below, with its own `lo` and `hi`
values. So a CC button can send, say, 64 when on and 0 when off.

- `momentary`: press sends `hi`, release sends `lo`;
- `toggle`: press sends `lo` if the target is on (its value is not `lo`, as the LED shows),
  else `hi`; release sends nothing;
- `trigger`: press sends `hi`, release nothing.

A flag suppresses the release message. The LED is on when the target's value is not `lo` (its own
state and incoming MIDI both reach it through the store), or always off / always on.

Notes: values above 0 are sent as Note On with that velocity. Value 0 releases the note in the
preset's Note Off form (FR-CTL-10, `note_off`, decided 2026-11-08):

- `9n nn 00`, Note On with velocity 0, by default, as the stock firmware sends it;
- `8n nn 00`, Note Off, if the preset asks for it.

A button's release uses the form of the preset its press was in, like its assignment, even if the
preset changed while the button was held.

### Incoming MIDI (FR-IO-06)

CC (7-bit; 14-bit MSB then LSB), NRPN and RPN (data entry into the parameter the sender selected,
per transport: [NRPN and RPN](#nrpn-and-rpn)), Note On/Off, Pitch Bend, Program Change and Channel
Aftertouch update the store, so faders re-check pickup and LEDs follow. Nothing is echoed. A MIDI
event's index is its transport (0 USB, 1 BLE); any other is a bad event. While Bluetooth is off,
MIDI from BLE (sent before the link went down) is ignored and counted (`midi_in_off`).

### Bluetooth (SPEC §5.5, FR-IO-04)

The core keeps whether Bluetooth is on (`bt_on`) and whether a central is connected (`bt_link`,
from `CORE_EV_LINK`). After `core_init()` there is none; the platform says it has it with
`core_set_bluetooth(c, 1, false)` (ctl does on `ctl_set_ble()`, the simulator always; -1: none,
so the long press does nothing). The BT button's long press, 800 ms (the stock firmware's 41 scans
of 20 ms, ⚠️ static analysis of V012), toggles it at the mark while the button is still held (on
a tick, or on the release if no event came between), once per press; a short press does nothing.
The core logs `bluetooth on/off` and calls `io->bluetooth`. The BT LED: off while Bluetooth is
off, blinking slowly while it is on and no central is connected, lit while one is. Where the
output goes is the control path's (ctl: USB always, BLE while on). ctl keeps the state over a
new configuration (`core_set_bluetooth()` after `core_init()`), and keeps it over a power-off
with the preset and the page (ctl.h, `ctl_state()` and `ctl_restore_state()`, which uses
`core_set_bluetooth()` and `core_select()`: no new core behaviour; [config.md](config.md), "The
panel's state"). Scenario: `bluetooth.jsonl` (on the device: with no central connected over BLE,
else its LED is on, not blinking; it turns Bluetooth off, which ends such a connection).

### Message types in the skeleton

| Type | Faders | Encoders | Buttons |
|---|---|---|---|
| CC 7-bit | ✓ | ✓ absolute | ✓ |
| CC 14-bit (MSB n, LSB n + 32) | ✓ | ✓ | — |
| CC relative (3 encodings) | — | ✓ | — |
| Pitch Bend 14-bit | ✓ | ✓ | ✓ |
| NRPN 14-bit (99/98, then 6/38) | ✓ | ✓ | ✓ |
| RPN 14-bit (101/100, then 6/38) | ✓ | ✓ | — |
| Note | ✓ (velocity) | — | ✓ |
| Program Change | ✓ | ✓ | ✓ (sends `hi`) |
| Channel Aftertouch | ✓ | ✓ | ✓ |

## Configuration

The core runs on a C structure, `struct core_config`, generated from `config/schema.json` with
its enums (`firmware/core/core_config.h`; the format, the files and the binary form the device
will store are in [config.md](config.md)):

- global: gesture mode and pause, the hybrid Shift time, default pickup, encoder acceleration
  scale/fine step, and the saving mode (the last is read by ctl, not the core);
- per preset: its MIDI channel, the Shift mode, the Note Off form, the null parameter after an
  NRPN gesture (`paramNull`), the 11 bottom-row buttons
  (global controls, FR-MOD-01) and, per page, 8 strips of a fader, an encoder and 4 buttons;
- per control: the Normal assignment, the Shift kind and the Shift assignment. An assignment is a
  message type, channel (0–15, or `CORE_CHANNEL_PRESET`: the preset's), number, mode, `n`, flags,
  `lo`, `hi` and `start`.

There are two configurations in the code:

- the **factory configuration**, `core_config_factory()`: what a fresh device will send (below);
- the **test configuration**, `core_config_test()`: what the unit tests and most scenarios run on.
  It stays as it was when the tests were written, so they test the core's behaviour and do not
  change when the factory layout does.

Both are also files, `config/factory.json` and `config/test.json`; a test checks that the firmware
encodes them byte for byte.

### Live edits (P-1)

A received configuration restarts the core (`core_init()`: the targets back to their defaults).
A live edit (CFG_SET, [link.md](link.md), "Configuration") changes the configuration in place
instead, between two events, and the core carries on:

- `core_release_control(c, preset, ctl)` is called for each control the edit replaces, before it
  changes: every note of that control which is on gets its Note Off now, and a held button of it
  releases nothing afterwards (log "notes off before a control changes: N"). A preset-settings
  record ends every note on (`core_notes_off()`: the channel or the Note Off form may change); a
  global record ends none.
- `core_reconfigured(c)` then rebuilds the target store (D-5) for the new configuration: it marks
  every target a control of any page, layer or preset still addresses, removes the others (with
  the backward shift of linear probing, so lookups stay correct), and adds the new ones at their
  defaults. The values of targets that stay are kept, with their pickup state; so are the preset,
  the page, Shift and the held buttons. The LEDs are redrawn. Log: "configuration edited: N
  targets, D dropped, A added".
- The global setting `save` (Q-26) is not the core's: the control path reads it for the autosave.

Cleanup writes zero through the target store, so aliases' LEDs and the next toggle agree with
the Note Off even when the edit preserves the target (R02). It also disarms held note releases.
The control layer visits a checked patch read-only before changing any settings or assignment;
cleanup uses the old channels and saved Note Off forms. A batch or preset-settings edit can span
several control rounds, without applying any record or consuming input in between. The old note
bitmap is the continuation: already released notes are skipped on the next visit. No dynamic
queue or second note bitmap is needed. A global-only edit releases no notes.

✅ Tested: `test_core.c` (`test_reconfigured`: 40 rounds of random edits, each store equal to a
fresh `core_init()`'s with the kept values; `test_release_control`), `test_cfglink.c`, and the
scenario `live-edit.jsonl` (in the simulator, through the device runner on devhost, and ✅ on the
mixer, controller 32, 2026-11-13, the device and the simulator agreeing).

### Factory configuration (FR-MOD-06)

Layout C, decided by the owner on 2026-11-08: **a preset is an instrument, and its pages are groups
of that instrument's parameters.** Preset k (1–4) is on MIDI channel k, and every assignment on
the preset's channel, so changing the preset's channel moves all of it. Its 4 pages give 64
different CCs, 8 faders and 8 encoders per page:

| Page | Faders 1–8 | Encoders 1–8 |
|---|---|---|
| 1 | CC 20–27 | CC 102–109 |
| 2 | CC 12–19 | CC 110–117 |
| 3 | CC 70–77 | CC 78–83, 85, 86 |
| 4 | CC 87, 89–95 | CC 3, 9, 28–31, 118, 119 |

- These are all 64 numbers that are not service CCs:
  - controllers with no fixed meaning (3, 9, 14–15, 20–31, 85–87, 89–90, 102–119);
  - general purpose controllers (16–19, 80–83);
  - effect control (12–13), the sound controllers (70–79) and the effect depths (91–95).
- Never used:
  - bank select (0, 32), modulation (1);
  - breath, foot and portamento (2, 4, 5, 65, 84), data entry (6, 38, 96, 97);
  - volume (7), balance (8), pan (10), expression (11);
  - the LSBs (32–63), sustain and the other pedals (64–69);
  - the velocity prefix (88), (N)RPN (98–101), channel mode (120–127).

  `test_factory_layout` checks this.
- Faders: absolute CC, the global pickup mode (`pickup`). Encoders: absolute CC, `acc2`, starting
  from 64; Shift turns the same target in `fine` mode.
- Shift `hold`; notes released as `9n nn 00`.

**Buttons: keys** (decided 2026-11-08):

- All buttons send on the preset's channel, as keys: Note On with velocity 127 on press, the
  release (`9n nn 00`) on release.
- The strip buttons are 32 chromatic keys from note 48, the same on every page:

  | Row | Strips 1–8 |
  |---|---|
  | M | notes 72–79 |
  | S | notes 64–71 |
  | R | notes 56–63 |
  | □ | notes 48–55 |

  The □ row has the lowest notes; it is the bottom row of the strip buttons (✅ the owner's photo
  of the panel and the check by hand, 2026-11-11: top to bottom M, S, R, □).
- The bottom row (Play … ▶) goes on with notes 80–90.
- Toggle, momentary or trigger, and a note or a CC with its values, can be changed for each
  button in the preset ("Buttons" above).

The factory configuration has 428 targets: 64 CCs, 32 keys and 11 bottom-row notes per preset. The
device toolchain check (`check_device.sh`) builds it, since it is what the device will run. The
scenario `factory-layout.jsonl` walks through it.

### Test configuration

- bank (preset k, page p, both from 0) uses MIDI channel `4k + p + 1`, so the 16 banks never
  collide (fixed channels: the presets' own channel, 1, is not used);
- fader n: CC `20 + n`, pickup; encoder n: CC `102 + n`, `acc1`, Shift = `fine`;
- M n: note `24 + n` toggle, S n: note `32 + n` toggle, R n: note `40 + n` momentary, □ n: note
  `48 + n` momentary;
- the bottom row: notes 84–94 momentary on the preset's first channel (`4k + 1`);
- Shift `hold`, notes released as `9n nn 00`, as in the factory configuration.
- bank 16 (preset 4, page 4, channel 16) is the 14-bit bank, every range 0–16383: faders 1–6 NRPN
  256 + n (MSB 2, LSB n), fader 7 RPN 1, fader 8 CC 14-bit 27; encoder n NRPN 384 + n (MSB 3, LSB
  n), `acc1`, starting at 8192, Shift `fine`. No test before it used that bank.

## Simulator and scenarios

`firmware/sim/smcsim.c` is the core built for the host, on the test configuration (or the factory
one: `set config factory`; or any configuration's binary form, `smcsim --config FILE`, which then
takes no `set`: the editor's contract tests run it so, [integration.md](integration.md), T-1), with the host
smclog port and a virtual clock: the log's timestamps are the events' times, so a run is
reproducible to the byte. It reads one command per line on stdin (events with their times,
settings, step markers, state queries; listed at the top of the file) and prints the MIDI it sends
(`M t cause bytes`) and LED changes (`L t led state`). With `--log FILE` it writes the smclog
stream, a header and frames as the device sends them, decoded with the dictionary of the
`smcsim` binary itself.

`tools/smcsim/smcsim.py` (standard library only) runs scenario files through it, checks their
expectations and can write a session directory in the LOG-21 format, so `smclog.py` and later
analysis tools read the simulator's log like the device's (LOG-40).

```sh
make -C firmware sim                                       # builds build/host/smcsim
python3 -I tools/smcsim/smcsim.py run firmware/tests/scenarios/pickup.jsonl --session logs/sim/pickup
python3 -I tools/smcsim/smcsim.py run-all firmware/tests/scenarios   # make -C firmware test runs it (LOG-42)
python3 -I tools/smcsim/smcsim.py replay logs/sim/pickup   # the session's input through the simulator
```

### Scenario files

JSON Lines, one step per line; `#` lines are comments. Time is virtual: it starts at 0, and every
input step comes 10 ms after the previous one unless it says `"after_ms"`. Controls are named as
in the table above (`m1`, `shift`, `enc3`, `fader1`, …) or given by index. Pages and presets are
numbered from 1, as on the panel.

| Step | Options | What it does |
|---|---|---|
| `{"say": "…"}` | | a step for a person on the device; the simulator only prints it (`-v`) |
| `{"set": {"shift_mode": "hybrid"}}` | `config` (`test`, `factory`); per preset, as `NAME` (every preset) or `NAME_p1`…`NAME_p4` (one preset): `shift_mode` (`hold`, `latch`, `hybrid`), `note_off` (`9n`, `8n`), `channel` (1–16), `param_null` (`off`, `gesture`); global: `layer_mode`, `pickup`, `gesture_ms`, `hybrid_ms`, `enc_accel` (50–200), `fine14` (`range`, `lsb`) | settings; before any input. `config` replaces the whole configuration: the runner sends it first, and it is refused after another setting. The names and values are parsed by `firmware/core/core_settings.c`, the same code in the simulator and on the device |
| `{"press": "shift"}`, `{"release": "shift"}` | | |
| `{"tap": "up"}` | `"hold_ms"` (50) | press, then release |
| `{"turn": "enc3", "clicks": -5}` | `"every_ms"` (50) | one event per click |
| `{"fader": "fader1", "to": 100}` | `"from"`, `"steps"` (1), `"every_ms"` (10); `"to_pos"`/`"from_pos"` for raw positions 0–16383 | `to`/`from` are values on a 0–127 range |
| `{"midi_in": "B0 14 40"}` | `"transport"`: `usb` / `ble` | |
| `{"wait_ms": 400}` | | time passes, a tick every 10 ms |
| `{"select": {"preset": 2, "page": 3}}` | | remote selection (`core_select`); on the device a CFG_SELECT (P-3, [link.md](link.md), "The panel's state"), applied by the control task in its next round |
| `{"edit": {"preset": 1, "page": 2, "control": "fader1", "to": {"type": "cc7", "number": 30}}}` | `"to": {}` (the control sends nothing); `{"edit": {"global": {…}}}`, `{"edit": {"preset": 1, "settings": {…}}}` (settings as in a file) | a live edit (P-1): that control, those settings replaced whole, as `codec.py` writes their records ([config.md](config.md), "Edits"), applied between two events (smcsim's `edit` command; on the device a CFG_SET) |
| `{"expect_midi": ["B0 14 40", …]}` | | exactly these messages since the last `expect_midi` (`[]`: none) |
| `{"expect_led": "up", "state": "on"}` | `off`, `on`, `slow`, `fast` | |
| `{"expect_state": {"preset": 1, "page": 2, "shift": false}}` | any subset | |
| `{"expect_log": "regex"}` | `"module"` | a record since the last `expect_log` matches |

Any input step can carry `"injected": true`: the event is marked as injected (LOG-33) and the log
says so.

A failure names the file and line of the expectation, with what came and what was expected. A
scenario also fails if it ends with a note on (a Note On with no Note Off after it): on the device
the next scenario's set-up restarts the core without a Note Off, so the note would keep sounding in
the receiver (found 2026-11-12, "Encoders, 2026-11-12"). The corpus is `firmware/tests/scenarios/`; every feature comes with a scenario (LOG-31).

The same files run on the device (next section); `say` steps are for a person.

### Scenarios on the device (LOG-35)

`smcsim device` runs scenario files on the controller (the `app` variant) over the command link,
without a person:

```sh
python3 -I tools/smcsim/smcsim.py device firmware/tests/scenarios --port /dev/cu.usbmodemXXXX \
    --dict build/app/app.smclog.json [--session logs/device/run1] [--midi coremidi:SMC-Mixer]
```

For each scenario (`tools/smcsim/device.py`):

1. **Setup.** Injection on, then CORE_SETUP ([link.md](link.md)) restarts the core on the
   scenario's configuration and settings, in RAM only. A scenario without `config` means the test
   configuration, as in the simulator, so the runner always names it. The runner waits for the
   core's "core start" record.
2. **Play.** The input steps become INPUT_INJECT commands sent at their times, in real time (events
   at the same time go in one command). Meanwhile the device's log streams in: the runner sets
   `input` to DEBUG and `engine` to TRACE, so the core's input records, its MIDI out (`out …`), LED
   states (`led N = S`), and page and Shift changes all arrive. After the last step it waits
   300 ms (`--settle-ms`).
3. **Check.** The device's input records must be the scenario's inputs, one for one, all marked
   injected (real input during the run fails it; so does a log with lost records). The device's
   records are split into steps: an input record and everything its event caused (the same cause
   ID, LOG-04) belong to the input's step; records with no cause (ticks: blinking, timeouts) go
   to the first step that had not ended by their time. The scenario's expectations are then checked
   on those records with the simulator's own checker (`expect_state` from the `preset … page …` and
   `shift …` records). An `edit` step is a CFG_SET sent at its time; the runner waits for the
   core's "configuration edited", and what the device logged from the command until then belongs
   to the edit's step. A `select` step is a CFG_SELECT, the same way: the runner waits for the
   control task's "select asked: …" and the core's "preset P page G" after it.
4. **Agree.** The simulator gets the device's inputs at the times the device stamped them, and
   each edit at the time of the device's "edit: preset …" record (each selection at its "select
   asked" record's), with a common origin aligning the earliest recorded input, edit or selection
   to its scenario time. Every action retains its actual interval to every other action, including
   a delayed first input after an earlier selection/edit; no negative action time is clamped.
   Step boundaries follow the latest applied action. The simulator gets a
   tick every 10 ms like the control task. Its MIDI must equal the device's `out` records and its
   final LED states the device's: the core on the computer and on the device must agree.
5. **Back.** At the end the device goes back to the factory configuration, injection off, and its
   log levels as they were.

Reading a failure: if the device and the simulator agree but an expectation failed, the scenario
depends on timing the device did not meet, and the report says how far the inputs were off (the
host sends them over USB, a few ms late at most is normal). A disagreement is a bug in one of
the two. Every result line gives the inputs' largest offset from the scenario's times.

**What the host received** (`--midi SOURCE`, `tools/smcsim/midiin.py`): the MIDI that arrived on
the performance port from the core's start until the end of the settle time, channel messages in
order, must equal the device's `out` records (a message lost or changed on the way through the
MIDI queue, the USB task or USB itself fails the scenario; timing is not compared). Sources:

- `raw:PATH`: a raw MIDI byte stream, read in a thread: an ALSA rawmidi device on Linux
  (`/dev/snd/midiC1D0`), or a pipe or file (devhost's, below);
- `coremidi:NAME`: on macOS, the CoreMIDI source named NAME (`SMC-Mixer`, the performance port),
  through ctypes. ✅ Run on 2026-11-11 against the mixer: the corpus's 67 messages all arrived.
  The mixer offers two sources, `SMC-Mixer` (performance) and `SMC-Mixer Config`.

The result line then says "N MIDI messages, M received by the host".

`--session DIR` writes a directory per scenario: `meta.json` (build ID, settings), `device.bin` (the
raw stream), `device.jsonl` (the records from the core's start), `markers.jsonl` (each step's
host time), `result.json`, and with `--midi` also `midi.jsonl` (what the host received: `dir:
"rx"`, `src: "host"`, the host's monotonic time in ns).

Without hardware the runner is tested against `build/host/devhost` (`firmware/sim/devhost.c`): the
controller's portable code (link, injection, event queue, control path, core, MIDI queue,
CORE_SETUP) behind a pseudo-terminal, with the host's clock in microseconds like the device's.
`test_smcsim.py` runs the whole corpus through it (about 16 s), checks a session, a skipped
scenario and a refused hardware-test variant, and checks that a stand-in that ignores the settings
is caught as a disagreement. `device.analyse()` is also tested on records the simulator made with
other timing than the scenario's. With `--midi PATH` devhost writes the MIDI bytes of the packets
the USB task would send to PATH (a FIFO in the tests; opened when the first message goes out, and
again after the reader went away), and `--fault drop-midi` leaves out every third message: the
tests check that the runner's `--midi raw:FIFO` matches the first and catches the second, while
the core itself still agrees with the simulator.

`devhost` also writes the boot journal ([logging.md](logging.md), "Boot journal"). With
`--keep FILE` it keeps what the device keeps in RAM across a reset in a shared mapping of FILE;
SIGUSR1 stands in for a crash (a crash record, exit status 70), and the next run reports it. The
link's RESET restarts it in place (it executes itself again on the same terminal and keep file),
as a reset restarts the device; `firstrun.py`'s tests use that. Its log clock counts from its
start, as the device's does.

✅ On the mixer, 2026-11-11 (the controller's first image, build `0adc03b0`; sessions
`logs/device/2026-11-11-app1b` and `-app1-midi`): all 12 scenarios pass, the device and the
simulator agree, and with `--midi coremidi:SMC-Mixer` the host received every message (67). The
inputs arrived at most 4 ms off the scenario's times. The first try failed 10 of 12: a fader left
mid-travel jittered and its events counted as real input. Parking the faders at an end was the
workaround for that build. The filter/gate fix then passed three runs with the faders mid-travel
(controller 5, 2026-11-11; [hwtest.md](hwtest.md), "The faders measured"). Host scheduling jitter
remains a separate risk for the device runner.

### Sessions and replay

`run --session DIR` writes `meta.json` (simulator binary, build ID, configuration, settings),
`device.bin` (the raw stream), `device.jsonl` (decoded records; `t_host_ns` is the virtual time),
`midi.jsonl` (the MIDI out in `smcmidi`'s format, `dir: "rx"`, plus the cause ID), `markers.jsonl`
(the steps) and `result.json`.

`replay DIR` takes the input records of a session's device log (module `input`: buttons, encoder
clicks, fader positions, incoming MIDI, injected or not), runs them through the simulator with the
session's settings and compares the MIDI with the `out` records the session's core logged. With a
session recorded on the device this is LOG-41 at the core's level and the LOG-35 check that the
core agrees on both sides; it needs the device's `input` and `engine` modules at DEBUG. Raw input
(LOG-07: ADC values, matrix states) is recorded by the input layer ([hwtest.md](hwtest.md));
this core-level replay does not reconstruct debouncing/filtering from those raw samples.

## On the device

The application's `app` variant is the controller (`firmware/app/controller.c`): panel scan,
the core on the saved configuration (factory when none loads), USB MIDI and optional BLE MIDI.
The hardware-test variant has no core.

```
 TIMER3 interrupt (scan, prio 7) ──events──→ input queue (evq, 64) ──→ control task "smc_ctl"
 USB task: MIDI from the host ─────events──→        │                    ctl_run(): events → core
 command link: INPUT_INJECT ───────events──→        │                    tick, LED blinking
                                                     wake (semaphore)          │ MIDI      │ LEDs
                                                                               ▼           ▼
                       USB task: tud_midi_packet_write ←── MIDI queue (midiq, 64)    scan LED frame
                       (FIFO 16 packets)        └─ room again: wake ──→ control task
```

- **The control task** `smc_ctl` (priority 2, 768 words of stack) waits on a semaphore, at most one
  OS tick (10 ms). The scan interrupt queues its events (the faders' too: the ADC is read in that
  interrupt, [hwtest.md](hwtest.md)) and, since it may not call the RTOS, wakes the task through
  the software interrupt SOFT0, whose handler posts the semaphore (✅ on the mixer since controller
  4, 2026-11-11: until then it raised ILAT bit 4, which is not SOFT0's, and the task found the
  scan's events only at its 10 ms timeout, 5 ms late on average); MIDI in, injection and the
  command link post it directly. Each time the task runs it runs `ctl_run()`
  (`firmware/ctl/ctl.c`): every queued event into the core, a TICK, the LED blinking, and a
  wake-up for the USB task when MIDI was queued.
- **Time.** Events carry the log clock (µs) of the moment they were made. Events from different
  sources can reach the queue out of order by a few µs; `ctl` never gives the core an earlier time
  than the last one (it raises it and counts it, "times raised" in the statistics), so the core's
  timers only move forward. The µs clock wraps after 71 minutes; comparisons are by difference.
- **MIDI out.** The core's messages become USB-MIDI packets on the performance port and go into
  a collected unit, then the USB queue and, when enabled, the BLE queue ([usb.md](usb.md),
  "USB MIDI"; [ble.md](ble.md), "The route"). Each queue takes the collected unit whole or
  refuses it whole; backpressure reserves the full 38-message bound before invoking the core.
  Configuration cleanup uses bounded chunks before mutation/restart (R01). The core already logged each
  message with its cause, so `ctl` logs only the refusals.
- **MIDI in.** Channel messages from the host on the performance port become `CORE_EV_MIDI_IN`
  events (transport USB) and update the targets and LEDs (FR-IO-06).
- **LEDs.** The core reports a state per LED (off, on, slow, fast); `ctl` draws it into the scan's
  LED frame, only on change. Blinking: slow 125 ms on / 125 ms off, fast 50 / 50; a blink starts
  lit at the moment the state was set.
- **Injection** (LOG-33) puts events into the same queue, so the core cannot tell them from the
  panel (they are logged as injected by the injection code).
- **Setup** (LOG-35): while injection is on, CORE_SETUP ([link.md](link.md)) asks the control task
  to restart the core on a built-in configuration with settings (`ctl_request_setup()`); subsequent
  rounds release the old notes in bounded chunks, then apply it before queued events. The test
  configuration is linked in for that.
- **Statistics** every 10 s and on METRICS_NOW: the scan's, then "control: … rounds, … events (…
  injected, … bad), … times raised; MIDI … out, … in" and the MIDI queue's counters.
- **Metrics** (LOG-06) at the same times: the control task writes the heartbeat after its round
  ([logging.md](logging.md), "Metrics"). `ctl_run()` measures each event's wait (its stamp →
  taken by the core) and its own run time; the core's MIDI goes into the queue with the time of
  the event it answers, and the USB task measures input → USB when TinyUSB takes the packet.
- RAM: the configuration (24 KB since `number` became 16-bit for NRPN, 21 KB before) and the core
  (9.3 KB) are static; the application's `.bss` was 49 KB and the heap about 67 KB before
  `CORE_RAM` ([build.md](build.md)); `build_app.sh` prints the heap of each build (37 984 bytes for the
  controller on 2026-11-11, build `e0e720a7`: the core in RAM, NRPN, the configuration protocol's
  16 KB buffer and its flash storage).

**Latency.** ✅ On the mixer (controller 4, 2026-11-11, 74 events): input → core 0.45 ms at
most, input → USB 1 ms at most; the CPU load 9% over the session, 11% at most; the stacks keep
2.5 KB (control) and 1.6 KB (USB) free. The scan interrupt reads the faders itself, one
conversion at a time in turn ([hwtest.md](hwtest.md), "Fader ADC"): ✅ each fader every 1.62 ms.
⚠️ The fader filter then lags a moving fader by about 13 ms (hwtest.md, "The faders measured",
modelled, not measured on a move): above SPEC NFR-01's 3 ms for faders, the price of a fader quiet
at rest at the ADC's resolution.

**By hand** (controller 5, build `2ac736b6`, 2026-11-11, session `2026-11-11-app5-manual`, 22
minutes, the owner with ShowMidi and a synth): ✅ every strip button in the four rows (notes
48–90, 375 presses and 375 releases), the bottom row, the faders on page 1 (every value 0..127
on each of CC 20–27, pickup), the encoders with and without Shift, the pages and the presets with
Shift (pages 2–4 and channels 2–4 seen), the LEDs as the factory configuration says. The 18 765
messages the core sent (its `out` records) and the 18 765 the host received through CoreMIDI
are the same sequence; no log record lost while subscribed, no queue refused. The owner's
impression of the feel: fine; to be judged again with musical material once the firmware is
configurable. ⚠️ One 10-second window had a latency tail: four faders swept fast together
(about 1 900 events a second for 0.24 s) with the input log at DEBUG and the engine at TRACE;
the input queue held up to 55 events, input → core reached 24 ms and input → USB 26 ms (109
events over 10 ms); the rest of the session stayed under 10 ms at p99 5 ms.

**Fast sweeps** (controller 5, every module at INFO, 2026-11-11, session `2026-11-11-app5-sweep`,
the owner sweeping four, then all eight faders with the palm): ⚠️ the log is not the cause. In
the 10-second windows of the sweeps input → core reached 30 ms and input → USB 40 ms (p50 up to
40 ms in the two busiest windows), the input queue (64 events) filled and refused 2 769 fader
events; the scan offers a refused fader again with its newest value, so the moves thinned out
and every fader still ended on its last value (0; the 11 259 messages sent were all received).
The CPU stayed about 76% idle over each window, so the control task keeps up on average and
falls behind only during a sweep: it spent about 0.3 ms per fader event (148 per mille for
4 402 events in a window; in the verbose run the core's records came every 0.5 ms while it
drained), and eight moving faders bring up to about 2 000 events a second. Where those 0.3 ms
go (the core, the code's fetch from flash, other tasks in between) was measured next.

**Where the time goes** (controllers 6, 7 and 8, 2026-11-11, the same sweeps; sessions
`2026-11-11-app6-sweep`, `-app7-sweep`, `-app8-sweep`). The control task logs "control timing"
(`core_event()` per queued event, the fader events apart, the MIDI queue and LED calls),
"control rounds" (events and time of the busiest round, the tick and blinking) and "control
wake" (the scan's wake → the round) at each heartbeat; the scan's "wake:" line adds the wake
handler's time. Two build switches: `CORE_RAM` puts the control path's functions (`SMC_HOT`,
smc_ram.h) in RAM, about 7 KB of the heap; on by default since controller 12, `CORE_RAM=0` leaves
them in flash. `CTL_PROBE=1`, for measurements only, runs every 32nd event through the core with
interrupts off and logs "control probe". The objects do not depend on the switches: build with
`-B` when one changes (`tools/env/build_app.sh -B CTL_PROBE=1`, and `-B` again without it).

| Build | Core per fader event, slow moves | the same, sweeps | Core alone (interrupts off) |
|---|---|---|---|
| 6, from flash | 92–163 µs | 260–300 µs, at most 1.6 ms | |
| 7, `CORE_RAM=1` | 40–100 µs | 230–280 µs | |
| 8, `CTL_PROBE=1` | 123–134 µs | 250–300 µs | 34–42 µs, at most 87 µs |

The same core on the Mac takes 40 ns per fader event (a host benchmark of `core_event()` with
the factory configuration). ✅ On the mixer the core itself takes about 40 µs from flash (about
3 800 CPU cycles at 96 MHz); the rest of the 250–300 µs a fader event costs the control task
during a sweep is time taken from it by what runs above it: the scan interrupt (about 20% of
the CPU at all times), the wake interrupt (17 µs per event on average, at most 108 µs: one per
event, 7 634 in the busiest window), the USB task and its interrupt (77 per mille of the CPU for
5 717 MIDI packets in that window, about 130 µs per packet). The MIDI queue and the LED calls
cost little (34 ms in that window), and the control task starts 70 µs after its wake on average
when events are rare. So the cost grows with the number of events and of MIDI packets, and a
sweep of eight faders (about 2 000 events a second at the ADC's full resolution, most of them
without a new CC value) asks more than the CPU gives the control task. ⚠️ Which of these takes
most is inferred from the window totals, not measured per event.

At power-on the first eight fader events' MIDI is dropped (8 "dropped" in the first window): the
host has not configured USB yet.

**Fader events merged.** Since controller 9 (2026-11-11) the controller's input queue
(`input/evq.c`, `evq_set_merge()`) merges a fader event from the panel into that fader's event
still waiting there: the waiting event keeps its place and takes the newest position, so the
queue holds at most one waiting event per fader and the core takes the newest position. Since
controller 19 it takes the newest position's stamp too (it kept the first one's before): the
position sent is the newest, so input → core and input → USB count from it; the older positions
are never sent. A stamp newer than the events queued behind it makes those raised to it ("times
raised" in the control statistics), by a few ms at most.
Any other event (a button, an encoder step, MIDI in) and every injected event end merging for
the fader events queued before them, so a position never moves across a page, preset or Shift
change. The scan wakes the control task only when its event found the queue empty
(`controller.c`, `sink()`): the task takes every queued event before it waits again. The
hardware test does not merge. The statistics add "input queue: … queued, … merged, … refused".

✅ The same sweeps on controller 9 (session `2026-11-11-app9-sweep`, 38 016 events): the input
queue held 8 events at most and refused none (controller 8: 63 and 2 583), input → core 3.5 ms
at most, p99 3 ms (was 30 ms); 3 344 fader events merged in the busiest windows, wakes about half
the events during sweeps. ⚠️ Input → USB still reached 12.9 ms, p99 10 ms, above NFR-01's 5 ms:
the MIDI packets now wait in the MIDI out queue (36 at most), and the USB task takes about 250 µs
of its time per packet (248 per mille for 9 383 packets in the busiest window, preemption
included). The USB side came next (the USB path's locks, below).

**The USB path's locks** (controller 10, the owner's option c1). The USB interrupt's enable bit is
written under our interrupt lock (`irq_br25_enable()`, irq_br25.h) instead of the SDK's
`bit_set_ie()`/`bit_clr_ie()`, which take the ROM's patched lock (TinyUSB masks the USB interrupt
at every event of its queue), and the USB task's metrics use our lock too. Its heartbeat adds
"USB task: … runs, avg … max … us; interrupt: …, avg … max … us; … packets to the host". ✅
Measured (session `2026-11-11-app10-sweep`): the USB interrupt takes 37 µs on average, about one
per USB packet, and a USB packet carries about two MIDI messages in sweeps: USB transfers cost
about 1% of the CPU. Input → USB did not change (p99 10 ms, 12.3 ms at most).

**Task priorities.** The USB task and the control task both had priority 2, and the RTOS shares
equal priorities in 10 ms slices: while the control task drained a sweep (rounds of up to 580
events and 177 ms), the MIDI packets it queued waited for the USB task's next slice. Since
controller 11 the USB task has priority 3 (`app_main.c`; the SDK's `btstack` has 3 too, not
running yet). ✅ Session `2026-11-11-app11-sweep`: sweeps of four faders now stay within NFR-01
(input → USB p99 5 ms, 6.2 ms at most; was p99 10 ms); a palm sweep of all eight (18 616 events
and 12 427 MIDI messages in one 10-second window, the control task 535 per mille) still reached
13.8 ms, with input → core 4.3 ms at most: the CPU is short during such a sweep. ⚠️ Where the
control task's time per event goes beyond the core's 40 µs is not accounted for event by
event.

**The CPU clock.** Since controller 15 the CPU runs at 192 MHz by default (`SYS_CLOCK_MHZ`,
`firmware/app/Makefile`: `clk_set("sys", …)` in `app_main()` before the scan starts; the board
still boots at 96 MHz), with the fader ADC's divider at /128 (`ADC_DIV`, below). Controller 12
was the first at 192 MHz, the clock the stock firmware runs at
([hardware.md](hardware.md), "Stock firmware internals", ⚠️ static analysis), with the core in
RAM. ✅ Session `2026-11-11-app12-sweep` (the same sweeps, the busiest 10 s window 29 488 fader
events, more than controller 11's 18 616): input → USB p50 0.5 ms, p99 2 ms, 3.0 ms at most (was
p99 13.8 ms); input → core 0.55 ms at most; the core 83 µs per event in that window (was 263 µs);
the input queue at most 4, nothing merged or refused; no scan step skipped (was about 44 000 in
the session); the scan 106 per mille of the CPU at most (was 201), the CPU load 469 per mille at
most (was 610), 74 over the session. The timer stays at 24 MHz, so the scan's period and the µs
clock do not change. Clocks that follow the system clock: SPI2's source 64 MHz (the divider is
computed from it: still 2 MHz) and the ADC's 64 MHz (was 32), so a conversion takes half as
long: each fader is read every 1.19 ms at the slowest (was 1.63).

The ADC at that clock (✅ sessions `2026-11-11-app12-rawfaders-mid`, `-app13-`, `-app14-`: raw
fader lines for 60 s, the faders still and placed the same, two at the top, two at the bottom,
four mid-travel; one of them at code 501, where the ADC's mid-scale wrong conversions show):
at 96 MHz (controller 13, ADC 32 MHz / 96) 1.5% of that fader's samples read 512; at 192 MHz
with the same /96 (controller 12, 64 MHz / 96) 21%. The median and the gate kept them from making
events, but a fader moving through that range would have been rougher. With /128 (controller 14,
500 kHz: `ADC_DIV=7`; ⚠️ the field's codes read from the SDK's `get_adc_div()` table) none of
them read 512, σ 1.2 instead of 3.9 on that fader, the others as at 96 MHz; a fader is read every
1.27 ms. The current from USB (the owner's USB meter, 5 V, LEDs as after start, the faders still):
0.25 A at 96 MHz (controller 13), 0.27 A at 192 MHz (controller 12), 0.19 A later at 192 MHz
(controller 14, at rest, no capture running). ⚠️ The readings spread more than the clock could
account for (the log stream, what the host does on the USB, the meter's 10 mA steps): the clock's
own cost is at most about 20 mA, 0.1 W, and not measured more closely. After the sweeps the same
meter read 0.06 A at 5.22 V on controller 15 (192 MHz, running; the bottom row's LEDs, BT and
Shift lit dimly). ⚠️ Most likely the earlier 0.19–0.27 A included the battery's charging, which
does not depend on the firmware ([hardware.md](hardware.md): the chip's charger is off, a separate
one charges), and the battery was full by then; so those readings do not compare the clocks. The stock's current was not measured (its
image is not on the mixer).

✅ Controller 15, the defaults (192 MHz, ADC /128, the core in RAM, with the encoder acceleration),
session `2026-11-11-app15-sweep2` (the owner's sweeps of four, then eight faders, and the
encoders): 81 770 input events, 63 965 MIDI messages; input → USB p50 0.5 ms, p99 2 ms, 2.5 ms at
most; input → core 1.3 ms at most; queues at most 5 and 16, nothing merged, refused or dropped;
the CPU load 55% at most in a window, the control task 51%; one scan step skipped in 193 s.

✅ Controller 19 (`302fbbfa`, with the MIDI queue's backpressure), the same factory configuration,
session `2026-11-12-factory-sweep-c19` (the owner's eight faders at once for about 15 s): 54 091
input events, 38 965 MIDI messages; input → USB p50 ≤ 0.5 ms, p99 ≤ 1.7 ms, 1.7 ms at most;
input → core 0.9 ms at most; queues at most 4 and 9, nothing merged, refused or dropped; 481 rounds
held for the MIDI queue (busiest window: 41 003 events, 30 216 messages, about 3 a millisecond);
the control task 62% at most in a window. The host received every message sent while it listened
(38 938; the other 27 went out before the listener started).

**The MIDI queue under load** (controller 19, the owner's decision of 2026-11-11 after the NRPN
sweep: [The run of 2026-11-11](#the-run-of-2026-11-11); built as `302fbbfa`, `make device`
passes, heap 37 536 bytes. ✅ Runs on the mixer (2026-11-12): enumerates, `info` shows
`302fbbfa`, the boot journal is clean, `metrics --check` ok. The first install (2026-11-11) never
came up on USB, and neither did seven bisect images, one of them controller 18's own code with only
the build ID changed: every one was planned without `cfg_tool.bin` (`installplan.py --cfg` left
out), so each start reset in the SDK's first `syscfg_read()` (bringup.md, "Step 4: boot marks",
run 3). Planned with it, the same `302fbbfa` came up at once; `installplan.py` now requires
`--cfg` ([image.md](image.md), "Installing"). The eight-fader NRPN sweep on it: ✅ "The run of
2026-11-12", below.) Eight NRPN faders swept at once ask for more messages than the USB carries (about 11
a millisecond measured on controller 18), and the MIDI out queue refused them one by one,
splitting values. Three changes (`ctl/ctl.c`, `midi/midiq.c`, `input/evq.c`,
`app/usb/tusb_config.h`, `app/usb/usb_app.c`):

1. **Whole or nothing at queue admission.** Up to `CTL_UNIT_MAX` (38) messages are collected and
   go into the queue as one unit (`midiq_push_unit()`), all or none. A unit that does not fit is
   refused whole and counted; backpressure prevents that for the derived bound. `timers()` runs
   before input: eight faders plus eight encoders can emit two null messages each (32), then the
   input can emit four NRPN/RPN selection/data messages plus two for an immediate-layer channel
   change or a button release. This conservative sum is 38; a tick needs at most 32, navigation
   and `core_select()` emit none. USB's low mark 8 plus 38 fits its 63 usable slots; BLE reserves
   38 free slots too (including when a pending BT long press can enable it). Static assertions
   guard both capacities. NRPN/RPN groups and null order are unchanged.
   Upload, Revert, CORE_SETUP, preset edits and multi-control edits release at most 38 notes in
   a round, after checking room in both active transports. The existing bitmap retains unsent
   notes; no configuration mutation, selection, event or timer runs until cleanup finishes.
   Each chunk is flushed before restart, so `core_init()` cannot erase unqueued output.
   An exactly full chunk conservatively needs another round to confirm completion.
2. **The newest value wins.** A round takes the next event, or the tick, only while at most
   `CTL_MIDI_LOW` (8) packets wait; otherwise it stops ("held"), asks the queue for room
   (`midiq_want_room()`) and the USB task wakes the control task once 8 or fewer wait
   (`midiq_room_due()`, `usb_app_midi.room`; the 10 ms wait still ends a round otherwise).
   Meanwhile the events stay in the input queue, where a fader's waiting event takes each newer
   position ("Fader events merged", above): what the USB cannot carry is the stale positions, per
   fader, never a part of a value. It is not a deadband (Q-21 d-1): every position the scan
   reports goes out when the USB keeps up. Buttons, encoder steps and MIDI in wait in order too
   (64 events; a full input queue refuses and counts, the scan offers a refused fader again). A
   held tick runs in the next round (a gesture's end, its null and the blinking a little later).
3. **Short queues.** TinyUSB's MIDI transmit FIFO is 16 packets (was 64): behind the USB packet on
   its way wait at most 16 packets there and about 8 + one unit in ours, about 40 messages, ⚠️
   about 3.5 ms at the 11 a millisecond measured (the 128 before were the 11 ms seen).

The statistics add "… rounds held" to "MIDI out queue:", and "control rounds:" each heartbeat
"… held for the MIDI queue". ✅ On the mixer the eight-fader NRPN sweep meets NFR-01 with nothing
split ("The run of 2026-11-12", below); the factory configuration's sweep too (above, after
controller 15's).

Tests on the host: `firmware/tests/test_ctl.c` (the control path with the factory configuration:
LEDs drawn at start, events to USB-MIDI packets, one wake-up per round, MIDI in to LEDs, the fast
blink's phases, time that never goes back, backpressure and the newest value winning, setup, the
metrics' window) and `firmware/tests/test_midi.c` (packets, the queue, units and backpressure).

## Size on the device

`tools/env/check_device.sh` builds the core with the JieLi toolchain (`-Oz`, `-Werror`) into a
check image with smclog and the factory configuration, and reports (2026-11-11):

| What | Size |
|---|---|
| `core.c` code + read-only data | 6.0 KB |
| `struct core` (RAM), of it the target store | 9.3 KB, 8 KB (1024 × 8 bytes) |
| `struct core_config`, decoded whole (generated from the schema, D-4) | 24 KB (14 bytes an assignment) |
| `core_config_factory.c` code + read-only data | 0.5 KB |

The current controller keeps the whole decoded configuration in RAM. Keeping only the current
preset decoded, or reading a compact flash representation, is a possible future RAM saving,
not an implemented stage 3–4 feature (D-4, Q-3). The store's 1024 entries cover the 812 targets of
the test configuration (the factory one has 428).

## Encoder acceleration

**Status:** implemented on 2026-11-11; ✅ checked on the mixer on 2026-11-12 with 14-bit NRPN
and 7-bit CC targets (controller 19, "On the hardware" below). `acc3` and steady-speed turns
for the full range remain unmeasured. The
encoders accelerate for CC and NRPN, 7-bit and 14-bit: a slow turn is precise, a fast turn covers
the range in about a turn. The owner's choices (Q-20, decided 2026-11-11): (a) a slow click on a
14-bit target is the range / 2048, (b) `fine` is the same step, (c) the curves below, (d) constants
in the firmware plus two global settings.

### The algorithm (`firmware/core/core.c`, `on_encoder`)

Everything is integer, from the event's clicks and timestamp (D-2, D-3). Per click:

1. **The fine step `u`**: 1 for 7-bit targets and relative types; for 14-bit targets the range /
   2048, at least 1 (7 on 0–16383), or 1 with `global.fine14 = lsb`.
2. **The speed index `s`**: `ticks = (time per click) × 2/3 / 156.25 µs × 100 / encAccel`,
   `s = 255 − ticks`, held within 0–247 (0 when a click takes about 60 ms or more at `encAccel` 100). The
   time per click is the time since the encoder's last event divided by the event's clicks; the
   first click of a gesture is not accelerated.
3. **The level `k`** (1 = no acceleration), by the mode's curve:
   - 7-bit targets and relative types: `s` is raised to at least 39; `acc3`: `s / 39`; `acc2`: 1
     while `s < 100`, else `(s − 100) / 39`; `acc1`: 1 while `s < 140`, else `(s − 140) / 39`;
     held within 1–6.
   - 14-bit targets: the base is `s`, or `s / ⌊4095 / span⌋` when the span `hi − lo` is below 4095;
     `acc1`: base / 2, `acc2`: base, `acc3`: base × 4, at least 1.
4. **The step per click**: `u + 1.5 × (k − 1)`. Steps are kept in Q8: the fraction carries to the
   next click (2.5 gives 2, 3, 2, 3), and is cleared at a gesture's start and on a reversal.
5. `acc0` is `u` at every speed; `fine` (the default Shift layer) is `u`; `div` and `step` are
   unchanged.

The 7-bit bands at `encAccel` 100, time per click for each level:

| Mode | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|
| `acc1` | < 8.9 ms | | | | |
| `acc2` | < 18.3 ms | < 9.1 ms | | | |
| `acc3` | < 41.7 ms | < 32.6 ms | < 23.4 ms | < 14.3 ms | < 5.2 ms |

On 14-bit targets the level grows with every step of `s`, so acceleration starts as soon as a click
takes less than about 60 ms.

Turns for the whole range at a steady speed (48 clicks a turn), from a model of the formulas above
(the unit tests check the core):

| Range, mode | 0.25 turn/s | 0.5 | 1 | 2 | 3 |
|---|---|---|---|---|---|
| 0–127, `acc0` | 2.65 | 2.65 | 2.65 | 2.65 | 2.65 |
| 0–127, `acc1` | 2.65 | 2.65 | 2.65 | 2.65 | 1.08 |
| 0–127, `acc2` | 2.65 | 2.65 | 2.65 | 1.08 | 0.69 |
| 0–127, `acc3` | 2.65 | 1.08 | 0.50 | 0.40 | 0.40 |
| 0–16383, `acc0` | 48.8 | 48.8 | 48.8 | 48.8 | 48.8 |
| 0–16383, `acc1` | 48.8 | 5.35 | 2.65 | 2.12 | 1.98 |
| 0–16383, `acc2` | 48.8 | 2.81 | 1.35 | 1.08 | 1.02 |
| 0–16383, `acc3` | 48.8 | 0.75 | 0.38 | 0.29 | 0.29 |

With `fine14 = lsb` the 14-bit slow column becomes 341 turns.

**Configuration** (global, `config/schema.json`): `encAccel` (50–200 %, default 100) scales the
speed, so a value above 100 accelerates sooner; `fine14` (`range`, default, or `lsb`). Both are also
settings by name (`enc_accel`, `fine14`, `core_settings.c`) for scenarios and the device. The
curves are constants in `core.c` (Q-20 d).

**State**: `struct core_encoder` keeps the Q8 fraction in the divider's remainder and the last
direction (`dir`).

**Tests**: `test_core.c` (`test_encoder_acceleration_and_shift`, `test_encoder_14bit_and_fine`,
`test_encoder_14bit_acceleration`, `test_encoder_speed_scale`) and the scenarios
`encoder-acceleration.jsonl`, `encoders.jsonl`, `gesture-layer.jsonl`. The scenarios set
`enc_accel` to 200, which puts acc1's top step at any click under about 17 ms and its first step at
any click slower than that. Fast clicks at 5–10 ms and slow ones at 60 ms or more then stay in their
bands even when the device runs them a few ms off (`smcsim device`).

**On the hardware** (controller 19, 2026-11-12, a 14-bit target: NRPN 384–389 on bank 16, `acc1`;
"NRPN and RPN", "Encoders, 2026-11-12"): ✅ 4 581 encoder events, every one a single click, each
paired with its engine step. The step per click, grouped by the time per click from the log's
times, matches the formulas above in every band: 7 at 60 ms or more, 18.8 on average at 50–60 ms,
87.7 at 30–40 ms, 152.7 at 10–20 ms, 190 under 2 ms (the formula's averages: 7, 18.7, 87.7, 152.7,
190). The first click of all 63 gestures moved 7; the Shift layer (`fine`) moved 7 on all 1 128
clicks. A reversal clears only the Q8 fraction, as stated above: of 35 reversals inside a gesture,
20 moved 7 and 15 (quick back-and-forth, under 60 ms) moved 10–50, by their speed; the owner
keeps it so (2026-11-12: a fast turn back may change the value fast too). The top step,
190, crosses the range in about 86 clicks, 1.8 turns at 48 clicks a turn (the table's 1.98 at 3
turns a second). ✅ 7-bit targets the same day (CC 102–104 on channel 1, preset 1 page 1; sessions
`logs/sessions/2026-11-12-enc7bit-acc2-c19`, the factory configuration, `acc2`, and
`…-enc7bit-acc1-c19`, `setup config=test`, `acc1`): 1 014 and 1 165 encoder events, every one with
its engine step; the messages sent and received the same, 710 and 961; `metrics --check` ok
(input → USB 0.65 ms at most). The step per click falls in the table's bands: `acc2` 1 slower
than 18.3 ms, 2 and 3 in turn (2.5) at 9.1–18.3 ms, 4 under 9.1 ms; `acc1` 1 slower than 8.9 ms,
2 and 3 in turn under it; the first click of a gesture 1; 0 and 127 hold. The top steps cross
0–127 in 32 and about 51 clicks, 0.67 and 1.06 turns (the table's 0.69 and 1.08). ⚠️ Not
checked: `acc3`, and the turns for the range at a measured steady speed (the knobs have no
detents; the owner turned by feel, the times per click come from the log).

## NRPN and RPN

**Status (2026-11-12): implemented in the core, host tests pass; on the mixer ✅ the framing, the
null and pickup by hand (controller 18), ✅ eight NRPN faders swept at once on controller 19: NFR-01
met, no value split ("The run of 2026-11-12", below; on controller 18 it failed: "The run of
2026-11-11").** Faders and
encoders send NRPN and RPN, buttons NRPN, at 14 bits (FR-CTL-01). A fader's 14-bit position maps
onto `lo`…`hi` like any 14-bit type's ([Faders](#faders-fr-ctl-03)), so on the full range every
position the scan reports is one value: the owner rejected thinning out fader events because NRPN
and 14-bit targets need every fine step (2026-11-11). The framing below is the owner's decision of
2026-11-11 (SPEC Q-21: a-1, b-1, c-3, d-1).

### What goes out (`firmware/core/core.c`, `send_param`)

A value V of NRPN parameter P on channel n, MSB first in both pairs:

| Message | Bytes | When |
|---|---|---|
| parameter number MSB | `Bn 63 P>>7` | the channel's selection is not P (below) |
| parameter number LSB | `Bn 62 P&127` | with it |
| data entry MSB | `Bn 06 V>>7` | every value |
| data entry LSB | `Bn 26 V&127` | every value (Q-21 a-1) |

RPN: CC 101/100 (`65`/`64`) instead of 99/98. In a configuration P is one number, 0–16383
([config.md](config.md), "NRPN and RPN"). A receiver that applies a value on the MSB gets the coarse
value at once and the fine one right after; one that waits for the LSB gets both.

### The channel's selection

The core remembers, per channel, the parameter its output last selected (`param_out`), and sends
the parameter number only when it has to (Q-21 b-1):

- when the channel's selection is another parameter: two NRPN faders on one channel moved in turn
  select their parameter each time, 4 messages a value;
- at the first value of each fader or encoder gesture, and at every button press, so a receiver that
  lost the selection (restarted, or another controller on its input) has it again at the next touch;
- after a plain CC 98–101 we sent on that channel (a 7-bit assignment on those numbers).

Within a gesture on one parameter a value is 2 messages.

**The null parameter** (Q-21 c-3): a preset's `paramNull` (`off`, the default, or `gesture`) says
whether a gesture that sent NRPN or RPN ends with RPN 127/127 (`Bn 65 7F`, `Bn 64 7F`), after which
data entry from elsewhere changes nothing. A fader's or an encoder's gesture ends after the pause
(`gestureMs`, 300 ms), a button's at its release. One null per channel: two gestures that end
together send it once, and the next value selects its parameter again. A gesture that sent nothing
(a fader waiting for pickup) or only other types sends none. The preset is the one the value was
sent in; a gesture whose values moved to another channel (Shift in `immediate` mode) sends the old
channel's null at once.

### What comes in (`remote_param`)

For each transport (USB, BLE) and channel the core follows the sender's selection: CC 99/98 and
101/100 set the halves of an NRPN or RPN number (a half survives only within the same kind). Data
entry goes into the selected parameter's target: CC 6 sets it to MSB × 128, like a CC 14-bit MSB;
CC 38 replaces its low 7 bits if it has a value. Data after the null parameter (127/127) goes
nowhere; increment and decrement (CC 96/97) are not followed. So a fader in `pickup` waits for a
value an instrument reports by NRPN as for any other (scenario `nrpn.jsonl`).

### Load, and the router

Eight faders on NRPN targets of one channel swept at once send up to 4 messages per fader event,
CC 14-bit 2, CC 7-bit 1. ✅ Measured on controller 18 (below): about 3.7 messages per fader event
were asked for, and the USB MIDI path did not keep up (❌ NFR-01). Controller 19's repeat passed
(below). BLE's factory-control sweep was measured on controller 22 ([ble.md](ble.md), "The route
on the mixer"); that is not a separate eight-NRPN-fader BLE load measurement.
There is no deadband on 14-bit targets (Q-21 d-1): stale values are the router's to drop.

The USB path uses whole-unit queue admission and input backpressure; the BLE queue additionally
merges stale continuous values with their newer ones (FR-IO-08; [ble.md](ble.md), "The route").
NRPN selection/data groups must remain whole: dropping a stale CC 6 and keeping its CC 38, or a
99/98 pair without its data, sets the wrong value or parameter. The collection-overflow defect
(R01) above is corrected with bounded collection and separate cleanup continuation.

### Tests, and on the hardware

`test_core.c`: `test_fader_nrpn`, `test_nrpn_encoders_and_buttons`, `test_nrpn_null`,
`test_fader_14bit`; the scenarios `nrpn.jsonl` and `nrpn-null.jsonl` on bank 16 of the test
configuration; the codecs' tests take the 2-byte `number`. The device builds pass with it
(`make device`, 2026-11-11): the rampath and call-site checks hold, `core.c` 6.0 KB, `struct
core_config` 24 384 bytes; the controller's heap with the configuration protocol and its storage
added: 37 984 bytes (`make check`, 2026-11-11, build `e0e720a7`).

### The hardware check (ran 2026-11-11 on controller 18; needs the owner's go-ahead in that session)

The factory configuration has no NRPN, so the check uses the test configuration's bank 16 (preset
4, page 4, channel 16: faders 1–6 NRPN 256 + n, fader 7 RPN 1, fader 8 CC 14-bit 27, encoders NRPN
384 + n), loaded in RAM with CORE_SETUP. This is the historical controller-18 procedure, before
panel-state persistence. CORE_SETUP configurations are never saved; current firmware can still
save panel navigation/Bluetooth state after a quiet pause. Do not equate a RAM-only setup with
a guarantee of no flash writes ([config.md](config.md), "The panel's state"). In order:

1. **Build and install** the controller (`tools/env/build_app.sh`, then the usual installation,
   [bringup.md](bringup.md)); make the dictionary from that build's `build/app/app.elf`. Another
   session's build can rewrite `build/app` at the same time, so check the build ID (`smclog.py cmd
   … info`) against the image installed.
2. **Scenarios, no hands**: `smcsim device` (above) with `--midi coremidi:SMC-Mixer`, on the corpus
   or on `nrpn.jsonl` and `nrpn-null.jsonl` alone. Pass: the device agrees with the simulator and
   the host receives every message. These check the framing and the null on the device, not the
   faders.
3. **By hand, with a MIDI monitor and an instrument that takes NRPN** (the owner): put the test
   configuration in with
   `smclog.py cmd --port … --dict … inject on`, then `… setup config=test param_null_p4=gesture`;
   Shift + » then Shift + ▶ (preset 4, page 4). Then:
   - one fader slowly across the whole range: the monitor shows `BF 63 02`, `BF 62 0n` once, then
     only `BF 06`/`BF 26` pairs whose 14-bit value never jumps more than a few steps (the scan's
     resolution, NFR-04) and covers 0 and 16383;
   - let it rest 300 ms: `BF 65 7F`, `BF 64 7F`; touch it again: the parameter selected first;
   - restart (or reload) the instrument mid-session, touch the fader: it follows again;
   - the instrument's own NRPN for parameter 256 + n (if it sends any) makes that fader wait (the
     encoder LED blinks) until it is picked up;
   - with a capture running (`smclog.py capture … --session logs/sessions/…-nrpn --midi SMC-Mixer`,
     [logging.md](logging.md)), sweep all eight faders at once for 10 s, then `smclog.py metrics
     --check SESSION`: input → USB within NFR-01 (p99 ≤ 5 ms) at up to 4 messages per event;
     nothing refused in the MIDI queue (`ctl` stats).
   Afterwards `… setup config=factory`, `… inject off` (or a restart) puts the factory
   configuration back.
4. **Record** the run below and replace "not run on hardware" here. If the sweep misses NFR-01, the owner decided against a deadband (Q-21 d-1):
   look at the MIDI path first ("Where the time goes").

### The run of 2026-11-11

Controller 18 (build `e0e720a7`), steps 1 and 2 passed. Step 3 by the owner with a MIDI monitor that sends nothing back; the test
configuration in RAM (`setup config=test param_null_p4=gesture`), nothing pushed or saved;
afterwards `setup config=factory`, `inject off`. The MIDI was read from the sessions' `midi.jsonl`
(CoreMIDI, `smcmidi listen`).

- ✅ **Slow sweeps of fader 1** (NRPN 256; session `logs/sessions/2026-11-11-nrpn-manual`, 7
  gestures, 10 196 messages): each gesture starts with `BF 63 02`, `BF 62 00` once, then only
  `BF 06`/`BF 26` pairs; no framing fault; the values reach 0 and 16383. In the slow sweeps (20 s
  and 11 s end to end) a step is at most 32 (2 ADC counts of 1023) except at the ends: leaving an
  end jumps about 160 (twice the hysteresis) and the snap to an end about 90, both the scan's
  design ([hwtest.md](hwtest.md), "Faders"). Fast sweeps (1 s): steps up to 41 (merged events).
- ✅ **The null**: `BF 65 7F`, `BF 64 7F` 301–309 ms after a gesture's last value, every gesture;
  the next touch selects the parameter again.
- ✅ **Pickup on incoming NRPN** (session `2026-11-11-nrpn-pickup`): with NRPN 256 = 8192 injected
  as if from an instrument (`inject midi:usb:BF,63,02 …`), fader 1 moved up from the bottom sent
  nothing until its first value 8071, within the window of 128 around the target; the encoder's
  LED blinked meanwhile (the owner).
- ⚠️ **An instrument restarted mid-session**: not run (no instrument that takes NRPN 256–261 on
  hand). What makes it work is shown: the parameter number goes out again at each gesture's start.
- ❌ **Eight faders swept at once** (session `2026-11-11-nrpn-sweep`, the owner's palm on all
  eight for about 10 s, some encoders touched too): `smclog.py metrics --check` fails NFR-01.
  Busiest 10 s window: 17 427 fader events, 64 664 messages asked for (3.7 an event), 47 309
  sent, **17 355 refused by the MIDI out queue** (27%; 38 615 in the session); input → USB p50
  1 ms, p99 11.3 ms, 11.3 ms at most; input → core 2.8 ms at most; the input queue 8 at most,
  nothing refused there; the CPU 46% busy at most, the control task 42%, the USB task 6%. The
  busiest second carried 11 334 messages to the host.
- ❌ **The refusals split values.** The queue refuses message by message, so the received stream
  has 159 `BF 63` without their `BF 62`, 17 `BF 65` without `BF 64`, 311 `BF 06` without
  `BF 26`, 3 `BF 26` without `BF 06`, and 9 CC 27 without CC 59. A receiver that takes a lone
  `BF 63` keeps the old LSB and puts the next data into another fader's parameter. The faders'
  last values arrived (the owner checked faders 3 and 5, which stopped above the bottom), ⚠️ by
  luck: a refused last value would leave a stale one.

**Where the time went** (⚠️ inferred from the window totals). The queues hold about 128 messages
(the MIDI out queue 64, TinyUSB's MIDI transmit FIFO 256 bytes = 64), and the host took at most
about 11 messages a millisecond (11 144 USB packets in the window, about one a frame, 4.2
messages each on average): 128 messages at that rate are the 11 ms seen at p99. The sweep asked
for more than the USB carried, so the queues stayed full (63 waiting at most) and the rest was
refused. The core and the input path kept up (input → core under 3 ms). So two things for the
owner to decide: a value's messages must be queued whole or
not at all (the router's rule above), and when the USB falls behind, what goes: the oldest values
of a parameter whose newer value waits (the router's "last value wins", FR-IO-08, not a deadband)
or new values as now, and how deep the queues may be for NFR-01. The owner decided the same day:
all three, as proposed; done in controller 19 ("The MIDI queue under load", above).

### The run of 2026-11-12

Controller 19 (`302fbbfa`), the same set-up (bank 16 in RAM with CORE_SETUP, nothing written to
flash), session `logs/sessions/2026-11-12-nrpn-sweep-c19`, the owner sweeping all eight faders at
once for about 20 s:

- ✅ **NFR-01**: `smclog.py metrics --check` ok. Input → USB p50 ≤ 1 ms, p99 ≤ 2 ms, 2.8 ms at
  most over 149 750 messages (11.3 ms p99 on controller 18); input → core 2.2 ms at most.
- ✅ **Nothing refused, nothing split**: the MIDI out queue refused 0 and held at most 12 packets
  (the low mark 8 + one unit); every `BF 63` came with its `BF 62`, every `BF 06` with its
  `BF 26`, every CC 27 with its CC 59 (`nrpn_an`-style check of `midi.jsonl`: 0 framing faults).
  All seven 14-bit faders covered 0..16383.
- **What the backpressure did**, busiest 10 s window: 31 136 events, 116 418 messages to the host
  (about 11.6 a millisecond, the same ceiling as on controller 18), 13 901 fader positions merged
  into a waiting one (the newest wins), 9 053 rounds held for the MIDI queue. Each fader's value
  went out every 1.5–1.9 ms at the median, 2.3 ms at p90, 3.7–6.4 ms at p99; two successive
  values of a fader differ by 400–530 at p99, 700 at most (of 16 383, about 4%): the positions the
  USB had no room for.
- The parameter number goes out again before nearly every value (34 523 selects for 35 456
  values): the faders share channel 16 and take turns, so each value needs its select (the router's
  rule). Four messages a value, as planned.

### Encoders, 2026-11-12

Controller 19 (`302fbbfa`), the same set-up (bank 16 in RAM, `setup config=test
param_null_p4=gesture`, nothing written to flash; input and engine logs at DEBUG), session
`logs/sessions/2026-11-12-nrpn-encoders-c19b`, about 147 s: the owner turned encoders 1–6 (NRPN
384–389) slowly, fast, to both ends, with Shift, and encoder 5 and fader 1 by turns, watching a
MIDI monitor:

- ✅ **Framing**: 9 446 messages received, the same as the engine's `out` records; 392 selects,
  83 nulls, 0 framing faults (`nrpn_an`-style check of `midi.jsonl`); no data after a null without
  a new select. Each gesture starts with `BF 63 03`, `BF 62 0n` once; the values are `BF 06`/`BF 26`
  pairs.
- ✅ **No click lost or doubled**: every one of the 4 581 encoder events in the input log has its
  engine step; the values sent per parameter are those steps less the ones held at an end (130,
  966, 517, 1 295, 215, 53 for encoders 1–6), as received.
- ✅ **Steps**: start 8192, fine step 7, acceleration by the time per click as the formulas say
  ("Encoder acceleration", "On the hardware"); all six reached 16383 and encoders 2–4 also 0, and
  stayed there (1 405 clicks held at an end, nothing sent for them); Shift + turn moved 7 a click.
- ✅ **The null**: each of the 92 encoder gestures ended 300–310 ms after its last click; `BF 65 7F`,
  `BF 64 7F` followed every gesture that sent a value. Several encoders turned at once share
  channel 16, so 310 selects came with no null between: the parameter changed and was selected
  again, as for the faders.
- ✅ **Encoder 5 and fader 1 by turns**: the parameter selected again at each switch (no fault in
  the framing check).
- ✅ **NFR-01**: `smclog.py metrics --check` ok; input → USB p99 ≤ 1 ms, 1.04 ms at most; the
  MIDI out queue 4 at most, nothing refused or dropped.
- ⚠️ **An instrument**: not run (none on hand takes NRPN 384–389).
- **Observed on the bench**: before the run the monitor showed note 24 (C0) on, channel 1, left by
  `smcsim device`: `injected.jsonl` taps M1, a toggle, and the next scenario's CORE_SETUP restarts
  the core without a Note Off. `injected.jsonl` now taps M1 again, and a scenario that ends with a
  note on fails ("Scenarios"). The owner decided (2026-11-12): a configuration from the editor
  (`smcconf push`) sends a Note Off for every note on before it takes effect. Done on the host
  2026-11-12: the core keeps a bit per channel and note for each Note On our output sent and the
  Note Off form of the preset that sent it; `core_notes_off()` ends them (`ctl.c` now uses its
  bounded variant `core_notes_off_limit()`)
  before it decodes a received configuration (log INFO "notes off before the configuration
  changes: N", now per bounded chunk). Tests: `test_core.c` `test_notes_off`, `test_ctl.c`
  `test_config` (a key held through a push). CORE_SETUP now uses the same safe cleanup.
  ✅ The first-save run on controller 20, 2026-11-12, toggled M1 on before a push
  and recorded one cleanup Note Off ([config.md](config.md), "The first run"). This single-note
  result does not cover the later R01/R02 correction. Its host regressions cover 33/128 notes
  across pages, separately blocked USB/BLE queues, upload/Revert/setup/preset edits, 40 notes in
  a batched control edit, the 32-null + 4-input boundary, target aliases and note/channel changes.
  Scenario `note-cleanup-target.jsonl` covers preserved-target toggle/LED behavior.
  ✅ On a retail mixer, 2026-11-23 (USB): `note-cleanup-target` agreed with the simulator, and a
  probe configuration with all 128 strip buttons as note toggles, 128 held, then the whole
  configuration replaced, gave 128 distinct Note Off on the host, none missing.

## Not in the skeleton

Left for stage 3, in SPEC order: page inheritance (FR-MOD-04); the 7-bit forms of NRPN and Pitch
Bend (the MSB alone), Poly Aftertouch, MMC and SysEx templates (FR-CTL-01); NRPN increment and
decrement in (CC 96/97); fader curves and per-control dead zones (FR-CTL-03); "send while
pushed" and encoder push (no push on this hardware, FR-CTL-04/05); `cycle` and `radio` buttons,
long and double press (FR-CTL-06); LED `local`/`remote` distinction and blink modes (FR-CTL-07);
links (FR-CTL-08); learn (FR-CTL-09); Shift `action` kind and system actions (FR-ACT); remote page
and preset selection on the service channel (FR-PG-01); snapshots (FR-PG-05, FR-PG-07); the Shift
SysEx and the low-battery blink (FR-SH-06); the BT button's short press and Shift layer (§5.5).
The configuration's layout is
generated from the schema (D-4, [config.md](config.md)); how much of it stays decoded in RAM is
open (SPEC Q-3).
