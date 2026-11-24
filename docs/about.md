# better-smc-mixer

**A $30 controller that thinks like a pro one.**

What better-smc-mixer 0.1.0 does, in a musician's words. How to install and use it: the
[user guide](user/README.md). Why it exists: [IDEA.md](../IDEA.md); the full requirements:
[SPEC.md](SPEC.md).

The M-Vave SMC-Mixer costs about thirty dollars and gives you a lot of hands-on control for it:
eight faders, eight endless encoders, 45 lit buttons, a battery and Bluetooth, in a box that fits in
a backpack. Its stock firmware wastes most of that: it is built around a DAW, has one table of CCs,
a Shift button that changes nothing, and faders that jump every time you switch banks.

better-smc-mixer is alternative firmware that turns those same controls into a smart, universal
MIDI controller. It is made for people who play hardware synths and music apps: drum machines,
multitimbral synths, groove boxes, AUM, Loopy Pro, Drambo, or anything else that speaks MIDI on
your computer, tablet or phone.

---

## What you get

### 16 banks instead of one

**4 presets × 4 pages**, eight strips each: 128 faders and 128 encoders in one small box. Use a
preset for an instrument or a setup, and its pages for groups of parameters.

- Every control has its own MIDI channel, so one preset can play several synths at once. Or set a
  preset's channel once and let its controls follow it: move the whole preset to another channel
  with one setting.
- Shift + arrow jumps straight to a page, Shift + double arrow straight to a preset. No scrolling
  in circles.
- While Shift is held, the current page and preset are lit: you always know where you are.
- The mixer remembers where you left off and comes back there when you turn it on.

### Always in sync with your gear

The mixer listens. Every value that comes back from your synths and apps (a patch change, an
automation lane, a knob turned on the synth itself, a fader moved on the iPad screen) updates the
mixer's own picture of that parameter, wherever it lives: on any page, in any preset, in the normal
layer or under Shift. So whatever you reach for next starts from the value your gear really has:

- an encoder continues from the synth's current value, never from a stale one;
- a button's light and toggle state follow what the device reports;
- a fader knows when it no longer matches, and catches up the way you chose (below).

### Faders that don't jump

You switch pages, or the synth's cutoff changed on its own, and the fader is nowhere near the value
it controls. With the default pickup mode it will not slam the parameter to its position. Pick
how it catches up globally, or override that choice for one fader's assignment:

- **Pickup**: the fader takes over when it passes the current value;
- **Scale**: the value glides smoothly towards the fader;
- **Jump**: the classic way, straight to where the fader is.

Until a fader has caught up, a light on its strip blinks, so you can see where to move it.

### A second layer on every control

Shift is no longer just a button. Every fader, encoder and button can do something else of its own
while Shift is held:

- a fader controls volume, and with Shift the filter cutoff;
- the same fader with its range reversed, for a crossfade;
- an encoder turns into **fine adjustment** by default;
- hold Shift, latch it with a tap, or both.

By default the layer stays put for the whole gesture: let go of Shift in the middle of a move
and the parameter will not hop to another one. The global setting can instead switch it immediately.

### Encoders that speak synth

Most synths don't understand the relative messages of DAW controllers. Here the encoder keeps the
value itself and sends plain absolute CCs, with no jumps when you change pages.

- **Acceleration by speed**: turn slowly for single steps, quickly to sweep the whole range in
  about one turn. Four acceleration levels.
- **Dividers** for parameters with a small range (waveform, octave): one step per 2, 4 or 8 clicks.
- **Large steps** of any size.
- **Relative mode** for DAWs, plugins and devices that prefer it, in the three common encodings.

### Buttons your way

Momentary, toggle or trigger; a note, a CC, an NRPN or another message, with values you choose.
A button's light follows its value, including what comes back from the synth or the app: unmute a
channel on screen, and its mute button on the mixer goes dark.

### Any MIDI message

- 7-bit CC and **14-bit CC** (16 384 values)
- **NRPN and RPN**, for synths where cutoff or tuning needs real resolution
- Pitch Bend, Channel Aftertouch, Program Change, notes
- Any range on any control, reversed too

### Wired or wireless

- **USB MIDI**, class compliant, no drivers: computers, iPads, Android devices, USB MIDI hosts.
- **Bluetooth MIDI** to any compatible device: computers, iPads, phones.
- **Straight to hardware synths without a computer or cables**, through M-Vave's own wireless adapter
  (Bluetooth to a 5-pin DIN MIDI jack).
- When Bluetooth gets busy, a moving fader or encoder sends its newest value instead of every
  stale position in between. Notes and button presses keep their order.

### Setup

- An editor in the browser, at [https://doctorsolberg.github.io/better-smc-mixer/editor/](https://doctorsolberg.github.io/better-smc-mixer/editor/) (Google Chrome with Web MIDI
  on macOS, Windows and Android): a picture of the mixer, touch a fader and it is selected.
  Changes apply at once, so you can try them right away; save them on the mixer when you like
  them.
- **Map instrument**: pick a synth from the MIDI Guide parameter lists and put its parameters on
  the controls with a touch each. **Learn from the instrument**: move a knob on the synth and the
  control takes its message.
- Copy and fill across rows and pages, templates for particular synths.
- The whole setup is one readable file: back it up, move it to another mixer, share templates.
- Installation, updates and the way back to M-Vave's firmware run from the editor, over USB or
  Bluetooth. The installer saves a full backup of the mixer's memory first and checks the memory
  layout before it writes anything; going back needs no file.

### Small things

- The BT button works as before: a long press turns Bluetooth on and off.

---

## Coming soon

Ideas for later releases, without dates. More in [ideas.md](ideas.md).

- **Several messages from one control**: one fader sends up to four messages, each with its own
  channel and range, so it can crossfade between two synths.
- **The editor on iPad and iPhone**, over USB and over Bluetooth.
- **Light settings**: adjustable brightness, and unlit buttons either dark or glowing faintly so
  you can find them on a dark stage. (The lights of the bottom row, BT and Shift glow faintly
  whatever the firmware does: that comes from the board itself.)
- **Learn on the mixer**: teach a control its message from your synth, with no computer.

---

## In numbers

Measured on the mixer; the details are in [hwtest.md](hwtest.md) and [core.md](core.md).

| | |
|---|---|
| Price of the hardware | about $30 |
| Banks | 16 (4 presets × 4 pages) of 8 strips |
| Control scan | 2 500 times a second (every 0.4 ms) |
| Button response | ≈ 5 ms from the press (stock firmware: 40–60 ms) |
| Fader resolution | 1 024 physical positions (≈ 0.1% of the travel); 14-bit messages carry them all, against the 128 steps of a plain CC |
| Faders at rest | send nothing |
| Fader ends | 0 and the maximum are always reached |
| Encoders | ≈ 48 steps a turn |
| Fine adjustment | 1 step on 7-bit parameters, 1/2048 of the range on 14-bit ones |
| Layers | 2 on every control (normal and Shift) |
| Messages per control | 1 for each layer (up to 4: coming) |
| Connections | USB MIDI (class compliant), Bluetooth MIDI, DIN MIDI through the wireless adapter |
