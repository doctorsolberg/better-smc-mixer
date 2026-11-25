# SMC-Mixer: what we know about the device

Device facts collected in stage 0 (research, read-only). Requirements live in [SPEC.md](SPEC.md);
this file holds the facts they rest on. Confidence: ✅ confirmed (source given), ⚠️ inference or
assumption, ❓ unknown.

The user's device, 2026-11-07: firmware **SMC-Mixer_012** (the file in `materials` is V013, one
version newer).

**Scope of the tables:** the USB identity and flash map below describe that V012 stock snapshot,
not today's installed state. ✅ SincoOTA V012 → V013 and later installations/returns ran on
2026-11-14: V013 changed the USB PID/port names but retained the service protocol, and its OTA
erased `0x24000..0x7AFFF`, including our image and saved configuration. Install, update and
return cycles over USB and Bluetooth later completed with physical off/on and byte-exact
configuration read-back on 2026-11-23 (procedures: [image.md](image.md)). Do not infer current firmware from
this map.

## USB

| Item | Value | Source |
|---|---|---|
| VID:PID | `4353:4B4D` | ✅ ioreg |
| Manufacturer / product | "Jieli Technology" / "SINCO"; a serial number string is present | ✅ ioreg |
| Speed, versions | Full speed (USB 1.1), bcdDevice 1.00 | ✅ ioreg |
| Interfaces | 0 — Audio Control (name "SMC-Mixer"), 1 — MIDI Streaming, 2 endpoints | ✅ ioreg |
| MIDI ports | `SMC-Mixer-Private` (service port, answers the SINCO protocol) and `SMC-Mixer-Master` | ✅ CoreMIDI |
| bMaxPower | ❓ | — |

## Stock service protocol

The packet format is the same as on the SMC-PAD (SPEC §3.4). Tried on the mixer:

- `0x11` → `SMC-Mixer_012`, answered in about 4 ms (✅ `logs/stage0/01-info.jsonl`).
- `0x23`, type 0, 256-byte blocks: 512 KB in 4.1 s (~125 KB/s), no retries. Two full dumps match
  byte for byte, SHA-256 `02299a224a0179ad00131b165e0b3a7909429f470f65b477d9c36d3b73fa638a` (✅).
- `0x21` and `0x22`, type 0 (✅ 2026-11-10, [bringup.md](bringup.md), "Step 2b"): a 4 KB erase
  acknowledged in 4 ms; writes of 256 bytes acknowledged in about 4 ms each; sector 0 erased and
  written as stored, and the chip starts from it afterwards.

**The handlers** (static analysis of V012, 2026-11-10; what step 2b ran agrees with it):

- The packet handler (`0x1E0FEAA`, called from the main loop once a packet that starts `00 59` is
  complete) checks the length (`len + 7` = the packet) and the checksum. Commands: `0x11` (name),
  `0x12`, `0x15`, `0x21`–`0x24`.
- `0x21`–`0x24` go through a table of 8 types, chosen by the first payload byte. Each type has four
  functions: erase (`0x21`), read (`0x23`), write (`0x22`) and a fourth (`0x24`). The table is filled
  at start from constants:
  - type 0, the flash: erase `0x1E0FB78`, read `0x1E0FB80`, write `0x1E0FB84`, no `0x24`;
  - types 4 and 5: read and write only, into RAM (❓ what they hold);
  - the other types: none.
- The payload: type u8, address u32 LE, then for read and write a length u24 LE, then for write the
  data.
- **Erase**: the SDK's flash driver, a 4 KB sector erase (SPI `0x20`) of the address with its low
  12 bits cleared. The driver refuses only an address at the end of the flash. Nothing below it:
  sector 0 included.
- **Write**: SPI page program (`0x02`) of the bytes as received, in pages of up to 256 bytes. It
  refuses `address + length` past the flash size. No cipher on this path: below the VM the sender
  gives SFC-encrypted bytes, and sector 0 is written as stored.
- **Read**: below a boundary (a driver variable; `0x1000` by the dumps) a raw SPI read, then
  `doe(0xFFFF)` over the block of that call. That is why sector 0 reads through a keystream that
  restarts every 256 bytes: `smcmidi` reads 256-byte blocks. Above it, a copy from the SFC window
  (decrypted).
- **Replies**: erase and write always answer `00 59 00 01 00 00 00 FF` (command 0, status 0),
  whatever the driver returned. An unknown type answers command `0x12` with six zero bytes. Only a
  read shows whether a write took.
