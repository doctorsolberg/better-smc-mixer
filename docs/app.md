# The configuration editor

The companion app's UX (SPEC §6, FR-APP-01..24): what it is for, how it is laid out, how controls
are selected and edited, and what it needs from the firmware. The editor itself:
[`app/editor/`](../app/editor/) ("The editor's code" below).

**Status (2026-11-23):** the browser editor is joined to the firmware: real USB/BLE MIDI
configuration, live edits, Save/Revert, panel events and named MIDI, plus installation/update/
Go back through the stock firmware. Dated hardware runs are retained below.

The defects R01–R12 found in a review on 2026-11-15 (output/note cleanup, firmware-job reentry,
Update's Save first confirmation, configuration-identity detection, unsupported-browser
simulation, monitor rendering, host-platform and release routing) and a stock Bluetooth error
masked by assigning to `DOMException.message` in `js/blestock.js` are fixed and verified.
Regression tests run through `tools/editortest/run.mjs`; `node tools/editortest/browser-defects.mjs`
(Node 22+ and installed Chrome; `SMC_CHROME` overrides the executable) uses a fresh headless
profile, no native device APIs and synthetic StockDevice flash. Without Web MIDI the editor is an
offline file editor (R11, the owner, 2026-11-16). Firmware is installed only from the manifest
served beside the editor; installing from a local file or folder was removed on purpose (the
owner, 2026-11-21).

On 2026-11-23, on a retail mixer: USB and Bluetooth install/update/Go back cycles with physical
off/on and byte-exact configuration read-back (the revision aside: the mixer assigns revisions,
P-6); the end-to-end test 37/37 over USB; the page self-test 29/29; all 19 core scenarios equal to
the simulator; P-6 live (an outside write raises the revision and the open editor notices); 128
held notes released after a configuration replacement ([integration.md](integration.md),
"Hardware results, 2026-11-23").

## What the editor is for

The mixer drives hardware synths and iOS apps (SPEC §1). A preset is an instrument and its pages
are groups of that instrument's parameters (FR-MOD-06). The jobs, most frequent first:

| Job | What it takes now (files, `smcconf`) | What the editor makes of it |
|---|---|---|
| Map a synth's parameters onto a page | write JSON by hand, push | pick a built-in or MIDI Guide parameter list (read on demand from its GitHub repository), tap or touch 8 controls one after another |
| Fill and copy in bulk (8 faders on CC 20–27, strip n on channel n, page 2 = page 1 + 8) | edit 8 lines | select, "Series…", copy and paste with an offset |
| Check what a control sends | ShowMidi next to the mixer | a "Sends" box with the bytes; the monitor; touch the control and its record shows up |
| Edit the Shift layer | `shift` objects in JSON | a Normal / Shift switch over the same panel; the device's Shift button switches it too |
| Templates per synth or iOS app | copy files around | templates (a whole preset); an instrument's parameter list in the mapping mode |
| Move between presets and pages | Shift + arrows on the mixer | tabs; the editor follows the mixer and the mixer follows the tabs |
| Monitor | ShowMidi | a drawer with MIDI out and in, the preset, page and Shift |
| Device info, diagnostics | `smcconf info`, `smclog` | a device sheet: versions, storage, editor connection/operation log and a local support report; the firmware log stream is not wired |

## Principles

1. **The panel is the map.** The screen shows the mixer's face: 8 strips (encoder on top, the
   fader, M S R □ beside it), the bottom row of 11, BT and Shift ([hardware.md](hardware.md)). A
   control is found where it is on the device, and touching it on the device selects it here
   (FR-APP-10).
2. **Change it, try it.** Linked, device-affecting edits are sent automatically; wait for transfer
   confirmation. Names remain local. **Save** keeps what the mixer plays for power-on, and
   autosave is a setting, off by default (A-12, "Saving and trying"). The app shows not sent,
   sending, not saved, saving and saved; Save waits for acknowledgement and clean storage.
3. **Only valid choices.** Types come from what the control's kind takes, fields from what the type
   and mode use, ranges from the type ([config.md](config.md)); a field that stops applying goes
   away. Reading the schema at run time reduces duplicated field/range definitions (D-4);
   independent codecs and behaviour still need the checks in "One product" below. It is not a
   guarantee against drift.
4. **Bulk is the normal case.** A page has 59 controls. Selecting several and editing them at once
   is as easy as selecting one.
5. **Names live in the file** (D-8). The device has numbers; the file gives them names, and the
   panel shows the name first and the message under it.
6. **Undo everything.** Every change can be undone, the device included (an undo is one more edit
   sent to it).
7. **One product.** The editor and the firmware run on different devices and are built apart, but
   the user meets them as one thing: what the editor writes, shows and says must be exactly what
   the mixer does. Every change is checked on both sides ("One product" below).

## Information architecture

```
Top bar      configuration name · problems badge                 undo/redo · File ▾ · Map instrument
             · Settings · theme · Linked|Separate · the mixer: [connection, new firmware | state, Save/Send]
Second bar   the presets as folder tabs (name, channel)
Work area    the page block under the open preset: a notice when no mixer runs our firmware;
             Page tabs · Layer Normal|Shift; selection chips … Panel|List;
             the panel (or the list of the page's 59 controls, its grouping on top) · legend
             (a second tap on the current preset or page tab opens its menu)
Side         the selected control, or, in the mapping mode, the instrument's parameters
Drawer       MIDI monitor: the mixer's preset, page and Shift; the last message; the simulator
Sheets       Templates (from File and a preset's menu) · Choose an instrument · Settings (Mixer,
             Preset) · Mixer (link, saving, configuration, diagnostics, firmware) · Numbers in
             series · file views
```

Settings follows the levels: **Mixer** first (how the mixer saves, default fader behaviour,
acceleration sensitivity, the fine step on 14-bit targets, Shift during a move, the move and tap
times), then **the current preset** (name, channel, note names, Shift mode, note release, the
parameter deselect after NRPN/RPN). The schema's settings come from `config/schema.json`; their
labels and help are written for musicians in `app/editor/js/labels.js`. Proposals not supported
by the current firmware are kept in the documentation, not in Settings.

**The theme** follows the system by default (Auto) and can be fixed to Light or Dark, from the
sun/moon button in the top bar only (one setting did not earn a tab). The choice is the app's, not
the configuration's.

**The legend** under the panel names the message types' colours and the copy icon a control
carries when another control on the page sends the same message ("Also assigned elsewhere": an
independent copy, its settings its own; the owner, 2026-11-14);
on the Shift layer, the layer's marks as well.

**Messages** (the short notes at the bottom of the window) stay above an open sheet, last longer
the longer they are (2.4 s plus about 45 ms a character, at most 9 s), stay while the pointer is on
them, and have a close button.

### Layout

The configuration nests: a preset holds four pages, a page holds two layers (Normal and Shift).
The layout says so (A-18, the owner chose the folder tabs on 2026-11-13 over a one-line path):

- **The presets are folder tabs** in the second bar. The open one is joined to the work area
  below it and marked by a line in the ink colour along its top, so it stands out in the dark
  theme too; the others sit behind it on a fill.
- **The work area is the open preset's page block**: the page tabs and the layer switch at its
  top, then the selection chips and the view, then the panel.
- **The view** (Panel | List) sits in the page block on the chips' row, at its right end: it
  changes how the page is shown, so it lives with the page, not above the levels. The list's
  grouping (By strip | By kind) is on top of the list itself, so switching the view moves nothing
  on the chips' row and the chips keep their room on a narrow window.
- **The Shift layer** tints the panel (or the list), not the page block: the layer is inside the
  page.

**The top bar** keeps the configuration's name alone on the left; the tools (undo, File, Map
instrument, Settings, theme) and the mixer sit on the right. Templates are reached from File and
from a preset's menu, where they replace that preset. On a phone the bar takes two lines: the name
with the link switch and the mixer, then the tools.

**The mixer** is one capsule: on the left the connection (USB or Bluetooth) with a new firmware's
version when there is one ("↑ 0.21": the update belongs to the mixer, not to the configuration);
on the right where the configuration stands, with the one action that state asks for joined to it
(Save, Send, Connect, Install, Finish). A tap anywhere but the action opens the Mixer sheet.

**The look.** A quiet, neutral interface: near-black "ink" for the editor's own states (the
selected control, the current tab, primary buttons; near-white in dark), so colour is left for what
means something: the message types on the panel, Shift (amber) and the mixer's state (green). The
type colours share one lightness and saturation with their hues spread round the wheel (OKLCH), so
none shouts and each stays readable in both themes. Hairlines and soft shadows instead of boxes,
filled fields instead of outlined ones, segmented controls with a raised thumb, the system font
(SF Pro on the iPad) with tabular figures for numbers. The panel is a recessed surface with raised
keys; encoders show a pointer and a value ring, faders a cap and a value fill in the type's colour,
lit buttons a glowing dot in the preview. These pictures are not physical encoder rings (the
hardware has none) or a complete live value/LED telemetry view.

**Wording.** What the user reads is written for a musician, not for the firmware: "Sends", "Preset
channel (1)", "Behaviour", "Turning", "Also assigned to 2 other controls", "Saving in 3 s". Field
names of the file, command names, build IDs and versions stay out of the main screens; the Mixer
sheet keeps the firmware version and, under Diagnostics, the log.

## Screens

- **iPad landscape** (primary, 1180 × 820 and up): the panel on the left, the inspector on the
  right (300–380 pt), the monitor as a drawer at the bottom. Touch targets at least 44 pt where
  the panel allows; the strip buttons are narrower (about 43 pt) and are reached by the chips and
  the lasso as well.
- **iPad portrait and narrow windows** (under 1000 pt): the panel on top at full width, the
  inspector as a bottom sheet that folds down to its tabs. The four preset tabs stay (long names
  shortened); the pages and Normal | Shift wrap under them when the width runs out. The panel
  grows with the space it gets (faders get longer on a large screen).
- **Touch** (a coarse pointer): every target at least 44 pt, the strip buttons excepted (A-9).
- **Phone** (under 600 pt; not a target, but it must not break): the panel keeps a workable
  width and scrolls sideways; the bars drop their labels and keep the icons; the inspector's sheet
  starts folded and opens when a control is tapped.
- **Desktop** (a browser with Web MIDI, FR-APP-02): the same layout, plus the keyboard: ⌘Z / ⇧⌘Z,
  ⌘C / ⌘V on the selection, ⌘A selects the strips, Delete clears, Esc stops the selection and the
  parameter assignment, ⇧/⌘ click adds to the selection, a drag on the panel's background lassos.

## The panel

Each control shows its name (or its message when it has none) and its type's colour (CC, CC 14-bit,
NRPN, RPN, note, pitch bend, program, aftertouch, relative). Faders write their label along the
track; encoders show a preview value ring; buttons can light in the preview. Only with `?mock`,
a **Simulator** switch in the monitor bar lets the pictured panel simulate controls; without
`?mock` there is no Simulator switch. Real touches select controls
and real performance MIDI reaches the monitor, but the page does not subscribe to full target/
LED telemetry or feed every real message into these preview values. Do not read an on-screen
ring or LED as a read-back of the physical mixer/instrument state.

Marks:

- copy icon — another control on the same page sends the same message. The controls are
  independent copies: nothing in the editor ties their settings together, and changing one changes
  none of the others (the owner, 2026-11-14). They reach the same instrument parameter, so the
  mixer keeps that parameter's value once (D-5), as the instrument does. On one page that is usually a mistake, across pages usually intended (keys that are the
  same on every page), so the panel marks only sharing within the page; the inspector lists every
  place.
