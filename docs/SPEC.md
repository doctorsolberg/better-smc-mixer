# better-smc-mixer — specification

Alternative firmware for the M-Vave SMC-Mixer and a companion app to configure it.

| | |
|---|---|
| Document version | 0.6 (draft) |
| Date | 2026-11-15 (documentation reconciliation; no new product decision) |
| Status | Requirements. Progress and hardware results are not recorded here. Decisions and open questions: §13. Hardware is partly confirmed (§3); stage 0 findings are in [hardware.md](hardware.md), the build environment in [build.md](build.md), logging in [logging.md](logging.md), the configuration format in [config.md](config.md); firmware layout in [firmware/README.md](../firmware/README.md). Stock Shift/BT behaviour comes from the user manual and the decrypted firmware (§3.3) |

## 0. How to read this document

- This document is the single source of requirements. Development, tests and reviews refer to
  requirement IDs (`FR-PG-03`, `LOG-07`, etc.). A requirement changes only by editing this file.
- What is done and what is left is not a requirement, and progress is not recorded here. The
  short "(Implemented …)" notes under some requirements only point to the document that describes
  the implementation.
- Levels: **MUST** — required for version 1.0; **SHOULD** — done if it does not hurt the schedule;
  **MAY** — an idea for later. A requirement without a level is MUST.
- Device facts carry a confidence mark: ✅ confirmed (source given), ⚠️ inference or assumption,
  ❓ unknown.
- Prefixes: `FR` — functional requirements, `NFR` — non-functional, `LOG` — logging and
  diagnostics, `C` — constraints, `D` — decisions taken, `R` — risks, `Q` — open questions.

---

## 1. Goals and non-goals

### 1.1 Goals

1. Turn the SMC-Mixer into a flexible controller for **hardware synths and iOS apps**
   (AUM, Loopy Pro, Drambo, etc.), not for DAWs.
2. **Pages**: control more than 8 tracks or parameter groups, keep values per page and pick up
   non-motorized faders correctly.
3. **Shift layer**: every control has its own behaviour while Shift is held, set in the app
   (a different message, an inverted range, fine adjustment, system actions).
4. **Per-control settings** on the level of dedicated MIDI controllers (message types, 14 bit, ranges,
   acceleration / divider / large-step modes, button modes, multi-commands).
5. **A companion app** that runs on iPad/iPhone and on a computer, over USB and Bluetooth.
6. **Strong diagnostics**: logging, log capture and analysis as the main tool for manual and
   semi-automatic testing on hardware (§8).
7. **Reversibility**: the stock firmware can always be restored.

### 1.2 Non-goals (for version 1.0)

- Mackie Control / HUI emulation for DAWs. (MAY come later as a separate "stock compatibility"
  preset.)
- A native iOS editor and installer in the first public release. These are deferred, not
  rejected: the owner's 2026-11-14 direction is to support firmware operations over Bluetooth
  in the future mobile app as well as the desktop browser (FR-APP-20).
- The chip's audio features, USB HID, USB host.
- Hardware changes (board mods, adding a MIDI jack).

---

## 2. Use cases

| ID | Use case |
|---|---|
| UC-1 | **iPad + AUM over BLE.** 16 AUM mixer channels: page 1 — channels 1–8, page 2 — 9–16. Faders — volume, encoders — pan/sends, buttons — mute/solo. Switching with Shift + arrow (FR-PG-01). After a switch the faders do not jump the volume (pickup). |
| UC-2 | **Multitimbral synth via a USB host box or a BLE→DIN adapter.** Each page is 8 parts on their own MIDI channels; encoders — cutoff (14-bit NRPN), buttons — part mutes. |
| UC-3 | **One synth, a parameter panel.** Encoders with acceleration send absolute CC (synths rarely understand relative). Shift + fader — another CC; Shift + encoder — fine adjustment. |
| UC-4 | **Configuration.** In the app on an iPad the user picks a control on a picture of the mixer, changes its message, tries it right away, saves it to a preset and makes a backup file. |
| UC-5 | **Bridge.** An iPad sequencer sends notes to the mixer over BLE, the mixer forwards them over USB to a host box and a synth (and back). |
| UC-6 | **Debugging.** A tester walks through a test scenario; the tool collects device logs + the MIDI stream + step markers, checks the expectations automatically and produces a report. |

---

## 3. Hardware platform (what is known)

### 3.1 Facts

| Item | Value | Confidence / source |
|---|---|---|
| MCU | JieLi **AC6966C**, BR25 family (AC696N/AC636N), pi32v2 core, 4 Mbit (512 KB) internal flash | ✅ marking "…-66C4" on the photo (`materials/board-photo/Image 2.jpeg`) decoded with kagaimiq's scheme (66/C/4 → AC6966C, 4 Mbit); ✅ chip tag `AC636N` in the UFW header of the official firmware |
| Crystal | 24 MHz | ✅ photo |
| Board | `SK11_V01`, date `250725`, sticker `CX250901` on the back | ✅ photo. The SMC-PAD board is `SK12-V2.1` |
| Faders | 8 non-motorized slide potentiometers | ✅ photo |
| Encoders | 8, endless, no detents, **no push**, no pointer on the knob, no LED rings | ✅ photo, the user, capture 04 (pushing sends nothing). ⚠️ The stock firmware gives 47–48 steps per slow revolution with a divider of 2 — probably a 24-pulse encoder ([hardware.md](hardware.md)) |
| Buttons | 32 strip buttons (□ SELECT, R, S, M × 8) + 11 transport/navigation buttons, which send MIDI in stock; plus **separate BT and Shift buttons** (Shift is below BT), which send no MIDI. 45 in total | ✅ photo, Mixxx mapping, manual and `USRFLASH`; all 45 positions checked by hand on 2026-11-11 ([hwtest.md](hwtest.md), "What the run of 2026-11-11 found") |
| LEDs | 45 button LEDs and 8 encoder LEDs, 53 total | ✅ panel/LED mapping checked on the mixer on 2026-11-11 ([hwtest.md](hwtest.md)). The 13 coloured bottom-row/BT/Shift LEDs also glow faintly in stock; their electrical leakage path is ❓ ([hardware.md](hardware.md)) |
| ICs | 4 × SOIC-16, all **74HC595D**; no analog multiplexers (4051) visible | ✅ photo (`Image 3/4/6/7/8.jpeg`) |
| Power | 3.7 V Li-po **780 mAh** (2025-09-02) on a 2-pin connector; charger — **AiP4056** (linear Li-ion charger, a TP4056 equivalent), NTC; power switch | ✅ photo (`Image.jpeg`, `Image 5.jpeg`) |
| Connectors | USB-C and the switch on the edge; **no physical MIDI OUT** | ✅ photo of the back |
| Radio | BLE (PCB antenna) | ✅ |
| USB VID:PID (stock V012) | `4353:4B4D`, shared by several M-Vave products; V013 uses a different PID | ✅ Mixxx; ✅ device ([hardware.md](hardware.md)). Detect by the service protocol, not a single stock PID |

**Bottom row of buttons**, left to right (✅ panel drawing in the manual; ✅ the same order of
records in the factory `USRFLASH` table, §3.3):

| # | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Button | Play ▶ | Stop ‖ | Rec ● | Rew ◀◀ | FF ▶▶ | Bank « | Bank » | ▲ | ▼ | ◀ | ▶ |
| Role with Shift (FR-PG) | — | — | — | preset 1 | preset 2 | preset 3 | preset 4 | page 1 | page 2 | page 3 | page 4 |

In the rest of this document "double arrows" are buttons 4–7, "arrows" are buttons 8–11 and
"Play" is button 1.

How the stock firmware drives this (static analysis of V012, with panel operation subsequently
checked by our hardware-test firmware; [hardware.md](hardware.md), [hwtest.md](hwtest.md)):
the four 595s form one 32-bit
chain fed by SPI2 with DMA. 16 bits select a column of the **input matrix (16 columns × 4 rows, rows
on PB0–PB3)**, the other 16 bits are the rows of the **LED matrix (16 rows × 4 columns on PA2, PA5,
PC1, PC3; 53 LEDs)**. Row 0 of the input matrix carries the 16 encoder lines, rows 1–3 the 45
buttons (BT and Shift included). ✅ The 8 faders connect directly to ADC inputs; their pin order
was checked on 2026-11-11 ([hwtest.md](hwtest.md), "Faders"). The matrix scan rate limits encoder
reliability (R-4). The charge indicator is hardware (✅
manual: red — charging, green — charged), but **the MCU measures the battery voltage** (✅ manual:
Shift blinks on low battery); whether the MCU sees that charging is in progress is ❓.

### 3.2 Official firmware

- `materials/SMC-Mixer_013.fwsc`, V013 of 2026-09-09. Changes: fader touch CC in DAW mode
  (mechanism ❓ — the faders are not touch-sensitive), a fixed MIDI port conflict with other M-Vave
  products.
- `.fwsc` container: a JieLi UFW image with metadata inserted after every 0x2F bytes of the first
  20 blocks (name `SMC-Mixer_013`). ⚠️ Difference from SMC-PAD: the pad byte after the name is
  `0x40`, not `0x7D` (our `.fwsc` parser must accept both).
- The UFW header is encrypted with the JieLi ENC stream cipher, key `0xFFFF`. File entries start at
  `0x40` with a stride of `0xA0` (offset and size in the UFW; `USRFLASH` also has a flash address):

  | File | Size | What it is |
  |---|---|---|
  | `flash.bin` | 0x24000 | Flash `0x0..0x24000` in physical form. ✅ Sector 0 as stored; from `0x1000` SFC with **chip key `0x980F`** (the same as SMC-PAD) keyed on the CPU address: block key = `0x980F ^ ((0x1E00000 + offset − 0x1000) >> 2)`, 32-byte blocks. The stock area rebuilds byte for byte ([image.md](image.md)); the earlier `0x9C0F ^ (offset >> 2)` only matched odd 4 KB sectors |
  | `flash2.bin` | 0x23B00 | A second variant of the application image (same key; differs from `flash.bin`) ⚠️ purpose unknown |
  | `USRFLASH` | 0x188A → flash `0x7D000` (0x2000 sector) | ✅ Factory control tables for DAW and User modes (§3.3). ENC per 32-byte block with key `0x980F ^ (position in the UFW >> 2)` (transport only; stored plain in flash) |
  | `ota.bin` | 0x2AB1F | Update loaders (strings `ble_ota.bin`, `usb_update2.bin`, `uart_update.bin`…) |
  | `tail.bin` | 0x40 | Service tail |