- The driver never writes the flash's status register: no `0x01`/`0x31` command anywhere in it. If
  the flash's write protection covers a range, erase and write there fail silently with the same
  reply. ✅ Sector 0 and `0x7F000` are not protected while the stock firmware runs (step 2b); the
  installed area was also written/read-checked from stock on 2026-11-10 (bringup step 3), with
  browser installations repeated on 2026-11-14/15. This does not characterize write protection
  for arbitrary stock sectors. The ROM loader lifts it (recovery.md, "Rehearsal 2").
- Every flash command is framed by `0x17F66(0)` … `0x17F66(1)`, and nothing outside the driver
  calls it: **PD3 is the flash's chip select**.

**Two transports** (static analysis of V012, 2026-11-14; V013's GATT table is the same, read from
`sinco-read-v013-after-ota.bin`). The main loop calls the packet handler for two messages, each
with its own source, and the reply goes back the way the packet came (`0x1E0FE70`):

- source 0, **USB MIDI**: the packet arrives in SysEx (USB-MIDI events with CIN 4), 7-bit packed
  (`js/stocklink.js`, `pack7`). ✅ on the mixer.
- source 1, **BLE**: a GATT service of its own, **`AE40`** (handles 96–101): the packet's raw
  8-bit bytes written to **`AE41`** (handle 98, write without response), starting `00 59`; a packet
  longer than one write continues in the next writes until `len + 7` bytes have come (at most
  1 053); the reply as notifications of **`AE42`** (handle 100, its CCC 101). Not SysEx, and not on
  the BLE-MIDI characteristic: that is why a service SysEx sent on the BLE MIDI port got no answer
  (2026-11-14). ✅ On the mixer (V013, 2026-11-14, Chrome's Web Bluetooth on macOS,
  `js/blestock.js`, 20-byte writes): `0x11` answered `SMC-Mixer_013` in 38 ms; `0x23` of 256
  bytes at `0x1000` in 53 ms, equal to the same block read over USB (SHA-256); 8 more reads in
  445 ms (about 56 ms each, about 4.5 KB/s: a 512 KB backup takes about 2 minutes). A reply longer
  than one notification arrives in pieces that join by the header's length. No pairing was asked
  (the chooser showed the name macOS cached, "Better-SMC-Mixer"). ✅ Erase and write too: the
  editor's page installed ours over V013 through `AE40` alone (2026-11-14: the 512 KB backup in
  about 2 minutes, writing, reading back and sector 0 in about 90 s; ours started after a power
  cycle).
  One central at a time: connected as a Bluetooth MIDI device (Audio MIDI Setup), the mixer was
  missing from Chrome's chooser, and an entry chosen by name gave "Unsupported device" (⚠️).

The later 2026-11-15 stock GATT attempt failed at connection, before backup or write, including
after a physical power cycle. The owner reported connection after forgetting the pairing.
Assigning to the read-only `DOMException.message` property in `blestock.js` masked the original
connection error; `blestock.js` now wraps the error and keeps the original as its cause. A full
Bluetooth cycle (Go back, stock AE40 backup and install, off/on) then completed on 2026-11-23
(procedure: [image.md](image.md)). Earlier AE40 success and this negative result must both be retained.

The rest of the stock GATT table: GAP, GATT, Device information, Battery (`180F`), BLE-MIDI
(handle 114), and **`AE00`** (`AE01` write, handle 130, to a callback; `AE02` notify): JieLi's
RCSP, which M-Vave's SincoOTA uses for its update (⚠️ by the UUIDs and SincoOTA's run).

## Flash map (512 KB)

From the JLFS directory at `0x1000` (✅ dump, entry CRCs match) and from sector contents:

| Address | Size | What | Notes |
|---|---|---|---|
| `0x00000` | 4 KB | Sector 0: flash header, uboot, boot pointer | Stored transformed. Do not touch, except the boot pointer during installation (SPEC §3.4) |
| `0x01000` | 0x120 | JLFS directory of the application area | Entries: `app_area_head`, `app.bin`, `cfg_tool.bin`, `VM`, `PRCT`, `BTIF`, `EXIF`, `USRFLASH` |
| `0x01120` | 0x21DCC (≈136 KB) | `app.bin` — stock code | SFC-encrypted in flash, read back decrypted |
| `0x22EEC` | 0x360 | `cfg_tool.bin` — SDK settings (BLE name list `AC696X_1…20` and more) | ⚠️ format not decoded; may hold pin numbers |
| `0x2324C` | 0x1C0 | Resource tail: a `config.dat` entry and its 0x180 bytes, then the MD5 of `app.bin` | ✅ [image.md](image.md); the area is erased from `0x2340C` to `0x24000` |
| `0x24000` | 0x57000 per the directory (up to `0x7AFFF`) | `VM` — ⚠️ SDK settings storage. Only sector `0x24000` holds data; the next two rows lie inside this area | From this address on, flash reads back as stored (✅) |
| `0x25000…0x6DFFF` | 292 KB | **Empty (0xFF)** | Room to install our own image without opening the case |
| `0x6E000` | ≈52 KB | `LOADER.BIN` and `ble_app_ota.bin` — stock update loader | ⚠️ left over from an update, or needed by the stock OTA; do not overwrite while the stock firmware matters |
| `0x7B000` | 4 KB | `BTIF` — Bluetooth data (1.5 KB used) | ⚠️ probably pairing keys and the address. **Do not erase** |
| `0x7C000` | 4 KB | `EXIF` (1 KB used) | ⚠️ factory data. **Do not erase** |
| `0x7D000` | 8 KB | `USRFLASH` — DAW and User control tables | ✅ decoded below, stored in plain form |
| `0x7F000` | 4 KB | Empty | |