- In the Shift layer: faded — same as Normal; "fine" — the encoder's default (the same target, the
  fine step); "own" — a layer of its own; striped — nothing; lock icon — the 8 navigation buttons, whose
  Shift layer selects pages and presets and is the firmware's (FR-SH-02).

**The list view** lists the page's 59 controls, grouped **by strip** (as the panel reads) or **by
kind** (encoders, faders, the strip buttons row by row, the bottom row; A-3), with name, type and
number editable in place: for checking a whole page at a glance and for keyboard entry on a
desktop. Its selection is the panel's. The editor opens on the panel, then on the view used last;
the grouping is remembered too.

## Selecting controls

| How | iPad | Desktop |
|---|---|---|
| One | tap | click |
| Add or remove one | "Select several" on, then taps | ⇧ or ⌘ click |
| A region | drag on the panel's background (lasso) | the same |
| A row of a kind | chips: Faders, Encoders, M, S, R, □, Bottom row | the same |
| A strip | tap the strip's number above it | the same |
| By touching the mixer | touch a control: it is selected (an option) | the same |
| In the table | the row's checkbox | the same, and click |

The selection is kept per page view; switching pages keeps it on the same control positions, so
"these 8 faders" can be edited page after page.

## The inspector

**One control:** where it is (preset, page, kind), its name, the layer switch, then the fields its
type and mode use, in this order: message type; channel (the preset's, showing which it is now,
or fixed 1–16); number (with MSB : LSB beside an NRPN/RPN number, the note's name beside a note,
"MSB n, LSB n + 32" for CC 14-bit); mode (pickup, encoder mode with `n` for div and step, button
mode); range (bottom → top, with ⇅ to invert and "Full"); the encoder's start value; the button's
LED and "send nothing on release". Then **MIDI it sends**: the bytes
(`B0 63 03 · B0 62 0A · B0 06 msb · B0 26 lsb`). Then "Also assigned to n other controls" (folded:
who else sends it, and that each is independent), and Copy, Paste, Clear. The menus only offer
message types, modes and settings supported by the current schema for that kind of control.

**Several controls:** the count by kind, names by pattern (`Osc {n}`), and the same fields, where
one that differs shows "mixed" and an edit sets it on all. Channels, numbers and a button's on
value have **Series…** (below, "Series"). With different kinds selected, only the common fields
(type, channel, a channel series), and buttons to narrow the selection to one kind.

**Program numbers are shown 1–128**, as instruments number them, everywhere in the editor (range
fields, the series, "MIDI it sends", the monitor); the file and the message keep 0–127 (owner's
decision). The hex bytes stay as sent (`C0 02` is program 3).

## The Shift layer

One switch, Normal | Shift, in the bar and in the inspector, shows the whole panel in either layer;
holding Shift on the mixer switches it too, while the editor follows the device. In the Shift layer
the inspector starts with what the control does with Shift:

| Choice | Encoders | Faders, buttons |
|---|---|---|
| Fine | the default: the same target in `fine` | — |
| Same as Normal | `"inherit"` | the default |
| Own | a layer of its own, written in full | the same |
| Nothing | `"block"` | the same |

"Own" starts as a copy of what the control did under Shift until then (for an encoder, its fine
layer), and then lives on its own: it does not follow later changes of the Normal layer, and the
inspector says so. The navigation buttons say what Shift does with them and that it is fixed.

## Mapping an instrument

Mapping is a mode of its own, not a way of editing the selected control: **Map instrument** in the
top bar opens it, and the side panel shows the instrument's parameters instead of the control's
settings until **Done**. Clicking, touching or lassoing controls does not leave it.

**Choosing the instrument** ("Choose an instrument", opened the first time and by "Change…"):

1. A search over maker and instrument names.
2. **Recently used**: the last five, each opened at once, offline too ("saved for offline use").
3. **MIDI Guide**: makers and their instruments, read from the public GitHub repository
   `pencilresearch/midi` ("Parameter lists from midi.guide"); credited with a link and its
   licence (CC BY-SA 4.0).
4. **Built in**: the editor's own lists (GM2 sound controllers, drum pads, an NRPN synth).

