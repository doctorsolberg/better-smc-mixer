better-smc-mixer {version}: alternative firmware for the M-Vave SMC-Mixer and its browser editor.

**Install:** open the editor at https://doctorsolberg.github.io/better-smc-mixer/editor/ in Google
Chrome (macOS, Windows or Android), connect the mixer by USB or Bluetooth and choose **Install**.
The editor installs exactly the firmware listed below and checks every file's size and SHA-256.
It saves a full backup of the mixer's memory first; **Go back to M-Vave's firmware** returns the
mixer to its original firmware at any time. Guide:
https://github.com/doctorsolberg/better-smc-mixer/blob/main/docs/user/install.md

## What's new in 0.1.1

The firmware is 0.1.0's with the new version number; the changes are in the editor:

- Copy, Paste and Clear work on the layer you see (Normal or Shift), so one layer can be copied
  onto the other. A Shift layer of its own can have a name of its own.
- The bottom row is labelled as shared by all pages of a preset; Clear page asks whether to clear
  it too.
- Polish: firmware sheets keep their buttons in a footer that is never cut off, faders fit narrow
  screens, the instrument card's MIDI Guide link stays in its box, templates show a short
  Replace, longer button names take two lines, consistent spacing on phones and in dialogs.
- A new home page with pictures of the editor.

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
from source with identical bytes. 0.1.1 was installed over 0.1.0 through the editor over USB, and
the end-to-end test and the self-test passed on it.

## Files

`better-smc-mixer-{version}.zip` holds the editor, the firmware images, their log dictionaries,
the user guide, `manifest.json`, `SHA256SUMS` and the licences. The editor installs
`firmware/controller-ble.bin`.

{images}
