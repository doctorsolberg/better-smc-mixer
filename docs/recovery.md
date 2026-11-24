# Recovery through the ROM download mode

The last line of recovery (SPEC C-6). The BR25's mask ROM has a USB download mode ("UBOOT") that
runs before anything stored in flash. It is intended to recover the whole flash even when
sector 0, the bootloader and the application are broken; deliberately leaving sector 0 erased
has not been rehearsed on this mixer (R8 below). To enter the mode, a signal (the
"USB key") must be sent on the USB data lines while the chip starts. An RP2040 board sends it.

Confidence marks as in [hardware.md](hardware.md): ✅ confirmed (source given), ⚠️ inference or
taken from another chip or project, not yet seen on the mixer.

Status: the key firmware runs on a Raspberry Pi Pico (rehearsal 0 below, 2026-11-09).
The host tools and the procedure are ready and were reviewed on 2026-11-10 (`jlrom` no longer
replaces an output file and has a `selftest`; `compare.py` knows the stock firmware's VM;
rehearsal 1 starts with a fresh stock dump). **Rehearsal 1 passed on 2026-11-10** (below): the
key, the ROM mode, the loader and two full raw reads work on the mixer. **Rehearsal 2 passed the
same day** ("Rehearsal 2"): a sector written and restored; that run ended with the flash as
before. ✅ `restore --protected` to stock and back ran on 2026-11-11 (below), with whole-flash
verification after rewriting only differing sectors; image iteration
also used key entry after soft/watchdog resets. The deliberately erased-sector-0 case (R8)
remains untried. A passed restore is evidence for that run, not the current boot state.

On 2026-11-15 a CRC-checked full raw backup and a restricted 43-sector same-image reflash were
followed by a whole-flash byte comparison. The normal path no longer needs the key: the editor
installs, updates and goes back to the stock firmware over USB and Bluetooth, and these cycles
completed with physical off/on and byte-exact configuration read-back on 2026-11-23 (procedures:
[image.md](image.md)). The key and `jlrom` remain the safety net. `jlrom` needs root for USB
capture; run it from a terminal with sudo. Every device run still needs the owner's permission.

**Rehearsal 1 result** (2026-11-10, logs `logs/recovery/r1-read.jsonl`, `r2-read.jsonl`):

- `rom-read-1.bin` and `rom-read-2.bin`: `crc_check` `ok`, 512 KB in about 1.2 s each (about
  640 KB/s); equal byte for byte (SHA-256 `b32a406c…5ffd5a2fb`).
- Against the fresh stock read: the plain area equal; base `0x1000` explains all 4480 SFC blocks
  (`0` and `0x1120` none); sector 0 equal through the keystream: `result: ok`.
- The stock firmware answered (`SMC-Mixer_012`) after each read.
- Both reads are copied next to the other copy of the dumps, outside the project.

**First contact** (2026-11-10, ✅ on the mixer, logs `logs/recovery/r1-key.jsonl`, `r1-info.jsonl`):

