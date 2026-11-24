# Installing, updating and going back

better-smc-mixer is installed from the editor in the browser, over USB or Bluetooth. M-Vave's
firmware stays on the mixer next to ours, so you can go back to it at any time, without a file.

The firmware and the editor were verified on a retail SMC-Mixer bought from a shop, running
M-Vave firmware V012 and V013. Installation, update and going back to M-Vave's firmware were run
over USB and over Bluetooth on that mixer, each including a physical power off and on and a
byte-exact read-back of the configuration. A 37-check end-to-end test and the editor's 29-check
self-test pass on the mixer.

## What you need

- The mixer, running M-Vave's firmware (V012 or V013).
- Google Chrome on macOS, Windows or Android, with permission for MIDI and SysEx.
- **Over USB:** a USB data cable.
- **Over Bluetooth:** Chrome's Web Bluetooth on the same systems. The page's **Install over
  Bluetooth** button finds the mixer; you do not connect it as a Bluetooth MIDI device first.
- The editor, at [https://doctorsolberg.github.io/better-smc-mixer/editor/](https://doctorsolberg.github.io/better-smc-mixer/editor/). The firmware is published beside it, so the
  editor finds it by itself.

USB is the faster route: the backup takes about five seconds and installation about a minute.
Over Bluetooth allow about two minutes for the backup and a few more for installation.

## Before you start

- Save a copy of any configuration you want to keep (**File ▸ Save a copy ▸ Everything**).
- For a Bluetooth installation, disconnect the mixer from Audio MIDI Setup, music apps, phones and
  wireless adapters: it takes one Bluetooth connection at a time.
- Use a stable power supply, keep the computer awake and leave the editor page open until the
  editor says the job is done. Closing the firmware view does not cancel the job; Install, Update
  or Go back opens it again.

## Installing

1. Open the editor. Over USB, press **Connect** and allow MIDI with SysEx. Over Bluetooth, press
   **Install over Bluetooth** and choose the mixer (usually **SMC-Mixer**) in the browser's list.
2. The editor shows that the mixer runs M-Vave's firmware and its version. Choose **Install…**,
   then **Next** on "Check the mixer".
3. Choose **Read and save the backup**. The editor reads the mixer's whole memory (512 KB) and
   saves it as `SMC-Mixer backup …bin` in your downloads. Nothing has been written yet. Keep this
   file, and keep it private: it holds the mixer's settings and Bluetooth pairings.
4. Choose **Next**, then **Install**. Before writing, the installer checks the memory layout:
   M-Vave's firmware must be where it expects it, and the space better-smc-mixer uses must be free
   or hold an earlier better-smc-mixer. If anything does not match, it stops and writes nothing.
5. The installer writes better-smc-mixer beside M-Vave's firmware, reads every part back, and
   last switches the start-up over to it. Keep the power on during this step.
6. When the page says **Switch the mixer off and on**, use the mixer's power switch. Unplugging
   the cable is not enough: the battery keeps the mixer running.

Then connect again. Over USB, press **Connect**; a Mac may ask once whether to allow the new
accessory. Over Bluetooth, connect **Better-SMC-Mixer** as a Bluetooth MIDI device in Audio MIDI
Setup first, then press **Connect** in the editor ([connecting.md](connecting.md)).

## Updating

When a newer version is published, the mixer sheet shows **Update…**. **Firmware ▸ Check for
updates** in the mixer sheet asks for the latest release at any time and says whether a newer
version is available.

Save your configuration on the mixer first: wait for the capsule to show **Saved**, or use
**Save first** in the Update view, which waits until the mixer confirms the save. The mixer then
restarts on M-Vave's firmware for a moment, and the editor installs the new version the same way
as the first time: a fresh backup, writing and checking, then off and on. The configuration saved
on the mixer stays.

Over Bluetooth, the update asks you to **Choose the mixer**: pick M-Vave's **SMC-Mixer** in the
browser's list. If a Mac has connected to it by itself, disconnect it in Audio MIDI Setup first
([connecting.md](connecting.md)).

The editor installs firmware from the release published with it on the same site.

## Going back to M-Vave's firmware

In the mixer sheet, choose **Firmware ▸ Go back to M-Vave's firmware…**. Save a copy of your
configuration if you want it later, then choose **Go back**. The editor checks that M-Vave's
firmware is intact and switches the start-up back to it, and the mixer restarts on M-Vave's
firmware. No file is needed.

better-smc-mixer stays in the memory, unused. To use it again, install it from the editor as the
first time.

## Updating M-Vave's firmware

M-Vave's own updater (SincoOTA, over Bluetooth) still updates M-Vave's firmware once the mixer runs
it again. It removes better-smc-mixer and the configuration saved on the mixer, so save your
configuration as a file first. Afterwards, install better-smc-mixer again
from the editor and send your configuration.

SincoOTA may need two runs. The first sends an update loader and the mixer restarts into it (the BT
light goes dark). Open SincoOTA again, pick the mixer (it may show version 0) and start the update
again. If the phone knew the mixer while it ran better-smc-mixer, the pairing dialog may show
"Better-SMC-Mixer": it is the same mixer, pair it.

## If something stops

- If an installation stops before the last step, the mixer still starts M-Vave's firmware. Run
  the installer again: it picks up where it stopped.
- If an update is interrupted, the mixer waits in update mode and the editor offers **Finish the
  update** when you connect.
- In the rare case the mixer starts neither firmware, for example after power was lost during
  the start-up switch, the backup file and the wired recovery key bring it back
  ([recovery.md](../recovery.md)).
- For help, open the mixer sheet, **Diagnostics ▸ Save a report for support**, and attach the
  report to your question ([editor.md](editor.md), "Support reports").