What this means for the budget (SPEC Q-3): `0x1000…0x7AFFF` ≈ 488 KB is available for the
bootloader, application, configuration and black box (if the stock `LOADER.BIN` is not kept). The
stock application takes 136 KB.

SFC cipher: the mixer's chip key is `0x980F`, the same as the SMC-PAD's, keyed on the CPU address
(✅ `app.bin`'s CRC over the whole V013 area; the stock areas of V012 and V013 rebuild byte for
byte). The earlier `0x9C0F` (keyed on the flash offset) only agrees with it on odd 4 KB sectors.
Sector 0, the directory, the ciphers and the update containers are described in
[image.md](image.md).

## USRFLASH: control tables

Read with: `python3 -I tools/stage0/usrflash.py materials/dumps/smc-mixer_012_dump1.bin`.

| Offset | What |
|---|---|
| `0x0000` | 43 DAW button records, 0x47 bytes each |
| `0x0BED` | 8 DAW encoder records, 6 bytes each, then 8 DAW fader records, 5 bytes each |
| `0x0C45` | 43 User button records, 0x47 bytes each |
| `0x1832` | 8 User encoder records, 8 User fader records |

Button order: strip by strip M, S, R, □, then the bottom row left to right (Play, Stop, Rec, Rew,
FF, «, », ▲, ▼, ◀, ▶). On the panel each strip's buttons stand top to bottom in that order, M
at the top and □ at the bottom (✅ the owner's photo, 2026-11-11).

Button record: type, channel (0 = channel 1), number, min, max, length and a SysEx field (notes and
CCs carry an MMC template there — ⚠️ apparently unused); the last byte is the number again (DAW) or
0xFF (User). Types: `0x05` — note (DAW), `0x01` — CC toggle (each press sends max and min in turn,
release sends nothing), `0x02` — CC held while pressed (max on press, min on release). ✅ capture 04:
in the device's User table `0x01` is set on M, S, Play and Rec, and exactly those toggle.

Encoder record: mode, type, channel, number, two parameters. DAW: `03 02 00 1n 41 01` — relative
(0x41 / 0x01). User: `00 02 ch num 00 7F` — absolute 0–127, clamped at the ends (✅ capture 04).
Fader record: type, channel, number, min, max. DAW: `02 0n 28+n 00 7F` — Pitch Bend on the strip's
channel (✅ captures 04, 05). User: `00 ch num 00 7F` — CC (✅ capture 04).

**The User table on the device is custom**: the user edited it in the M-Vave app (✅ the user), so
it is not the V012 factory table. It is still useful as a sample of the format with a channel per
strip (strip n → channel n). CC numbers:

| Strip | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| Encoder | 3 | 18 | 24 | 30 | 48 | 54 | 60 | 81 |
| Fader | 9 | 19 | 25 | 31 | 49 | 55 | 61 | 82 |
| M / S / R / □ | 14 15 16 17 | 20 21 22 23 | 26 27 28 29 | 35 41 46 47 | 50 51 52 53 | 56 57 58 59 | 62 63 79 80 | 83 84 85 86 |

Bottom row (channel 1): Play 87, Stop 89, Rec 90, Rew 102, FF 103, « 104, » 105, ▲ 106, ▼ 107, ◀ 108, ▶ 109.

Since the vendor app can edit this table, the stock firmware accepts table writes from it. ❓ Over
which transport (USB, BLE) and with which messages.

## Stock behaviour (captures 04 and 05)

Recorded with `smcmidi session` on 2026-11-07 (`logs/stage0/04-stock-behaviour.jsonl`,
`05-stock-details.jsonl`; scenarios in `tools/stage0/scenarios/`). Read-only: nothing was sent to
the mixer.

- **Every message goes out on both ports**, `SMC-Mixer-Private` and `SMC-Mixer-Master`, with the
  same bytes and the same host timestamp (✅ all 1756 messages of capture 04).
- **Modes.** While Shift is held, the arrow of the active mode is lit. Shift + ◀ selects **DAW**,
  Shift + ▶ selects **User** (✅). The device was in User mode before the captures and is left in
  DAW mode after them.
- **Shift has no layer of its own**: faders, encoders and strip buttons send the same with Shift
  held, in both modes (✅).
- **DAW mode is Mackie Control** (✅):
  - R1–8 send notes 0–7, S notes 8–15, M notes 16–23, SELECT notes 24–31. Press sends velocity 127,
    release sends note on with velocity 0 (`90 nn 00`).
  - Bottom row: Rew 91, FF 92, Stop 93, Play 94, Rec 95, « 46, » 47, ▲ 96, ▼ 97, ◀ 98, ▶ 99.
  - Fader n sends Pitch Bend on channel n (n = 1, 2, 3, 4, 8 checked), 0…16256 in steps of 128:
    7-bit resolution, the low byte is always 0.
  - Encoder n sends CC 15+n on channel 1 (1, 2, 8 checked): 1 clockwise, 65 counter-clockwise, one
    message per step, no acceleration.
- **User mode** follows the device's custom table (CC numbers and channels match `USRFLASH`); fader
  CC 0–127 in steps of 1; encoder absolute 0–127, clamped.
- **Faders:** no messages at rest (no jitter in 5 s, both modes); about one value per 10 ms per fader
  while moving, faders independent; touching a fader cap sends nothing (no touch sensing).
- **Encoders:** endless, no detents, **no push** (pushing sends nothing; ✅ the user: the knobs do not
  press), no pointer on the knob. With a mark on the knob, one slow revolution gave 47–48 steps in
  DAW mode. ⚠️ That fits a 24-pulse encoder (96 edges per revolution) with the stock divider of 2
  (see below). A fast revolution gave 75 steps — ⚠️ probably more than one turn; not conclusive.

## Stock firmware internals (static analysis of V012)

The device's `app.bin` (from the dump) was disassembled with the JieLi toolchain's `objdump` at
its link address `0x1E00120`; the RAM code (`.data_code`, from `0x1E204E4` in the image) runs at
`0x16C00`. The strings are stripped, so functions are identified by what they do. Everything here
is static-analysis evidence unless a later hardware observation is explicitly marked below;
the matrix, LED and fader checks on 2026-11-11 settle only the stated properties.

**Input matrix: 16 columns × 4 rows.** An SPI2 interrupt (`0x17586`, IRQ 44) shifts a 32-bit word to
the four 74HC595s with DMA for each column: the upper 16 bits select one column (one bit low), the
lower 16 bits are LED row data. PB4 pulses around each transfer (595 latch). The rows are read on
**PB0–PB3** (pull-ups; a pressed key reads low) into a double-buffered 16-byte snapshot, one byte per
column.

- **Column bits.** The column table at `0x1E14358` (16 words) selects column *c* with bit 24 + *c*
  for *c* = 0–7 and bit 16 + *c* − 8 for *c* = 8–15 (that bit low, the other 15 high).
- **The pipeline.** Each SPI2 DMA-complete interrupt reads PB0–PB3 into the snapshot at the
  current column index, raises PB4 (the rising edge latches the word just shifted), moves to the
  next column, starts the DMA of that column's word (4 bytes from RAM, lowest byte first) and
  lowers PB4. So snapshot column *c* holds the rows read while the word for column *c* − 1 was
  latched. The tables below use the snapshot's numbering. Nothing paces the steps but the SPI:
  each interrupt starts the next transfer.
- **The setup** (`0x1E0FC06`): SPI2 `CON = 0x6020` (master, one data line, clock idle low, data
  changed on the falling edge, interrupt on, pending cleared), `BAUD = 0x0B`, then enabled. Its
  pins are not remapped, so it uses port A: **clock PB6, data PB7** (the SDK's SPI2 port A map).
  PB6 and PB7 are set as outputs without pulls, **PB4** as an output, low. **PB5** is driven low
  only after SPI2 is running: ⚠️ most likely the 595s' output enable (/OE). The interrupt has
  priority 5 (ours has 7: [hwtest.md](hwtest.md), "While flash is written"; the SDK's
  `local_irq_disable()` holds 7 off as well, ✅ run 24).
- The row pins come from a table at `0x1E147A0` (PB3, PB2, PB1, PB0: inputs, pull-ups on).

- Row 0 carries the encoders: encoder n uses columns 2n−1 and 2n (encoder 8: columns 15 and 0).
  ✅ Our hardware test, 2026-11-11: each encoder on its columns; turned clockwise, the line on
  column 2n changes first ([hwtest.md](hwtest.md), "Hardware layout it assumes"). 48 clicks a
  revolution (✅ 240 for 5 revolutions on encoders 1, 7 and 8, image 30). Of the four quadrature
  states, both contacts closed is the shortest, about 20% of a cycle (✅ the encoder probe). Encoder
  8's contacts drop out while closed far more often than the others' (✅ the probe: 1 199 pulses of
  100–200 µs against 79–96): a worn or dirty part, ⚠️ not opened.
- Rows 1–3 carry 45 buttons. The table at `0x1E14329` gives each button its (column, row) as
  `column << 4 | row`. Button indexes follow the `USRFLASH` order: 0–31 the strips (M, S, R, □ per
  strip), 32–42 the bottom row, **43 — BT, 44 — Shift**.

| Index | Buttons | (column, row) |
|---|---|---|
| 0–3 | strip 1 M S R □ | (1,3) (2,3) (3,3) (4,3) |
| 4–7 | strip 2 | (11,3) (10,3) (9,3) (8,3) |
| 8–11 | strip 3 | (12,3) (13,3) (14,3) (15,3) |
| 12–15 | strip 4 | (1,2) (2,2) (3,2) (4,2) |
| 16–19 | strip 5 | (11,2) (10,2) (9,2) (8,2) |
| 20–23 | strip 6 | (12,2) (13,2) (14,2) (15,2) |
| 24–27 | strip 7 | (6,1) (5,1) (4,1) (3,1) |
| 28–31 | strip 8 | (9,1) (10,1) (11,1) (12,1) |
| 32–42 | Play Stop Rec Rew FF « » ▲ ▼ ◀ ▶ | (5,3) (6,3) (7,3) (0,3) (5,2) (6,2) (7,2) (0,2) (1,1) (2,1) (13,1) |
| 43, 44 | BT, Shift | (7,1) (8,1) |

**LEDs: 4 columns × 16 rows.** The 16 rows come from the lower 16 bits of the 595 word; the four
columns are driven directly by **PA2, PA5, PC1, PC3**, one at a time, with a blank phase between
them (interrupt `0x17646`, IRQ 62, a software interrupt the matrix interrupt raises after each full
pass; priority 4). The table at `0x1E14B18` maps 53 LEDs (43 buttons, 8 encoders, BT, Shift) to
`row << 4 | column`. ✅ Our LED walk, 2026-11-11: the buttons' entries in the button order; the
encoders' 8 entries run right to left (the table's last is encoder 1); no other LED on the matrix.

- **Polarity: both active low.** A lit LED clears its row bit in the word; a column pin is driven
  low to show that column.
- **Timing.** A counter runs 0–31, one step per matrix pass; the column is the counter / 8. When
  the counter is a multiple of 8 the phase is blank: all four column pins high and the rows all 1.
  So each column is shown for 7 passes of 8.
- The column pins come from a table at `0x1E147B0` (PA2, PA5, PC1, PC3), set as outputs, low, at
  start.
- **The faint glow of 13 LEDs** (the bottom row, BT, Shift: the coloured ones) is not the
  firmware's (research of 2026-11-14 for the owner):
  - ✅ It stays with no column driven (our `LED_DARK=1`, controller 16) and with PB5 high as well
    (`LED_DARK=2`, controller 17; ⚠️ PB5 as the 595s' /OE is inferred), and the stock firmware
    shows the same glow (the owner, 2026-11-11).
  - ✅ (static analysis) The stock firmware sets up no pin that could feed them. Every GPIO it
    configures, through the SDK's port functions (the port table at `0x1E00B24`, its only user
    `0x1E0080A`) or directly: the matrix (`0x1E0FC06`: SPI2 PB6/PB7, latch PB4, PB5, rows
    PB0–PB3), the LED columns (PA2, PA5, PC1, PC3), the 8 fader inputs (`0x1E0FD3C`), USB D+/D−
    (`0x1E0E7EA`, `0x1E0E8A0`), the UART handler's pins (`0x1E0E9B6`, computed) and the flash's
    chip select PD3. The 53-entry LED table holds them on the matrix like the other LEDs.
  - The bottom row's 11 buttons share one translucent silicone strip (✅ `Image.png`); each strip's
    four buttons are separate caps.
  - ❓ The path. ⚠️ Most likely a second, always-on die in a two-colour LED (a backlight wired to
    the supply), or a leak on the board; light carried along the shared strip would not explain
    BT and Shift. To tell them apart at the mixer: whether a lit bottom button has the same colour
    as its glow (another colour: a second die); whether the glow follows the power (USB or
    battery, the switch, charging); a meter across one LED with the firmware's matrix dark.
  - So the firmware cannot darken them: neither ours nor the stock one has a pin for it. ⚠️ Left
    open only for an MCU pin no firmware sets up: a diagnostic image could walk those pins
    (output low, output high, input) while the owner watches the glow.

**Timing.** The same timer interrupt decodes the encoders (a quadrature transition table). Software
timers: buttons every 20 ms (a press counts after 3 scans, a long press after 41 scans, about
0.8 s); encoders every 50 ms, where the count is divided by 2 (a per-encoder shift, set to 1 at
start); an inactivity counter every 50 ms (1200 ticks = 60 s).

**CPU clock: 192 MHz after start** (⚠️ static analysis, 2026-11-11). The early init runs the
system clock at 24 MHz (the path around `0x1E163C0`, which also sets the core voltage for it).
Then one `clk_set("sys", 192000000)` (`0x1E1621E`, an LTO-specialized form with the constant at
`0x1E16250`) is called from a single place, `0x1E105BA`: the application's state machine on
APP_STA_START, which is the SDK's `clk_set("sys", BT_NORMAL_HZ)`. The only other call that
picks a core voltage for a frequency is the early one. So the stock firmware runs at 192 MHz from
the application's start on, Bluetooth or not; the SDK's board files for this chip use 24 or 48 MHz.
The matrix is paced by the SPI alone (above), so its pass rate follows the SPI's clock: ❓ not
computed (`BAUD = 0x0B` divides a clock whose source at 192 MHz is not traced).

**Events.** Button scan, encoders and faders post 32-bit events (`type << 24 | index << 16 |
value`; buttons: 0 release, 1 press, 2 long press, 3 long release) into a 256-entry queue, read by
one dispatcher (`0x1E0F526`). Its only special cases:

- BT (43), long press: wireless on/off. ⚠️ Not kept in flash (static analysis, 2026-11-12): the
  branch flips a byte in RAM and calls the stack's switch on or off; unlike the mode switch below
  it calls no VM write.
- Shift (44): press and long press set the Shift state and light its LED, release clears them.
  Ignored while ◀ or ▶ is held.
- With Shift held, ◀ (41) / ▶ (42) on press select mode 0 (DAW) / 1 (User). The mode is saved in VM
  item 44 (magic `0x3AA3`); without a saved mode the firmware starts in User mode. ✅ Seen in
  flash (2026-11-10, stock reads of 2026-11-07 and 2026-11-10 compared): the afternoon's mode
  switches appended three 8-byte records to the erased space of sector `0x24000` at `0x240A1`,
  `C8 2C 40 00 A3 3A mm CA` with `2C` = item 44 and `mm` = 0, 1, 0; no sector was erased and
  nothing else in the flash changed.
- Everything else, Shift held or not, goes to the normal control handler.

**No power-on key combinations.** At start the firmware reads the mode from VM, initializes the
matrix and LEDs and reads `usrflash` (6282 bytes; if the file is missing it builds a default table
in code). It never reads the keys outside the scan → events → dispatcher path, and the dispatcher
does not look at the time since boot. Holding keys at power-on therefore only produces their normal
events once scanning starts. The bootloader's long-press reset is **off**: the flash header has
`RESET=PB01_00_0` (hold time 00 = disabled). The header also has `UTRX=PB05` (the JieLi UART-update
pin) and `SPI=2_3_0_0`.

**Power.** The chip's power registers are written only through the ROM's P33 accessors
(`P33_CON_SET`, `p33_tx_1byte`, `p33_or_1byte`, `p33_and_1byte`). They sit at fixed ROM
addresses, so every write can be listed with its constant arguments:
`tools/re/listing.py LISTING args 117d60 117d58 117d54 117d50`. The same listing of our own SDK
build, which has symbols, names the code. The stock power code is the same SDK code as ours: the
same writes in the same order, and the same LTO-specialized `power_set_mode`.

- **Power mode LDO 1.5 V** (`PWR_LDO15`), no DCDC. The stock `power_set_mode` is the specialized
  form that can only select LDO 1.5 V, identical to ours.
- **Strong VDDIO 3.4 V** (`VDDIOM_VOL_SEL(6)`). Our SDK version also trims it against an ADC
  measurement; the stock one does not.
- **Weak VDDIO 2.4 V** (used only in power-down) is written at start in both. ⚠️ Our SDK version
  then picks a level of its own.
- **Crystal 24 MHz** (a constant in the stock power init).
- **The chip's own charger is off.** The stock firmware writes `CHARGE_EN(0)` and `CHGBG_EN(0)`
  and never sets a charge voltage or current (no writes to `P3_CHG_CON1/2`). So the battery is
  charged by something else: the board's **AiP4056** (a stand-alone linear Li-ion charger, a
  TP4056 equivalent; ✅ `materials/board-photo/Image 5.jpeg`, SPEC §2), which no MCU pin
  controls (⚠️ no trace followed), so charging does not depend on the firmware.
