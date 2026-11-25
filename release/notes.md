better-smc-mixer {version}: alternative firmware for the M-Vave SMC-Mixer and its browser editor.

**Install:** open the editor at https://doctorsolberg.github.io/better-smc-mixer/editor/ in Google
Chrome (macOS, Windows or Android), connect the mixer by USB or Bluetooth and choose **Install**.
The editor installs exactly the firmware listed below and checks every file's size and SHA-256.
It saves a full backup of the mixer's memory first; **Go back to M-Vave's firmware** returns the
mixer to its original firmware at any time. Guide:
https://github.com/doctorsolberg/better-smc-mixer/blob/main/docs/user/install.md

## What's new in 0.1.2

- **Light settings** (Settings ▸ Mixer ▸ Lights): the brightness of lit buttons and encoders in ten
  steps, full by default; and a faint glow of the buttons that are off, so you can find them on a
  dark stage, in ten steps too, off by default (the encoders' lights stay dark). While the editor
  is Linked, the mixer follows the slider as you move it; saved like any setting. The LEDs now
  refresh four times as often, so even the dimmest step shows without visible flicker. Needs
  0.1.2 on the mixer: 0.1.1 and earlier keep their lights at full and play the rest of a
  configuration as before.

## What it does

- Four presets of four pages, eight strips each; every fader, encoder and button sends CC, 14-bit
  CC, NRPN, RPN, notes, program change, aftertouch or pitch bend on its own channel.
- A Shift layer for every control; hold, latch or hybrid Shift.
- Fader pickup and scale modes; encoder acceleration, fine steps and relative modes.
- USB MIDI and Bluetooth MIDI at once; the mixer remembers its preset, page and Bluetooth state.
- The browser editor: live editing over USB or Bluetooth MIDI, touch a control to select it, Learn
  from an instrument, parameter lists from the MIDI Guide, files to keep and share, firmware
  install, update and going back.

## Verified

On a retail SMC-Mixer with M-Vave firmware V012 and V013: installation, update and going back
over USB and over Bluetooth, with power off/on and byte-exact read-back of the configuration;
the end-to-end test (37 checks) and the editor's self-test on the mixer; every image built twice
from source with identical bytes. 0.1.2 was installed over 0.1.1 through the editor over USB, the
configuration read back unchanged after power off/on, and the light levels and the glow were
checked by eye in the dark.

## Files

`better-smc-mixer-{version}.zip` holds the editor, the firmware images, their log dictionaries,
the user guide, `manifest.json`, `SHA256SUMS` and the licences. The editor installs
`firmware/controller-ble.bin`.

{images}