- The key: the chip answered 13 s into keying, when the power switch was turned on, and stayed on
  the bus (273 short lows, the host's resets, longest 58 ms). With the switch off the cable alone
  does not start the chip, so the cable may stay in: switch off, `r`, switch on.
- The ROM: `BR25UBOOT1.00`, `4C4A:2542`, inquiry `BR25` / `UBOOT1.00` / `1.00`; macOS mounted no
  disk. All 58 loader blocks were taken (so the CRC byte order of `FB06` is right) and the jump
  worked.
- The loader: chip key `0x980F` (`FC09`); `FC0A` flash type 3, ID bytes `13 70 CD 00`; `FC0B`
  `00 CD 70`; `FC14` buffer 32 KB.
- `FC15` (loader version) failed: 13 bytes instead of 16 in its data phase, no CSW, then the chip
  dropped off and restarted into the stock firmware. `info` no longer asks `FC15` or `FC16`;
  `jlrom` now logs a short data phase and takes a CSW that arrives in the data phase.
- `info` then passed cleanly (`r1-info-2.jsonl`), the same identity. When `jlrom` gives the device
  back, the chip restarts into the firmware in flash: every `jlrom` command needs its own entry
  into the ROM mode (switch off, `r`, switch on).
- 2026-11-11, attempt 31: `r` sent to the key's serial port from a script (not the console); the
  owner then switched the mixer on, the chip answered 4 s later as `BR25UBOOT1.00`, and
  `jlrom read` worked (`rom-read-bm-10.bin`). A command from a script works.
- 2026-11-11, attempt 32, with no hands: the mixer running boot-mark image 10, `r` sent to the key
  from a script, then `smclog cmd --port … reset soft` (the link's RESET, `chip_reset()`). The key
  saw D+ low 0.43 s after `r`, the chip answered after 6 keys, and it came up as
  `BR25UBOOT1.00`. ✅ A soft reset enters the ROM mode as a power-on does; the watchdog's
  reset too (attempts 33..35). So while our firmware's link answers, the loop needs no one at the switch:

  ```sh
  printf r > /dev/cu.usbmodemKEY        # the key's port (console.py finds it by name)
  python3 -I tools/smclog/smclog.py cmd --port /dev/cu.usbmodemMIXER reset soft
  sudo build/jlrom …                    # then the ROM mode, as after a power-on
  ```

  A firmware that hangs without restarting still needs the switch.
- 2026-11-11, attempts 33..37, with no hands: `r` sent to the key while the mixer's own firmware
  restarted by itself, no command sent. Boot-mark images 11 and 12, which ended every start in
  the watchdog's reset (`R0400`): attempt 33 (the read of `rom-read-bm-11.bin`), 34 (4.3 s after
  `r`), 35 (2.2 s). ✅ The watchdog's reset enters the ROM mode too. Image 13, which ended every
  start in the stall check's `chip_reset()`: 36 (1.6 s), 37 (14 s). Each was followed by a
  `jlrom write` or `jlrom read` that passed. So a firmware that restarts itself needs no switch
  either.

## How the mode works

**The key.** 16 bits, `0x16EF`, most significant bit first: D+ is the clock, D− the data, and the
chip takes a bit on each rising edge of D+. About 50 kHz is usual: the chip samples the lines in
software, so the clock must not be fast. The key repeats until the chip answers. The chip answers
by pulling both lines low for 1–2 ms, then turns on its D+ pull-up. The sender must let go of the
lines at once. (⚠️ kagaimiq, "USB key"; the open-smc-pad sketch detects the D+ pull-up.)

**Clock calibration.** Before it starts USB, the chip trims its clock against the 1 ms
start-of-frame packets on the bus. It cannot tell them from other traffic. A full- or low-speed
device on the same hub can spoil the calibration: the chip then resets and starts the firmware in
flash instead. (⚠️ kagaimiq saw this on an AC696N, the BR25 family.) **So the mixer goes into a port
of the Mac itself, not through a hub or dock.** The 2026-11-07 research found it on a dock behind
three hubs; that was the pre-rehearsal topology, not a current-device discovery. Rehearsal 1
worked through the direct-port tap on 2026-11-10; hub behavior was not tested.

**On the host.** The chip shows up as a USB mass-storage device. Commands are vendor SCSI command
blocks over bulk-only transport. Each command has no data phase, a data-in phase or a data-out
phase. A command without data returns a 16-byte reply in a data-in phase; its first two bytes echo
the opcode. The inquiry should name the chip, vendor `BR25`, and the product `UBOOT1.00`. (⚠️
jl-uboot-tool; it lists BR25 as working.)

**The loader.** The ROM ("UBOOT1.00") only writes RAM (`FB06`, with a CRC-16 of the block) and
jumps (`FB08`). Everything else comes from a loader run in RAM: `br25loader.bin` from the pinned
SDK (`cpu/br25/tools`).

- Size 29,696 bytes; SHA-256 `6cb7f0f9…a160ea83`, pinned in `jlrom`.
- The file is stored scrambled with JieLi's "MengLi" keystream, per 512-byte block. ✅ Decrypting it
  that way gives readable strings: "BR25 analog init sucessful!", "run loader ok", the norflash
  messages.
- The BR25 ROM removes the scrambling as the blocks arrive. The file is therefore sent as stored,
  in 512-byte blocks to `0x12000`, then started with argument 1 (target flash, no clock divider,
  SPI mode 0). (⚠️ jl-uboot-tool's settings for BR25.)

**Loader commands that `jlrom` uses** (protocol v2; ⚠️ from jl-uboot-tool, replies after the echo):

| Opcode | Command | Arguments | Reply |
|---|---|---|---|
| `FC09` | chip key | `00 AC 69 00` | bytes 4–5, reversed, MengLi-masked → 16-bit key, little-endian |
| `FC0A` | online device | — | byte 0 flash type, bytes 2–5 flash ID (little-endian) |
| `FC0B` | ID | — | bytes 0–2 |
| `FC14` | USB buffer size | — | bytes 0–3: the largest read per command |
| `FC15` | loader version | — | bytes 1–4 reversed, ASCII (the echo is not always right) |
| `FC16` | mask ROM ID | — | bytes 0–3 |
| `FD05` | read flash | address (4), length (2) | the data, as stored |
| `FC0E` | flash CRC-16 | address (4), length (2) | bytes 0–1 |
| `FC0C` | run the application | `00 00 00 01` | — (the chip restarts) |

The protocol also has erase (`FB00` block, `FB01` sector, `FB02` chip), write (`FB04`), chip key
write (`FC12`) and a flash command table (`FC0D`). `jlrom` builds only `FB01` and `FB04`, for the
write path of rehearsal 2 (described there, from the loader's own code); never the others.

**CRC.** CRC-16/XMODEM: polynomial `0x1021`, initial value 0, no reflection. The same CRC protects
JLFS entries.

**Loader side effects.** The loader's strings show that it sets the flash's write protection and
quad mode: "norflash_write_protect", "norflash_set_quad". Both are writes to the flash status
registers. Every JieLi programming session does this. ⚠️ It is unknown whether it happens at
start or only before writing.

**What a read returns.** The data as stored. The stock firmware's read (`0x23`) sees flash through
the SFC cipher, so its dumps are plain text below the VM area (`0x24000`) (✅ hardware.md). The two
must agree:

- from `0x24000` up, byte for byte;
- below that, after SFC decryption with the chip key `0x980F`. The SFC cipher keys each 32-byte
  block with `chipkey ^ ((address − base) >> 2)`; the vendor update says base `0x1000` (the CPU
  address, [image.md](image.md)), and `compare.py` tries `0x1000`, `0` and `0x1120` so the read
  shows what the chip really does.
- Sector 0: the stock firmware reads it XORed with the ENC keystream of `0xFFFF` restarting every
  256 bytes; `compare.py` checks the raw read against the dump through that stream.

**A raw read is a complete backup in stored form**, sector 0 and the encrypted areas included:
written back as is, it restores the device. A stock-firmware dump cannot do that.

## Hardware

- **An RP2040 board.** A Raspberry Pi Pico or an RP2040-Zero, 3.3 V logic.
  - The Zero has no plain LED (an RGB LED on GP16), so status comes only over the serial port.
- **Wiring** (as in the open-smc-pad sketch):

  | RP2040 | | USB line to the mixer | Usual wire colour |
  |---|---|---|---|
  | GP2 | through 270 Ω | D+ (key clock) | green |
  | GP3 | through 270 Ω | D− (key data) | white |
  | GND | direct | GND | black |
  | — | not connected | VBUS | red |

- **The cable.** The USB cable between the mixer and the Mac is tapped: a cut cable, or a USB
  breakout between two cables. The mixer stays connected to the Mac through it.
  - The owner's tap (2026-11-10): two USB-A socket breakouts on a breadboard, wired by their
    labels. Facing each other, their pins come in mirrored order (`GND D+ D− VBUS` against
    `VBUS D− D+ GND`), so straight wires would short VBUS to GND: check with a meter first.
  - The Mac's side needs a passive USB-C plug → USB-A socket adapter: its 5.1 kΩ on CC is what
    makes the Mac switch VBUS on. A legacy A-to-C cable with its C plug in the Mac does not work
    (its C plug says "host"). The owner's chain: adapter C→A, adapter A→C socket, A-to-C cable
    into the breakout; mixer's side: an A-to-C cable.
  - ✅ It passes the stock firmware's USB (2026-11-10: two stock reads through it, the key's
    resistors out). ⚠️ Keep the resistors out while the key has no power: the unpowered RP2040's
    pin protection may load the lines.
- **Power.** The RP2040 runs from its own USB cable to the Mac, which also carries its serial
  port; the two grounds meet in the Mac.
- **The port.** The mixer's side goes into a USB port of the Mac itself (see clock calibration).
- **The key firmware** is ours, `tools/usbkey` (SPEC Q-15): bare metal, no Pico SDK. It does what
  the open-smc-pad sketch does and reports more:
  - waits until D+ has stayed low for 10 ms (no device on the bus), then sends the key at about
    50 kHz;
  - stops when the chip's D+ pull-up appears, lets go of the lines at once;
  - then watches D+ for 3 s and says whether the chip stayed on the bus (short lows are the
    host's bus resets) or left it (a low of 200 ms or more: the clock calibration failed);
  - gives up after 60 s;
  - an attempt starts at power-up, with BOOTSEL, or with `r` from the console; `x` aborts it, `d`
    holds both lines low for 300 ms (the host sees an unplug and a replug), `s` prints the
    state;
  - its USB port is a serial port with a time-stamped log, kept from power-up;
  - LED (GP25, Pico only): 2 Hz waiting for an empty bus, 10 Hz keying, on after the answer, a
    short blink every 2 s when idle.

  Build: `tools/env/build_usbkey.sh` → `build/usbkey/usbkey.uf2` (it also checks the image and
  runs the host tests of the key logic against a model of the chip). Install once: hold BOOTSEL
  while plugging the board in, it appears as the `RPI-RP2` drive, copy `usbkey.uf2` onto it.

## Rehearsal 0: the key on the bench

Without the mixer: the key firmware, its serial port and its attempt logic on real pins, with
resistors in place of the host and the chip. Nothing here touches the mixer.

**First run** (✅ 2026-11-09, a Pico):

- Installed through `RPI-RP2` (UF2 bootloader v3.0): the board comes back at once as "JieLi USB
  key (RP2040)", `1209:0001`, full speed, with a serial port `/dev/cu.usbmodem…`. This is the first
  time our RP2040 USB driver (`dcd_rp2040.c`), the second-stage boot and the clock setup ran on
  hardware.
- `console.py` finds the port by name; `s`, `h`, `r`, `x` and `d` answer.
- The key keeps its log while no terminal is open and sends it when one opens (a line logged with
  the port closed arrived on the next open). `console.py` threw that backlog away: `tty.setraw()`
  flushes input by default. Fixed: it now sets raw mode without a flush, and every session starts
  with what came before it ("terminal connected" at least).
- With nothing on GP2/GP3 the inputs float: an attempt may read D+ as low and start keying. That
  says nothing about the logic; the bench below does.
- Keying rate, from the key counter: 141 438 keys in the 60 s of an attempt, 0.42 ms a key: 16
  bits of about 23 µs (43 kHz), then about 0.06 ms of the main loop. The chip takes a bit per
  rising edge of D+, so the gap does not matter.

**The bench** (optional; skipped by the owner on 2026-11-09: rehearsal 1 tests the same with the
real chip). On the Pico's pins (physical numbers):

| From | Through | To | Stands for |
|---|---|---|---|
| GP2, pin 4 | 270 Ω | point "D+" | the key's clock line, as in "Hardware" |
| GP3, pin 5 | 270 Ω | point "D−" | the key's data line |
| point "D+" | 10–47 kΩ | GND (pin 3, 8 or 38) | the host's pull-down: an empty bus |
| point "D−" | 10–47 kΩ | GND | the host's pull-down |
| point "D+" | 1–2.2 kΩ, touched by hand | 3V3(OUT), pin 36 | the chip's answer: its D+ pull-up |

Never connect 3V3 to GP2 or GP3 without the resistor: the key drives the lines low. Through
1.5 kΩ the current is under 2 mA.

| Step | Expected in the console |
|---|---|
| `r` with the pull-downs in place | "D+ low: sending key 0x16ef"; the LED blinks at 10 Hz |
| touch the 1.5 kΩ to 3V3 and hold it | "the chip answered after N keys …"; the LED stays on; after 3 s "the chip stays on the bus" |
| `r`, touch for less than 3 s, then let go | after 3 s "the chip left the bus (D+ low …)" |
| `r` with the 1.5 kΩ held before it | no keying: D+ is high, the key waits for an empty bus; after 60 s "no answer" |
| `x` while keying; `d` while idle | "aborted, lines released"; "replug: both lines low for 300 ms" |

Not covered: the key's timing at the chip (that needs an oscilloscope) and the BR25's real answer;
rehearsal 1 is the test of both.

## Rehearsal 1: read only

Passing it proves the path that recovery needs: entering the mode, running the loader, reading
everything back correctly. It erases and writes nothing in flash. The loader may set the flash
status registers, as noted above.

**Before you start**

- The stock firmware works.
- `jlrom` is built and passes its own checks (no device, no root: the CRC, the keystream, the
  command blocks, the pinned loader):

  ```
  swiftc -O -swift-version 5 -o build/jlrom tools/jlrom/main.swift
  build/jlrom selftest
  ```

- **A fresh pair of stock dumps, taken in this session**, with the stock firmware's own read
  (as in stage 0, read only):

  ```
  build/smcmidi sinco-read --port SMC-Mixer-Private --out materials/dumps/stock-r1-a.bin
  build/smcmidi sinco-read --port SMC-Mixer-Private --out materials/dumps/stock-r1-b.bin
  cmp materials/dumps/stock-r1-a.bin materials/dumps/stock-r1-b.bin
  ```

  The dumps of 2026-11-07 will not do: they were read at 14:18, and the captures of that
  afternoon switched between DAW and User mode several times. The stock firmware keeps the mode
  in its VM (⚠️ [hardware.md](hardware.md)), so the flash is no longer what those dumps hold, and
  a correct ROM read would be reported as a mismatch.
  - What changed since then is worth a look: `compare.py --raw` on the old dump and a fresh one
    lists the sectors (both are stock reads, so they compare as they are). It answers where the
    stock firmware writes ([image.md](image.md), "Still open").
- A copy of `materials/dumps/` somewhere outside the project folder: it is the only backup of
  this device's flash.
- The tap's wires to the RP2040 are short (a few centimetres): once the chip answers, they hang
  on full-speed USB lines as stubs.

**Steps**

1. Wire the RP2040 into the cable as above. The cable is unplugged **at the mixer**, and the
   mixer is **switched off**. It has a battery: switched on, the chip keeps running when the cable
   is pulled, and the key only works while the chip starts.
2. Plug the RP2040 into the Mac and open its console:

   ```
   python3 -I tools/usbkey/console.py --log logs/recovery/r1-key.jsonl
   ```

3. Plug the tapped cable's host end into a port of the Mac itself, its other end still not in the
   mixer. Start an attempt with `r` and **wait for "D+ low: sending key 0x16ef"** (the LED blinks
   at 10 Hz). With the host's pull-downs on the lines and no device, the key arms after 10 ms and
   then sends for 60 s.
   - The key must already be on the lines when the chip starts: how long the ROM listens for it
     is not known (⚠️). The attempt's first line says "power the mixer on now" before the key is
     armed; wait for the "sending key" line all the same.
4. Within those 60 s, plug the cable into the mixer, then switch it on during keying.
   ✅ Rehearsal 1 established that USB alone does not start the chip with its switch off
   (2026-11-10, "First contact" above).
   - Expected: "the chip answered", then after 3 s "the chip stays on the bus".
   - "the chip left the bus (D+ low for 0 ms, …)" is a false alarm of the key firmware: its last
     sample of D+ fell on a packet. Go by step 6.
   - Any other "the chip left the bus", or a timeout: see "If it fails".
5. If macOS says the disk is not readable, press **Ignore**, never **Initialize**: it would write
   to the chip.
6. `build/jlrom list` shows the device with `"verdict" : "chip"` (or `"maybe"` if macOS has no
   SCSI identity for it).
7. Read the chip's identity:

   ```
   sudo build/jlrom info --log logs/recovery/r1-info.jsonl
   ```

   This uploads and starts the loader, then prints the identity. Expected: `chip_key` `0x980F`
   (✅ reported in rehearsal 1), a flash ID and a buffer size.
8. Read the whole flash:

   ```
   sudo build/jlrom read --out materials/dumps/rom-read-1.bin --log logs/recovery/r1-read.jsonl
   ```

   Expected: `crc_check` `ok`. The loader's CRC of every 32 KB equals the CRC of what arrived.
   - `jlrom` never replaces a file: `--out` must be a new name (under `sudo` even a read-only
     dump would give way otherwise).
   - The data is saved as `rom-read-1.bin.unverified` as soon as it has arrived, and renamed once
     the CRC check is through: to the name asked for, or to `rom-read-1.bin.crc-mismatch` if the
     check fails. Only a file under the name asked for is a checked read.
   - The output goes to `materials/dumps/`: it holds Bluetooth keys and vendor code (SPEC C-7).
9. Compare the read with the fresh stock dump:

   ```
   python3 -I tools/jlrom/compare.py materials/dumps/rom-read-1.bin materials/dumps/stock-r1-a.bin
   ```

   Expected:
   - the plain area is equal (sectors of the stock firmware's VM, `0x24000` up to `0x6E000`, may
     be listed and marked: the stock firmware rewrites them, and that does not fail the
     comparison; any other sector does);
   - base `0x1000` explains the whole area below `0x24000`, and sector 0 is equal through the
     keystream (write the result into image.md).
10. Leave the mode: `sudo build/jlrom run-app`, or unplug the mixer. Then check that the stock
    firmware answers:

    ```
    build/smcmidi sinco-info --port SMC-Mixer-Private
    ```

11. Repeat steps 1–10 once more, reading into `rom-read-2.bin` (logs `r2-…`), and compare the two
    reads with each other:

    ```
    python3 -I tools/jlrom/compare.py --raw materials/dumps/rom-read-1.bin materials/dumps/rom-read-2.bin
    ```

    Expected: equal, or different only in the stock firmware's VM (it ran in step 10).

**It passes when**

- `info` shows the chip key `0x980F`;
- both reads pass the CRC check (each is saved under the name asked for);
- `compare.py --raw` reports `ok` for the two reads: equal outside the stock firmware's VM;
- `compare.py` reports `ok` for each read against the fresh stock dump;
- the stock firmware runs afterwards.

Then copy the two reads next to the other copy of the dumps: a raw read is the only complete
backup.

The logs record every SCSI command: its command block, lengths, CSW status and time. Flash data
never goes into the logs, only its CRC-16, so the logs can be shared.

**If it fails**

| Symptom | Likely cause, what to try |
|---|---|
| `no answer in 60 s` | Wiring: D+/D− swapped, no common ground. The chip was already running when the key started: switch the mixer off (the battery keeps it running without the cable), `r`, wait for "sending key", then plug it in or switch it on during the attempt |
| `the chip left the bus`, or no USB device appears, or the stock firmware starts | Clock calibration: a hub or other devices on the bus. Use another port of the Mac. (With "D+ low for 0 ms" it is the key's false alarm: look at `jlrom list`) |
| `list` shows nothing that could be the chip | Check `ioreg -p IOUSB` for what appeared; `list` prints every mass-storage device with its verdict |
| The inquiry does not name `BR25` | `jlrom` gives the device back and stops. Look at the log |
| The first loader block is refused (the log's first `FB 06` command has CSW status 1) | Stop and inspect the command, pinned loader and transfer log. The CRC byte order is already confirmed by rehearsal 1 (all 58 blocks accepted); do not blindly reverse it from this old hypothesis. A refusal before loader start is not a flash write |
| A later loader block or the jump fails | The log shows the failing command and its CSW. Try again from step 1 |
| `info` fails right after the log's `loader_started` | ⚠️ The loader may come up as a new USB device, which the running `jlrom` no longer holds. Run `info` again: it finds the loader running and goes on |
| `read` stops after the data has arrived | What was read is in `….unverified`, not checked against the chip's CRC. Move it away and read again |
| `crc_check` lists ranges (the read is saved as `….crc-mismatch`) | Read again under another name. If it repeats at the same addresses, the read path is wrong, not the data |
| `compare.py` reports a sector outside the stock firmware's VM | Not a pass. Was the stock dump taken in this session? Read once more and compare the two reads (`--raw`) before concluding anything about the image model |

## Rehearsal 2: write

The step that makes C-6 complete before the first write by our own firmware (stage 2). The write
path in `jlrom` exists since 2026-11-10 (the owner's approval of that day). **Passed on the mixer on
2026-11-10** (the result below).

**What the loader does** (✅ from `br25loader.bin` of the pinned SDK, descrambled and disassembled
into `materials/re/loader/`, not from jl-uboot-tool; erase/write were subsequently confirmed
by rehearsal 2):

- `FB01` sector erase: address in bytes 2–5, big-endian. The reply echoes `FB 01`; byte 2 is the
  result of the flash driver's erase (its meaning is not known: `jlrom` logs it and judges by the
  chip's CRC of the sector afterwards).
- `FB04` flash write: address in bytes 2–5, length in bytes 6–7, big-endian; the data follows in
  the data-out phase and goes to the flash driver 256 bytes at a time. The loader checks no CRC
  (jl-uboot-tool: "now absent"); `jlrom` still fills bytes 8–10 as in `FB06`. Flash must be erased
  first.
- Both writes can pass through an AES step, but only after a separate command sets a key; `jlrom`
  never sends it, so the data is written as given: the stored form, which is what a raw read
  returns.
- ✅ The loader lifts the flash's write protection by itself (`norflash_write_protect` in its
  strings): erase and write worked with no other command (2026-11-10).

**The write path** (`tools/jlrom/main.swift`):

- `erase` and `write` change whole 4 KB sectors inside `--allow START:END`, which is required and
  may not overlap sector 0, `BTIF`, `EXIF` or `USRFLASH`. Each sector: erase, check it reads
  erased (the chip's CRC), write, check (the chip's CRC and a read back). Every argument is checked
  before the device is taken; an unknown option stops the run.
- `restore --in RAW` writes a whole raw read back. It refuses a file that is not 512 KB, a stock
  read (its directory at `0x1000` is plain) and an erased sector 0. It reads the flash first and
  lists the sectors that differ; only with `--apply` (and `--before F`, where the flash as it was
  is saved) does it rewrite them, and a protected sector only with `--protected`, sector 0 last.
  Then it checks the whole flash against the file.
- `selftest` checks the command blocks and every refusal above.
- The logs carry the CRC-16 and SHA-256 of what was written, never the data.

**Steps** (each `jlrom` command needs its own entry into the ROM mode: switch off, `r`, switch on):

1. Write a test pattern into the empty last sector (erased in both raw reads of 2026-11-10):

   ```
   sudo build/jlrom write --addr 0x7F000 --pattern --allow 0x7F000:0x80000 --log logs/recovery/r2-write.jsonl
   ```

   Expected: "written and checked: 0x0007F000", `check` `ok`.
2. Read the flash and compare with the raw read: only `0x7F000` may differ.

   ```
   sudo build/jlrom read --out materials/dumps/rom-read-r2-pattern.bin --log logs/recovery/r2-read-pattern.jsonl
   python3 -I tools/jlrom/compare.py --raw materials/dumps/rom-read-1.bin materials/dumps/rom-read-r2-pattern.bin
   ```

3. Undo it with the restore path, so that it is rehearsed on the same safe sector: first the plan,
   then the write. Expected plan: `sectors_differ` `["0x0007F000"]` (and maybe the stock VM's
   sector, if the stock firmware wrote it), `protected_differ` empty.

   ```
   sudo build/jlrom restore --in materials/dumps/rom-read-1.bin --log logs/recovery/r2-restore-plan.jsonl
   sudo build/jlrom restore --in materials/dumps/rom-read-1.bin --apply --before materials/dumps/before-r2-restore.bin --log logs/recovery/r2-restore.jsonl
   ```

   Expected: "restored and checked" for each sector, `crc_check` `ok`.
4. A last raw read must equal `rom-read-1.bin` (`compare.py --raw`: `ok`), and the stock firmware
   must answer (`smcmidi sinco-info`).

**Result** (2026-11-10, logs `logs/recovery/r2-write.jsonl`, `r2-restore-plan.jsonl`,
`r2-restore.jsonl`, `r2-read-final.jsonl`; step 2 was left out: the restore plan reads and compares
the whole flash):

- `write --pattern` into `0x7F000`: erase (reply result byte `00`, 0.4 ms), the sector checked
  erased; `FB04` with 4 KB in 30 ms; the chip's CRC `46B3` equal to the pattern's; the read back
  equal. The stock firmware ran afterwards.
- `restore` plan: `sectors_differ` `["0x0007F000"]`, `protected_differ` empty: the write touched
  that sector alone, and the stock firmware's VM had not changed.
- `restore --apply`: the flash as it was saved to `before-r2-restore.bin` (it differs from
  `rom-read-1.bin` in `0x7F000` alone); the sector erased (the reply after 0.5 ms, result `00`; the
  CRC that followed shows it erased, so the loader waits for the erase before the next command);
  the whole flash checked against `rom-read-1.bin`: `crc_check` `ok`.
- The last raw read `rom-read-r2-final.bin` equals `rom-read-1.bin` (`compare.py --raw`: `ok`);
  the stock firmware answers `SMC-Mixer_012`. Copied next to the other dumps outside the project.
- The CSW of every loader command reports a residue equal to the whole transfer, data or not:
  the loader does not count it, and `jlrom` does not rely on it.

**If it fails**

| Symptom | What it means, what to do |
|---|---|
| "the sector does not read back erased … Write protection?" | The erase did not take; nothing was written. Look at the reply's result byte in the log. The loader may need the flash's protection lifted: find where `norflash_write_protect` is called (`materials/re/loader/loader.lst`) before trying again |
| "the chip's CRC is …, the data's …" after a write | The write went wrong or went elsewhere. The sector is in an unknown state: read the flash (step 2) and compare before anything else |
| The device drops off during a command | Enter the mode again; read the flash and compare. The sector may be half written: `restore` puts it back |

**Historical follow-ups after rehearsal 2**

- Restoring a raw read over the whole flash, sector 0 included (`--protected`): only when it is
  needed and authorized. ✅ It ran both ways on 2026-11-11 (next section). R8, entering ROM with
  sector 0 deliberately left erased, was not tested by rewriting it in one uninterrupted run.
- The installation, if the owner chooses this path for it (SPEC Q-18). `tools/image/installplan.py`
  lays out its writes, their CRC-16s and the rollback, made from the raw read ([image.md](image.md),
  "Installing: the dry run"). The original proposal was an additional guarded sector-0 case;
  current `restore --protected` can apply a checked raw image, while `write` still excludes
  sector 0. There is no dedicated plan-execution command. The normal public installation route
  is now the browser over stock firmware, not `jlrom`.
  - That sector differs from the raw read in the `app_dir_head` entry alone, and `jlrom` will have
    to check this in any future dedicated plan-execution command.
  - The sector has to be erased first (4 bits of the entry go from 0 to 1), so until it is
    written back the chip has no uboot. This is the step the key exists for.

## The rollback to the stock firmware and back (2026-11-11)

Run with the owner's go-ahead, to see whether the stock firmware shows the LEDs' faint glow
([hardware.md](hardware.md), "The faint glow of 13 LEDs"). Logs `logs/recovery/app15-read.jsonl`,
`stock-restore.jsonl`, `app15-restore-2.jsonl`.

1. With our controller running: the ROM mode by `reset soft` and the key, then
   `jlrom read --out materials/dumps/rom-read-app15.bin` (the CRCs agreed). Against
   `rom-read-1.bin` it differs in 22 sectors: sector 0 and `0x41000..0x55FFF`; the stock area
   untouched.
2. The ROM mode again, then `jlrom restore --in materials/dumps/rom-read-1.bin --apply --before
   materials/dumps/before-stock-restore.bin --protected`: ✅ the 22 sectors restored and checked,
   sector 0 last, `crc_check` `ok` over the whole flash. Released, the stock firmware started at
   once ("SINCO" on the USB), with no power cycle.
3. Back: the stock firmware has no soft reset to the ROM mode, so the key (`r`) and a power cycle
   by hand. The first try: the key keyed the chip into the ROM mode, but `jlrom` ran a minute
   later, when the mixer had been switched again and the stock firmware ran ("no device"). The
   second: a wait in the same shell, `until ioreg -p IOUSB -l | grep -q BR25UBOOT; do sleep 0.5;
   done; sudo build/jlrom restore …`, so `jlrom` takes the chip as soon as it appears. ✅
   `restore --in materials/dumps/rom-read-app15.bin --apply --before
   materials/dumps/before-app15-restore.bin --protected`: the 22 sectors written and checked,
   `crc_check` `ok`; our controller started (build `ab3645cb…`).

So the way back to the stock firmware and forward again works with the key alone, about 10
minutes. ❓ R8 (whether the ROM mode works with sector 0 erased) is still not known: sector 0 was
written in the same run, never left erased.

## Sources

- kagaimiq, jielie: [USB key](https://kagaimiq.github.io/jielie/isp/usb/usb-key.html), [ISP](https://kagaimiq.github.io/jielie/isp/).
- jl-uboot-tool (MIT), the USB protocol and its BR25 settings: https://github.com/kagaimiq/jl-uboot-tool.
  - Read for the facts above; no code was taken from it.
  - It talks to the device through Linux or Windows SCSI pass-through only, so it does not run on
    macOS.
- open-smc-pad, `hw/jl-usbkey/jl-usbkey.ino`: the key sender and its wiring; our firmware follows
  its method (no code taken).
- The loader: `cpu/br25/tools/br25loader.bin` of the pinned JieLi AC63 SDK.

## Tools

- `tools/usbkey/` — the RP2040 key firmware: `key.c` (the key logic, hardware-independent; tests
  in `test_key.c`), `main.c` (clocks, pins, log, commands), `dcd_rp2040.c` (our TinyUSB driver
  for the RP2040's USB controller), `boot2.S` (the second-stage boot), `uf2.py` (boot2 CRC and the
  UF2 file); `console.py` — its terminal on the Mac (finds the key's own port through ioreg).
- `tools/jlrom/main.swift` — the host side.
  - No dependencies: IOUSBHost and IOKit of macOS.
  - Root is needed to take the device from the macOS mass-storage driver.
  - Commands: `list`, `info`, `read`, `crc`, `erase`, `write`, `restore`, `run-app`, `selftest`.
  - It erases and writes flash only through `erase`, `write` and `restore` (rehearsal 2), one
    sector at a time; `selftest` checks the guards and that it knows no block or chip erase and no
    chip key write.
  - `read` never replaces a file and saves what it read before the CRC check.
  - `selftest` covers protocol construction/guards without a device. The USB path has run on
    the mixer (rehearsals 1/2, the 2026-11-11 rollback and the 2026-11-15 restricted reflash);
    these do not exhaustively test IOUSBHost failures or all protected-sector writes.
- `tools/jlrom/compare.py` — compares a ROM read with a stock dump, or two ROM reads with each
  other (`--raw`); a difference in the stock firmware's VM is marked and does not fail it unless
  `--strict`. Tests: `python3 -I tools/jlrom/test_compare.py` (run by `make quick`).
