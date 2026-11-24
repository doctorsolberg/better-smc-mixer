# Your first preset

A small example for a hardware synth or a music app: one fader, one encoder and a note button.
The example uses MIDI channel 1, CC 74, CC 71 and note 36. These numbers are examples: check what
your instrument expects in its manual, and keep its audio level low while you try new assignments.

## 1. Connect and keep a copy

Open the editor in Chrome, connect the mixer by USB and press **Connect**
([editor.md](editor.md)). If the mixer still runs M-Vave's firmware, install better-smc-mixer first
([install.md](install.md)).

To start from what the mixer plays, choose **Load from the mixer** in the mixer sheet (or **Take
the mixer's** when the editor asks). Then choose **File ▸ Save a copy ▸
Everything**, give the file a useful name and press **Download**. This file is your backup of the
current setup.

## 2. Prepare one preset

1. Select **Separate** beside the capsule. Edits now stay in the editor until you press **Send**.
2. Select **Preset 1**. Click its tab again to open its menu, choose **Clear preset…**, then
   confirm **Clear**. This empties the four pages and the bottom row of this preset in the editor;
   the other presets stay as they are.
3. Select **Page 1**. The capsule shows **Not sent**.
4. In the preset menu, choose **Channel and Shift…** and check **MIDI channel: 1**. Leave Shift at
   Hold for this example.

## 3. Assign the three controls

Click each control in the picture and set it in the inspector. Leave **Channel** at **Preset
channel (1)**. The **MIDI it sends** box shows the message the control will send.

| Control | Sends | Number | Other settings |
|---|---|---|---|
| Fader 1 | CC | 74 | Behaviour: Pickup; range 0–127 |
| Encoder 1 | CC | 71 | Turning: No acceleration; range 0–127; Starts at 64 |
| M1 | Note | 36 | Press: Momentary; off value 0, on value 127; Send nothing on release unchecked |

M1 sends the note when pressed and releases it when you let go. The octave name of note 36 differs
between instruments; the number is what counts.

Choose ordinary **CC** for the encoder; Relative CC is for instruments that ask for relative
encoders. **Starts at** is the encoder's value until the mixer learns the real one from your
instrument or another control.

With Pickup, a fader waits for the value the mixer knows before it takes over. See
[panel.md](panel.md) for pickup, scale and values that come back.

> If you have the instrument connected to the computer, **Learn from the instrument** fills a
> control in from a knob you move on the synth ([editor.md](editor.md)).

## 4. Send, play and save

1. Press **Send** and wait for **Sending…** to finish. The mixer now plays your preset.
2. Choose **Linked**, then select **Preset 1** and **Page 1**. In Linked mode the mixer switches to
   the preset and page you choose in the editor.
3. Press **Save** and wait for **Saved**. The mixer now starts with this configuration. (With "The
   mixer saves: By itself", it saves a few seconds after the last change.)
4. Save a file copy with **File ▸ Save a copy ▸ Everything**, named for this setup.

## 5. Try it on the instrument

Set the synth or app to receive on channel 1.

- **USB:** in a music app, choose the **SMC-Mixer** port (not **SMC-Mixer Config**). To reach a
  hardware synth with a USB port, use a computer or a USB MIDI host in between.
- **Bluetooth:** connect Better-SMC-Mixer from the app or the wireless DIN adapter
  ([connecting.md](connecting.md)). Keeping the editor on USB leaves Bluetooth free for a phone,
  a tablet or the adapter.

Move the fader, turn the encoder and press M1, at a low audio level. If nothing happens, check the
instrument's MIDI input, channel and CC/note assignments. The instrument's manual says what
CC 74, CC 71 and note 36 do on it.

## Optional: a sound-selection button

Set a button to **Program Change** to select a sound. Every press sends the program on the right;
release sends nothing. The value on the left only decides when the button's light is off. The
editor numbers programs 1–128; some instruments number them 0–127.