- The stock firmware is built on the JieLi AC63 SDK (`BTCTRLER-@20210611`, `btstack`, `app_core`)
  with a vendor layer (`JLAISDKsincoaudio`). It has almost no debug strings: the Shift/BT logic
  cannot be recovered from strings and needs a disassembler (Ghidra + ghidra-jieli) — part of R-3.
  Decrypted files are vendor files: local only, never in the repository (C-7).
- The stage-0 device ran V012; the owner updated the stock firmware to V013 on 2026-11-14.
  Its recorded flash map and control tables are in [hardware.md](hardware.md): JLFS directory
  at 0x1000, application up to 0x24000, originally free space 0x25000–0x6DFFF (292 KB),
  `BTIF`/`EXIF`/`USRFLASH` at 0x7B000–0x7EFFF. Our installation uses part of that gap
  ([image.md](image.md)).
- Stock updates: SincoOTA (iOS/Android) over BLE; probably also M-UPGRADE over USB MIDI.

### 3.3 Stock behaviour: MIDI mapping, Shift and BT

Sources: the Mixxx mapping; the user manual (2026-03-23 edition, Appendix B); `USRFLASH`
decrypted from V013 (§3.2). Whatever is not yet checked on the device is marked ⚠️.

**DAW mode** (buttons and encoders — ✅ Mixxx and ✅ `USRFLASH` agree; faders — ✅ Mixxx):

| Control | Message |
|---|---|
| R 1–8 | Note 0x00–0x07, channel 1 |
| S 1–8 | Note 0x08–0x0F |
| M 1–8 | Note 0x10–0x17 |
| □ (SELECT) 1–8 | Note 0x18–0x1F |
| « / » | Note 0x2E / 0x2F (previous / next 8 tracks) |
| Transport | Note 0x5B–0x5F (rew, ff, stop, play, rec) |
| Arrows | Note 0x60–0x63 |
| Encoders | CC 0x10–0x17, relative (0x01 / 0x41) |
| Faders | Pitch Bend, channels 1–8 |

**User mode, factory table** (✅ factory `USRFLASH` in V013; ⚠️ not checked on a device), all on
channel 1:

| Control | Message |
|---|---|
| M / S / R / □ of strip n (n = 0…7) | CC 20+n / 28+n / 36+n / 44+n, values 0–127. ⚠️ Strip 7 □ is CC 51, the same as strip 8 — looks like a bug in the factory table |
| Bottom row, left to right | CC 52–62 |
| Encoders | CC 30–37 (mode ⚠️ not decoded) |
| Faders | CC 40–47 |

The User table can be edited in the vendor's M-Vave app. On the user's device (V012) it was edited
there (✅ the user): each strip has its own MIDI channel and the CC numbers avoid the standard ones.
The table and the `USRFLASH` record format are in [hardware.md](hardware.md). So the stock firmware
accepts table writes from the vendor app; ❓ over which transport and with which messages.

**Shift and BT in stock** (they send no MIDI; the firmware handles them):

| Action | What happens | Source |
|---|---|---|
| BT, long press | Wireless mode on/off. The BT LED blinks while waiting for a connection and stays lit when connected | ✅ manual |
| Shift + ◀ / ▶ (buttons 10–11) | ◀ — DAW (Mackie Control), ▶ — User; while Shift is held the arrow of the active mode is lit. The mode survives power-off (⚠️ saved in VM; without a saved mode the firmware starts in User) | ✅ manual, capture 04; ⚠️ static analysis |
| Shift on its own | Blinks on low battery | ✅ manual |
| Encoder LED | Blinks when the fader does not match the track volume (DAW) | ✅ manual |
| Power-on key combinations, factory reset, MIDI channel change | **None.** Keys are only read by the scan → events → one dispatcher, whose only special cases are BT long press and Shift + ◀ / ▶; the bootloader's long-press reset is off (`RESET=PB01_00_0`) | ⚠️ static analysis of V012 ([hardware.md](hardware.md)); not tried on the device |
| Shift with other controls | Nothing: faders, encoders and buttons send the same with Shift held | ✅ capture 04 |

For wireless MIDI OUT the vendor sells separate adapters: "A" — BLE → DIN (IN/OUT), "B" — a USB BLE
dongle for a computer (✅ manual).

What our firmware keeps (answer to Q-11): the BT long press and the meaning of its LED (§5.5);
the Shift blink on low battery (FR-SH-06); the "fader not picked up" indication on the encoder LED
(FR-PG-04, the same as stock). Switching DAW/User with Shift + ◀ / ▶ is not kept: in our firmware
these buttons select a page (FR-PG-01) and presets replace the modes; a DAW-like mode is §5.9.

### 3.4 Related project: open-smc-pad (a knowledge source, not a dependency)

