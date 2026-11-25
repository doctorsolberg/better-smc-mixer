# The panel

What each control does with better-smc-mixer, and the layout the mixer starts with.

## Presets and pages

The mixer holds **4 presets of 4 pages**. A page gives each of the eight strips its own fader,
encoder and four buttons (M, S, R and the square select button). The bottom row (Play, Stop, Rec,
◀◀, ▶▶, «, », ▲, ▼, ◀, ▶) belongs to the preset: it does the same on all four pages.

With Shift active (hold it in the factory setting), press:

| Button | ▲ | ▼ | ◀ | ▶ | ◀◀ | ▶▶ | « | » |
|---|---|---|---|---|---|---|---|---|
| Shift + | page 1 | page 2 | page 3 | page 4 | preset 1 | preset 2 | preset 3 | preset 4 |

- While Shift is active, the current page's arrow and the current preset's double arrow are lit; the
  one just chosen blinks fast for a second.
- Switching presets lands on the page you last used in that preset.
- Without Shift these eight buttons are ordinary controls of the preset.
- The mixer remembers the preset and page, and whether Bluetooth was on, when it is switched off.

## Shift

Every fader, encoder and button has a second assignment of its own while Shift is active. The
factory layout leaves it the same as the first, except the encoders, which turn into **fine
adjustment**: one step a click, without acceleration.

By default a move keeps the layer it started in: letting go of Shift in the middle of a fader
move does not switch it to another parameter. A new move starts after a pause of 0.3 s. The
editor can change that pause or choose to switch layers immediately instead.

The editor sets how Shift works, per preset: **hold** (active while held; the factory setting),
**latch** (a press turns it on, another off) or **hybrid** (a short tap without using another
control switches the latch; holding or using another control while held is temporary).

## Faders

The mixer tracks values it sent or received; it cannot read a synth's knob by itself. When it
already knows a parameter value and the physical fader differs:

- **Pickup** (the factory setting): the fader takes over when it reaches the known value.
- **Scale**: movement scales from the known value towards the end of travel until they meet.
- **Jump**: the fader sends its position straight away.

If no value is known yet, the first move sends the fader's position in all three modes. The mixer
learns a value from what it sends and from what the instrument sends back.

While a fader waits to be picked up, the light at its strip's encoder blinks: slowly when the
value is above (move up), fast when it is below (move down). A fader at rest sends nothing.

## Encoders

The factory encoders send absolute values: page changes do not automatically send their new
parameter values. Turning uses the value the mixer knows. If none is known, it starts at the
assignment's **Starts at** value (the middle by default), not a value read from the instrument.
Incoming MIDI can update it.

Acceleration modes take larger steps when turned quickly; Fine, No acceleration, Fixed step and
Slow behave as selected in the editor. Relative CC modes send steps instead of keeping the
instrument's absolute value.

## Buttons

In the factory layout every button sends a note while it is pressed (velocity 127 on press, a
velocity-0 note on release). The editor gives each button a mode: **momentary** (pressed = on),
**toggle** (each press switches) or **trigger** (sends once on press). A button's light shows its
value, including values that come back from the synth or the app.

A **Program Change** button is different: every press sends its on-press program in every mode,
and release sends nothing. Its other program value is the LED-off comparison, not a second
program sent on release. Programs are shown as 1–128 in the editor; some instruments show the
same choices as 0–127.

## Values that come back

The mixer listens to the MIDI it receives over USB and Bluetooth. A value it receives for a
parameter one of its controls sends (same message, channel and number) becomes that parameter's
value, on every page and preset that has it: encoders continue from it, button lights follow it,
faders wait to pick it up.

## The BT button

A **long press** turns Bluetooth off and on. Its light is off while Bluetooth is off, blinks
twice a second while the mixer waits for a connection, and stays lit while a device is
connected. With Bluetooth on, the controls send over USB and Bluetooth at once; MIDI from both
reaches the mixer.

The lights of the bottom row, BT and Shift glow faintly when off, as they do with M-Vave's
firmware: that comes from the board.

**Brightness.** Settings ▸ Mixer ▸ Lights sets how bright the lit buttons and encoders are (ten
steps, full by default) and whether the buttons that are off glow faintly, so you can find them in
the dark (ten steps, Off by default; the encoders' lights stay dark). While the editor is Linked, the
mixer shows each step as you move the slider; like any setting, it stays after a switch-off once it is
saved.

## The factory layout

All four presets are laid out alike; preset *n* sends on MIDI channel *n*. The strip buttons send
the same notes on every page.

| Controls (strips 1–8) | Page 1 | Page 2 | Page 3 | Page 4 |
|---|---|---|---|---|
| Faders | CC 20–27 | CC 12–19 | CC 70–77 | CC 87, 89–95 |
| Encoders | CC 102–109 | CC 110–117 | CC 78–83, 85, 86 | CC 3, 9, 28–31, 118, 119 |
| M buttons | notes 72–79 | | | |
| S buttons | notes 64–71 | | | |
| R buttons | notes 56–63 | | | |
| Select buttons | notes 48–55 | | | |

The CC numbers skip the ones synths commonly use for something else (bank select, modulation,
volume, pan, sustain, the data entry and NRPN numbers).

| Bottom row | Play | Stop | Rec | ◀◀ | ▶▶ | « | » | ▲ | ▼ | ◀ | ▶ |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Note | 80 | 81 | 82 | 83 | 84 | 85 | 86 | 87 | 88 | 89 | 90 |

Learn from the instrument, Map instrument and the other editor tools are described in the
[editor guide](editor.md).
