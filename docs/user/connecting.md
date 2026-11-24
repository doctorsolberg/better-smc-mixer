# Connecting

## The editor's browser

The editor connects to the mixer in Google Chrome with Web MIDI on macOS, Windows and Android.
Allow MIDI with SysEx when Chrome asks. Installing over Bluetooth uses Chrome's Web Bluetooth on
the same systems ([install.md](install.md)).

Where a browser has no Web MIDI, the page opens as an **Offline file editor**: you can open, edit
and save configuration files and use templates, and nothing is sent to a mixer. Its capsule says
**Offline**.

## USB

The mixer is a class-compliant USB MIDI device: no driver is needed. It shows two MIDI ports:

| Port | What it is for |
|---|---|
| **SMC-Mixer** | your music: the controls' MIDI out, and MIDI in for the lights and the values |
| **SMC-Mixer Config** | the editor talks to the mixer here; leave it out of your music apps |

Some MIDI monitors show only the device's name, so the two entries can look alike. In music apps,
choose **SMC-Mixer**.

## Bluetooth

With better-smc-mixer, the mixer is a Bluetooth MIDI device named **Better-SMC-Mixer**. Bluetooth
is on the first time; after that the mixer remembers the last setting. A long press of BT turns
it off and on, and its light blinks twice a second while the mixer waits for a connection.

Connect it from the other side:

- **Mac**: Audio MIDI Setup ▸ Window ▸ Show MIDI Studio ▸ the Bluetooth button, then **Connect**
  next to Better-SMC-Mixer.
- **iPhone and iPad**: in your music app's Bluetooth MIDI settings (AUM: the Bluetooth button in
  its MIDI sources), tap Better-SMC-Mixer. The app's MIDI list is the place to connect; the
  system's Bluetooth settings do not list MIDI devices.
- **M-Vave's wireless MIDI adapter** (Bluetooth to a 5-pin DIN jack): it finds and connects to the
  mixer by itself as soon as both are on.

No pairing code is needed. After the mixer restarts, connect it again from the Mac, the app or
the adapter.

With Bluetooth on, the controls send over USB and Bluetooth at once, and MIDI from both reaches
the mixer. Running on its battery with no USB host, the mixer sends over Bluetooth only.

### Configuring and installing over Bluetooth

There are two Bluetooth routes in the editor:

- **Connect** configures better-smc-mixer over Bluetooth MIDI. Connect Better-SMC-Mixer in Audio
  MIDI Setup or the music app first, then press **Connect**.
- **Install over Bluetooth** installs better-smc-mixer on a mixer running M-Vave's firmware. It
  opens Chrome's Bluetooth list, where M-Vave's firmware appears as **SMC-Mixer**.

better-smc-mixer and M-Vave's firmware have separate Bluetooth names and identities, so a Mac or
phone that knows both lists two entries; one of them is offline. The editor's mixer sheet has the
same help under **Bluetooth connections**.

### Not listed?

1. Disconnect the mixer wherever it is connected: Audio MIDI Setup, a music app, another phone or
   tablet, M-Vave's wireless adapter. The mixer takes one Bluetooth connection at a time and is
   not listed while it has one.
2. After going back to M-Vave's firmware, or during an update over Bluetooth, a Mac may reconnect
   to **SMC-Mixer** by itself before Chrome can find it. Disconnect it in Audio MIDI Setup, then
   choose it in Chrome's list.
3. If it reconnects at once or stays missing, forget it and connect again. **Mac:** System
   Settings ▸ Bluetooth ▸ (i) beside it ▸ Forget This Device. **iPhone and iPad:** Settings ▸
   Bluetooth ▸ (i) ▸ Forget This Device, if it is listed there.