[fedepaj/open-smc-pad](https://github.com/fedepaj/open-smc-pad) (MIT) is open firmware for the
SMC-PAD **on the same AC6966C chip**. We do not depend on it (D-1); we take facts and approaches
from it. Everything we rely on is re-checked on the mixer and written into our own documents
(`docs/hardware.md` and others).

Facts:

- the stock service SysEx protocol: commands 0x11 name/version, 0x21 erase 4 KB, 0x22 write ≤1 KB
  (raw data; below the VM start it must be SFC-encrypted), 0x23 read (returns decrypted data);
- flash layout, the JLFS directory, the ENC/SFC ciphers (chip key `0x980F` on both, §3.2,
  [image.md](image.md)), the `.fwsc`/UFW format, ROM download mode;
- undocumented behaviour of the BR25 USB controller: write MaxP as 0xFF; the RX FIFO holds two
  packets, so wait for a non-zero length; one buffer per direction; register the interrupt handler
  before enabling the pull-up (their `docs/usb-br25.md`);
- BLE: the address is the MAC mixed with a hash of the GATT database (macOS caches the database per
  address); batching BLE-MIDI messages while the stack queue is not empty (median latency
  757 → 23 ms); a 96 MHz clock instead of 24 (BLE stack calls 9 → 1.3 ms);
- toolchain pitfalls: the IPRA optimization in JieLi clang breaks weak functions
  (`-mllvm -enable-ipra=false`); the macOS "allow accessory" prompt after USB descriptors change.

Approaches:

- installation without opening the case: our image is written into free flash with 0x22, the boot
  pointer is redirected to it, and from there it rewrites the main area;
- a USB DFU bootloader without the SDK (bare metal) with an application CRC check, a crash-loop
  guard (4 crashes in a row within the first 3 s) and safe self-update through a staging area;
- crash records in non-initialized RAM and a `chip_reset` hook; bounded waits in the flash driver;
  a USB driver journal in flash when the host does not configure the device;
- recovery through ROM download mode with an RP2040 key (`hw/jl-usbkey`) — the last line of defence
  (C-6).

---

## 4. Architecture (high level)

### 4.1 Decisions taken

- **D-1. Our own firmware; open-smc-pad is a knowledge source, not a dependency.** The bootloader,
  the platform layer (USB, BLE glue, flash, crash handling), board support and everything above are
  written in our repository. From open-smc-pad we take facts and approaches (§3.4); its code is not
  pulled in — neither as a submodule nor as a copy of the project. If porting a single file with
  adaptations is better than rewriting it, it moves into our code and keeps its MIT notice and a
  link to the source in `NOTICE` (MIT requires this). The one candidate so far, the BR25 USB
  controller driver, was written ourselves instead ([usb.md](usb.md)). The
  only external components are those the chip cannot run without (C-4): the JieLi toolchain, the
  AC63 SDK (for the BLE stack) and TinyUSB (a frozen copy). No Arduino layer. No collaboration offer
  to the author (Q-12).
- **D-2. The logic core is portable.** Model, pages, Shift, pickup, mapping, acceleration — plain C
  (C11) without SDK dependencies. The core builds both for the device and for the host (simulator,
  tests, log replay).
- **D-3. The core is a deterministic state machine.** Input — timestamped events (raw control
  events, incoming MIDI, configuration commands, timer). Output — MIDI messages, LED states, log
  records. The same input gives the same output. This is the basis of bug replay (§8.5).
- **D-4. The configuration schema is described once** (one machine-readable description) and generates: C structures
  and (de)serialization for the firmware, TypeScript types and validation for the app, the JSON
  Schema of the configuration file (FR-APP-21), field documentation.
  (Implemented for the firmware, the files and the docs, [config.md](config.md): the description
  is `config/schema.json`, a format of our own and not a JSON Schema; `tools/smcconf/gen.py`
  writes the C structures, the firmware codec's
  tables, the JSON Schema and the field reference. The current editor is plain JavaScript and
  reads the schema directly; generated TypeScript types remain a design target, not a build step.)
- **D-5. Values are stored per target, not per control.** A target is the identity of a MIDI
  parameter (type + channel + number). Two controls, layers or pages addressing the same parameter
  see one value. Incoming MIDI updates the target's value. This gives correct pickup for the Shift
  layer and pages. Controls that send the same message are otherwise independent: their settings are
  their own, nothing ties them together (the owner, 2026-11-14, keeping D-5).
- **D-6. The app is a shared web UI + a native shell for iOS** (§6.1).
- **D-7. The project is open, MIT licensed.** Third-party code lives only in `third_party/` with its
  own licenses, listed in `NOTICE`.
- **D-8. Preset, page and control names are stored only in the app**, in the configuration file;
  the exchange format is JSON (FR-APP-21).
- **D-9. One firmware for users and for testing** (2026-11-08). There is no separate release build
  without logs or debug functions: what is tested (the scenarios on the device, LOG-31, LOG-35;
  the latency measurements) is what users run, and a user's problem can be logged without
  changing the firmware.
  - Logging is always built in with every level (LOG-08); at run time it is at INFO, and DEBUG or
    TRACE are switched on per module by command. Without a subscriber the records only overwrite
    the RAM ring.
  - Debug functions (input injection, CORE_SETUP, raw input) are off until a command switches them
    on for the session; they change RAM only and a reboot ends them (LOG-34).
  - The hardware-test image (`hwtest`) is not a debug build: it is a separate program for the
    first bring-up of the board.
  - If something must differ for some hosts (for example CDC next to MIDI on iOS, to be checked),
    it becomes a setting, not another build. If flash runs short, TRACE call sites may be left out
    at build time.

### 4.2 Firmware layers

```
┌───────────────────────────────────────────────────────────────┐
│ Config protocol (SysEx)  │  Log transport  │  System actions  │
├───────────────────────────────────────────────────────────────┤
│ Control engine: presets → pages → controls → layers           │
│ target store · pickup · acceleration · buttons · LED logic    │
├───────────────────────────────────────────────────────────────┤
│ Input layer: debounce, encoder quadrature,                    │
│ fader filter        │  MIDI router (USB / BLE, in/out, thru)  │
├───────────────────────────────────────────────────────────────┤
│ Board support: matrix scan, ADC, 595, LED │ storage │ power   │
├───────────────────────────────────────────────────────────────┤
│ Platform (our code): BR25 USB driver + TinyUSB (MIDI+CDC),    │
│ BLE-MIDI glue, flash driver, crashes, clock                   │
├───────────────────────────────────────────────────────────────┤
│ JieLi AC63 SDK: BLE stack (closed libraries), OS, startup     │
└───────────────────────────────────────────────────────────────┘
  DFU bootloader (our code, no SDK) — a separate image below the application
```

Tasks (guideline): input scan on a timer ≥1 kHz; core task; MIDI output task; BLE stack; storage
task (low priority); log upload task (lowest priority).

The control engine (the core) and its simulator: [core.md](core.md).

---

## 5. Functional requirements — firmware

### 5.1 Data model

- **FR-MOD-01.** Hierarchy: *Global settings* → *Presets* → *Pages* → *Strips (8)* → *Controls*.
  Strip controls: fader, encoder, buttons □/R/S/M (the encoders have no push, §3.1). Plus *global controls* (the 11
  transport/navigation buttons) at preset level, with an option "own assignment on each page".
- **FR-MOD-02.** Initial counts: **4 presets × 4 pages** (16 banks of 8 strips), each with direct
  selection (FR-PG-01, FR-PG-06). Expansion comes after estimating how much space is needed and how
  much flash is available (Q-3). So the numbers of presets and pages are firmware parameters reported
  in GET_INFO; the file format and the protocol do not depend on them. The active preset and page
  are remembered and restored at power-on.
- **FR-MOD-03.** Every control has two layers: **Normal** and **Shift**. The data model allows more
  layers later without breaking the format.
- **FR-MOD-04 (SHOULD).** **Page inheritance**: a page can be declared "like page M, but channel +k
  (or CC/note +k)". Covers "16 tracks on 2 pages" with almost no memory and no manual copying.
- **FR-MOD-05.** Preset, page and control names are **not stored** on the device (D-8): they live in
  the app's configuration file. The device stores a configuration ID and a revision number that the
  app uses to find the matching file (FR-APP-24).
- **FR-MOD-06. Factory configuration** (decided 2026-11-08, Q-16): **a preset is an instrument,
  its pages are groups of that instrument's parameters.**
  - Preset k (1–4) is on MIDI channel k, and all its assignments are on the preset's channel
    (FR-MOD-07), so one setting moves the whole preset to another channel.
  - Its 4 pages × (8 faders + 8 encoders) are 64 different CCs, none of them a service CC: no bank
    select, modulation, data entry, volume, pan, expression, LSBs (32–63), pedals (64–69), (N)RPN or
    channel mode messages.
  - All buttons send on the preset's channel too, as keys: Note On on press, the release on
    release, the same on every page. Each button's mode (toggle, momentary, trigger) and message
    (a note, or a CC with its values) can be changed in the preset (FR-CTL-01, FR-CTL-06).
  - The numbers are in [core.md](core.md), "Factory configuration".
- **FR-MOD-07. Channels** (decided 2026-11-08, Q-19): a preset has a MIDI channel; an assignment's
  channel is either fixed (1–16) or "preset", the default: the preset's channel at the time it
  sends. Changing a preset's channel moves its assignments on "preset" and none of the fixed ones.

### 5.2 Control assignment

**FR-CTL-01. Message type.**

| Type | Applies to | Notes |
|---|---|---|
| CC 7-bit absolute | all | the main one |
| CC 14-bit (MSB 0–31 + LSB +32) | fader, encoder | |
| CC relative | encoder | encodings: 2's complement (1/127), offset 64 (63/65), sign-magnitude (1/65) |
| NRPN 14-bit / 7-bit (MSB only) | fader, encoder, button | MSB/LSB address |
| RPN | fader, encoder | SHOULD |
| Pitch Bend 14-bit / 7-bit | all | |
| Channel Aftertouch | all | |
| Poly Aftertouch | all | MAY |
| Program Change | all | encoder — browse programs |
| Note (on/off, velocity) | all | |
| MMC / short SysEx template | buttons | SHOULD; template with a value slot |
| System action | buttons | see FR-ACT |
| None (off) | all | |

**FR-CTL-02. Common fields:** channel: fixed, 1–16, *or* "preset", the preset's channel
(FR-MOD-07; page inheritance, FR-MOD-04, may add an offset to it);
number (CC, note, NRPN MSB/LSB); **lower and upper value** (7 or 14 bit). Lower greater than upper
means an **inverted** control. A default value (for reset and initialization).

**FR-CTL-03. Fader:**
- pickup mode: `jump` / `pickup` (the control takes effect once its physical position crosses the
  target's current value) / `scale` (the output smoothly converges to the fader position) —
  `pickup` by default; a global default plus a per-control override;
- curve: linear / logarithmic (audio) / exponential (SHOULD);
- end calibration: dead zones at the bottom and top so that 0 and the maximum are always reached.

**FR-CTL-04. Encoder — rotation mode:**
- `Acc0`…`Acc3` — no acceleration … maximum (by rotation speed);
- `Div2`/`Div4`/`Div8` — one value step per N clicks (for parameters with a small range);
- `Step2`/`Step4`/`Step6` (and any step N) — large steps (switches over a large range); N (1–127)
  must be given for div and step;
- acceleration by the time per click (Q-20, [core.md](core.md), "Encoder acceleration"): a slow
  click moves the fine step, 1 on 7-bit targets and the range / 2048 on 14-bit ones (or 1, global
  `fine14`), and a fast turn crosses the range in about a turn; a global speed scale (`encAccel`)
  moves it;
- `fine` — no acceleration: the fine step;
- the default mode is `Acc2`; an encoder that has no value yet starts from its `start` value, by
  default the middle of its range (decided 2026-11-08, Q-19);
- **with Shift held — the same target in `fine` mode** (the default; the Shift layer can be any
  assignment instead);
- an encoder in absolute mode takes its value from the target store, so a page change causes no
  jumps (encoders need no pickup);
- (a mode "send values also while the encoder is pushed" does not apply: no push, §3.1.)

**FR-CTL-05. Encoder push — not applicable to the SMC-Mixer:** its encoders have no push (§3.1,
✅). Kept as a record of the feature: like a button (FR-CTL-06) or special functions:
temporary `Acc3` / `Step6` while pushed ("push and turn"), set min / max / default of the
linked encoder. What remains reachable here: setting a control's value to default / min / max as
a button's system action (FR-ACT).

**FR-CTL-06. Button — mode:**
- `momentary` (press = upper value, release = lower value);
- `toggle` (on/off);
- `trigger` (press only);
- `cycle` — SHOULD: each press sends the next value from a list (a waveform switch);
- `radio` — SHOULD: an exclusive group of buttons;
- note-off suppression (lower value 0 → do not send) and an explicit flag;
- long press / double press as separate actions — MAY.

**FR-CTL-07. Button LED:** `local` (from its own state) / `remote` (from incoming MIDI addressed to
this button — any type, not only the stock's hard-wired notes) / `local+remote` / `off` /
`always on`. A blink mode for special states (see §5.5).
(Implemented so far, [config.md](config.md) field `led`: `value` — lit while the target's value is
not `lo`, which is `local+remote`, since the button's own messages and incoming MIDI both reach
the target store (D-5); `off`; `on`. `local` alone, `remote` alone and the blink mode come with
stage 3.)

**FR-CTL-08. Link — several messages from one control:** up to 4 messages, each with its own
type/channel/number/range (one can be inverted → a crossfade between two synths).

**FR-CTL-09 (SHOULD). Learn:** hold a combination, touch a control, send a MIDI message to the
device — the type/channel/number are assigned to the control.

**FR-CTL-10. Note release form**, per preset (decided 2026-11-08, Q-17):
- Note On with velocity 0, `9n nn 00`, as the stock firmware sends it: the default;
- Note Off, `8n nn 00`: an option of the preset.

A release goes out in the form of the preset its press was in. Incoming MIDI accepts both forms.

### 5.3 Shift

- **FR-SH-01.** Shift is **always** the native Shift button (below the BT button); its role cannot
  be reassigned. Modes: hold; latch on press; hybrid (a short press latches, holding is temporary).
  The mode is set **per preset**; the default is hold (decided 2026-11-08). The mode of the preset
  in use governs: changing into a hold preset drops a latched Shift, and a hold preset never keeps
  Shift on after the button is released ([core.md](core.md), "Layers and Shift").
- **FR-SH-02.** For each control the app sets its behaviour with Shift:
  - `inherit` — same as without Shift;
  - `override` — an assignment of its own, written in full: another CC/type/channel, an inverted
    or narrowed range, another encoder or pickup mode;
  - `action` — a system action (FR-ACT);
  - `block` — do nothing.

  Defaults: faders and buttons — `inherit`; encoders — `override` with their Normal assignment in
  `fine` mode (FR-CTL-04).

  The exception is the **8 navigation buttons**: the Shift layer of the double arrows (presets) and
  the arrows (pages) is fixed by the firmware and cannot be edited in the app, so navigation works
  the same in every preset and cannot be broken by a configuration. Without Shift these buttons are
  ordinary configurable controls.
- **FR-SH-03. Shift + fader with another target** (another CC): the fader behaves relative to the new
  target's value under the pickup rules. Example: Normal = CC7, Shift = CC74. After Shift is released
  the fader is in pickup again relative to CC7.
- **FR-SH-04. Shift + fader with the same target and an inverted range:** the output still goes
  through pickup — entering the inverted mode must not make the value jump. If an instant effect is
  wanted, the Shift layer overrides the pickup mode with `jump`.
- **FR-SH-05.** Pressing/releasing Shift while a control moves is a **global setting**:
  - `gesture` (default) — the layer is fixed at the start of a gesture (the first change after rest)
    and does not change until the gesture ends; a gesture ends after a pause (guideline 300 ms,
    configurable);
  - `immediate` — the layer switches at once, even mid-movement.

  Both modes are deterministic and logged (LOG-05).
- **FR-SH-06.** The Shift state is shown on the Shift button's LED and (optionally) sent as a SysEx
  message for scripts in iOS apps. On low battery the Shift
  LED blinks, as in stock (§3.3); while Shift is held it stays lit.

### 5.4 Pages and presets

- **FR-PG-01.** Page switching: **Shift + arrow** ▲ / ▼ / ◀ / ▶ (buttons 8–11 of the bottom row,
  §3.1) → page 1 / 2 / 3 / 4, direct selection, fixed (FR-SH-02). Remotely — with a MIDI message on
  the service channel (Program Change / CC, configurable) and with a configuration command.
- **FR-PG-02.** On a switch: the values of all targets are kept; faders of the new page go into the
  "not picked up" state if their position does not match the target value (within a tolerance);
  encoders continue from the stored values; LEDs show the state of the new page.
- **FR-PG-03.** Indication: while Shift is held, the arrow of the current page and the double arrow
  of the current preset are lit; on a switch the selected button blinks (~1 s). Without Shift these
  buttons' LEDs show the state of their normal assignments. ✅ The navigation LED mapping and
  indication were checked on the mixer ([hwtest.md](hwtest.md), [core.md](core.md), "By hand").
- **FR-PG-04.** "Fader not picked up" indication: while such a fader moves, the LED at its strip's
  encoder blinks (if it is visible through the case); the direction is shown by blink rate or, as a
  fallback, by the S ("move up") / M ("move down") LEDs. Enabled/disabled in global settings.
- **FR-PG-05 (SHOULD).** On a page switch, optionally send a snapshot of the page's values
  (`none` / `all` / `toggle buttons only`), `none` by default.
- **FR-PG-06.** Preset switching: **Shift + double arrow** ◀◀ / ▶▶ / « / » (buttons 4–7 of the
  bottom row) → preset 1 / 2 / 3 / 4, direct selection, fixed (FR-SH-02). Also remotely and from the
  app.
- **FR-PG-07 (SHOULD).** Value snapshots: up to 8 stored value sets of the current
  page, recalled with sending.

### 5.5 System actions (FR-ACT)

Assigned to buttons (usually in the Shift layer; in addition to the fixed navigation of
FR-PG-01/06): select page n / next / previous; select preset; Shift; panic (All Notes Off + Reset
All Controllers on selected channels); page snapshot; reset a control's value to default / min /
max; BLE on/off; show the battery level on the LEDs; learn mode.

By default the BT button keeps its stock role: a long press turns Bluetooth on/off; the LED blinks
while the device waits for a connection and stays lit when connected (as in stock, §3.3). The app
can reassign its short press and its Shift layer; the long press stays with Bluetooth.

### 5.6 MIDI transports and routing

- **FR-IO-01. USB MIDI**, class compliant: a "Performance" port and a "Config/Debug" port (a
  separate USB-MIDI cable, so service traffic does not disturb apps). Port names are stable.
- **FR-IO-02. USB CDC (virtual COM port)** for debugging on a computer. Not needed on iOS.
- **FR-IO-03. BLE-MIDI peripheral** per the Apple specification; connection interval 7.5–15 ms;
  several messages batched into one notification while the stack queue is not empty (the
  open-smc-pad approach, §3.4).
- **FR-IO-04.** Bluetooth on: controller data goes to USB and BLE and arrives from both; Bluetooth
  off: USB alone. The BT long press controls this and the state survives power-off (Q-25,
  superseding the earlier `global.outputs` selector).
- **FR-IO-05 (SHOULD). Bridge:** BLE in → USB out and USB in → BLE out (UC-5), with message type
  filters; off by default.
- **FR-IO-06.** Incoming MIDI from any transport updates the target store and the LEDs (feedback).
- **FR-IO-07.** Universal Device Inquiry (`F0 7E 7F 06 01 F7`) → Identity Reply. The app finds the
  device with it.
- **FR-IO-08.** When a link is overloaded (BLE especially) the rule is **"last value wins"**: for
  continuous controls the queue keeps only the latest value per target. Stale values are not sent.
  Button and note messages are never dropped or reordered.
- **FR-IO-09.** Work with the vendor's BLE→DIN adapter ("Wireless adapter A", §3.3) — a
  path to synths without USB (a MUST since 2026-11-12, the owner: the easy way to hardware synths). ✅ The adapter does not pick the device by name or address
  (one unit, 2026-11-12, [ble.md](ble.md), "The vendor's adapter on the mixer"), so our name may
  differ from the stock one.

### 5.7 Global settings

Default pickup; Shift behaviour during a gesture and the hybrid Shift time (FR-SH-05, FR-SH-01;
the Shift mode and the Note Off form are per preset); the service channel for remote page/preset
selection; persistent Bluetooth on/off with USB always enabled (FR-IO-04/Q-25); bridge
(FR-IO-05); LED brightness (one global level; the
owner, 2026-11-11: per-LED levels are not needed for now) and the unlit LED's look: dark, or a faint
glow so the controls can be found in the dark (a global choice, the owner, 2026-11-11; done in the
scan's LED steps, [hwtest.md](hwtest.md), "LEDs"; the bottom row's, BT's and Shift's LEDs glow
faintly by the board's design, the stock's too, so "dark" darkens the others); pickup indication; BLE
name and BLE on/off; auto power-off/sleep on battery; power-on behaviour (last preset/page);
per-module log levels (§8).

### 5.8 Power-on key combinations

- **FR-BOOT-01.** **Shift + Play at power-on** → enter the DFU bootloader (decided, Q-10). The
  bootloader checks it before starting the application, so it works with a broken application too.
- **FR-BOOT-02.** A combination → reset the configuration to factory (confirmed by holding).
- **FR-BOOT-03.** A combination → safe mode: factory configuration in RAM without writing flash,
  verbose logging.
- **FR-BOOT-04.** Boot and fatal error codes are shown by blinking LEDs (there is no screen).

### 5.9 Stock compatibility (MAY)

A preset that reproduces the stock DAW mode (§3.3), for those who need a Mackie-like mode.

---

## 6. Functional requirements — companion app

### 6.1 Platforms and architecture

- **FR-APP-01.** The main platform is iPadOS/iOS; also macOS and Windows/Linux.
- **FR-APP-02 (D-6).** A shared web UI (TypeScript) + thin shells:
  - desktop — a browser with Web MIDI (Chrome/Edge/Firefox), like the 16n and OpenDeck editors; no
    installation;
  - iOS — a native shell app (Swift, WKWebView) with a bridge to CoreMIDI.

  Why not plain web on an iPad: on iOS every browser, Chrome included, uses the WebKit engine, and
  WebKit has no Web MIDI. (⚠️ Apple allows other engines in the EU, but we cannot rely on that.)
- **FR-APP-03.** Works over both USB and BLE. For BLE on iOS — the built-in BLE-MIDI connection
  dialog.

### 6.2 Features

- **FR-APP-10.** A visual picture of the mixer; a control is selected by click/tap or by physically
  touching it (the device reports which control was touched).
- **FR-APP-11.** An editor of control fields for the Normal and Shift layers; changes are applied
  **right away in the device's RAM** ("change it, try it") while the editor is linked to the
  device, or on "Send" while it is separate; they are written to flash by an explicit Save
  command, or by the device's own save 3 s after the last change and touch (Q-22) when the
  device's storage mode is "auto". Manual is the default; the mode is a setting of the device
  (Q-26; the model: [app.md](app.md), "Saving and trying").
- **FR-APP-12.** Page and preset tabs; page inheritance (FR-MOD-04).
- **FR-APP-13.** Bulk operations: copy a field to all controls of a page/row; fill
  numbers/channels incrementally (left to right and in reverse); copy/paste a control, strip, page,
  preset.
- **FR-APP-14.** Export and import of the configuration to a file: the whole device, a preset, a
  page. For backups, moving between devices and sharing with other users (templates for a specific
  synth or iOS app).
- **FR-APP-15.** Monitor: live control values, current page/Shift, MIDI in/out monitor.
- **FR-APP-16.** Device info: firmware version, build ID, schema version, battery level, free space
  for the configuration.
- **FR-APP-17.** Diagnostics: live device logs, black box download, diagnostic bundle export
  (§8.6).
- **FR-APP-18 (SHOULD).** A template library (CC maps of popular synths and iOS apps).
- **FR-APP-19 (SHOULD).** Offline editing with later sync; detection of configuration divergence
  (a revision counter/hash on the device).
- **FR-APP-20.** The browser editor installs and updates through M-Vave's service transport:
  USB MIDI SysEx, or the stock AE40 GATT service over Bluetooth. Our firmware's STOCK command
  returns to M-Vave's intact area before an update; the installer then runs again (Q-18,
  A-15/A-16 in [app.md](app.md), "Decisions"). No dependency on our unfinished DFU bootloader
  for this path. The owner's 2026-11-14 direction is Bluetooth parity for these operations in
  the future native mobile app too; that app remains deferred beyond the first public release.

### 6.3 Configuration file

- **FR-APP-21. The format is JSON** (UTF-8), convenient both for programs and for hand editing:
  - readable key and value names (`"type": "cc7"`, `"pickup": "scale"`), not numeric codes;
  - stable key order and formatting, so changes read well in diff and git;
  - a `schemaVersion` field; a published JSON Schema (from D-4) for validation and completion in
    editors (VS Code etc.);
  - preset, page and control names live only here (D-8);
  - default values may be omitted: a short file that sets only the differences from the defaults is
    valid too.
- **FR-APP-22.** The app reads files of older schema versions and migrates them to the current one;
  validation errors are shown with the path to the field and a clear message. Migrations start with
  the first public release: until then a format change rewrites the files (decided 2026-11-08), and
  the schema's version stays 1 (Q-29). The formats are kept ready for them now, so that later
  updates of the firmware and the editor work across versions (Q-29).
- **FR-APP-23 (SHOULD).** A command-line tool `smcconf`: `pull`, `push`, `validate`, `diff`,
  `format` — for editing the configuration in a text editor and keeping it in git.
- **FR-APP-24.** Matching with the device: the device stores a configuration ID (UUID) and a revision
  number; the app keeps a library of files and finds the matching one on connection. Without a match
  it shows numbers instead of names and offers to load a file.

---

## 7. Configuration protocol (SysEx)

- **FR-CFG-01.** One protocol over USB MIDI and BLE MIDI. Frame format (Q-28):
  `F0 7D 53 4D <the CDC link record including CRC-16, 7-bit packed> F7`.
  Manufacturer ID — `0x7D` (reserved by the MIDI Association for non-commercial use; fine for an
  open project, D-7; an own ID is needed only for a commercial release).
- **FR-CFG-02.** A size limit for one message (guideline ≤ 256 bytes) and chunking of large data —
  for BLE reliability.
- **FR-CFG-03.** Every command gets an ACK/NACK reply with the same `seq` and an error code; a retry
  with the same `seq` is idempotent.
- **FR-CFG-04.** A whole-configuration upload is a transaction: begin → chunks → commit. Commit
  validates the complete configuration before applying it to RAM. Persistence is a separate Save,
  or the optional autosave (FR-APP-11/Q-26), using two slots with CRC and a generation counter.
  An interrupted upload leaves the working configuration unchanged; an interrupted save leaves
  the previous complete saved configuration available.
- **FR-CFG-05.** Commands (minimum): IDENTIFY; GET_INFO (version, build ID, schema version,
  capabilities, memory, battery); GET/SET global settings; GET/SET preset (in chunks); GET/SET a
  single control (live editing); SELECT preset/page; GET_STATE (current values, page, Shift); SAVE;
  FACTORY_RESET; REBOOT (including into DFU); PING; logging commands (§8.3).
- **FR-CFG-06.** The device reports events on its own (page/preset change, Shift, "control N
  touched") to subscribed clients — for the monitor and for selecting a control by touch.
- **FR-CFG-07.** Versioning: protocol and schema versions in GET_INFO; compatibility rules are kept
  next to the schema; the app refuses to write to a device with an incompatible schema and offers to
  update the firmware or the app.

---

## 8. Logging, diagnostics and log analysis

Principle: **observability is the main way to check behaviour on hardware.** Automated code tests
check the core on the host, but real problems (encoder bounce, fader noise, BLE latency, power,
races in the stack) show up only on the device. So logging is designed **before** the features and
grows with them. What is built so far — formats, tools, ports — is described in
[logging.md](logging.md).

### 8.1 Logging on the device

- **LOG-01. Tokenized records.** Format strings are not stored in the firmware: a record is an event
  ID + binary arguments. The ID → format/module/level dictionary is generated at build time and tied
  to the **build ID** (a hash of the build). Saves flash and CPU time.
- **LOG-02. Record fields:** timestamp (µs since start), sequence number, level
  (ERROR/WARN/INFO/DEBUG/TRACE), module (input, engine, midi-usb, midi-ble, storage, cfg, power,
  boot, …), event ID, arguments, **cause ID** (see LOG-04).
- **LOG-03. A ring buffer in RAM** (guideline 8–16 KB), drained by a low-priority task. On overflow —
  a counter of lost records and a separate "lost N" record.
- **LOG-04. Cause chains.** Every input event (fader movement, encoder click, press, incoming MIDI
  message, configuration command) gets an ID. All records that follow from it (layer/page decision,
  pickup, the MIDI message formed, sending over USB/BLE, LED change) refer to it. The log reconstructs
  a chain like "fader 3 → page 2, Shift → CC 74 ch. 5 = 100 → USB after 0.8 ms, BLE after 14 ms".
- **LOG-05. All state transitions are logged:** page, preset, Shift, pickup caught/lost, USB and BLE
  connect/disconnect, BLE connection parameters, configuration changes, flash writes, power mode
  changes.
- **LOG-06. Metrics (heartbeat)** every N seconds: CPU load, scan cycle time (min/avg/max),
  input→output latency (histogram), USB/BLE queue depths, messages sent/received, log losses, minimum
  task stack headroom, free heap, battery voltage, temperature (if available).
  (Implemented every 10 s and on METRICS_NOW, [logging.md](logging.md) "Metrics": total CPU load
  from the idle share plus our own code's share; stack headroom sampled rather than painted;
  battery and temperature later.)
- **LOG-07. Raw input mode** (enabled by a command): fader ADC values before filtering, encoder line
  states, raw button matrix states with microsecond timestamps. Needed to tune filters and debounce
  and for replay (§8.5).
- **LOG-08. Levels are set at run time** per module (with a configuration command), with a maximum
  level set at build time. In the release build logging is **on** at INFO level — behaviour must not
  differ with and without logs.
- **LOG-09. Budget:** ≤ 5 µs per record for a typical event; logging must not add more than 0.2 ms
  to the input→MIDI latency (checked with the LOG-06 metrics).
- **LOG-10. A black box in flash:** the latest WARN/ERROR records (guideline 4–8 KB, a ring) and a
  **crash record**: reset cause, PC/LR/registers, task, the latest records from RAM. Survives a
  reboot, read with a command, cleared explicitly. Written with flash wear in mind.
  (The RAM part is implemented, [logging.md](logging.md) "Boot journal": a crash record from our
  exception handler and the latest records, kept in RAM across a reset and reported by the next
  boot's journal. The flash black box remains unimplemented; rehearsed recovery under C-6 is
  no longer its outstanding blocker.)
- **LOG-11. Boot journal:** version, build ID, reset cause, results of configuration checks,
  subsystem init times.
  (Implemented, [logging.md](logging.md) "Boot journal": version, build tag, reset cause, the
  previous run's last heartbeat, crash record and latest records, init times on the µs clock;
  BOOT_LOG writes it again. The configuration checks come with the configuration, stage 4.)
- **LOG-12. Log transports:** USB CDC (desktop); a SysEx stream on the Config/Debug port over USB and
  BLE (works on iOS); black box reads through the configuration protocol. Every stream starts with a
  header (build ID, format version, timer frequency).

### 8.2 Host tools

- **LOG-20. The `smclog` CLI tool** (Python):
  - `capture` — collects at the same time: the device log (CDC/USB SysEx/BLE SysEx), all of the
    device's MIDI traffic visible to the host with host timestamps, manual markers from the keyboard
    ("it broke here") and scenario step markers. Host and device clocks are synchronized (periodic
    timestamp exchange);
  - `decode` — decodes with the dictionary of the matching build ID (a mismatch is an error, not
    garbage);
  - `view` — live view with filters by module/level/cause ID;
  - `analyze` — timeline, statistics (latency, jitter, message rates, "noisy" faders), anomaly search
    (repeated equal values, backward encoder steps, sending before pickup);
  - `check` — checks invariant rules on a session (rules in YAML/Python, for example
    "p99 fader→USB latency < 5 ms", "no output from a fader that is not picked up",
    "every button press → exactly one message");
  - `diff` — compares two sessions (before/after a fix);
  - `report` — an HTML report of a session.

  (The names above are the plan. Built so far, [logging.md](logging.md) "Host tool":
  `smclog dict`, `decode`, `capture`, `cmd` (one command over the link), `metrics` (a session's
  heartbeats, with `--check` for NFR-01), `clock` (the device's clock against the host's) and
  `boot` (the boot journal). Not built: `view`, `analyze`, `check`, `diff`, `report`. Scenarios
  are run by another tool, `tools/smcsim/smcsim.py`: LOG-30, LOG-35.)
- **LOG-21. Session format:** a directory with `meta.json` (device, build ID, configuration, app
  version, scenario), `device.jsonl`, `midi.jsonl`, `markers.jsonl` and the raw binary data. The
  format is versioned.
- **LOG-22.** The log dictionaries of every build are archived with the binary (a build artifact).

### 8.3 Logging control through the protocol

LOG_SUBSCRIBE / LOG_UNSUBSCRIBE (transport, levels), LOG_SET_LEVEL (module, level),
LOG_RAW_INPUT (on/off, which inputs), BLACKBOX_READ / BLACKBOX_CLEAR, METRICS_NOW,
INPUT_INJECT (on/off for the session, events; LOG-33).

### 8.4 Testing on hardware: semi-automatic and automatic

- **LOG-30. Test scenarios** are files with steps for a person ("turn encoder 3 slowly 10 clicks to
  the right") and expectations on the log/MIDI for each step.
  `smclog session --scenario pages-basic` shows the steps, places markers, checks the expectations
  after each step and produces a pass/fail report with log excerpts.
  (`smclog session` is not built. The scenario format exists ([core.md](core.md), "Scenario
  files") and has `say` steps for a person; `smcsim run` and `smcsim device` run the machine steps
  (LOG-35). On the device a person's steps are recorded with `smclog capture` and its markers.)
- **LOG-31.** The scenario set is part of the repository; every new feature comes with a scenario.
  Before a release the full set runs on the device over USB and over BLE.
  First matrix (Q-8): **AUM** and **MIDI-capable iOS synths** (AUv3 inside AUM and standalone apps),
  over BLE and USB. Hardware synths come next.
- **LOG-32.** Exploratory testing always runs with `capture` on: any problem found already has a log
  with a marker.
- **LOG-33. Input injection.** A debug command (INPUT_INJECT, over USB CDC and the Config/Debug
  SysEx port) puts input events into the device's input event queue as if the scan had produced
  them:
  - button press, release and long press (by index);
  - encoder steps;
  - fader values;
  - incoming MIDI.

  Injected events take the same path as real ones: dispatcher, core, MIDI out over USB and BLE,
  LEDs, log. Only the scan layer is skipped: the matrix, debounce, encoder decoding and the fader
  ADC. Every injected event is logged as injected, so a log always tells injected input from real
  input.
- **LOG-34. Injection limits.**
  - Off until a command enables it for the current session; off again after a reboot.
  - Changes the configuration in flash only through the configuration commands a scenario sends
    explicitly.
  - Never reaches power-on decisions: the bootloader and the power-on combinations (FR-BOOT) read
    the real keys.
  - When the event queue is full, an injected event is refused and counted, never dropped
    silently.
- **LOG-35. Automatic scenarios.** The LOG-30 scenario format also has machine steps: inject
  events, wait, expect MIDI, log records or LED states.
  - `smclog session --scenario X --auto` runs them without a person: it injects the inputs,
    records the MIDI the host receives and the device log, and checks the expectations.
    (Implemented as `smcsim device`, [core.md](core.md) "Scenarios on the device"; it compares the
    core's MIDI records in the device log and, with `--midi`, the MIDI the host receives. The core
    is set up for a scenario with the CORE_SETUP debug command, RAM only.)
  - The same scenario files run in the simulator (LOG-40) and on the device. A difference between
    the two fails the test: the core on the computer and on the device must agree.
  - Before a release the automatic set runs on the device over USB and over BLE, without a person.
- **LOG-36. What stays manual:** only the scan layer and what the eye sees — the matrix and
  debounce, encoder decoding, fader ADC and filtering, LED brightness. These are covered by short
  human scenarios in raw input mode (LOG-07). Their recordings are replayed in the simulator
  (LOG-41) and join the regression corpus (LOG-42).

### 8.5 Replay

- **LOG-40.** The simulator (the core built for the host, D-2/D-3) writes logs in the same format,
  analysed with the same tools.
- **LOG-41.** A recording of raw inputs (LOG-07) + incoming MIDI from the device can be fed to the
  simulator to get the same MIDI output — a hardware bug is reproduced on a computer.
- **LOG-42.** A regression corpus: saved sessions with expected output run through the simulator in
  CI; a mismatch fails the test.

Decided (2026-11-07): no emulator of the whole chip that runs the firmware image, as
[fm1-emu](https://github.com/joakimeriksson/fm1-emu) does for the M-VAVE FM-1 (a different JieLi
chip with a CPU of the same pi32v2 family). The core is tested in the simulator, everything around
it on the device with injection (LOG-33…35). An emulator would also have to reproduce the chip's
mask ROM and the SDK's closed libraries, which our application depends on.

### 8.6 App logs and the diagnostic bundle

- **LOG-50.** The app logs the protocol exchange (commands, replies, retries, timeouts), errors and
  user actions in the same JSONL format.
- **LOG-51.** "Export diagnostics": one archive with device info, the current configuration, the
  black box, the latest device and app logs, versions. For bug reports.
- **LOG-52.** Logs are never sent anywhere automatically; everything stays local.

---

## 9. Non-functional requirements

| ID | Requirement | Guideline |
|---|---|---|
| NFR-01 | Input → MIDI latency over USB | ≤ 3 ms typical, ≤ 5 ms p99 |
| NFR-02 | Latency over BLE | ≤ 1–2 connection intervals (7.5–15 ms) + batching; median ≤ 25 ms |
| NFR-03 | Encoders | not a single missed or backward step during fast rotation (metric from LOG-07) |
| NFR-04 | Faders | zero messages at rest (hysteresis); 0 and the maximum are reached; fader resolution in 14-bit mode is the real one (≈10-bit ADC, to be measured) |
| NFR-05 | Configuration reliability | a power loss at any moment does not corrupt the working configuration (FR-CFG-04) |
| NFR-06 | Watchdog and crash-loop guard | in our bootloader: after N crashes in a row within the first seconds of running, stay in DFU (the open-smc-pad approach: 4 crashes within 3 s) |
| NFR-07 | Flash wear | values are saved no more often than once per N seconds after a change and on command; a write counter in the metrics |
| NFR-08 | Power | battery life no worse than stock; sleep when idle with no connections |
| NFR-09 | iOS | a USB descriptor with a minimal `bMaxPower` (≤ 100 mA), so an iPad behind an adapter does not refuse power |
| NFR-10 | Stable USB descriptors | our own VID:PID (Q-14); do not change VID/PID/descriptors between versions (macOS on Apple Silicon asks for "allow accessory" again) |
| NFR-11 | Code quality | the core is covered by unit tests on the host; `-Wall -Werror`; static analysis; a reproducible build (Docker) |
| NFR-12 | Reversibility | before the first installation — a full dump of the stock flash, verified by a second read; instructions for returning to stock |

---

## 10. Constraints

- **C-1. Hardware:** no screen; no physical MIDI OUT; non-motorized faders; encoders without rings;
  LEDs only under the buttons and one per encoder → all indication (page, Shift, pickup, battery,
  errors) goes through them.
- **C-2. Chip resources:** 512 KB flash for the bootloader, application, BT data, configuration and
  black box; 153.5 KB SRAM (16 KB of it is instruction cache), part of it taken by the BT stack.
  Budgets are fixed after stage 0.
- **C-3. USB device only:** synths with a USB device port are reached through a USB host box or over
  BLE through a DIN adapter (WIDI, M-Vave MS1, etc.). BLE is peripheral role only (in 1.0).
- **C-4. External components we cannot do without** (everything else is our code, D-1):
  - **The JieLi toolchain** (clang for pi32v2 and packaging tools): binary, Linux x86_64 only,
    downloaded from pkgman.jieliapp.com, redistribution not allowed. There is no open replacement.
    Builds run in Docker; on an Apple Silicon Mac with amd64 emulation (slower, but works).
  - **The JieLi AC63 SDK** (Apache 2.0, the official repository `Jieli-Tech/fw-AC63_BT_SDK`): the BLE
    controller and stack are closed libraries (`btctrler.a`, btstack), plus the OS, startup and the
    linker script. Without them there is no BLE-MIDI (UC-1). Fetched at a pinned commit by the
    environment setup script (`tools/env/fetch_deps.sh`, the commit hash is the check; the
    toolchain archives are checked by SHA-256); not copied into our repository. Pins: [build.md](build.md).
  - **TinyUSB** (MIT): only the needed files (device core, MIDI, CDC, DFU), as a frozen copy in
    `third_party/tinyusb` with its version noted. The alternative, our own minimal USB stack, is not
    chosen: it gives the user nothing, and the risk of enumeration bugs on macOS/iOS is high.
  - The bootloader builds without the SDK — the toolchain and TinyUSB only.
- **C-5. iOS:** no browser on iOS has Web MIDI; DFU from an iPad is impossible. An Apple Developer
  account will be set up later; until then — installation from Xcode with a free profile (a build
  runs for 7 days, then needs reinstalling).
- **C-6. No flash write before a rehearsed recovery.** The last line of recovery (ROM download
  mode) needs an RP2040 board sending the USB key. The procedure must be rehearsed against the
  mixer before relying on it; completed rehearsals and write results are recorded in
  [recovery.md](recovery.md), not assumed absent. Before the prerequisite is met:
  - stage 0 is read-only through the stock protocol (safe);
  - **before the first flash write** (stage 2) an RP2040 board is required (Raspberry Pi Pico or
    similar; our own key firmware `tools/usbkey` (Q-15); an external recovery tool, not part of the
    mixer's firmware)
    together with a tested recovery procedure on it. Procedure and host tool `jlrom`:
    [recovery.md](recovery.md) (rehearsal 1 read-only, rehearsal 2 write).
- **C-7. Legal:** opening the device and replacing the firmware voids the warranty; the project is
  not affiliated with M-Vave or JieLi. The project is open, MIT (D-7), so vendor files
  (`materials/*.fwsc`, flash dumps) are in `.gitignore` and never reach the public repository.

---

## 11. Risks and unknowns

| ID | Risk / unknown | How we resolve it |
|---|---|---|
| R-1 | The service protocol differs on the mixer (port name, commands) | ✅ Read/name commands checked in stage 0; erase/write, sector-0 switching and browser installs later checked on the mixer ([image.md](image.md)). Stock names/PIDs vary by version; discovery must use its service response |
| R-2 | The flash layout differs from the pad (the UFW has `flash2.bin`, `tail.bin`) | ✅ Resolved in stage 0: flash map in [hardware.md](hardware.md) |
| R-3 | Remaining electrical pin-map unknowns beyond the working panel/fader/LED mapping | The panel mapping was checked on hardware; unresolved power/battery, LED leakage and other pins are listed in [hardware.md](hardware.md), "Still unknown". Static analysis and continuity tests must remain distinguished |
| R-4 | The full 16-column matrix scan limits encoder rotation speed | ✅ Short quadrature states and encoder 8's contact dropouts were measured; skipped-state inference was tested, not perfect at every speed. Evidence and optional faster-scan trade-offs: [hwtest.md](hwtest.md) |
| R-5 | Fader conversion noise and ADC sharing | ✅ All eight direct ADC inputs and the filtering/clock changes were checked on hardware. Preserve the scoped idle/load results and SDK-arbitration counters rather than claiming all noise cases solved ([hwtest.md](hwtest.md), "Faders") |
| R-6 | Corrupting the bootloader/sector 0 without a working recovery = a brick | C-6: a rehearsed recovery (the RP2040 key and `jlrom`) before the first write |
| R-7 | Facts from open-smc-pad were found on the pad and may differ on the mixer (some already do: the `.fwsc` pad byte; the chip key first seemed to and does not) | No dependency on the project (D-1); every fact is re-checked on the mixer and written into our documents with a confidence level |
| R-8 | The configuration does not fit in flash | A compact binary schema, page inheritance, a budget after stage 0 |
| R-9 | BLE latency and losses under dense traffic | FR-IO-08, LOG-06 metrics, load scenarios |
| R-10 | The JieLi BLE stack is closed: its bugs cannot be fixed, its behaviour is known only from outside | SDK at a pinned commit; a thin wrapper that logs all stack calls and events (LOG-06); load scenarios |
| R-11 | The vendor's BLE→DIN adapter may connect only to devices with a certain name or service | ✅ The tested adapter accepted different names/addresses and paired without bonding; notes reached it. Continuous fader/encoder traffic and other units still need their own evidence ([ble.md](ble.md), "The vendor's adapter on the mixer") |
| R-12 | The application's board settings come from the SDK's demo board (power mode LDO 1.5 V, pins, long-press reset): wrong values on the mixer may misbehave or harm it | ✅ Checked statically against V012 (2026-11-07, [hardware.md](hardware.md) "Power"): power mode, VDDIO, crystal and charger (off in both) match; the empty application drives no pin. Left: watch current draw and temperature on the first run; the mixer's own pins come with the stage-2 drivers |
| R-13 | The SDK's update paths are compiled into the application (`lib_update_config.c`: a JieLi test box over BLE or Bluetooth LMP, the RCSP phone app, UART). They are a way to rewrite the firmware without our bootloader. The uboot's UART update (pin PB05 in the flash header) may also be a recovery route | ✅ All SDK update modes are off in our application (`firmware/app/config/lib_update_config.c`; the test box handler is no longer registered, `app.bin` 57 → 46 KB). This does not touch the ROM download mode or the uboot. Left: whether the uboot's UART update on PB05 can serve as a recovery route — research with the Pico (C-6) |
| R-14 | The priority-7 RAM scan shares CPU/interrupt time with the closed BLE stack (R-10). The original premise that SDK locks cannot mask priority 7 was disproved on the mixer; flash commands can pause the scan | Recorded scan/flash gaps and BLE advertising, connection and load measurements: [hwtest.md](hwtest.md), "While flash is written", and [ble.md](ble.md). New builds/loads still need scoped measurements; early passing runs are not proof of all timing conditions |

---

## 12. Stages

Each stage ends with checkable criteria. Logging grows from stage 1 on, not at the end.

Progress on each stage (what is done, what is left, what has run on hardware) is not recorded
here.

- **Stage 0 — research (read-only).**
  USB enumeration (descriptors, ports), command 0x11, a full flash dump through 0x23 (two dumps,
  compared), analysis of the flash layout and `USRFLASH` (the factory part is already decoded, §3.3),
  the marking of the fourth SOIC-16 (the other photos exist), an initial pin map from disassembly.
  Checking stock functions on the device: what User mode sends, which arrow selects which mode,
  whether there are power-on combinations (§3.3).
  *Done when:* there is a verified backup and `docs/hardware.md` with a pin map (with confidence
  levels).
- **Stage 1 — tools.**
  Build environment (JieLi toolchain in Docker, the SDK at a pinned commit, TinyUSB in
  `third_party/`), building an empty application and bootloader — without writing to the device; a
  logging skeleton (buffer, tokenization, decoder, `smclog capture/decode`), a core simulator
  skeleton, an RP2040 board and a rehearsed recovery.
- **Stage 2 — running on hardware.**
  Our own bootloader and installer (the §3.4 approach: write into free flash + redirect boot), USB
  MIDI + CDC, a "hardware test" firmware: all inputs and LEDs, raw input mode. Input test scenarios.
  Input injection (LOG-33, LOG-34) and the automatic scenario runner (LOG-35).
- **Stage 3 — core.**
  Data model, pages, Shift, pickup, acceleration, buttons, target store; unit tests and replay in
  the simulator. Its scenarios run automatically both in the simulator and on the device
  (LOG-35).
- **Stage 4 — configuration.**
  Schema (D-4), A/B storage, SysEx protocol, a desktop web editor (MVP: editing a control and pages,
  saving, backup).
- **Stage 5 — BLE and iOS.**
  BLE-MIDI, bridge, iOS shell, logs over BLE, load scenarios.
- **Stage 6 — polish.**
  Learn, links, snapshots, power saving, release packages, user documentation.

---

## 13. Open questions

### 13.1 Open

| ID | Question |
|---|---|
| Q-3 | Can 4 presets × 4 pages be expanded, and at what RAM cost? Current flash and decoded/binary sizes are in [config.md](config.md), "Sizes"; BLE costs and physical versus virtual heap accounting are in [ble.md](ble.md). The protocol buffer is 16 KB, not the full maximum binary form, so larger valid files may exceed a build's upload limit. Do not use the pre-BLE 38-KB heap estimate as today's budget. More presets/pages or decoding only the active preset require an owner decision and matching format/editor/firmware checks |
| Q-10 | Power-on combinations for the configuration reset (FR-BOOT-02) and safe mode (FR-BOOT-03) — to be chosen. DFU is decided: Shift + Play |

### 13.2 Closed

| ID | Decision |
|---|---|
| Q-1 | Shift is always the native Shift button below BT, not reassignable (FR-SH-01). Pages — Shift + arrows, presets — Shift + double arrows, fixed (FR-PG-01, FR-PG-06) |
| Q-2 | Start with 4 presets × 4 pages with direct selection; expand if flash allows (FR-MOD-02, Q-3) |
| Q-4 | Names live only in the app; configuration export/import in a machine-readable format for backup, transfer and sharing (D-8, §6.3) |
| Q-5 | Web UI + iOS shell; the Apple Developer account comes later (D-6, C-5) |
| Q-6 | Configurable; the default is locking the layer for the gesture (FR-SH-05) |
| Q-7 | Open project, MIT (D-7) |
| Q-8 | First AUM and MIDI-capable iOS synths (LOG-31) |
| Q-9 | No stock compatibility mode in 1.0 (DAWs are not a goal; §1.2, §5.9) |
| Q-10 (part) | DFU — Shift + Play at power-on (FR-BOOT-01) |
| Q-11 | Stock Shift and BT functions were found in the manual and the firmware (§3.3). We keep BT (long press), the Shift blink on low battery and the pickup indication; DAW/User switching is replaced by pages and presets. Unverified items are checked in stage 0 |
| Q-12 | No joint maintenance offer to the open-smc-pad author |
| Q-13 | open-smc-pad is not a dependency; our own firmware, taking facts and approaches from it (D-1, §3.4). The only external components are the JieLi toolchain, the AC63 SDK (BLE) and a TinyUSB copy (C-4); for the RP2040 recovery key, Debian's `gcc-arm-none-eabi` (Q-15) |
| Q-14 | Our own USB VID:PID, not the stock `4353:4B4D` (decided 2026-11-07). A PID from pid.codes (free for open projects) is requested once the public repository exists. Until then development builds use the pid.codes test ID `1209:0001`, never a release. Not changed after the first release (NFR-10) |
| Q-15 | The RP2040 USB key runs our own firmware (`tools/usbkey`, decided 2026-11-07): bare metal, TinyUSB for its serial log, no Pico SDK, no Arduino. Built with Debian's `gcc-arm-none-eabi` (pinned) in the build image |
| Q-16 | Factory layout C (decided 2026-11-08): a MIDI channel per preset, a different CC block on each of its pages, 64 parameters per preset, no service CCs; the buttons are keys on the preset's channel (FR-MOD-06) |
| Q-17 | Notes are released as Note On with velocity 0 by default, as in the stock firmware; Note Off (`8n`) is a per-preset option (decided 2026-11-08, FR-CTL-10) |
| Q-18 | **Installation, update and return.** The owner chose the stock service path without extra hardware (2026-11-10/12/14): the browser installs through USB MIDI SysEx, and over Bluetooth through stock AE40 GATT; our STOCK command returns to M-Vave's intact area without a backup file or recovery key (A-15/A-16). The stock update loader at `0x6E000` stays. The RP2040 key and `jlrom` remain the recovery safety net, not a required normal installer. Each device action/write needs the owner's current-session permission, regardless of prior successes. Procedures and dated evidence: [image.md](image.md), [app.md](app.md), "Firmware" |
| Q-19 | The configuration format ([config.md](config.md), decided 2026-11-08): a Shift layer of its own is written in full in files (type required, nothing taken from the Normal layer), besides `"inherit"`/`"block"`; encoders default to `acc2`, and their default Shift layer is the same target in a separate `fine` mode (7-bit: 1 per click; the original 14-bit rule was superseded by Q-20); `n` is required for `div`/`step`; an encoder's `start` (was `default`) defaults to the middle of `lo`..`hi`; a preset has a channel and each assignment a fixed channel or `"preset"` (the default), stored on the device and resolved when sending (FR-MOD-07); no migrations before the first public release (FR-APP-22) |
| Q-20 | Encoder acceleration (decided 2026-11-11; [core.md](core.md), "Encoder acceleration"): (a) a slow click on a 14-bit target is the range / 2048, (b) `fine` is the same step, (c) the curves in [core.md](core.md), where a fast turn crosses the range in about a turn, (d) constants in the firmware plus two global settings, `encAccel` (speed scale) and `fine14` (`range` or `lsb`) |
| Q-21 | How 14-bit values go out (decided 2026-11-11; [core.md](core.md), "NRPN and RPN"): (a-1) every value sends its MSB and then its LSB (CC 6/38; CC 14-bit n/n + 32); (b-1) the NRPN/RPN parameter number (CC 99/98, 101/100) goes before a value when the channel's selection is another parameter, at the first value of each gesture and at every button press; (c-3) the null parameter (RPN 127/127) when a gesture that sent NRPN or RPN ends is a setting per preset, `paramNull`, off by default; (d-1) no deadband on 14-bit targets: every position the scan reports goes out |
| Q-22 | Storing the configuration (decided 2026-11-11; [config.md](config.md), "Storage"; [link.md](link.md), "Autosave"): (a-1) two slots of our own, 2 × 32 KB (at `0x5E000..0x6DFFF` first; at `0x25000..0x34FFF` since 2026-11-12, Q-23), the binary form with CRC-32 and a header written last with a generation; start takes the newest complete configuration that checks, else factory. (b-3) the original always-on autosave was superseded by Q-26: manual Save is the default, and optional autosave waits 3 s after the last change and touch, proceeds step by step and pauses while the panel is in use. Only protocol-received configurations are saved, never CORE_SETUP. The 16 KB transfer buffer is reused for encoding a save. First-write evidence is in config.md, "The first run"; it is not a pending C-6 rehearsal |
| Q-23 | Flash for Bluetooth (decided 2026-11-12; [ble.md](ble.md), "Flash"): with the BLE stack `app.bin` is 156 KB (140 KB without pairing) and did not fit below the slots at `0x5E000` (109 KB). The configuration's two slots move into the free gap below our area, `0x25000..0x34FFF`; our VM runs from the area's end up to the stock update loader at `0x6E000`, which stays (room for `app.bin`: 175 KB). Not taken: the slots over the stock loader, or 2 × 16 KB slots. Reads there go through the SDK's raw path (ble.md); ✅ saves and reads ran there on the mixer the same day (config.md, "At the new place"). The saved configuration is pushed again after the first install with the new place |
| Q-24 | **BLE bonding.** Decided (the owner, 2026-11-13): pairing Just Works when a central asks, no bonding, no keys in flash. macOS and iOS ask no pairing; the vendor's adapter asks for encryption on every connection and works without stored keys (✅ controller 28, [ble.md](ble.md), "Without bonding on the mixer"). The security manager cannot be left out: the SDK crashes at start without it (✅ controller 27, ble.md, "Without the security manager"). ❓ Android not tried yet. |
| Q-25 | **Bluetooth at power-on, and the BLE name.** Decided (the owner, 2026-11-12): the BT button's long press turns Bluetooth on and off as in the stock firmware; on, controller data goes to USB and BLE and comes from both; off, USB alone (FR-IO-04; this replaced the `global.outputs` setting of controller 22). After power-on Bluetooth is as it was left, kept in flash together with the selected preset and page (✅ 2026-11-13, controller 29, a power-off included; [config.md](config.md), "The panel's state"). The BLE name is "Better-SMC-Mixer", so a host's list tells it from the stock firmware (controller 28). [ble.md](ble.md), "Bluetooth on and off" |
| Q-26 | **FR-APP-11 and the Save command.** The owner approved A-12 on 2026-11-12 ([app.md](app.md), "Saving and trying"): edits are tried in RAM when linked; Separate keeps a draft until Send; explicit Save writes startup configuration; autosave after a quiet pause is optional and off by default. This supersedes Q-22's always-on autosave/no-SAVE design. CFG_SAVE, CFG_REVERT, storage mode and the running-versus-saved state belong to the same model, on both sides; implementation and verification are tracked in [integration.md](integration.md), "I-6", not as a pending decision here |
| Q-28 | **The SysEx frame of the configuration protocol** (P-4, FR-CFG-01; [link.md](link.md), "Over SysEx"). FR-CFG-01 fixes `0x7D`. Proposed (a): `F0 7D 53 4D <the CDC link's record and its CRC-16, 7-bit packed> F7`: a 2-byte device tag "SM", then the same records, `seq` and retries as on the CDC, so the device and the tools reuse their record code and only the framing differs; at most 227 bytes a frame. Alternatives: (b) the SPEC's literal order `<tag> <cmd> <seq> <payload> <crc>` with a 7-bit `seq` (a second record format); (c) no device tag. **Decided (the owner, 2026-11-13): (a), with the tag.** |
| Q-29 | **Versions before the release, and migrations after it** (the owner, 2026-11-13). Until the first public release the schema's version stays 1 and a format change (such as the global field `save` of Q-26) rewrites the files and tests (FR-APP-22). Migrations must be provided for, so that later updates of the firmware and of the editor cause no trouble: each format carries its version (the file's `schemaVersion`, the binary form's version, the configuration protocol's), and the plan for the release says how a file, a saved configuration in the mixer's flash and an editor and a firmware of different versions are carried across ([config.md](config.md), "Versions and migrations") |
| Q-27 | **Live editing in the protocol** ([app.md](app.md), "What the editor needs from the firmware", P-1..P-6). The owner's chosen order (2026-11-12): Bluetooth first, then P-4 (SysEx), P-1 (SET without restarting the core), P-2/P-3 (events and SELECT), P-5/P-6 (named output and identity/revision). Whole-configuration upload is no longer the only edit route. Implementation: [integration.md](integration.md) |

---

## Appendix A. Feature list → decisions for the SMC-Mixer

| Feature | Decision |
|---|---|
| Encoder message types: relative CC (two's complement, offset 64), absolute CC, Program Change, 14-bit CC (MSB/LSB), Pitch Bend, Aftertouch, Note, NRPN | Take all (FR-CTL-01) + sign-magnitude relative, RPN |
| 7-bit mode for Pitch Bend / NRPN (MSB only) | Take |
| Encoder modes: dividers 2/4/8, acceleration levels 0–3, large steps 2/4/6 | Take (FR-CTL-04), any step size |
| Shift held → fine adjustment without acceleration | Take as the default behaviour of the encoder's Shift layer |
| Encoder push: strong acceleration or a large step while pushed | Not applicable: the mixer's encoders have no push (§3.1, FR-CTL-05) |
| Button types: Off, Note, CC, Program Change, Pitch Bend, Aftertouch, group and setup selection, acceleration and large step for an encoder, Min, Max; momentary / toggle | Take; group/setup selection = our page/preset; add trigger, cycle, radio |
| Lower > upper = inversion; lower = 0 → no note-off; upper = 0 → inverted indication | Take the conventions + explicit flags in the app |
| Link: a chain of controls, several messages from one | Take as "up to 4 messages per control" (FR-CTL-08) |
| Setups × groups × controls; the last group is remembered per setup | Our presets × pages; the page is remembered per preset |
| Stored value sets, a snapshot (send all values of a group) | FR-PG-07, FR-PG-05 |
| Feedback: incoming MIDI updates values, "no jumps" | FR-IO-06 + target store (D-5) |
| Learn mode | FR-CTL-09 |
| Copy/paste/fill (two fill directions) | In the app (FR-APP-13) |
| USB↔MIDI routing modes | USB↔BLE bridge (FR-IO-05) |
| Monitor mode | Monitor in the app + logs (FR-APP-15, §8) |
| SysEx: Shift status, user keys, Shift+buttons as commands, remote setup/group selection | FR-SH-06, FR-PG-01, FR-CFG-06 |
| Display, short names, screensaver | No display → names only in the app's configuration file (D-8) |
| Backup as a SysEx dump; old formats are read by new firmware | FR-APP-14, FR-CFG-07 |

## Appendix B. Sources

- open-smc-pad — https://github.com/fedepaj/open-smc-pad (docs: sinco-protocol, flash-layout, bootloader, hardware, bluetooth)
- kagaimiq / jielie — https://kagaimiq.github.io/jielie/ (BR25, chip markings, firmware formats)
- jl-uboot-tool (JieLi USB download protocol, BR25 loader settings; MIT) — https://github.com/kagaimiq/jl-uboot-tool
- FM-1: reverse engineering — https://github.com/aroum/fm1-custom-fw/blob/main/REVERSE_ENGINEERING.md
- FM-1 emulator (pi32v2 CPU, peripherals, headless test runner; MIT) — https://github.com/joakimeriksson/fm1-emu
- SMK-37 Pro custom mod — https://github.com/amalahama/smk37-firmware-custom-mod
- M-Vave downloads — https://www.m-vave.com/download
- SMC-Mixer user manual (7 languages, 2026-03-23 edition) — https://manualf.oss-cn-hongkong.aliyuncs.com/manual/MIDI-KEYBOARD/SMC-MIXER_M-VAVE-manual.pdf
- A user review of the stock firmware (Shift + arrows, mode LED) — https://medium.com/@spxl/m-vave-smc-mixer-bluetooth-midi-controller-42976cfc8e9c (only a search snippet was read)
- JieLi AC63 SDK — https://github.com/Jieli-Tech/fw-AC63_BT_SDK ; toolchain — http://pkgman.jieliapp.com/doc/all
- Mixxx mapping for the SMC-Mixer — https://github.com/mixxxdj/mixxx/blob/main/res/controllers/MVave-SMC-Mixer.midi.xml
- Loopy Pro wiki and forum (user experience) — https://wiki.loopypro.com/SMC-Mixer
- OpenDeck (SysEx configuration, web configurator) — https://github.com/shanteacontrols/OpenDeck/wiki/Sysex-Configuration
- 16n editor (SysEx configuration over Web MIDI) — https://github.com/drkzrg/16n-editor
- Web MIDI in Safari — https://caniuse.com/webmidi , https://bugs.webkit.org/show_bug.cgi?id=107250