- **Pin reset.** The stock firmware leaves `P3_PINR_CON` as the uboot sets it from the header
  (`RESET=PB01_00_0`, off).
- **Low-power timer 1.** The stock firmware sets it up (`P3_LP_TMR1_CON`); ours does not. ❓
  What it is used for.

**Historical stage-1 application against this** (`firmware/app`, before scan/storage/BLE were
added; static analysis):

- Power mode, VDDIO and the crystal: the same.
- The charger: off, as in stock.
- Pin reset: written off.
- No GPIO is configured while it runs. Only sleep and soft power-off set pins (all to high
  impedance, except the flash pins). Neither is reached: low-power sleep is off, and no soft
  power-off code is linked.
- It writes to flash in one case: if it finds no Bluetooth MAC address in its settings, it makes a
  random one and stores it (`cfg_file_parse` → `syscfg_write(CFG_BT_MAC_ADDR)`).
  - The item is in the SDK's `BTIF` table (the hid example's `user_cfg.c`), so it goes to
    `BTIF`, not to the VM.
  - It is written only into an empty slot.
  - On the V012 dump the stock MAC item is valid, so nothing is written
    ([image.md](image.md), "Installing: the dry run").
- The SDK's own update paths are off (`config_update_mode = 0`, SPEC R-13).

