# Ideas for later

Features for later releases. The product vision is in [IDEA.md](../IDEA.md), what 0.1.0 does
in [about.md](about.md). Some entries are already requirements for version 1.0
([SPEC.md](SPEC.md)); none of them has a date.

| Idea | What it gives | In the SPEC |
|---|---|---|
| MIDI learn | Configure a control without the app: hold a combination, touch a control, send it a MIDI message from the synth or app; the message's type, channel and number become the control's assignment | FR-CTL-09 (SHOULD) |
| Several messages from one control | One fader or encoder drives several parameters at once, each with its own type, channel, number and range (a reversed one makes a crossfade between two synths) | FR-CTL-08 (up to 4 messages); a MUST of version 1.0, planned after the first release |
| A library of MIDI maps of popular devices | Make a preset for your own synths in a few taps: pick a device, pick its parameters, place them on controls | FR-APP-18 (SHOULD) |
| Transport messages | A button sends MIDI Start, Stop or Continue (system real-time), or an MMC command (Play, Stop, Pause, Record, Rewind, Fast forward), so it runs a drum machine's or sequencer's transport or a DAW's | MMC: FR-CTL-01 (SHOULD); real-time: planned for 0.2.0 |
| The bottom row on each page | The 11 bottom-row buttons get their own assignment on every page, like the strip controls, with a way to make one the same on all pages | FR-MOD-01 ("own assignment on each page"); planned for 0.2.0 |

## Notes

- **MIDI learn.** The editor already learns from an instrument ([user/editor.md](user/editor.md),
  "Learn from the instrument"). On the mixer itself, which combination starts it, and how the
  device shows the learn state without a screen (LEDs), are open. Learning into the Shift layer
  needs a way to say so.
- **Several messages.** The current format stores one assignment per layer. One design keeps a
  bounded pool of extra messages instead of reserving four assignments everywhere; its memory
  budget and the matching configuration-file and firmware changes are still to be decided.
- **Device library.** The editor's **Map instrument** already reads parameter lists on demand
  from [midi.guide](https://midi.guide/), a community database of hardware instruments' MIDI
  implementations, under its CC BY-SA 4.0 terms ([app.md](app.md), "Parameter lists from
  midi.guide"). What remains is a curated set of ready-made presets for popular devices. The
  library belongs to the app, not the firmware.
- **Transport messages.** MIDI has two standard ways. System real-time messages are one byte with
  no channel: Start `FA`, Stop `FC`, Continue `FB` (and the clock `F8`); there is no pause: Stop,
  then Continue. Drum machines, groove boxes and sequencers follow them. MIDI Machine Control is a
  SysEx, `F0 7F <device> 06 <command> F7` (Stop 01, Play 02, Fast forward 04, Rewind 05, Record 06,
  Pause 09), understood by DAWs and recorders. Both are new message types: a change of the binary
  form, the three codecs and the firmware's output. A Play/Stop toggle on one button (Start, then
  Stop) is worth having.
- **The bottom row on each page.** Today the row belongs to the preset: one assignment for all its
  pages (the editor says so above the row). On each page it becomes 44 more controls a preset,
  which takes the largest binary form from 30 KB to about 35 KB, past the 32 KB storage slot: the
  slots grow and the flash layout moves. A configuration of 0.1.x converts by copying the row onto
  every page.
