# Ideas for later

Features for later releases. The product vision is in [IDEA.md](../IDEA.md), what 0.1.0 does
in [about.md](about.md). Some entries are already requirements for version 1.0
([SPEC.md](SPEC.md)); none of them has a date.

| Idea | What it gives | In the SPEC |
|---|---|---|
| MIDI learn | Configure a control without the app: hold a combination, touch a control, send it a MIDI message from the synth or app; the message's type, channel and number become the control's assignment | FR-CTL-09 (SHOULD) |
| Several messages from one control | One fader or encoder drives several parameters at once, each with its own type, channel, number and range (a reversed one makes a crossfade between two synths) | FR-CTL-08 (up to 4 messages); a MUST of version 1.0, planned after the first release |
| A library of MIDI maps of popular devices | Make a preset for your own synths in a few taps: pick a device, pick its parameters, place them on controls | FR-APP-18 (SHOULD) |

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
