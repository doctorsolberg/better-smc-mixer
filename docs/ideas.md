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
| A DAW template like the stock firmware's | A built-in template that makes the mixer work with DAWs' Mackie Control scripts as on M-Vave's firmware (Logic, Cubase, Ableton, Reaper, Bitwig): the same messages, the button LEDs set by the DAW | FR-CTL-07 (`remote` LEDs); requested by users, 2026-11-25 |

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
- **A DAW template like the stock firmware's.** Many users want the mixer with DAWs on our
  firmware too (the owner, 2026-11-25). What the stock firmware sends and receives in DAW mode is
  in [hardware.md](hardware.md), "Stock behaviour (captures 04 and 05)" and "Stock DAW mode:
  incoming MIDI (static analysis, 2026-11-25)". Compared with ours:
  - **Already the same:** every message out (R/S/M/SELECT notes 0–31, the bottom row's notes,
    the encoders CC 16–23 on channel 1 as `relativeSignedBit` 1/65, the faders Pitch Bend on
    channels 1–8); a Note On from the DAW lights a button whose assignment is that note
    (`led: value`); our faders send 14 bits where the stock sends 7, which Mackie Control expects
    anyway; the `pickup` fader mode with its encoder LED blink matches the stock's mismatch blink
    (the stock waits about 500 ms after the DAW's last Pitch Bend, ours compares at once).
  - **Missing: LEDs the DAW alone sets.** Our button stores its own value when pressed, so M lights
    at once even if the DAW does not mute the track; the stock lights it only when the DAW says so.
    FR-CTL-07 already has the mode (`remote`); it needs a value of `led`, the target's value from
    incoming MIDI kept apart from the button's own, in the binary form and the three codecs. Other
    configurations do not change.
  - **Not needed:** the Mackie handshake and device query, V-Pot rings, meters, the LCD and the
    displays: the stock firmware has none of them, and the DAWs' scripts work without them.
  - **Different, harmless:** a Note Off from the DAW turns our LED off (the stock ignores it).
  - The template: R/S/M/SELECT and the transport buttons as the stock's notes with `remote` LEDs;
    «, » and the arrows lit while held; encoders CC 16–23, channel 1, `relativeSignedBit`; faders
    Pitch Bend, channels 1–8, `pickup`. Check it on the mixer with a DAW.
  - **Open questions.** Blink on velocity 1 (Mackie's "flash"; the stock lights it solid; we
    have blinking LEDs, and it would serve any configuration)? The stock's extras (`9F 7F 7F` all
    off, Rude Solo on note 115, `B0 00 41`): proposed not to copy. Which DAW to check with, and
    whether the template also needs the stock's 500 ms wait before the fader blinks.
- **A USB host for instruments.** The chip's USB could act as a host and play a synth that has
  only a USB device port, but the mixer's USB socket gives no 5 V (VBUS). An Elektron Digitone II
  checked on 2026-11-25 through an adapter with its VBUS wire removable: not found without VBUS,
  found with it. So a host mode needs 5 V from outside (a Y-cable with a power supply, or a boost
  converter); the SPEC leaves the USB host out (§2).