Opening a MIDI Guide instrument downloads its CSV files, converts them (`fromMidiGuide`,
"Parameter lists from midi.guide") and keeps the result in the app's storage, so it opens without
a network next time. The index of makers and instruments is cached for 24 hours. A failure is
named in place (offline, GitHub's rate limit, not found, data the editor cannot read), and the
instruments opened before still work.

**The instrument card** at the top of the side panel: "Instrument", its name, "Change" at the right,
and one line on where its list comes from: the number of parameters and a "MIDI Guide" link with an
icon to this instrument's own file in MIDI Guide's repository, where its data can be read and
corrected. The source and its licence are named in the "Choose an instrument" dialog.

**Onto this page**: each section of the list (Oscillator, Filter, Envelope…) has a button that puts
the whole section on the page in one undo step, by a rule simple enough to predict:

- knobs and sliders go onto the faders left to right, then the encoders; switches and drum notes
  onto the strip buttons, row by row (M, S, R, then the select buttons);
- a control that already holds one of this instrument's parameters is kept, so pressing the button
  for several sections fills the page in turn; a control holding anything else (the factory's
  CCs, another instrument's) is free to take;
- a parameter already on this page is skipped.

Pointing at the button outlines the controls it will fill (dashed). The message names the first
and last control and counts what did not fit or was already there; when nothing is free it says
so and suggests mapping one by one or another page.

The list holds the parameters with their messages (name, type, number, and where useful a range or
an encoder mode). Tap a parameter to arm it, then
tap a control on the panel or touch it on the mixer: the control gets the parameter's message and
name, and the next parameter is armed; the panel is outlined while a parameter waits, and the
selection is kept. The armed parameter shows in a card ("Next: Cutoff") with **Stop**, and, when
controls are selected, **Fill n selected controls**, which assigns the list in order to them. The
list can be searched; a tick marks a parameter already on this page, and "page 2" one placed on
another page of the preset. A page of 8 faders is mapped with 8 touches, without leaving the
mixer. A parameter a control cannot send (a note on an encoder) is refused with a message.

Parameter lists may have groups (Oscillator, Filter…), shown as headings and searched with the
names. A parameter can carry its range (`lo`, `hi`), its values for a switch (two values make a
toggle button) and a fixed channel (drum notes on channel 10). A parameter the firmware cannot
send (`needs`, for example a 7-bit NRPN) is hidden from the list and skipped by "Onto this page"
and "Fill n selected controls". The format is the editor's own
(`app/editor/js/templates.js`), not part of the configuration schema; "Parameter lists from
midi.guide" says where the lists come from (A-4).

### Learn from the instrument

"Learn from the instrument" (A-11) sits above "Sends" for one or several selected controls. While
it runs, a banner names the control that waits ("Learning: Fader 3 · 3 of 8") and the panel
outlines it; moving a knob, a fader or a key on the instrument fills in that control's type,
channel (the preset's when it matches) and number, one undo step per control, and moves on to the
next selected control, row by row. A parameter already learned in this pass is ignored, since one
knob sends many messages. Recognised: CC; CC 14-bit (CC n followed within 60 ms by CC n + 32);
NRPN and RPN (the parameter number, then the data entry); notes; Pitch Bend; Program Change;
Aftertouch. A message the control cannot send (a note on an encoder) is refused with a message and
the control keeps waiting. Escape or "Cancel"/"Done" ends it.

Learn listens to the instrument, not to the mixer. Connect the instrument to the computer and
enable MIDI output on it (often "MIDI out: CC/NRPN"). Start Learn, allow MIDI without SysEx, and
choose its input from the banner. MIDI access without SysEx is requested only when Learn starts.
The mixer's own ports are not offered, whether recognised by name or because the editor's mixer
connection uses them. A previously selected port is listed first, not opened silently. Learn
stops listening when it finishes or you cancel, and when the configuration, preset, page or layer
changes. Only `?mock` offers simulated messages. Learn has been tested with fake ports in the
browser, not with a real instrument.

## Bulk fill, copy and paste

### Paste a page with an offset

The page menu's "Paste with numbers moved on…" (FR-MOD-04 done as a copy, A-6) replaces the page
with the copied one, its numbers moved on by k. What moves, per type:

| Type | Moves | When it would leave the range |
|---|---|---|
| CC, relative CC | the CC number | the control keeps its number |
| CC 14-bit | the MSB's CC (the LSB's CC n + 32 with it) | the same (0–31) |
| NRPN, RPN | as chosen: the LSB (3:10 → 3:18), the MSB (3:10 → 11:10) or the whole number (counts across the LSB's end) | the same: LSB and MSB stay 0–127 each |
| Note | only when "Move notes too" is ticked (off: drum pads keep their sounds) | the same |
| Program Change, Pitch Bend, Aftertouch | nothing: no number | |
| Any type with a fixed channel | the channel, by "Add to fixed channels"; the preset channel stays | the channel stays |

Values, ranges and modes stay. Own Shift layers move by the same rules; "Same as Normal" layers
follow by themselves. Names: "Count on" (Track 1 → Track 9: a number at the end moves by k), keep,
or remove; the page name the same. The bottom row belongs to every page and is not pasted.

Nothing is clamped: before, a CC 14-bit at 28 + 8 became 31 and an NRPN LSB ran into the next MSB,
silently making two controls share one message. The dialog now shows each control that changes,
before → after, marks in red those that keep their number, and links those that would send the
same as a bottom-row key. One undo step.

### Series

"Series…" beside the channel, the number, or a button's values gives each selected control the next
value. The dialog speaks the type's terms, because one start and one step do not fit every type:

| Series of | Start | What goes up | Range |
|---|---|---|---|
| Channels | first channel | the channel; controls on the preset channel become fixed | 1–16 |
| CC, relative CC | first number | the CC | 0–127 |
| CC 14-bit | first number | the MSB's CC; the preview shows both, `CC 20+52` | 0–31 |
| NRPN, RPN | **What goes up: LSB, MSB or Whole number.** LSB and MSB take two start fields (MSB, LSB) and keep the other part fixed: `3:10, 3:11…` or `3:10, 4:10…`; Whole number counts across the LSB's end | as chosen | 0–127 each, or 0–16383 |
| Notes | a name (`C3`, `F#2`, `Bb-1`) or 0–127; middle C is C4 (60) | the note, in semitones | 0–127 |
| Programs (buttons) | first program, 1–128 | the program the press selects; "Send the program only on press" (on when a button is Momentary) sets the Trigger mode, so release does not send a second program | 1–128 |
| Other button values | first value | the on value (one CC that picks a waveform) | the type's |

- **Which controls take part:** those whose type has the field. The others stay in the preview,
  greyed with "—", and do not use up a value: a series over M1…M8 with M4 sending nothing gives
  M5 the fourth value.
- **Order:** left to right; when the selection spans several rows (M and S, or encoders and
  faders), "Row by row" (along a row, then the next) or "Strip by strip" (down a strip, then the
  next). "Reverse the order" turns either round.
- **The start** is taken from the first control in that order and follows the order until it is
  edited. A start that would run past the end of the range (a button's on value is often the top)
  starts at the bottom instead.
- **Names:** optional, `Osc {n}` names the controls in the same pass.
- **The preview** shows every control with its value. Out of range: red, the summary says so and
  Apply waits. The same message as another control on the page (either layer) or another one in
  the series: an outline and the copy icon, its title names the other control ("Already assigned
  to …"); Apply still works: the same message on two controls is allowed, each an independent
  copy.
- One undo step; a toast says how many controls were filled.

- **Controls:** Copy takes the selection; Paste of one control fills every selected control of the
  same kind; Paste of several keeps their shape, moved so the leftmost strip lands on the
  selection's leftmost strip. Copy, Paste and Clear act on the layer shown, Normal or Shift, and
  leave the other layer and the names as they are (the owner, 2026-11-24: the panel shows one
  layer, so an operation on the hidden one was a surprise). A layer copied under Normal pastes onto
  Shift and back: a Shift layer that differs in one setting starts as a copy. A layer pasted onto
  another kind keeps what that kind takes (a fader's pickup is not a button's mode); a message the
  kind cannot send is skipped and the toast counts it. Clear under Normal makes the Normal layer
  send nothing and keeps a Shift layer of its own; under Shift it makes the control do nothing
  with Shift.
- **The bottom row** belongs to the whole preset, not to a page (FR-MOD-01): the panel says so
  above it ("Bottom row · the same on all pages of …"). "Clear page…" asks, and clears the row too
  only when "Also clear the bottom row" is ticked; "Clear preset…" clears everything. A row of its
  own on each page is in [ideas.md](ideas.md) for 0.2.0 (the owner, 2026-11-24).
- **Pages and presets** (a second tap on the open tab, or a right click): rename, copy, paste,
  paste a page **with an offset** (numbers +k, fixed channels +k; note numbers only when asked,
  names dropped unless kept), save a copy, clear; a preset also opens its settings, is saved as a
  template or takes one ("Use a template…"). The offset paste is FR-MOD-04 ("like page M, but
  +k") done as a copy in the editor, with no firmware feature (A-6).

## Checking what a control sends

Three ways, from quick to thorough: the inspector's Sends box (what it would send, as bytes); the
panel's ⇄ and the inspector's list of places sharing the target; the monitor, where each message
the device sends carries the control that sent it, so touching a control on the mixer shows both
the control (selected) and its messages.

## Presets, pages and following the device

The tabs show the preset's name and channel and the page's name and its Shift + arrow. While
Separate, a green dot marks the preset and page the mixer is on (Linked, that is always the open
tab, so no dot).

**Linked / Separate** (A-1) is a switch beside the mixer capsule, one tap to change, shown
whenever our firmware is connected; on narrow windows the current mode keeps its label and the
other shows its icon.

- **Linked:** the editor shows the preset, page and Shift layer the mixer is on, choosing a tab
  switches the mixer, and touching a control on the mixer selects it here (A-2). A touch on a
  control that is already in the selection keeps the selection, so a group chosen in the editor
  can be tried by hand. In the mapping mode, with a parameter armed, the touch assigns it instead.
  Every edit plays on the mixer at once (A-12, "Saving and trying").
- **Separate:** the editor and the mixer each keep their own preset and page; touches on the mixer
  only play it (and show in the monitor). The tabs' green dot says where the mixer is. Edits
  stay in the editor until **Send** (A-12).

The choice is the app's and is remembered. With no mixer, or firmware other than ours, the switch
is not shown: there is nothing to link to ("Without a mixer").

## Without a mixer

The editor stays whole without a mixer running our firmware (A-19, decided): preparing presets away
from the mixer, an iPad without Web MIDI (A-13), templates and files all need it. The intended
disconnected presentation is below. **Owner decision, 2026-11-16 (R11):** without Web MIDI the
normal page is an offline file editor, not an automatically connected mock. Import, editing,
templates and file export still work; the capsule says Offline, never physical Saved. Simulation
requires explicit `?mock` and is labelled as simulation. `?devhost` remains host-only integration.

| What is connected | The mixer capsule | The notice over the page | Gone or off |
|---|---|---|---|
| Nothing | "No mixer", **Connect** | "No mixer connected. Everything here works on the configuration in this app…", Connect | Linked/Separate, Save, Send; tabs do not switch a mixer; no green dots |
| M-Vave's firmware | "M-Vave firmware", **Install** | "This mixer runs M-Vave's firmware, which this editor cannot configure…", Install… | the same |
| Update mode | "Update mode", **Finish** | "The mixer waits for firmware: an update was interrupted…", Finish the update | the same |
| Ours | the saving state, Save or Send | none | |

With no mixer the notice is calm (no warning colour) and can be put away until the next
connection, since working without a mixer is normal; the other three stay, in amber. With
`?mock` the simulator still plays the panel; otherwise the monitor bar says messages show once
the mixer is connected. The side panel's first screen says which case it is in one line. **Connecting** (or reconnecting) when the mixer plays something other than the
editor's copy (edits made with no mixer, or the mixer switched off unsaved) asks as when switching
to Linked: Send mine, Take the mixer's, or Stay separate (A-12, rule 6).

A first-run sheet ("Connect the mixer", "Install better-smc-mixer", "Edit without a mixer") is a
possible later step, not built and not asked for.

## Live editing and saving

The mixer capsule shows the link (a USB or Bluetooth icon) and where the configuration stands, from
the storage state the firmware reports (CFG_INFO, [link.md](link.md)): the model and the words are
in "Saving and trying" (A-12). With the autosave on, a touch of the panel restarts the quiet pause,
as on the device.

**The undo history** (A-8) is kept in the app's storage, per configuration, never in the file: a
restart brings back the last 100 steps, as long as the working copy is the one the history belongs
to. A browser keeps it in IndexedDB (megabytes, where `localStorage` holds about five: each step is
a whole configuration, 20–60 KB). A browser may still clear it (site data removed; Safari also
clears the storage of a site not opened for 7 days unless it was added to the Home Screen), and the
editor then starts with an empty history, as it does for a new file.

Offline (no device), the editor works on its own copy ("Without a mixer"); connecting later
compares the two and, when they differ, asks: "Send mine", "Take the mixer's" or "Stay separate"
(A-12, rule 6). On real connection `sameContent()` in `js/mixer.js` compares canonical content,
ignoring names, ID and revision; the mock compares text. During a connection EVENT and poll
detection compare both the previous ID and revision before adopting metadata (R08). Either
changing invalidates the incremental baseline and pending edits without changing the editor's
working copy. The existing choice asks to send that copy explicitly, read the actual mixer bytes,
or stay separate; ordinary edits do not silently overwrite the outside change. Since the P-6
decision (2026-11-20) the mixer assigns a new revision on every accepted change, whoever sends
it, so an outside write cannot keep both ID and revision; the editor adopts the mixer's revision.
✅ On the mixer 2026-11-23 ("What the editor needs from the firmware", P-6). A side-by-side
Compare is not designed.

## Files and templates

- **Open a file** replaces the configuration in the editor as one undo step. While Linked it is
  also sent to the mixer; Separate keeps it here until Send. "Start from the factory settings"
  asks first.
- **Save a copy** asks for a file name; the file's text is folded under "Show the file".
- The working configuration is kept by the app (the browser editor: the browser's storage) and written
  as the canonical file `smcconf format` writes: defaults left out, keys in order, a control on one
  line (FR-APP-21). The editor's writer gives `config/factory.json` and `config/test.json` back
  byte for byte.
- **Export**: the whole configuration, a preset, a page. A partial export is still an ordinary
  configuration file (one preset, or one preset with one page), so `smcconf` validates it and any
  file can be imported. **Import into this place**: a file's first preset into the current preset,
  its first page into the current page. The UI calls these File ▸ Save a copy and Bring in from
  a file. A-5's `scope` tag is decided but not implemented ("Schema proposals").
- **Templates** are whole presets to start from, and nothing else (the instruments' parameter
  lists moved to the mapping mode). The sheet says how a template differs from a file: **a
  template** is one preset kept in this app on this computer, used in any preset with one click;
  **Save a copy** makes a file of the configuration or a part of it, to keep as a backup, send to
  someone or open on another computer. Its sections: **My templates** (A-11: "Save as a template…"
  in a preset's menu keeps the preset, with a name, an optional note shown on its card, and the
  editor's settings for it such as the note names, in the app's storage; each can be removed),
  and **Ready-made templates** ("Mixer, 8 channels", "One synth, 14-bit faders"). Each template's button says what it
  does: **"Replace Preset n"**, the preset becoming a copy of the template, name and channel
  included, one undo step; the message after it names both ("Preset 1 now holds the template
  'Mixer, 8 channels', and is named Mixer"). Templates open from File and from a preset's menu.
- A file that fails the checks is not opened; its faults are listed with their paths, in the
  tools' words (`presets[0].pages[1].strips[0].encoder.n: needed in mode div`). A fault badge in
  the top bar counts the faults of the configuration being edited (the editor's choices keep it
  at zero).

## The monitor

A drawer: the device's preset, page and Shift; the last message; open, rows with time, bytes,
decoded message and control/source; filters (All, Sent, Received), pause, clear. Transport is
kept in each buffered entry but not shown as a column. Real `MixerDevice` listens to the mixer's
performance output, not arbitrary instrument input; Received is not a record of everything
entering the physical mixer. Only with `?mock` and the simulator on, "Instrument sends" plays an
instrument answering: the value reaches the target (D-5), the fader or ring moves, the button's
LED follows.

## Device info and diagnostics

The Mixer sheet (a tap on the capsule): connect / disconnect, USB or Bluetooth; a firmware card
with its action when there is something to do ("Firmware" below: an update, M-Vave's firmware,
update mode); firmware/editor versions and whether the configuration is saved on the mixer;
Linked / Separate with what it means; **Saving**: what the mixer plays now and what it starts
with, Send (Separate), Save, "Go back to the last save…", and one line on how the mixer saves
with a link to Settings › Mixer; "Load from the mixer". **Diagnostics** is also available offline:
it shows the editor's connection/operation log and saves a local JSON report with editor/browser
details, current connection and firmware metadata, recent editor logs and a monitor event count.
Including the editor's configuration is opt-in. This is the editor-side subset of SPEC §8.6, not a
flash backup, black-box dump or firmware smclog export. The report is saved as
`BSM-Report <date>.json`. Under **Firmware**, "Check for updates" and "Go back to M-Vave's
firmware…". A real connection also shows Build
and protocol/binary/schema Versions; configuration ID, revision and generation are not displayed
there. Power is shown only for the mock, not measured on a real connection.
Firmware is installed or updated only from the release manifest published beside the editor
(the same site, over HTTPS). "Check for updates" fetches that manifest again (no cache) and
reports a newer release, up to date, no manifest, an invalid manifest, a network failure or an
HTTP error; it does not look for binaries elsewhere. Going back to an older version of ours is
possible only if that version is published. Installing from a local package (a file or folder)
is deliberately not offered in the first release: the editor has no signature or authenticity
check for local files (the owner, 2026-11-21). The wizard still requires a matching mixer and a
verified flash layout and backup. "Check for updates" has not run against the physical mixer.

## Planned settings

SPEC asks for settings that `config/schema.json` and the firmware do not have yet. The owner
decided on 2026-11-20 to remove them from the interface entirely: no setting, mode, type or
per-control option below is shown. Legacy values kept by older builds are not part of a config file
and are not applied. Except for the obsolete output row below, these are **schema proposals** for
the owner, not release options.
Pickup indication and last preset/page already have fixed behaviour; their adjustable settings
are proposed. USB/BLE routing is instead decided by Q-25, not a pending output-selector feature.

| Where | Setting | Choices | SPEC |
|---|---|---|---|
| Settings › Mixer | Show pickup on LEDs (`pickupLed`) | on, off | FR-PG-04, §5.7 |
| Settings › Mixer | End dead zones (`deadZone`) | 0–10 % | FR-CTL-03 |
| Settings › Mixer | Tell apps about Shift (`shiftSysex`) | off, on | FR-SH-06 |
| Settings › Mixer | Remote page and preset change (`remoteChannel`) | off, channel 1–16 | FR-PG-01, FR-PG-06, §5.7 |
| Settings › Mixer | On page change send (`pageSnapshot`) | nothing, all values, toggle buttons | FR-PG-05 |
| Settings › Mixer | At power-on (`powerOn`) | last preset and page, preset 1 page 1 | §5.7 |
| Settings › Mixer | Obsolete, removed: Send controls to (`output`) | formerly USB and Bluetooth, USB, Bluetooth; not an approved future setting | Superseded by FR-IO-04/Q-25: USB always, BLE while BT is on |
| Settings › Mixer | Pass MIDI between USB and Bluetooth (`bridge`) | off, on | FR-IO-05 |
| Settings › Mixer | LED brightness (`ledLevel`), unlit LEDs (`ledUnlit`) | 5–100 %; dark, faint glow | §5.7 |
| Settings › Mixer | Bluetooth name (`bleName`), turn off on battery after (`autoOff`) | text; never, 10, 30, 60 min | §5.7, Q-25 |
| Settings › Preset | On preset change send (`pcOnSelect`) | nothing, Program Change | (new: the instrument follows the preset) |
| Fader | Response curve (`curve`), reset value (`default`) | linear, logarithmic, exponential; a value | FR-CTL-03, FR-CTL-02 |
| Encoder | Reset value (`default`) | a value | FR-CTL-02 |
| Button | LED reacts to (`ledSource`), LED blinks while on (`ledBlink`) | presses and MIDI, presses, MIDI; no, yes | FR-CTL-07 |
| Button | Long press (`long`) | nothing, act as Shift, next / previous page, all notes off, reset the strip | FR-CTL-06 (MAY), FR-ACT |
| Any control | Also send (`extra`): up to 3 more messages, each with its own range | | FR-CTL-08 |
| Message types | Poly Aftertouch; NRPN 7-bit; on buttons: mixer action, MMC / SysEx, RPN | | FR-CTL-01, FR-ACT |
| Button modes | Cycle through values, radio group | | FR-CTL-06 |

## What the editor needs from the firmware

Proposals for the owner, built where "Today" says so. P-1 and P-2 are the ones "change it, try it"
depends on.

| ID | What | Why | Today |
|---|---|---|---|
| P-1 | **Edit one control, a preset's or the global settings without a restart**: a SET command for one assignment (page, control, layer) and for the settings, applied between core rounds, keeping the target store | A whole PUT restarts the core: every target goes back to its default and the notes are released ([link.md](link.md), "Commit"), so each edit would jump every value of the synth. FR-CFG-05 lists GET/SET of a single control | built (I-6, 2026-11-13): CFG_SET with the binary form's records for one preset ([link.md](link.md), "Configuration"; [config.md](config.md), "Edits"); the editor sends a small change as SETs, a large one as a PUT. ✅ On the mixer (controller 32, 2026-11-13; [integration.md](integration.md), "I-6") |
| P-2 | **Events** (FR-CFG-06): "control touched" (page, control, layer), preset/page changed, Shift on/off, and storage state changes, to a subscribed client | select by touch (FR-APP-10), follow the device, the mixer capsule | built (I-7, 2026-11-13): EVENTS on, then an EVENT with the whole panel's state at most every 40 ms when it changes: preset, page, Shift, Bluetooth, the touches counted and the last one's binary-form address and layer, the storage, revision and ID ([link.md](link.md), "The panel's state"); `js/mixer.js` follows it. ✅ on the mixer 2026-11-14 (controller 34, USB MIDI and BLE MIDI; the page in Chrome over USB) |
| P-3 | **SELECT preset/page** (FR-CFG-05) | the tabs switch the mixer | built (I-7): CFG_SELECT, applied by the control task in its next round as the panel's own buttons; the tabs send it while Linked. ✅ on the mixer 2026-11-14 (controller 34) |
| P-4 | **The protocol over SysEx** (FR-CFG-01) | iPadOS gives apps MIDI, not the CDC serial port: the iPad editor can only talk SysEx (USB MIDI or BLE MIDI) | built: the firmware, `smcconf --midi` and the editor's `js/link.js` (I-2, I-3; ✅ on the mixer 2026-11-13, controller 31, over USB MIDI and BLE MIDI) |
| P-5 | **What the device sent, with its source**, for the monitor: the app sees the MIDI on the port, but not which control sent it | the monitor's "control" column; checking a page by touching it | built (I-7, 2026-11-14; configuration protocol 4): EVENTS' bit 1 sends SENT records, every message the core sent with the control, page, preset and layer that sent it (the last 63 kept; CFG_SENT polls the same); `js/mixer.js` joins them to the port's MIDI by order and bytes, and the monitor names the control ("Encoder 6 (Shift) · Cutoff"); a message no record names stays "the mixer" ([link.md](link.md), "What the mixer sent"). ✅ on the mixer 2026-11-14 (controller 35: `make e2e` over USB MIDI and BLE MIDI, the monitor in Chrome over USB). Targeted Chrome client checks on 2026-11-15 also named performance MIDI over both real transports; the whole monitor page over BLE has not been checked separately |
| P-6 | **Configuration ID and revision** (they exist: CFG_INFO) and a cheap "has it changed" query | offline editing and divergence (FR-APP-19, FR-APP-24) | the firmware's part built (I-7): CFG_STATE and every EVENT carry the revision and ID (30 bytes, no pull); ✅ revision events on the mixer 2026-11-14 (controller 34). **Decided by the owner, 2026-11-20:** the mixer assigns the revision on every accepted change (upload, SET/live edit, revert, setup), whoever sends it; Save does not advance it; it wraps from `UINT32_MAX` to 0 ([link.md](link.md), "The panel's state"). The editor adopts the mixer's revision and treats an outside write as divergence (rule 6); it still sends a revision for older firmware. It keeps the mixer's ID for a PUT with no file ID; names-only edits stay local; reconnection compares content without names/identity. Host and browser tests ([integration.md](integration.md), "P-6 follow-up"); ✅ on the mixer 2026-11-23: an `smcconf push` of the same file with the same ID raised the revision (17→18, 18→19) and the connected editor showed "The editor and the mixer differ" |

## Parameter lists from midi.guide

A-4: the instruments' parameters come from MIDI Guide (https://midi.guide), an open, community
database of MIDI implementations of hundreds of instruments, kept as CSV files in the GitHub
repository `pencilresearch/midi` and served by a read-only API (`https://api.midi.guide/v4/`, no
key, no rate limit). Read on 2026-11-12 from its documentation:

- `GET /v4/devices/` lists makers and devices (name, slug, `parameter_count`, `trigger_count`,
  `last_updated`); `GET /v4/devices/{maker}/{device}/` gives one device: `metadata`, `ccs` (the
  parameters by section) and `triggers` (drum notes by section); `GET /v4/dump/` everything.
- A parameter: `parameter_name` ("Filter: Cutoff"), `parameter_description`, `cc_msb`, `cc_lsb`,
  `cc_min_value`, `cc_max_value`, `cc_default_value`, the same for `nrpn_`, `orientation`
  (`0-based` or `1-based`), `usage` ("0: Unmuted; 1: Muted"), `notes`. A trigger: `sound_name`,
  `midi_channel`, `note_number_min_value`, `note_number_max_value`,
  `note_number_default_value`, `velocity_usage`.
- The data is **CC BY-SA 4.0**; MIDI Guide asks to be credited with a link. A list made from it
  says so in the editor and links back; a file that copies it carries the same licence.

**What the editor makes of it** (`fromMidiGuide` in `app/editor/js/templates.js`, from the
device that `js/midiguide.js` reads out of the repository's CSV files):

- the sections become the list's groups; "Filter: Cutoff" is named "Cutoff" under "Filter", and
  the name goes onto the mixer's control when the parameter is assigned;
- the finest form the mixer can send: an NRPN with more than 128 steps as NRPN; a CC whose LSB is
  CC n + 32 as CC 14-bit; otherwise the CC; an NRPN of 128 steps with no CC would need
  "NRPN 7-bit", which the firmware lacks: it is marked `needs`, hidden and skipped;
- the range (`cc_min_value`, `cc_max_value`) becomes the control's range;
- `usage` with values: two values make a toggle button with those values; more are the steps of a
  switch (a "Cycle through values" mode is only a proposal) and can feed a series of buttons, one value each
  ("Series", the button's values);
- triggers become notes with their channel, for the strip buttons as drum pads.

A page per section is built ("Onto this page", "Mapping an instrument"). Ideas not built yet: the
default value as the encoder's start, a parameter's description as the control's help, and value
names in the monitor ("CC 20 = 2 (Triangle)").

**Getting the data into the editor** (A-14, decided by the owner 2026-11-20: option (b)). The
API sends no CORS header (checked 2026-11-12: no `Access-Control-Allow-Origin` on
`/v4/devices/…`), so a page on GitHub Pages cannot read it; the repository's raw files can be read
(`raw.githubusercontent.com` answers `Access-Control-Allow-Origin: *`). The options were (a) a
scheduled action copying the data into the editor's site, (b) reading the CSV files from the
repository on demand, (c) asking MIDI Guide for CORS on the API. `js/midiguide.js` implements (b):
the index of makers and instruments comes from GitHub's tree API
(`api.github.com/repos/pencilresearch/midi/git/trees/main?recursive=1`), cached for 24 hours; an
instrument's CSV files come from `raw.githubusercontent.com` when it is opened, and instruments
opened before are kept for offline use. Errors are typed: offline, rate limit, not found, bad
data. The editor ships no MIDI Guide data and no made-up catalogue; the sheet credits MIDI Guide
with a link and CC BY-SA 4.0.

**Community presets** (A-4, planned, not published): a separate repository of shared presets, one file each with
`"scope": "preset"` (A-5) and a short description, and an index file the editor reads (same way as
the firmware manifest below). Adding one is a pull request; no backend, no accounts. A user's own
presets are files they keep, and My templates in the app.

## Firmware

What the editor does with the mixer's firmware: see what runs, install ours over M-Vave's, update
ours, go back to M-Vave's. With the mocked mixer the page shows the screens ("Preview: another
mixer" in the mixer sheet picks what the editor finds) and writes nothing. With a real mixer
(`MixerDevice`) the real sheets run the code below, which writes the mixer's flash. The dated
USB/BLE runs are recorded below; the latest, on 2026-11-23, completed install, Go back and update
cycles over USB and Bluetooth with physical off/on and configuration read-back
([integration.md](integration.md), "Hardware results, 2026-11-23"). The mock's firmware
screens retain older preview flows (including a backup-file Go back); they are not instructions
for the real no-file STOCK route.

**What the editor finds when a mixer connects:**

| State | How it is told | What the editor shows |
|---|---|---|
| Ours, no newer installable package loaded | our MIDI ports and the link's identity (build ID, version) | the capsule's storage state; Check for updates fetches a fresh manifest from this editor's site and says whether it holds a newer release |
| Ours, older than the loaded release | the version against the package manifest; STOCK and install files required | an up arrow and the package version on the capsule; a card with Update; real release notes are not populated yet |
| M-Vave's firmware, any version | service name request (`0x11` → `SMC-Mixer_012`) over USB SysEx or stock AE40 GATT ([hardware.md](hardware.md), "Stock service protocol"); SMC-Mixer name required, no version allowlist | "M-Vave firmware" on the capsule; the editor works on its own copy; "Install better-smc-mixer…" |
| Update mode (preview only) | selected in the mock; no implemented real bootloader discovery | "The mixer is waiting for firmware"; Install; real interrupted installations normally return to stock firmware, subject to the sector-0 recovery limit below |

**No list of M-Vave's versions** (the owner, 2026-11-14: M-Vave updates its firmware, so nothing
may be tied to one version). The installer takes any firmware that answers the service SysEx as an
SMC-Mixer and decides on the backup alone, with the plan's checks: sector 0 parses, M-Vave's area
reads as the uboot reads it (CRCs, the entry point), our sectors are free or hold an earlier
installation of ours, and the regions M-Vave's directory declares (`BTIF`, `EXIF`, `USRFLASH`, the
update loader) lie outside ours. A version that moved any of these is refused before anything is
written, with the check that failed. STOCK (going back) does the same on the mixer: it checks
M-Vave's area as it is, whatever its version. What every version must keep: the service SysEx
(read, erase, write; ✅ V012 and V013 on the developer's mixer, dated below) and a flash layout
that leaves our sectors alone. A version without the service SysEx cannot be installed over from
the browser: the recovery key ([recovery.md](recovery.md)) is the way then.

**After an M-Vave update** (the owner's question; ✅ run on the mixer 2026-11-14). The mixer ran
M-Vave's V012 (ours gone back with STOCK, controller 36 left in flash) and SincoOTA on an iPhone
updated it to V013 over Bluetooth, in two runs: the first sent the update loader and the mixer
restarted into it (the BT LED dark, nothing on USB: the loader serves BLE only); SincoOTA, opened
again, found it (version 0) and wrote the firmware, and the mixer started V013. A stock read
before and after (`materials/dumps/sinco-read-app36-before-v013.bin`,
`sinco-read-v013-after-ota.bin`) shows what the update did:

- **It erases the whole range M-Vave's directory calls its VM, `0x24000..0x7AFFF`**: our
  configuration slots and the panel's state, our area and our VM, and the old update loader at
  `0x6E000`. So after an M-Vave update better-smc-mixer and the configuration saved on the mixer
  are gone; the editor's own copy stays.
- It writes M-Vave's area at `0x1000` (V013's) and its VM's first sector; sector 0 reads the same
  (V013's equals V012's; the start points at `0x1000`). `BTIF`, `EXIF` and `USRFLASH` unchanged.
- V013 shows itself differently on USB: the device "SMC-Mixer" (V012: "SINCO"), PID `0x3143`
  (V012: `0x4B4D`), MIDI ports "-Private" and "-Master" (V012: "SMC-Mixer-Private", "…-Master").
  The service SysEx is the same: ✅ name (`SMC-Mixer_013`) and the whole read (`0x23`).
- The plan on that read holds as for a fresh mixer (19 checks): installing ours over V013 is the
  first installation again (`found` empty), not `earlier`. ✅ The page did it the same day in
  Chrome (V013 found by its name on the new ports, the backup saved, 6.8 s); after the power switch
  our 0.1.0 runs (the factory configuration: the saved one was erased) and `stock check` is ok on
  V013's area, so going back leads to V013.

So the earlier local check, which wrote only V013's `flash.bin` over the flash and left our
sectors, was wrong about the updater; the plan and STOCK on V013's area (`test_bootsel`) stand.
The user guide warns before an M-Vave update: the configuration on the mixer is lost, keep it in
the editor.

**Installing over M-Vave's firmware** (a step-by-step sheet): 1. show the version M-Vave's
firmware names; 2. **read and save a backup** of the whole
memory (512 KB) to the user's computer: M-Vave's firmware, its settings and Bluetooth pairings;
it is never uploaded (SPEC C-7). Keep it for recovery; ordinary Go back needs no file. The page
requests a download and pauses before writes; it cannot verify the file was durably saved on the
computer, so check the download yourself. 3. write our firmware (about a minute over USB; keep
power on); 4. request physical off/on, then reconnect to verify ours and its factory or previously
saved configuration. Facts behind it: the stock firmware's SysEx reads the
whole flash (`0x23`, 4.1 s, ✅) and erases and writes it (`0x21`, `0x22`, ✅ 2026-11-10, the
hardware-test image was installed that way: [bringup.md](bringup.md), "Step 3"); the installation
leaves M-Vave's area untouched and changes only sector 0 and our area (✅ [recovery.md](recovery.md),
"The rollback to the stock firmware and back"). ✅ Browser USB and Bluetooth installation ran
on the developer's mixer on 2026-11-14, and again as update cycles with physical off/on on
2026-11-23.
No deliberate physical power-cut test of the final sector-0 write is claimed.

**The installer's code** (the owner, 2026-11-14: the editor installs; SPEC Q-18). Built and tested on
the host, wired into the page and exercised on the developer's mixer as dated below:

The connected device now owns one firmware job from preparation through backup, confirmation,
writes, read-check and completion (R09). Install, Update and Go back reopen that job's retained
view instead of starting competing transports. Closing the dialog hides it, never cancels it.
The sheet's footer button (Cancel while the job runs, Close once it has ended) settles a pending confirmation; there is no mid-write
cancellation. Update keeps cancellation blocked through its sector-0 switch and uncertain
reconnect, then unlocks it only after a confirmed USB/Bluetooth stock connection; the backup
confirmation can be cancelled without starting the installation. Disconnect and device
navigation/configuration edits are blocked while it owns the device; Revert is disabled in the
Mixer sheet and checked again immediately before its transport write, including queued requests.
Safe local file export remains available. Dismiss the result explicitly before a new job.
Update's Save first shows Saving, waits for the Save acknowledgement and clean storage, and only
then enables Update. Refusal, failed storage or connection loss keep an honest failure/retry
state, never Saved (R07). A late stock-check result cannot re-enable Save first for a cancelled
Update, and its retained Save callback is inert. A disconnect during asynchronous backup-plan
preview is rejected before registering confirmation, so it cannot strand an active job. Direct
`MixerDevice.backToStock()` calls also protect the in-flight sector-0 switch from cancellation,
not only wizard callers. The installer algorithm's backup-first, directory-last and sector-0-last
write/read-back ordering is unchanged. GATT setup errors are wrapped with their stage, original
name and `cause`; read-only DOMException fields are never mutated.

The monitor appends synchronously to its existing bounded 400-message history but coalesces
message-driven footer/list rendering into one animation frame (R10). Only the last 200 filtered
rows are visible; a hidden list is not rendered. A pending frame reads current open/pause/filter/
Clear state, not a captured old snapshot. Tests assert DOM work counts, not latency estimates.

- `js/flashimg.js` and `js/installplan.js`: `tools/image/flashimg.py` and `installplan.py`'s plan in
  JavaScript, the report word for word. `run.mjs` holds them to the Python tool on the synthetic
  flashes of `vectors/install.json` (12 cases: as stored and as the stock firmware reads it, the MAC
  item empty or foreign, a tail, our sectors in use, a VM too small, a slot off the key grid, not
  512 KB, already installed, an earlier installation of ours (the update), a run that stopped, a
  page that is not this installation's: [image.md](image.md), "Installing again"). On 2026-11-14 they also gave the same plan as the Python tool on a
  V012 dump and a raw ROM read (a local check, nothing tracked).
- `js/stocklink.js`: the stock service SysEx (name, read, erase, write; the packets and 7-bit
  packing of `smcmidi`) and `install()`: the name must be an SMC-Mixer's (any version), the backup of the whole flash handed to the page before anything
  is written, the plan on that backup (nothing is written unless every check holds), our area sector
  by sector, each read back, then the whole area read back, then sector 0: still as in the backup,
  erased (an erase that did not take means write protection: it stops with nothing written there),
  written and read back, written again on a mismatch. Erase and write refuse sector 0 (except that
  step), the stock area and its VM, the update loader, BTIF, EXIF and USRFLASH, as `smcmidi` does.
- `js/blestock.js`: the same packets over Bluetooth LE, raw on M-Vave's `AE40` service through
  Web Bluetooth ([hardware.md](hardware.md), "Two transports"): a raw transport for
  `js/stocklink.js`. Host tests install through it in 20-byte pieces; ✅ name and read on the
  mixer (V013, 2026-11-14: about 56 ms a 256-byte block; ✅ the page installed ours through it,
  below). The page: "Install over Bluetooth" beside Connect at the top, in the notice and in the
  Mixer sheet (when the browser has Web Bluetooth) opens the browser's chooser, every device near by;
  `MixerDevice.connectBluetooth()` asks the name over `AE40` and connects as `fw.kind` `stock`,
  `transport` `BLE`; Install then runs the same plan and checks as over USB (the texts give
  Bluetooth's times). The mixer takes one Bluetooth connection at a time: connected as a Bluetooth
  MIDI device elsewhere it is not listed (the page says so); on the Mac it was listed only after
  it was removed from the system's Bluetooth devices. Ours over Bluetooth is found by Connect, over
  Bluetooth MIDI. Host test: the whole installation through a stand-in Web Bluetooth device. ✅ On
  the mixer, 2026-11-14 (Chrome, macOS, V013): backup 2 min (M-Vave's code equal to USB's dump),
  install about 90 s, ours ran after a power cycle (0.1.0, `stock check` ok).
  Update and Go back over Bluetooth: ours is reached over Bluetooth MIDI; after STOCK back the
  Update's step offers "Choose the mixer" (the browser's chooser needs a click) instead of
  looking by itself as over USB, then the same installation; Go back ends with "Choose the
  mixer"; the last steps say to connect ours as a Bluetooth MIDI device again. ✅ Update on the
  mixer over Bluetooth alone (2026-11-14): the saved configuration kept. Each switch of firmware
  needed the Mac's old pairing removed first with the shared BLE address. ✅ Build `a628aa6a`
  with its own address connected in Audio MIDI Setup without forgetting (2026-11-14). ✅ The
  page's Go back over BLE then restarted M-Vave's V013; it automatically connected to the Mac and
  stayed out of Chrome's chooser until the owner disconnected and forgot it. Separate addresses
  do not prevent that automatic connection ([ble.md](ble.md), "Our own address"). These remain
  valid 2026-11-14 successes. On 2026-11-15 a stock `gatt.connect()` failed before backup or
  writes, also after off/on (its error is now reported with its stage and cause). ✅ On
  2026-11-23 a full Bluetooth cycle on the release candidate completed with USB unplugged: Go back
  over BLE MIDI, stock AE40, a fresh 512 KB backup, install and read-back, off/on, ours over BLE
  MIDI again with the configuration content byte-identical (the revision aside). Chrome listed the
  stock mixer only after the browser was restarted. iOS with the new address and reliable
  no-forget switching remain unchecked.
- **Bluetooth help** (the owner, 2026-11-14): one text, `bluetoothHelp()` in `js/app.js`, a closed
  section "Bluetooth: connecting, and when it does not" in the mixer sheet (not connected, or over
  Bluetooth) and in every Bluetooth step (the backup over Bluetooth, the update's "Choose the
  mixer", the last steps after a Bluetooth install or update, Go back over Bluetooth): ours as
  "Better-SMC-Mixer", a Bluetooth MIDI device connected in the system first (Audio MIDI Setup on a
  Mac; the music app's list on iPhone and iPad, whose Settings do not list MIDI devices); M-Vave's
  as "SMC-Mixer" through the page's Bluetooth button; not listed: one connection at a time, where
  to disconnect it, the BT light and button. After Go back, the Mac may connect to M-Vave's firmware
  automatically: disconnect in Audio MIDI Setup first; if it reconnects or stays missing, forget
  “SMC-Mixer” in System Settings, Bluetooth, then use the page's Bluetooth button. This can happen
  with separate addresses too. Refused or dropping: disconnect other connections first, then try
  forgetting a cached pairing if needed; two entries, one offline, is expected. The Bluetooth
  chooser's cancellation toast points at it; generic MIDI errors do not add unrelated Bluetooth
  instructions.
- `tools/editortest/stockdev.mjs`: a stand-in for M-Vave's firmware (its read views, the same
  acknowledgement for every erase and write, faults on demand). `run.mjs` installs on it: the flash
  after equals the plan's byte for byte; with lost replies the same; another version installs on the
  plan's checks alone, another device's firmware (`SMC-PAD_…`) is refused after the name; a write-protected sector 0 or area
  stops with sector 0 as it was; an installed flash is refused with nothing erased; the guards hold;
  over an earlier installation the flash after equals the plan's; the cable cut at nine points of
  a run, a second run gives the plan's flash after.
- `js/mixer.js`: when no port answers the link, `connect()` asks each port pair for M-Vave's name
  (the service SysEx, "SMC-Mixer-Private" first) and connects any matching SMC-Mixer name as
  `fw.kind` `stock`; there is no unknown-version allowlist. `installFirmware()` runs `install()` on it. `run.mjs`
  checks it behind a Web MIDI stand-in with the stock firmware's two port pairs.
- `js/release.js`: the release the page installs, the package's `manifest.json` beside the editor
  (`../manifest.json`) or the dry package of `make release` (`build/release/`, for the editor
  served from the repository by `serve.py`); its `install` names `firmware/controller-ble.bin` and
  `firmware/cfg_tool.bin` (`tools/release/release.py`), and each file is checked against the
  manifest's size and SHA-256 before the installer gets it.
- `js/app.js`, the sheets with a real mixer: **Install** (the version M-Vave's firmware names, any; read the backup and save it as a file, `SMC-Mixer backup <version>
  <date> <time>.bin`, and wait; install with a progress bar; then "switch the mixer off and on"
  with its power switch (the battery keeps it running without the cable) and Connect), **Update** and **Go back** (below). A failure says whether anything was written.

**Historical USB runs (controller 36, 2026-11-14):** ✅
`smclog cmd stock check` (ok, pointer `0x41000`) and `stock back` (ok, the mixer restarted on
M-Vave's firmware, `SMC-Mixer_012`); then **the page's Install in Chrome** over it (`earlier`):
M-Vave's firmware found by its name, the backup saved as a file, the area written and read back in
5.8 s, and after a power cycle our 0.1.0 running the saved configuration, the page connected.
✅ **The page's Update** the same day (a test package announcing 0.1.1, `build/updtest/`): STOCK
check and back from the page, the page found M-Vave's firmware again by itself (3.3 s from the
click), the backup, the install (2.5 s); after the power switch our firmware ran the saved
configuration. Unplugging the cable did not restart it (the battery): the sheet now says to use the
switch.
The page's Go back ran over BLE on 2026-11-14 and, as part of Update, over USB on 2026-11-15
and 2026-11-23 (four updates that day, each with a fresh backup and off/on, the saved
configuration kept). V013's OTA-erased space also exercised the empty-area install, but a first
installation on a never-modified mixer remains untested.

**Updating ours** (the owner, 2026-11-14: through M-Vave's firmware, A-15): a card when the
loaded package offers a newer installable version (real release notes are not populated yet).
The update is going back and installing again: our firmware
points the mixer back at M-Vave's (STOCK, [link.md](link.md); "Going back" below), the mixer
restarts on it, the editor finds it again (up to 20 s) and installs the new version as the first
time, over the earlier one ([image.md](image.md), "Installing again": the configuration saved on
the mixer and the panel's state are kept). The sheet: Before (STOCK check; "Save first" when the
mixer plays unsaved changes), Restart on M-Vave's firmware, the backup, Install, switch off and on.
An interruption before the final boot-pointer write leaves M-Vave's firmware starting, and
Install from its notice can rerun the plan. A power failure during sector 0 can require the wired
recovery key; directory/sector-0-last and read checks do not make that write power-loss atomic.
No bootloader of ours is needed for it; the "update mode" state of
the mock (a bootloader of ours, FR-BOOT-01) stays a preview. The real mixer's card shows only when
the release installs something and the firmware has STOCK.

**Going back to M-Vave's firmware** (A-16, the owner, 2026-11-14: no file): keep a copy of the
configuration (M-Vave's firmware does not use it); the editor asks our firmware to check (STOCK
check: M-Vave's area intact, sector 0 pointing at ours) and, on Go back, to point sector 0 back at
M-Vave's area ([image.md](image.md), "Going back"); the mixer restarts on M-Vave's firmware, and
the sheet offers to connect. No backup file and no M-Vave update file: the installation never
touched M-Vave's area. Our area stays in flash unused. ✅ Our firmware's write of sector 0 ran on
the mixer (controller 36, 2026-11-14, `smclog cmd stock back`): M-Vave's firmware started.
✅ The page's own Go back ran over BLE on 2026-11-14 and 2026-11-23, and over USB on 2026-11-15
and 2026-11-23. The sheet runs against devhost too (`serve.py`'s `?devhost`, with
`--boot`): check, Go back, devhost exits as the mixer restarts.

**Firmware-workflow defects found on 2026-11-15, fixed:** R09 (a reopened wizard interfering
with an active write), R07 (Save first confirming too early) and a GATT failure masked by
`DOMException.message` mutation. The corrections are described above; the 2026-11-23 USB and
Bluetooth cycles ran on the corrected editor. Keep one page and connection per mixer while a
firmware job runs.

## Hosting on GitHub Pages

The editor is published on GitHub Pages at https://doctorsolberg.github.io/better-smc-mixer/editor/,
and the firmware as GitHub releases (pre-releases while the version is 0.x).

- **The site** is a release package plus a home page: the site root holds `editor/`,
  `manifest.json` and `firmware/` side by side, as `make release` lays them out
  ([build.md](build.md), "Release packages"), so the published editor reads `../manifest.json`
  and installs the images it names from the same site. Older versions stay available under
  `/vX.Y.Z/`, each a whole package, so going back to an older version of ours means opening that
  version's editor.
- **The editor is static files**: no server of our own. Editing a loaded
  copy needs no device connection, but no service worker or offline-installable shell is built.
  Offline reload/Home Screen availability is not guaranteed by today's browser storage.
- **Talking to the mixer from a browser** needs Web MIDI with SysEx and actual permission;
  Chrome on macOS is the hardware-checked route, not a certification of every browser/OS.
  The current editor has no
  Web Serial/CDC transport; native tools use CDC. Stock BLE installation additionally needs Web
  Bluetooth; it is not BLE MIDI. Safari on macOS/iPadOS lacks this editor's real device route:
  from GitHub Pages an iPad can edit files but cannot reach the mixer, install or update. An iPad
  that talks to the mixer needs a native app (CoreMIDI), the same editor in a web view (A-13).
- **Releases:** GitHub Actions builds a release from its tag (`.github/workflows/release.yml`):
  it builds every image from source, compares the image hashes with those of the candidate that
  was verified on the mixer, and creates a draft pre-release with the package. Pages is deployed
  from the release after the owner publishes it ([build.md](build.md), "Publishing"). `make release` makes the same package locally;
  `js/release.js` reads `../manifest.json` beside the packaged editor (repository fallback
  `../../build/release/manifest.json`), and files are checked by size and SHA-256. Release
  notes in the editor and minimum-editor gating are not implemented. Historical research: the
  GitHub API answers CORS (✅ checked 2026-11-12) but allows 60 requests an hour per address,
  and ⚠️ release downloads are not expected to allow a page to read them (not checked): the
  editor therefore reads the copies on the Pages site, not the release assets.
- **An update of the editor and of the firmware go together:** the package carries one product
  version and the link checks protocol/binary/schema compatibility. Minimum-editor manifest
  enforcement and a cached-editor reload prompt are not implemented.

## Saving and trying

A-12, decided 2026-11-12 (Q-26 with A-7): save by hand by default, so edits can be undone and
presets built without wearing the flash or changing what the mixer starts with; the autosave as a
setting of the mixer; and every change still tried on the mixer at once.

**The model: three copies.**

| Copy | Where | The editor calls it | Changed by |
|---|---|---|---|
| The working copy | the editor (the app's storage, with its undo history) | what you see here | every edit |
| What the mixer plays | the mixer's RAM | **Plays now** | each edit while Linked; **Send** while Separate |
| What the mixer starts with | the mixer's flash | **Starts with** | **Save**, or the autosave when it is on |

Rules:

1. **Linked:** edits, undo and redo are queued for the mixer's RAM automatically; wait for
   confirmation. Small expressible edits use SET without a core restart, otherwise PUT restarts
   it. Names-only edits stay local. These transfers do not themselves save flash; the autosave
   setting can schedule a subsequent save.
2. **Separate:** edits stay in the editor; the mixer keeps playing what it has. **Send** sends the
   working copy (SETs when small and expressible, otherwise PUT with a core restart) and the mixer
   plays it, unsaved. Switching to Linked when the two differ asks "The editor and the mixer differ": **Send
   mine** (the mixer plays what the editor shows), **Take the mixer's** (the editor shows what the
   mixer plays; one undo step brings the editor's back) or **Stay separate**.
3. **Save** (RAM → flash) is a button joined to the state in the capsule whenever the mixer plays
   something it does not start with.
4. **The autosave** is a setting of the configuration (Settings › Mixer): "The mixer
   saves: When I press Save | By itself", the global field `save` (`manual` | `auto`). The mixer
   follows the configuration it plays, so it holds whichever editor is connected; changing it is
   an edit like any other (undo, Linked sends it). By itself: RAM → flash 3 s after the last change
   and touch. **Off by default.**
5. **Go back to the last save…** (the Mixer sheet, after a confirmation) reloads the flash's copy
   into RAM (CFG_REVERT; the factory configuration when nothing is saved) and into the editor, one
   undo step, so nothing is lost.
6. **Power off** with unsaved changes: the mixer starts with the flash's copy. On reconnection the
   editor compares the running content with its working copy and asks as in 2 when they differ;
   mixer assigns the revision (P-6), so an outside write is noticed (R08). Closing the editor
   while the mixer plays unsaved changes, or while edits wait for Send, brings the browser's own
   "Leave the page?" warning; the mixer keeps playing them until it is switched off. A firmware
   update with unsaved changes says the update restarts the mixer and offers "Save first".
7. **One editor at a time** must write to the mixer. This is an operating rule, not enforced
   connection ownership or a read-only lease for a second USB/Bluetooth client.

**The words** (what the user reads, chosen to need no manual):

| The capsule says | It means | Beside it |
|---|---|---|
| Saved | the mixer plays this configuration and starts with it | |
| Sending… | a device transfer is not yet confirmed | |
| Not saved | the mixer plays your changes, but would start without them | **Save** |
| Saving in 3 s / Saving… | the autosave is on and at work, or Save was pressed | |
| Not sent | this editor copy is not confirmed on the mixer, in either mode | **Send** |
| Save failed | the flash write failed; the mixer still plays the changes | **Save** (again) |

Each state has a one-sentence tooltip. The Mixer sheet's "Saving" section shows the two copies on
the mixer side by side, "Plays now" and "Starts with", each with what it holds ("What you see
here", "Not your latest changes", "The last save", "The same") and a line on what changes it; then
Send (Separate only), Save, "Go back to the last save…", and one line saying how the mixer saves
with "Change in Settings": the setting itself lives in Settings › Mixer only. The side panel's
first screen says it in one line: "Changes play on the mixer at once. Save keeps them when the
mixer is switched off." (Linked), "Separate: changes stay here until you Send them to the mixer."

What it takes from the firmware: SET without a restart (P-1), a SAVE command and the storage
mode setting (FR-CFG-05 already lists SAVE), a REVERT command, the "running differs from saved"
state in CFG_INFO, and the revision (P-6). Built on both sides in I-6 (2026-11-13): CFG_SET,
CFG_SAVE, CFG_REVERT, the setting `save` and the storage state `unsaved` ([link.md](link.md),
"Configuration"); `js/mixer.js` sends a change live when the codec can express it in up to eight
SETs, otherwise as a PUT, which restarts the core. Opening a file or pressing Send follows the
same rule, not an unconditional whole upload. The mixer assigns the revision on every accepted
write (P-6), and the editor compares ID and revision before adopting them (R08). Editing a draft
while playing another configuration is Separate, not a third mode. Concurrent writers are not
prevented. Update's Save first waits for the acknowledgement and clean storage (R07).

Open (A-17): should the mixer itself show that it plays unsaved changes (an LED, say), and how?
To try with the editor and the mixer together.

## Schema proposals

Changes to `config/schema.json` the editor needs, decided by the owner but not made yet (all
affected codecs and generated files must agree; no migrations before the first release):

- **`scope`** (A-5): `"config"` (default, omitted), `"preset"` or `"page"`, so an export says what
  it holds and the editor offers the right place to import it. Not written into the binary form.
- **The note names of a preset** (A-11): `"noteNames": "C3"` (default `C4`, omitted), for the
  editor only; the firmware ignores it and the binary form leaves it out. Until then the editor
  keeps it in the browser beside the configuration, as an editor-only setting.

The storage-mode proposal (A-12), global `"save": "manual" | "auto"`, was implemented in I-6 on
2026-11-13 on both sides; default `manual` is omitted. It is no longer a pending schema change.

## One product: the editor and the firmware

The editor (a page in a browser, later an app) and the firmware (on the mixer) are two programs
on two devices that work as one product. A configuration the editor writes is what the mixer
plays; what the editor says a control sends is what the mixer sends; every word the editor shows
about the mixer's behaviour is what the firmware does. Neither side is right on its own, so a
change on one side is checked against the other in the same step.

**What they share** (the contract), and where each part is defined:

| Shared | Defined in | Firmware | Editor |
|---|---|---|---|
| The configuration: fields, ranges, defaults, which fields a type and mode use | `config/schema.json` (D-4) and the files generated from it ([config.md](config.md)) | `firmware/config/`, `core_config.h` | reads `schema.json` at run time; `js/schema.js` (the questions and the checks, after `tools/smcconf`) |
| The file: the canonical JSON text | [config.md](config.md); `smcconf format` | (reads the binary form only) | the writer in `js/config.js` |
| The binary form and its versions | [config.md](config.md); `cfgbin.c` and `tools/smcconf/codec.py`, which must agree | `firmware/config/cfgbin.c` | `js/cfgbin.js` (the owner's choice (a), 2026-11-13: `codec.py`'s rules in JavaScript; `run.mjs` holds it to `codec.py` byte for byte, `vectors/binary.json`) |
| The link: commands, CFG_INFO, the storage state, versions, the SysEx frame (P-4), SET, SAVE, REVERT (P-1, Q-26; protocol 2, I-6); the panel's state, EVENTs and SELECT (P-2, P-3, P-6; protocol 3, I-7); what the mixer sent, SENT and CFG_SENT (P-5; protocol 4, I-7) | [link.md](link.md); "What the editor needs from the firmware" here | `firmware/link/`; the transcript `tools/editortest/vectors/link-v1.json` (`transcripts.py`, against devhost) | `js/link.js` (I-3, I-6: the SysEx frame, CFG_INFO, pull, push, set, save, revert, the retries; checked in Node against the transcript) and `js/mixer.js` (I-5: the mixer behind the mock's interface, ✅ on the mixer 2026-11-13; I-6's live edits, Save, Go back and the setting ✅ on the mixer 2026-11-13; I-7's events, touch selection and the tabs switching the mixer ✅ on the mixer 2026-11-14; P-5's monitor column, its join checked in Node (`link.mjs`, `sentJoin()`), ✅ on the mixer 2026-11-14 over USB, controller 35) |
| Live edits: the records that turn one configuration into another | [config.md](config.md), "Edits" | `cfgbin_patch()`, `ctl_request_edit()`, `core_reconfigured()` | `Codec.edits()` in `js/cfgbin.js` (`run.mjs` holds it to `codec.edits()` byte for byte, `vectors/edits.json`) |
| What a control sends: the bytes of each type and mode, 14-bit pairs, the NRPN order, the channel's selection and the null, note release, program numbers, a button's press and release values, a fader's position mapping, an encoder's first step | [core.md](core.md); SPEC FR-CTL | `firmware/core/` (its `out …` log records) | `js/midi.js`: `messages` with `ParamSelection`, `buttonValue`, `faderValue`, `encoderDelta`, `encoderValue`, `targetKey` (the Sends box, the monitor, the simulator) |
| Firmware: going back to M-Vave's (STOCK: payload, statuses, the reset after), the update's two halves, what the release installs | [link.md](link.md); [image.md](image.md), "Going back", "Installing again"; "Firmware" here; the manifest's `install` (`tools/release/release.py`) | `firmware/store/bootsel.c`, `firmware/link/` (capability bit 10), `controller.c`; devhost `--boot` | `js/link.js` (`stock()`), `js/mixer.js` (`stockCheck`, `backToStock`, `installFirmware`), `js/release.js`, `js/app.js`'s sheets; `run.mjs`, `link.mjs` on the transcript, `live.mjs` against devhost. ✅ USB/BLE installation/update/back runs above, the latest on 2026-11-23 over both with off/on and configuration read-back; R07, R09 and the stock BLE error masking fixed |
| The behaviour the editor names: pickup, acceleration, Shift modes and layers, the navigation buttons, saving | [core.md](core.md); SPEC | `firmware/core/`, `firmware/app/` | `js/labels.js`, the inspector's help |
| Versions: the product's one version (`VERSION`: the firmware, the editor and the package carry the same, [build.md](build.md), "The version"); the protocol's, the binary form's and the schema's | [link.md](link.md) (FR-CFG-07); "Hosting on GitHub Pages" (the manifest) | `app_version.h`: the boot journal, GET_INFO, `bcdDevice`; CFG_INFO | `js/version.js`: the device sheet shows the mixer's firmware and the editor's version; refuses a mixer whose versions it does not know, and says which side to update; a newer file says "update the editor". `tools/release/check_versions.py` (`make quick`) holds every copy of each version to its source, on both sides |

**The rule.** A change to any row is made on both sides in one step, or not at all:

1. The definition first (`schema.json`, [link.md](link.md), [core.md](core.md)), then the firmware,
   then the editor, then both sides' tests.
2. Both sides' checks run (below). One side passing is not enough.
3. The area's document and this table say what changed. A behaviour the editor describes in words
   (`js/labels.js`, help texts) is read again against what the firmware does.
4. A version moves when a change breaks the other side; the editor refuses a mixer whose protocol,
   binary or schema version it does not know (FR-CFG-07). Today's dry manifest records these
   versions and the package editor's version; minimum-editor gating remains a hosting design.
5. The confidence marks hold for the contract too: a part checked on the host is not ✅ until the
   editor and the mixer have run it together.

**The checks today:**

- the firmware side: `make test` (C/Python codecs against each other, the scenarios, the link) and
  `make check` (the device builds);
- both sides together on the host, in `make quick` and `make test` (A-20 decided 2026-11-13: Node
  in the build image, with shared vectors; [integration.md](integration.md), T-1):
  `tools/editortest/vectors.py` writes what the firmware does (smcconf's faults, the core's
  messages for every control of the test, factory and generated configurations, from the
  simulator), and the editor's modules are checked against it in Node: files byte for byte, the
  configurations the editor's edit code writes, the editor's checks naming smcconf's faults, and
  `js/midi.js` against the core's messages, message for message;
- the editor's link client (`js/link.js`) against the transcript of the device's own link code
  (T-2, `tools/editortest/link.mjs` in `run.mjs`): every request byte for byte, every reply read,
  the session's pull, push, save, set, revert and refusals through `LinkClient`;
- on the mixer (`make e2e`, T-3, I-4, the owner's go-ahead per session): the editor's
  configurations pushed and pulled over SysEx (USB MIDI) and the CDC, and the MIDI the mixer sends
  for the probe plan equal to the simulator's, so to the editor's prediction (✅ 2026-11-13,
  controller 31: four configurations, 206 probes each, [integration.md](integration.md));
- the page on the mixer: I-5 checks passed in Chrome over USB on 2026-11-13; the full page
  self-test passed 29/29 on the mixer on 2026-11-23 ([integration.md](integration.md));
- ❓ not checked yet: the help texts against the core's behaviour (T-1 test 4).

**Joining them** is implemented through `js/mixer.js` over Web MIDI SysEx, alongside the mock,
not through a browser CDC client. The [integration layers](integration.md) record I-1..I-7,
host checks and dated physical evidence, and what is still unchecked (reconnection faults,
native routes on other operating systems).

## The editor's code

**Connection lifetime** (2026-11-15): each `MixerDevice` connection has an editor-local epoch.
Disconnect drops queued configuration edits and waiting Save actions, not the configuration
being edited in the page. The old link is closed too: no more retries, queued calls or later
configuration-upload chunks are sent. Already-accepted acknowledgements remain valid. These
rules concern the configuration link, not cancellation of a firmware installation. A late
reply or failure from an old connection cannot change the replacement connection, clear its
busy flag, send old queued edits or perform a waiting Save there. Operations already sent to the
old mixer cannot be undone; after reconnection, its configuration is read and the usual
comparison rules apply. Permission/device choices cancelled by disconnect are not continued.
Poll, edits, Save, Revert, Load, selection and STOCK are covered by deferred-client regression
tests in Node and the browser ([integration.md](integration.md), T-1). The normal page self-test
against `devhost` also passes; these cancellation races have not been deliberately tested on a
physical browser-to-mixer reconnect. Normal wizard reconnection is separate evidence.
Generic MIDI connection errors report that error alone, not unrelated Bluetooth instructions;
the Bluetooth chooser retains its own troubleshooting text.

**Transfer confirmation** (2026-11-15): the capsule shows **Sending…**, not Saved, while a
configuration transfer is unconfirmed. A names-only edit is not described as a device transfer.
An unapplied editor copy is **Not sent** in Linked too, not only Separate. If a device transfer
fails and a full-send fallback also cannot be confirmed, the connection closes: a multi-SET
operation may have applied partly, so reconnect must read the actual configuration before
another delta is computed. A local validation error before any attempt keeps the known device
connection. The editable file stays in the page. Deferred-client tests cover these states and
the current page self-test checks Sending before acknowledgement (29 checks against `devhost`).

`app/editor/`: plain HTML, CSS and JavaScript modules, no build step, no dependencies, no
downloads. It reads `config/schema.json` and `config/factory.json` (or `config/test.json`) at
start, edits them and writes the canonical file. The device is the mixer itself where the browser
has Web MIDI (`js/mixer.js`; Connect asks for MIDI with SysEx; I-5, [integration.md](integration.md),
T-4). Without Web MIDI it remains an offline file editor. The mocked device requires explicit
`?mock` in the address (`app/editor/js/mock.js`, R11): what it plays and what it
starts with (A-12), Save, Go back, the autosave with the 3 s pause, the touch event, the MIDI the
device would send (no pickup, a crude acceleration), a log. Run:

```sh
python3 -I app/editor/serve.py      # then open http://localhost:8765/app/editor/
```

(A page opened as a file cannot read the JSON files; `serve.py` is the standard library's server
without the reverse DNS lookup that hangs `python3 -m http.server` on some Macs.)

With `?devhost` the mixer is the firmware built for the host: `serve.py` starts
`build/host/devhost` for the page (`make -C firmware sim` first) and `js/devhost.js` offers it as
the mixer's two USB ports, in any browser, with or without Web MIDI. Add `&selftest` and the page
runs T-4's checklist on itself and reports in the corner, in the console and in the tab's title
([integration.md](integration.md), "T-4: the real page").

| File | What |
|---|---|
| `index.html`, `app.css` | the layout; Auto, Light and Dark themes; widths from a phone to a desktop |
| `js/schema.js` | the schema's questions (types per kind, which fields apply, ranges, defaults) and the file checks, after `tools/smcconf` |
| `js/config.js` | the configuration being edited, its edits, the Shift layer states, the canonical writer |
| `js/midi.js` | labels, the Sends text and bytes, the mock's messages, decoding for the monitor |
| `js/mock.js` | the mocked device |
| `js/mixer.js`, `js/link.js`, `js/cfgbin.js` | the real mixer model, Web MIDI SysEx client and binary codec |
| `js/flashimg.js`, `js/installplan.js`, `js/stocklink.js`, `js/blestock.js`, `js/release.js` | firmware layout/installation, stock USB/GATT transports and manifest-checked package files |
| `js/devhost.js` | `?devhost`: a stand-in for Web MIDI over a WebSocket to `serve.py`'s devhost |
| `js/selftest.js` | `?selftest`: T-4's checklist, run by the page on its own functions |
| `js/templates.js` | the ready-made templates and the built-in parameter lists; `fromMidiGuide`, MIDI Guide's device format into a parameter list |
| `js/midiguide.js` | MIDI Guide on demand: the index from GitHub's tree of `pencilresearch/midi`, an instrument's CSV files, the 24-hour index cache, the attribution, typed errors |
| `js/learnport.js` | Learn's instrument input: Web MIDI without SysEx, the mixer's own ports excluded, the last choice first, listening stopped on finish or cancel |
| `js/filenames.js` | default names of saved configuration files (`BSM-Config`, plus ` - <scope>` for a preset or page) |
| `js/labels.js` | what the user reads: names and help for types, modes and settings |
| `js/icons.js` | one SVG icon set (20 × 20 strokes in the text's colour), the bottom row's faces |
| `js/app.js` | the UI |

Checked on 2026-11-14 (a browser with `?mock`; measured in the page):

- **Fixed:** the strip numbers above the panel were `--faint` on the panel, 2.05:1 in light and
  3.32:1 in dark at 11 px, and the BT and SHIFT labels 4.26:1 in light: both now 15% toward the
  text colour, 5.2:1 light and 6.7:1 or more dark (WCAG AA asks 4.5:1). The number fields that
  sit in a wrapper with their unit (Settings: encoder acceleration, gesture and hybrid times, the
  planned dead zone and light level) had a `<label for>` that pointed at the wrapper, so a screen
  reader read them unnamed: `field()` now labels the control inside. The planned ("Coming")
  fields of the inspector had no ids, so the same: they have now. (The planned fields were
  removed from the interface on 2026-11-20.)
- **Found fine:** every other button, tab and field has a name (the panel, the inspector for each
  kind of control and the bulk inspector, the list, Settings, the instrument sheet, the mixer
  sheet, the File menu); no other text under 4.5:1 in either theme; no console error on any of
  them; the page does not scroll sideways at 375 px (the panel and the chip row scroll inside
  their own boxes, as designed); dialogs open from the keyboard, take the focus, close on Escape
  and give the focus back to their button; a focus ring on every button; the first load is 15
  requests, 401 KB, 33 ms from `serve.py`; the release package's editor (`build/release/editor/`)
  starts from its own `data/`.
- **Proposals, accepted by the owner and done the same day:** at 375 px the configuration's name
  was cut without an ellipsis (now `text-overflow: ellipsis` while not focused) and the chip row's
  last chip sat half under the Panel/List switch (now the row's edge fades where more chips hide,
  on either side: `fadeChips()`, on render, scroll and resize); the page asked `../../config/`
  before `data/`, two requests that fail on GitHub Pages (now the page asks `../../config/` first
  only when it is served as `app/editor/` in the repository, `data/` first anywhere else).

`factory.json` and `test.json` come back byte for byte through the editor's reader and writer
and pass its checks; a configuration edited through every kind of change (types, NRPN, `div`, each
Shift state, an inverted Shift layer, button flags, fixed channels, a cleared control, preset and
global settings) and its preset and page exports pass `smcconf validate` and `smcconf format
--check`; the checks name the faults `smcconf` names for six broken files. Seen in a browser at
widths from 390 to 1440 pt, light and dark; not on a real iPad.

Checked by hand in the browser on 2026-11-12 (second round): the Linked/Separate switch and touch
selection; the list grouped by kind; learn from four pretend messages (NRPN, CC 14-bit, note, CC);
My templates; note names; the undo history after a reload; the page paste with an offset (NRPN by
LSB, MSB and whole number, out-of-range kept, notes when ticked); the firmware sheets (M-Vave's
firmware, install, update, back to M-Vave's); the MIDI Guide sample mapped onto buttons (a toggle
with its values, a drum note on channel 10).

Checked by hand in the browser on 2026-11-12 (third round): messages above an open sheet, held
while hovered; Settings in the order Mixer, preset; the instrument sheet (search, recent, a load, a
network failure, offline after a reload) and "Onto this page" for a section; both layouts at 1440
and 375 pt; the saving model (Linked edit → "Not saved" → Save → "Saved"; Separate edit → "Not
sent"; back to Linked → the divergence sheet, "Take the mixer's"; "Go back to the last save").

Checked by hand on 2026-11-13 (fourth round, after the owner's review): the folder tabs in light
and dark; the top bar at 1440 and 375 pt; the view switch not moving when the grouping appears;
templates from a preset's menu with "Replace Preset 1"; "Onto this page" for three sections in turn
(kept, skipped, outlined first); the mixer disconnected (no link switch, the notice, Connect, the
divergence sheet on reconnection); the update sheet's layout.

Checked by hand on 2026-11-13 (fifth round, a review of layout and wording): sheets keep their
title (and the settings' tabs) in view while they scroll; the list's grouping on top of the list,
so the chips keep their room at 1024 pt; the four preset tabs at 375 and 768 pt; the top bar in
two lines under 900 pt, the name no longer squeezed out at 768 pt and the buttons no longer
squeezed at 910 pt; the mapping card calm in dark, "Fill n selected controls" only with a
selection; the no-mixer notice calm and put away with ×; the firmware cards holding their action;
help texts that named places that had moved (Shift, note names).

The dated rounds above predate the 2026-11-20 release-readiness changes: they used the made-up
MIDI Guide sample and Learn's simulated messages, both now gone or limited to `?mock`.

Not in the editor yet: the firmware's own log export (the support report is editor-side only),
Web Serial, a service worker/offline shell, a native mobile shell or a
first-run sheet. Planned settings are not shown. Firmware from a local file or folder is
deliberately not offered in the first release (the owner, 2026-11-21: no authenticity check for
local files). MIDI Guide over the network, instrument-port
Learn, the support report and the fresh manifest check are built and tested in Node and the
browser with fakes; none has run with the physical mixer or a real instrument.

## Decisions

The owner's answers to A-1..A-12 (2026-11-12), A-18 and A-19 (2026-11-13). The sections above say what
the editor does now.

| ID | Decision |
|---|---|
| A-1 | **Linked / Separate is an explicit mode**, always visible and one tap to change ("Presets, pages and following the device"). |
| A-2 | Follows A-1: linked, a touch on the mixer selects; separate, only the editor's own clicks select. A touch inside the current selection keeps it, so a group can be tried by hand. |
| A-3 | The panel opens first, then the last view is remembered. The list groups by strip (as the panel) or by kind of control. |
| A-4 | Parameter lists come from the midi.guide database ("Parameter lists from midi.guide"); its names name the mixer's controls when mapping. Shared presets live in a separate community repository, read by the editor, no backend, no accounts; a user's own presets are files they keep. |
| A-5 | A file says what it holds: whole configuration, a preset or a page (`scope`). A schema change, not made yet ("Schema proposals"). |
| A-6 | "Page = page + k" stays a copy made by the editor ("Paste a page with an offset"). |
| A-7 | Bluetooth first. Then P-4, P-1, P-2 and P-3, P-5 and P-6, in that order; all built ("What the editor needs from the firmware"). |
| A-8 | The undo history is kept in the app's storage, per configuration, not in the file. |
| A-9 | Strip buttons stay as they are for now. |
| A-10 | Superseded by the owner on 2026-11-20: planned settings stay in the documentation, not in the interface. |
| A-11 | Learn from the instrument (editor side now; on the mixer later, FR-CTL-09), "Save this preset as a template", and a preset setting for how notes are named (middle C is C4 by default, or C3). No per-control test: trying the control on the mixer does that. |
| A-12 | The saving model of "Saving and trying" as proposed: Linked tries every edit on the mixer at once, Separate keeps a draft until Send, Save writes the flash, the autosave is a setting kept on the mixer and off by default. Names and explanations written for a musician. |
| A-18 | The presets are folder tabs over the work area, which holds the open preset's pages, layers, selection and view ("Layout"). |
| A-19 | The editor works without our firmware, on its own copy, with a notice and Connect / Install, so a configuration can be made with no mixer at all ("Without a mixer"). |

## Open questions

For the owner; each is a choice, the editor shows one side of it.

| ID | Question |
|---|---|
| A-13 | **The iPad**: Safari has no Web MIDI, so the editor on GitHub Pages cannot reach the mixer from an iPad. A native iOS app around the same editor (a web view and CoreMIDI), later or never? Until then the iPad edits files only. |
| A-14 | **MIDI Guide's data**: copied into the site by a scheduled action, read from its repository on demand, or ask MIDI Guide for CORS? And the licence of what the editor copies (CC BY-SA 4.0). **Decided (the owner, 2026-11-20): read the CSV files from the GitHub repository on demand, with CC BY-SA 4.0 attribution** ("Parameter lists from midi.guide"). |
| A-15 | **Updating our firmware**: USB DFU (WebUSB, Chrome and Edge) or the image over the SysEx link into a staging area? Either needs our bootloader (FR-BOOT-01) and a flash write path (C-6). **Decided (the owner, 2026-11-14): (c) through M-Vave's firmware**: ours points the mixer back at it, and the editor installs the new version as the first time ("Firmware", "Updating ours"). No bootloader of ours for now. |
| A-16 | **Going back without the backup file**: should the installer keep M-Vave's original sector 0 in a spare sector of the mixer, so the mixer itself can go back? The owner asked (2026-11-14) whether M-Vave's official update file could serve. It holds sector 0 and the whole stock area (`flash.bin` in the `.fwsc`, [image.md](image.md), "Update containers"; V013's sector 0 equals V012's, ✅ `flashimg.py check`). But the installation never touches the stock area, so the way back needs only the boot pointer back at `0x1000`, which our firmware can compute from sector 0 as it is, with no file. Either way our firmware must write sector 0 (Q-18). **Decided (the owner, 2026-11-14): no file**, the pointer back ("Firmware", "Going back"; [image.md](image.md), "Going back"). |
| A-17 | **Unsaved, on the mixer**: should the mixer show that it plays changes it would not start with (an LED, a blink at power-off), and how? To decide after trying the editor and the mixer together. |
| A-20 | **Checking the editor automatically** ("One product"): (a) allow Node as a host-test tool for the editor only; (b) shared test vectors, files in the repository (a configuration, its canonical text, the bytes each control sends) that the C and Python tests read in `make test` and the editor's tests read too; (c) keep the editor's checks by hand; (d) JavaScriptCore's shell `jsc`, which macOS ships. **Decided (the owner, 2026-11-13): (a) Node, in the build image** (Debian's pinned package, no npm, the tests run in the container on any OS), **with (b)'s shared vectors** ([integration.md](integration.md), T-1). (d) was chosen first the same day and dropped: nothing macOS-only may be a required part. |
