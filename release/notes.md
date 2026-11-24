better-smc-mixer {version}: alternative firmware for the M-Vave SMC-Mixer and its browser editor.

**Install:** open the editor at https://doctorsolberg.github.io/better-smc-mixer/editor/ in Google
Chrome (macOS, Windows or Android), connect the mixer by USB or Bluetooth and choose **Install**.
The editor installs exactly the firmware listed below and checks every file's size and SHA-256.
It saves a full backup of the mixer's memory first; **Go back to M-Vave's firmware** returns the
mixer to its original firmware at any time. Guide:
https://github.com/doctorsolberg/better-smc-mixer/blob/main/docs/user/install.md

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
the end-to-end test (37 checks) and the editor's self-test (29 checks) on the mixer; every
image built twice from source with identical bytes.

## Files

`better-smc-mixer-{version}.zip` holds the editor, the firmware images, their log dictionaries,
the user guide, `manifest.json`, `SHA256SUMS` and the licences. The editor installs
`firmware/controller-ble.bin`.

{images}
