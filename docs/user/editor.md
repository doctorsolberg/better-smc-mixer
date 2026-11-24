# The editor

The editor is a web page, at [https://doctorsolberg.github.io/better-smc-mixer/editor/](https://doctorsolberg.github.io/better-smc-mixer/editor/), that sets up every control of the
mixer. It connects to the mixer in Google Chrome with Web MIDI on macOS, Windows and Android; allow
MIDI with SysEx when Chrome asks.

Without a mixer, the editor works on its own copy of the configuration: edit, save a copy, use
templates. Where a browser has no Web MIDI, the page opens as an **Offline file editor** for files.

For a first setup step by step, see [first-preset.md](first-preset.md).

## Connect

Plug the mixer in, open the editor and press **Connect**. The capsule at the top shows the
connection and whether your changes are saved; click it to open the mixer sheet, with the
connection, saving, firmware and diagnostics.

If the mixer runs M-Vave's firmware, the editor says so and offers to install better-smc-mixer
([install.md](install.md)).

**Connect** reaches better-smc-mixer over USB, or over Bluetooth MIDI once Better-SMC-Mixer is
connected in Audio MIDI Setup or the music app. The separate **Install over Bluetooth** button is
for installing on a mixer that runs M-Vave's firmware ([connecting.md](connecting.md)).

If the connection drops, your working copy stays in the editor. When you connect again and the
two differ, the editor asks which one to keep: **Send mine**, **Take the mixer's** or **Stay
separate**.

## Linked or Separate

The switch beside the capsule decides how the editor and the mixer follow each other:

- **Linked**: every change plays on the mixer at once. The editor shows the preset, page and Shift
  layer the mixer is on; touch a control on the mixer and it is selected in the editor; choose a
  tab in the editor and the mixer switches to it.
- **Separate**: changes stay in the editor until you press **Send**. The editor and the mixer each
  keep their own preset and page.

## Saving

Sending a change updates what the mixer plays, but the mixer **starts with** the configuration it
saved last:

| The capsule says | It means |
|---|---|
| Sending… | the change is on its way to the mixer |
| Saved | the mixer plays this configuration and starts with it |
| Not saved | the mixer plays your changes, but would start without them: press **Save** |
| Not sent | the editor has changes the mixer does not play yet: press **Send** |

**Go back to the last save…** in the mixer sheet brings back what the mixer saved, and Undo in the
editor takes even that back. The mixer can also save by itself a few seconds after your last
change: Settings ▸ Mixer ▸ "The mixer saves: By itself".

Switching the mixer off with unsaved changes loses them on the mixer; the editor keeps them and
offers to send them when you connect again.

Use one editor at a time. If another app changes the mixer's configuration, the editor notices and
asks you to load the mixer's copy or send yours.

## Mapping an instrument

**Map instrument** in the top bar puts a synth's parameters on the controls. Choose the instrument,
tap a parameter, then tap a control on the panel or touch it on the mixer; the next parameter is
ready at once. **Onto this page** places a whole section (Oscillator, Filter…) in one step.

The instrument lists come from **MIDI Guide**, a community collection of instruments' MIDI
implementations. The editor reads them on demand from MIDI Guide's GitHub repository and credits
them under their licence, CC BY-SA 4.0. An instrument you have opened stays available offline.
The editor also has a few built-in lists.

## Learn from the instrument

**Learn from the instrument**, above "Sends" in the inspector, takes a control's message from your
synth. Connect the instrument to the computer and enable its MIDI output, start Learn, and choose
the instrument's MIDI input in the banner. Then move a knob, a fader or a key on the instrument:
its message goes to the waiting control, then to the next selected one. Undo takes learned
controls back one by one.

## Checking what the mixer sends

The **MIDI** bar at the bottom shows the preset and page the mixer is on and the last message.
Open it for a list of messages with the control behind each one, filtered by **All**, **Sent** or
**Received**.

## Files

- The **File** menu: **Open a file…**, **Templates…**, **Start from the factory settings**, **Save
  a copy** (Everything, This preset or This page) and **Bring in from a file** (a preset or a page,
  into the one you are on).
- An unnamed configuration is saved as `BSM-Config.json`; a preset or a page adds its name to the
  file name.
- **Templates** are whole presets kept in the editor: save one from a preset's menu and start any
  preset from it.
- The `SMC-Mixer backup …bin` file made by the installer is a copy of the mixer's memory, not a
  configuration file. Keep it with your other backups.

## Firmware

The mixer sheet's **Firmware** section has **Check for updates** and **Go back to M-Vave's
firmware…**. When a newer version is published, the sheet offers **Update…**. See
[install.md](install.md).

## Support reports

Open the mixer sheet, expand **Diagnostics**, and choose **Save a report for support**. This
downloads a JSON report with browser and firmware information and the editor's recent log. It
works even if the mixer did not connect. It includes the configuration only if you tick **Include
the configuration open in the editor**. The log can contain MIDI port names, so look it over before
sharing. The editor never sends a report by itself.