This is not the current application's GPIO/write inventory. Current firmware drives the panel,
saves configuration at `0x25000..0x34FFF` and panel state at `0x35000..0x36FFF`, and can rewrite
sector 0 on STOCK back; the SDK also opens its own VM. Those paths have dated mixer evidence
in [config.md](config.md), [hwtest.md](hwtest.md) and [image.md](image.md). BLE pairs without
bonding by default ([ble.md](ble.md)); no bond-key write was seen in those recorded runs.

**Other findings:**
- A UART command handler (`0x1E0EBBE`) — some debug interface; pins not identified.
- A function sets PD3 as an output and writes it (`0x17F66`, 31 call sites): the flash's chip select
  (⚠️ every caller is the flash driver; "Stock service protocol", "The handlers").
- PB5, PB6, PB7: the SPI2 clock (PB6) and data (PB7), and PB5 driven low after SPI2 starts (⚠️ the
  595 output enable); see "Input matrix".

## Stock DAW mode: incoming MIDI (static analysis, 2026-11-25)

How V012 handles MIDI from the host, for a template that makes our firmware work with DAWs' Mackie
Control scripts as the stock firmware does ([ideas.md](ideas.md), "A DAW template like the
stock firmware's"). Static analysis of `materials/re/v012/app.lst` only, addresses at run time;
nothing was run on the mixer. "Code" means confirmed by the code, "inferred" not.

- **One receive path for USB and BLE MIDI.** The MIDI task (`0x1E10154`) decodes USB-MIDI events
  (parser `0x16C26`) and passes each complete message to `0x1E0EF78`; the BLE-MIDI write handler
  (`0x1E0D5C4`) strips the BLE header and timestamps into the same queue (code; the BLE handle from
  the GATT table above, inferred).
- **Only Note On, CC and Pitch Bend are read** (code). Note Off (`8n`), channel pressure (Mackie
  meters `D0`), polyphonic aftertouch, program change and every SysEx but the service packets are
  ignored: the Mackie LCD (`F0 00 00 66 14 12 …`), the device query and the host handshake get no
  answer, and nothing is sent at connect (inferred from the callers of the MIDI send routine; the
  code holds no `00 00 66 14`/`15` sequence).
- **Note On lights a button** (code): the channel is ignored; the note is looked up in the DAW
  button table: R 0–7, S 8–15, M 16–23, SELECT 24–31, Rew 91, FF 92, Stop 93, Play 94, Rec 95.
  Velocity ≠ 0 lights the LED, 0 turns it off. Velocity 1 (Mackie's "flash") lights it solid:
  nothing blinks. «, », ▲, ▼, ◀ and ▶ have no host note (their record's last byte is `FF`).
- **In DAW mode those LEDs are the host's only** (code): pressing M with no host sends the note
  and leaves the LED dark; it lights when the host sends the note back. «, » and the arrows light
  while held. In User mode every button LED is local (a toggle shows its state, the others light
  while held) and incoming MIDI never changes them.
- **Pitch Bend from the host** (DAW mode, code): the MSB per channel is the track's fader value.
  About 500 ms after the last Pitch Bend of a strip, the fader is compared with it; more than 3
  steps (of 127) apart (host 115–119 with the fader at or above it counts as met), the strip's
  encoder LED blinks 0.6 s on, 0.6 s off, and the fader sends nothing until it meets the value
  (between 0 and 3 steps above it): a pickup.
- **Incoming CC** changes no LED (code). One whose number and channel are an encoder's sets that
  encoder's value silently (in DAW mode the encoders are relative, so it does nothing); Mackie's
  V-Pot rings (CC 48–55) and displays (CC 64–75) are ignored.
- **Extras** (code, purpose inferred): `9F 7F 7F` turns every host-lit LED and the encoder LEDs
  off; note 115 (Mackie's Rude Solo light) on makes each M LED show that strip's S; `B0 00 41`
  stops the fader blink until the next Pitch Bend. Encoders set to Pitch Bend in the User table
  send Mackie touch notes (`90 68+n 7F`, then `00` after about 200 ms idle); DAW faders send none.
- **SysEx it does act on** (code; meaning inferred): `F0 35 59 xx xx xx F7` sets a flag; the USB
  receive routine reboots into the JieLi loader on `F0 22 24 35 7D F7` (`0x1E0E4B6` → `0x17BD0`).

## Data and tools

- `materials/dumps/` — flash dumps and their read logs (the logs contain flash contents). This is
  vendor data plus Bluetooth pairing keys: local only, never published (SPEC C-7). The files are
  write-protected.
- `logs/stage0/` — `smcmidi` session logs (JSON Lines, host timestamps in ns).
- `tools/smcmidi/main.swift` — MIDI tool (CoreMIDI, no dependencies). Build:
  `swiftc -O -swift-version 5 -o build/smcmidi tools/smcmidi/main.swift`. Commands: `list`,
  `listen`, `session`, `sinco-info`, `sinco-read`; since 2026-11-10 also `sinco-erase`,
  `sinco-write` and `sinco-sector0` (the stock firmware's `0x21`/`0x22`, guarded; ✅ on the
  mixer, 2026-11-10: [bringup.md](bringup.md), "Step 2b") and `selftest`. It has no upgrade-mode command.
- Recovery ([recovery.md](recovery.md)): `tools/jlrom` (ROM download mode from the Mac, reads and
  guarded writes/restores;
  `compare.py` checks a raw read against a dump) and `tools/usbkey` (the RP2040 key firmware and
  its console).
- `tools/stage0/flashmap.py`, `tools/stage0/usrflash.py` — dump decoders;
  `tools/stage0/capture_report.py` — per-step summary of a `session` log. Python standard library
  only.
- `tools/stage0/scenarios/` — guided capture scenarios for `smcmidi session`: `stock-behaviour`
  (log 04), `stock-details` (log 05). `capture_report.py` leaves out a mirror port (a source that
  repeats another one exactly).
- `tools/re/disasm_stock.sh` — cuts `app.bin` out of a dump and disassembles it with the JieLi
  `objdump` in the build container, into `materials/re/v012/` (vendor code: local only, C-7);
  `tools/re/listing.py` — navigates that listing (resolved call targets, RAM code at its RAM
  address, ROM function names from the SDK linker script, GPIO register loads);
  `tools/re/rampath.py` — on our own image's listing, follows everything a function reaches
  (calls, branches, fall-through) and fails on code or constants in flash: `build_app.sh` runs it
  on the scan interrupt ([hwtest.md](hwtest.md), "While flash is written").
- The firmware build environment (toolchain, SDK, Docker) is described in [build.md](build.md).

## Chips

- Four SOIC-16, all **74HC595D** (marking "74HC595D XJS 24G UnL2416W"; ✅ `materials/board-photo/Image
  7.jpeg`, `Image 8.jpeg`). Together: a 32-bit output chain, 16 bits for the key matrix columns and
  16 for the LED rows (see above).
- MCU marking "C098802-66C4" with the JL logo (✅ `Image 8.jpeg`). The photo shows leads on all four
  sides (⚠️ a QFP-style 32-pin package; the SPEC said QFN32 — the pin count is what matters).

## Still unknown

- The rest of the pin map (R-3): battery measurement, what PB5 is (⚠️ the
  595 /OE). The matrix and LED parts are known from the stock firmware and ✅ checked with our
  hardware test on 2026-11-11 (above). ✅ The fader channels, found by the same test: of the 13 pins
  the ADC can read, the matrix, the 595s and USB leave exactly 8 free, and those are the faders:
  faders 1–8, left to right, on PC5, PC4, PC2, PA6, PA4, PA3, PA1, PA0; bottom reads 0, top
  1023 ([hwtest.md](hwtest.md), "Hardware layout it assumes").
- The path of the 13 coloured LEDs' faint glow (❓, "Stock firmware internals", LEDs).
- How the vendor app writes `USRFLASH`.
- ~~Whether USB powers the chip while the power switch is off~~ ✅ It does not (2026-11-10,
  rehearsal 1): with the switch off and the cable in, the bus stays empty; the chip starts when
  the switch is turned on.
- bMaxPower.
