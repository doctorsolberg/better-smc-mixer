# Bring-up plan

How our firmware first meets the mixer: the order of the steps, what to do when a step fails, and
one register of everything the code assumes about the device without having seen it. The
procedures themselves stay in their documents ([recovery.md](recovery.md), [image.md](image.md),
[hwtest.md](hwtest.md)); this file puts them in order and says what each one settles.

**Current scope:** this began as the 2026-11-10 pre-contact plan. The ladder ran on
2026-11-10/11: ROM read/write rehearsals, stock SysEx installation, diagnostic starts,
hardware-test manual checks and controller scenarios. The failed runs and hypotheses below are
chronological evidence, not today's status. Installation, update and return over USB and
Bluetooth (procedures: [image.md](image.md)) ran on a retail mixer on 2026-11-23 with physical
off/on and byte-exact configuration read-back.

Every step talks to the mixer, so every step needs the owner at the bench and the owner's
go-ahead for that session ([AGENTS.md](../AGENTS.md), "Hard rules").

## Rules for the bench

- **One unknown at a time.** A step changes one thing against the last state known to work.
- **The way back before the way forward.** No step is taken whose failure cannot be undone with
  what earlier steps have proven.
- **Stop at the first failure.** Record it, leave the device in a known state (the stock
  firmware, if the step can be rolled back), and work out the cause offline. No second attempt
  without a new hypothesis written down.
- **Every step leaves a record** under `logs/` (a session directory or a `.jsonl` log), named in
  the step's result. `logs/` and `materials/` stay local (SPEC C-7).
- **An assumption is settled only by what was seen.** When a step settles a row of the register
  below, the row's ⚠️ or ❓ in its home document becomes ✅ with the session as the source, or the
  document and the code change.

## The ladder

| Step | What | Procedure | Needs | Writes flash |
|---|---|---|---|---|
| 0 | Preconditions | below | | no |
| 1 | Rehearsal 1: the ROM download mode, read only | [recovery.md](recovery.md), "Rehearsal 1" | the key tapped into the cable | no (⚠️ R7) |
| 2 | Rehearsal 2: a write path, on an empty sector | recovery.md, "Rehearsal 2" | a write path in `jlrom` (W1); the owner's approval | one empty sector |
| 2b | The SysEx path: an empty sector, then sector 0 with its own contents | "Step 2b" below | `smcmidi`'s write path; the owner's go-ahead | one empty sector; sector 0 (same bytes) |
| 3 | Install the hardware-test image | [image.md](image.md), "Installing: the dry run" | SPEC Q-18 decided; step 2 passed | our area, then sector 0 |
| 4 | First power-on | "First power-on" below | | the SDK's own settings (⚠️ I5) |
| 5 | The first run | [hwtest.md](hwtest.md), "On the device: the first run" | the CDC port | no |
| 6 | The manual test procedure | hwtest.md, "On the device: the test procedure" | a person at the panel | no |
| 7 | The controller (`app` variant) | [core.md](core.md), "Scenarios on the device" | steps 5 and 6 passed; the tables corrected | a new image |
| 8 | Later checks | "Later" below | | |

### Step 0: preconditions

- The stock firmware works and answers: `build/smcmidi sinco-info --port SMC-Mixer-Private`.
- A fresh pair of stock dumps is taken in this session and the two match (recovery.md, "Before
  you start": the dumps of 2026-11-07 are older than the last writes of the stock firmware). A
  copy of `materials/dumps/` exists outside the project folder.
- `build/jlrom selftest` passes.
- `make -C firmware test`, `tools/env/build_app.sh` in both variants and `tools/env/build_boot.sh`
  pass on the tree that will be installed; the image's build ID is written down.
- The key runs on the Pico (recovery.md, "Rehearsal 0") and is tapped into the mixer's cable; the
  mixer's side is in a port of the Mac itself (R3).

### Step 1: rehearsal 1

Settles R1–R7. It also gives the first raw read, which is the only complete backup
(recovery.md, "What a read returns") and the input the installation plan should be made from.

Failures and what to try: recovery.md, "If it fails". If the mode cannot be entered at all, the
ladder stops here: there is no way back from a failed write, so there is no write.

### Step 2: rehearsal 2

Settles R9: erase, write and CRC check through the loader, on a sector nothing uses (`0x7F000`),
and that the stock firmware still runs afterwards.

What it cannot settle is R8, that the mode still works once sector 0 is erased. Nothing short of
erasing sector 0 can. The grounds for it are that the mask ROM's mode runs before anything in
flash is read (⚠️ kagaimiq; recovery.md). The owner decides whether that is enough before step 3.

### Step 2b: the SysEx path

The owner's direction for SPEC Q-18 (2026-11-10): first see whether the stock firmware can
install without extra hardware. Settles R10 and R11. The key stays tapped in as the safety net.
Passed on 2026-11-10 (result below). Each run needs the owner's go-ahead.

`smcmidi` builds erase and write packets only for a checked sector (the header of
`tools/smcmidi/main.swift`, "Safety"), and checks every result by a read: the stock firmware
acknowledges both the same way whatever happened ([hardware.md](hardware.md), "Stock service
protocol"). `make smcmidi` builds it and runs its self-test. With the files of this chip, still
offline:

```sh
build/smcmidi selftest --raw materials/dumps/rom-read-1.bin --stock materials/dumps/stock-r1-a.bin --in materials/install/hwtest/sector0-after.bin
```

✅ 2026-11-10: the raw read is accepted, the stock read's sector 0 equals it through the per-read
keystream, and the plan's new sector 0 differs in bytes `0x60`, `0x61`, `0x66` alone (boot
pointer `0x41000`).

Set-up: as for the stock reads of rehearsal 1 (recovery.md, "Before you start"): the mixer through
the tap in a port of the Mac itself, the key's resistors out, the switch on.

1. The stock firmware answers: `build/smcmidi sinco-info --port SMC-Mixer-Private`.
2. A pattern into the empty sector `0x7F000`:

   ```sh
   build/smcmidi sinco-write --port SMC-Mixer-Private --addr 0x7F000 --pattern --allow 0x7F000:0x80000 --log logs/recovery/s1-write.jsonl
   ```

   It refuses unless the sector reads erased; `result` `ok` means the read back equals the
   pattern. Optional, independent: the chip's CRC in the ROM mode (`sudo build/jlrom crc --start
   0x7F000 --size 0x1000`) is `46B3`, as in rehearsal 2.
3. Erase it again, and see the stock firmware still answers:

   ```sh
   build/smcmidi sinco-erase --port SMC-Mixer-Private --addr 0x7F000 --allow 0x7F000:0x80000 --log logs/recovery/s1-erase.jsonl
   ```

   Steps 2 and 3 settle R10. The flash is then as `rom-read-1.bin` again.
4. Sector 0 with its own contents, plan only: `chip_now` must be `as_raw_read`.

   ```sh
   build/smcmidi sinco-sector0 --port SMC-Mixer-Private --raw materials/dumps/rom-read-1.bin --log logs/recovery/s2-plan.jsonl
   ```

5. The same with `--apply` (`--log logs/recovery/s2-apply.jsonl`). It erases sector 0, checks the
   erase, writes the 16 pages, checks them, and reports `ms_erase_to_written`: how long the chip
   had no uboot. If the erase does not take, it stops with nothing written: write protection
   (R11 false), and the SysEx path cannot install.
6. Switch the mixer off and on: the stock firmware must start (the uboot works), and
   `sinco-info` answers. Settles R11 for sector 0.

**Result** (2026-11-10, `logs/recovery/s0-info.jsonl` … `s2-after-check.jsonl`): passed.

- `sinco-write` of the pattern into `0x7F000`: 16 packets of 256 bytes, each acknowledged in
  3.6–4.3 ms (67 ms for the sector); the read back equal, CRC `46B3` as in rehearsal 2.
- `sinco-erase`: acknowledged in 4 ms; the sector reads back erased (the next command waits for
  the erase); the stock firmware answers.
- `sinco-sector0 --apply`, sector 0 with its own contents: one attempt. The erase took (the sector
  read back erased before anything was written: no write protection on sector 0), the 16 pages
  written and read back equal. **The chip had no uboot for 113 ms** (erase to last page); the
  check read took 35 ms more.
- Switched off and on: the stock firmware starts and answers `SMC-Mixer_012`; sector 0 reads as
  `rom-read-1.bin` (`chip_now` `as_raw_read`).

So the stock firmware can install without extra hardware: sector 0 is the only risky write, and
its window is about a tenth of a second.

**If it fails**

| Seen | What to do |
|---|---|
| A step refuses before sending anything (not erased, `chip_now` `other`) | Nothing changed. Read the message; do not force it |
| "the erase did not take: sector 0 is unchanged" | Nothing written. Write protection: the SysEx path cannot write sector 0 |
| `sinco-sector0` fails after the erase, or the reply stops | Do not switch off. Run it again with `--repair`. If that fails too: the key, `jlrom restore --in materials/dumps/rom-read-1.bin --apply --before F --protected` (recovery.md) |
| The mixer does not start after step 6 | The key: enter the ROM mode, read, compare with `rom-read-1.bin`, `restore --protected`. R8 decides whether this works |

After it passed, step 3 is prepared this way: "Step 3" below.

### Step 3: installing

This is the historical first-install procedure, prepared and run on 2026-11-10 after step 2b.
It wrote/read-checked our area and switched sector 0; the first boot failed as the diagnostic
record below shows. **Do not reuse the following archived command as a current recipe:** its
missing `--cfg` caused that startup failure; current `installplan.py` requires `--cfg` or an
explicit `--no-cfg` ([image.md](image.md), "Installing: the dry run"). Every new run requires
the owner's go-ahead and a plan from the exact current backup/build.

The inputs, made without hardware on 2026-11-10:

- the hardware-test image, build ID `ceb992ab770883e20096a35d44dbbe7d9c807cf0`
  (`VARIANT=hwtest tools/env/build_app.sh`, after `make test`; `build_app.sh` and
  `build_boot.sh` passed too);
- the plan from the raw read, in `materials/install/hwtest-r1/` (17 checks hold):

  ```sh
  python3 -I tools/image/installplan.py materials/dumps/rom-read-1.bin --app build/hwtest/app.bin --out materials/install/hwtest-r1
  ```

  Our area `0x41000..0x51D77` (17 sectors, 270 writes of 256 bytes, CRC-16 `0x1F17`); sector 0
  with the boot pointer `0x41000` (bytes `0x60`, `0x61`, `0x66` change; CRC-16 `0x535F`). The plan's
  "flash after" differs from the raw read in sector 0 and those 17 sectors alone. `smcmidi
  selftest --raw … --in …/sector0-after.bin` accepts the new sector 0.

Set-up as for step 2b. With `D=materials/install/hwtest-r1`:

1. The stock firmware answers (`sinco-info`), and sector 0 is still the raw read's:

   ```sh
   build/smcmidi sinco-sector0 --port SMC-Mixer-Private --raw materials/dumps/rom-read-1.bin --in $D/sector0-after.bin --log logs/recovery/i1-plan.jsonl
   ```

   `chip_now` must be `as_raw_read`.
2. Our area. It refuses unless all 17 sectors read erased, and checks each one by a read:

   ```sh
   build/smcmidi sinco-write --port SMC-Mixer-Private --addr 0x41000 --in $D/area-phys.bin --allow 0x41000:0x52000 --log logs/recovery/i2-area.jsonl
   ```

   **Stop and check:** switch the mixer off and on; the stock firmware must still start
   (`sinco-info`). Nothing points at our area yet, so this half is harmless. Undo it with
   `sinco-erase --addr 0x41000 --size 0x11000 --allow 0x41000:0x52000`.
3. Just before the switch, the area is still as written (the stock firmware's VM entry spans it,
   though so far it writes only `0x24000`):

   ```sh
   build/smcmidi sinco-read --port SMC-Mixer-Private --start 0x41000 --size 0x10D78 --out /tmp/area-check.bin && cmp /tmp/area-check.bin $D/area-phys.bin
   ```

4. The boot pointer, about 0.1 s without a uboot (step 2b):

   ```sh
   build/smcmidi sinco-sector0 --port SMC-Mixer-Private --raw materials/dumps/rom-read-1.bin --in $D/sector0-after.bin --apply --log logs/recovery/i4-sector0.jsonl
   ```

   `result` `ok`: sector 0 reads back as `sector0-after.bin`. The stock firmware keeps running
   until the next power-on.
5. Switch the mixer off and on: step 4 below.

**The way back is the key from here on.** Once sector 0 points at our area the stock firmware
does not start, so its SysEx is gone, and the hardware-test image cannot write flash (its link
changes RAM only, [link.md](link.md)). Rollback: the ROM mode, `jlrom read` (keep it), then
`jlrom restore --in materials/dumps/rom-read-1.bin --apply --before F --protected`. It rewrites
sector 0 and erases our sectors; it also puts the stock VM sector back as it was on the day of
the raw read. ✅ It ran on 2026-11-11, both ways ([recovery.md](recovery.md), "The rollback to
the stock firmware and back"). A way back
without extra hardware for other people subsequently became STOCK back in the controller:
✅ 2026-11-14 over USB and BLE MIDI ([image.md](image.md), "Going back"). The hardware-test
variant still lacks that controller operation; the ROM key remains the recovery fallback.

### Step 4: first power-on

Look in this order. Each line needs less to work than the one after it.

| # | Sign | It shows |
|---|---|---|
| a | LEDs light one after another, one a second (the LED walk) | the image boots, the SDK's timers run, the 595 chain and the LED matrix work at least in part |
| b | A new USB device: `ioreg -p IOUSB` shows `1209:0001`, "SMC-Mixer hardware test" | our USB driver attaches and enumerates |
| c | A serial port `/dev/cu.usbmodem*` and two MIDI ports | the descriptors and the shared endpoint are accepted |
| d | `smclog.py cmd --port … info` answers with the build ID of step 0 | the command link works: go to step 5 |

macOS may ask to allow the accessory (new descriptors); allow it.

**If it fails**

| Seen | Likely | Find out | Then |
|---|---|---|---|
| No LEDs, no USB device | The image does not start (I1, I3, I4), or it crashes early and the watchdog resets it every 8 s (P3) | The key's console, `s`: D+ high means our driver switched the pull-up on, so the image runs. Enter the ROM mode and read the flash: does it equal `flash-after.bin` (a bad write if not)? Are the sectors of our VM still erased (I5: if the SDK's settings code ran, they are not)? | Roll back. Do not retry the same image |
| LEDs walk, no USB device at all | Our code runs; the USB driver does not attach (U1, U2) | `s` on the key: D+ low means no pull-up. macOS log for a device that appears and fails | Roll back or leave it: the stock is one rollback away. Next image: W2 or W3 |
| A device appears, enumeration fails or no serial port | Endpoint 0 or the descriptors (U3–U7) | `ioreg -p IOUSB -l`, the macOS log (`log show --last 5m --predicate 'subsystem == "com.apple.iokit.usb"'`, ⚠️ not tried) | The fallback of [usb.md](usb.md), "Endpoints in the application"; a new image |
| USB works, no LEDs | The 595 chain, PB5 or the LED polarity (N1, N7, N8) | The link works: raw input mode and the log say what the matrix reads | Go on with step 5; this is the easy case |
| It comes and goes every 8 s or so | A hang (the watchdog) or a crash loop | If the link comes up in between: `smclog.py cmd … boot`, the journal names the reset and a crash record | Roll back unless the journal explains it |
| The port answers, `info` shows another build ID | Not the image of step 0 | | Stop: find out what was installed |

There is no log channel besides USB in the image as it is: the UART is off (its default pin, PC4,
is one of the fader candidates), no LED shows a state, and nothing is kept in flash. With the case
closed, the LED walk and the D+ level are all there is. W2 and W3 are about that.

#### Step 4: boot marks

A diagnostic build, `BOOTMARK=1 VARIANT=hwtest tools/env/build_app.sh` (into
`build/hwtest-bootmark/`), appends one record to the SDK's VM at each stage of the start: item 49,
one byte, the stage (`firmware/platform/br25/bootmark_br25.h`). The VM is stored as is, so a raw
read in the ROM mode shows how far the start got. Never install it except for this diagnosis: it
writes flash on every start. Make arguments add diagnostic switches (off by default except
the running clock and core-in-RAM defaults)
(`firmware/app/Makefile`): `SYS_CLOCK_MHZ=n` sets the CPU clock first thing in `app_main()`
(run 14; the board starts at 96 MHz since run 19, and the current default is 192 MHz in
`app_main()` since controller 15), `SCAN_PROFILE=1` logs where the scan handler's time goes
(run 15), `NO_SDK_ADC=1` holds the SDK's ADC scan off (run 12), `EMU_CHAIN=1` builds our IRQ 0
handler (run 9). Example: `BOOTMARK=1 VARIANT=hwtest tools/env/build_app.sh SYS_CLOCK_MHZ=48`.

What the SDK does before our code, from the disassembly of the hwtest build (static analysis):
`main` → task `app_task` → `app_task_init`: early initcalls (`sys_event_init`, `sdfile_init`),
platform initcalls (`syscfg_tools_init`: opens the VM and writes its header `55 AA AA 55` on an
erased VM), then `board_init` (ours, inlined), the other initcalls, then our `app_main` (inlined).
A mark works only after the VM is open: before, `vm_write` stores nothing. `vm_write` also skips a
record equal to the item's stored value, so a repeated start adds nothing.

Install: a plan from the raw read of the stock flash (it gives the same sector 0, pointer
`0x41000`), the area padded with `0xFF` up to `0x6E000`, so one `jlrom write` also erases our VM:

```sh
D=materials/install/hwtest-bm2
python3 -I tools/image/installplan.py materials/dumps/rom-read-1.bin --app build/hwtest-bootmark/app.bin \
    --cfg deps/fw-AC63_BT_SDK/cpu/br25/tools/cfg_tool.bin --out $D
python3 -I -c "import sys; a=open(sys.argv[1],'rb').read(); open(sys.argv[2],'wb').write(a+b'\xff'*(0x2D000-len(a)))" $D/area-phys.bin $D/area-vm-phys.bin
sudo build/jlrom write --addr 0x41000 --in $D/area-vm-phys.bin --allow 0x41000:0x6E000
```

Since 2026-11-12 the controller keeps its configuration in two flash slots at
`0x25000..0x34FFF`, below our area ([config.md](config.md), "Storage"; from 2026-11-11 they were
at `0x5E000..0x6DFFF`). Padding to `0x6E000` as above leaves them alone and erases the whole VM
(old slots at `0x5E000` included), so this is again the one way to install. The first install of a
build with the new place starts on the factory configuration: push the configuration again.

`jlrom` gives the chip back and it starts the new image at once. Wait half a minute, enter the ROM
mode again, `sudo build/jlrom read --out …`, and look at the VM from `0x52000`.

| Run | Image | Marks | Seen |
|---|---|---|---|
| 1, 2026-11-10 | build ID `a103525a…`; marks from the end of `cfg_file_parse` on | none: the VM holds its header only (`rom-read-bm-1.bin`) | LEDs dim, no USB, as before |
| 2, 2026-11-10 | marks also at `board_init`'s entry (`0x0A`, right after the VM opens) and after `board_power_init` (`0x0B`), `adc_vbg_init` (`0x0C`), `adc_init` (`0x0D`) | 1020 records, `0A 0B 0C 0D` 255 times over, the last `0D` (`rom-read-bm-2.bin`): the marks work, and every start ends inside `cfg_file_parse`. 255 starts in at most 140 s (the restart after `jlrom write`, a power cycle, the ROM entry), so about two a second: the chip resets, it does not hang (the watchdog's 8 s would give at most 17) | LEDs dim, no USB, as before |
| 3, 2026-11-10 | the SDK's `cfg_file_parse` replaced by a copy with a mark after each step (`0x50`..`0x5C`, `bootmark_br25.h`); before `0x0A`, the reset's cause, raw (item 48, 2 bytes: `P3_RST_SRC`, `P3_PMU_CON1`) | the VM filled its first half and was compacted into the second (`0x54000`; the first erased), keeping about 67 starts (`rom-read-bm-3.bin`): `0A 0B 0C 0D 50` each time, never `51`. **Every start ends in the first `syscfg_read(CFG_BT_NAME)`.** Reset cause: `01 00` once (bit 0, VDDIO POR: the power cycle), else `00 40`: no cause bit set, P33 kept (bit 6 of `P3_PMU_CON1` from the start before), so no power loss, no watchdog | LEDs dim, no USB, as before |

Why, from the disassembly (static analysis, ⚠️ the fault itself not seen): item 101 is not in the BTIF table nor stored in the VM, so `syscfg_read` asks `cfg_tool.bin`. Our area has no `cfg_tool.bin` (image.md, "Still open"), so `syscfg_file_init` leaves `bin_cfg` zero and `__syscfg_bin_read` walks a table at address 0 with length 0. That read is what stops each start; the SDK's exception path resets the chip without a cause bit. The SDK has a default `cpu/br25/tools/cfg_tool.bin` (`0x360` bytes, the stock size; items 101 name `AC696X_1`, 601, 607 and the rest), which `installplan.py --cfg` puts after `app.bin`. Run 4 installs it.

| Run | Image | Marks | Seen |
|---|---|---|---|
| 4, 2026-11-10 | run 3's image, the area with the SDK's default `cfg_tool.bin` (`materials/install/hwtest-bm4`) | about 35 starts after a compaction (`rom-read-bm-4.bin`), each `0A 0B 0C 0D 50 51 52 53 54 55 56 58 59 5A 5B 5C 10 11 12 13 14 20 21`: `cfg_file_parse` and `board_init` pass, the SDK starts our `app_main`, the µs clock starts, **and every start ends inside our `diag_br25_begin`**; reset cause `00 40` again | LEDs dim, no USB |
| 5, 2026-11-11 | marks inside `diag_br25_begin` (`0x60`..`0x64`) and, through a hook of the boot-mark build, between the steps of `diag_boot_begin` (`0x70`..`0x77`) | about 16 starts (`rom-read-bm-5.bin`), each up to `21 60`, never `61`: **every start ends in the `request_irq` that installs our handler for the emulator exception (`IRQ_EMUEXCPT_IDX`)**. Likely an emulator event already pending, which fires once our handler is in; the handler records the crash in the kept block and resets the chip (⚠️ inferred) | LEDs dim, no USB |
| 6 | before that `request_irq`: the debug unit's registers (item 47: `EMU_MSG`, `MSG`, `PRP_MMU_MSG`, `PRP_WR_LIMIT_MSG`, `EMU_CON`) and the last start's crash record, when the kept block holds one (item 46: cause, `RETI`, `RETE`, `RETX`, `RETS`, the four messages, four trace PCs); after an emulator exception the next start leaves the SDK's handler (`0x65`) to see how far it gets | 2026-11-11, `rom-read-bm-6.bin`, 10 starts. They alternate: a start that installs our handler ends after `60` as before; the next one finds an emulator-exception record (cause 0), leaves the SDK's handler, and runs the whole start (`65 61 … 77 22 23 30 … 33 24 25 26`) into the LED walk (`40`, once `41`). Then the watchdog resets the chip (`R0400`, P3_RST_SRC bit 2) and the pair repeats. The debug unit before the `request_irq`: all four messages 0, `EMU_CON` 0xf. The crash record: `EMU_MSG`, `MSG`, `PRP_MMU_MSG`, `PRP_WR_LIMIT_MSG` all 0; `RETE` 0x112a20 (the ROM's `local_irq_disable`, its `cli`); `RETS` 0x113e92 (inside the ROM's `xTaskGetCurrentTaskHandle`, after its call); the trace: `exception_emu` ← 0x112a20 ← 0x113e8c (`xTaskGetCurrentTaskHandle`) ← 0x117df4 (its ROM entry stub); `RETI` differs per start (once 0x1e01954, `request_irq`+0x10, right after its store into the vector table). So our handler is entered at once, in the critical section that `request_irq` opens, with no message bit set: not a check of the debug unit that we can name. ⚠️ Not explained yet. ✅ USB: with this image running, the host lists the device "SMC_Mixer hardware test" (VID 0x1209, PID 1; macOS I/O registry). The walk sometimes stops at its first LED (the top M button of channel 1) until the watchdog resets the chip; on one start it ran on. ⚠️ Possibly the same event, reaching the SDK's handler, which loops in place when `exception_analyze()` returns (`goto` to itself in `exception_irq_handler`), so the watchdog ends it | first start: the LED walk, stopping at the first LED; later starts: the walk running on; USB present |
| 7 | our IRQ 0 handler installed by writing its vector slot alone (the SDK's setup already enabled IRQ 0 at priority 2), not by `request_irq()`; item 47 adds the vector slots of IRQ 0 and 1 as the SDK left them and `IPCON0`; a start skips ours only after a crash at its installation (before 1.5 s), so a later event reaches our handler and leaves a crash record instead of the SDK's loop and the watchdog; no mark after the start (no flash write during the walk) | 2026-11-11, `rom-read-bm-7.bin`, `logs/sessions/2026-11-11-bm7-log`: the vector slot alone does the same as `request_irq()`. Our handler is entered at the first of the ROM's interrupt functions run after it; this time `RETE` is 0x11465a, the ROM's `cpu_irq_disabled`, called through `os_mutex_pend`. **✅ The cause, from item 47 and the disassembly: IRQ 0 is not the SDK's crash handler.** `breakpoint_init()` (SDK, `request_irq(0, 3, breakpoint_isr)`) sets three hardware breakpoints (`EMU_CON` 0xf; registers from 0x10F088) on the ROM's `local_irq_disable` (0x112a20), `local_irq_enable` (0x112a2e) and `cpu_irq_disabled` (0x11465a). Its `breakpoint_isr` sends each to a RAM replacement (`bp_tab`: `local_irq_disable_rewrite` …) and returns. The slot held `breakpoint_isr` (0x16a56); IRQ 1's held `exception_irq_handler` (0x1818c). Taking IRQ 0 over turns every call of those ROM functions into a crash: this, not the hardware, ended every start of runs 5 to 7 (and, until `cfg_tool.bin`, came after the crash in `cfg_file_parse`). A real emulator exception under `breakpoint_isr`: no match in `bp_tab`, `rte` to the same instruction, again, until the watchdog (⚠️ from the disassembly). The starts that kept the SDK's IRQ 0 ran and served USB. They ended in watchdog resets (`R0400`, 6 in this dump), at least once while a host read the log | LEDs: the walk; USB present |
| 8, 2026-11-11 | every build leaves IRQ 0 to the SDK (our handler on IRQ 1 only); the boot-mark build still records items 46 and 47 at the start, and no mark after the start | Every start runs; none lasts. The port is on the bus for about 8 s, off for about 1 s, again and again, with no host attached. The walk lights 2 or 3 LEDs, stops, and the panel goes dark at the reset. A capture opened as the port appeared received nothing: the link's first reply takes longer than a start lasts (6.5 to 10 s in runs 6 and 7). ⚠️ Inferred: a real emulator exception that `breakpoint_isr` sends back to the same instruction until the watchdog. The same end came after most starts of runs 6 and 7; a few ran for minutes | the walk, 2 or 3 LEDs, then a reset about every 9 s |
| 9, 2026-11-11 | our IRQ 0 handler again, now passing the SDK's three breakpoints on: it finds `bp_tab` after `breakpoint_isr` (the slot's handler), checks its addresses against the hardware breakpoint registers, and on a hit does what `breakpoint_isr` does (`rete` = the replacement, `rte`); anything else goes to the crash entry | `rom-read-bm-9.bin`: every start runs to `24` (`variant_start()` returned) and never reaches `25`: it ends inside `usb_app_start()`. No crash record (no item 46), so not our crash entry; the reset cause is the watchdog's (`R0400`, the same value at every start, so stored once). The host sees the device appear and never finish enumeration (no serial port). Run 8's starts, the same code without this handler, passed `usb_app_start()`. ⚠️ Why the handler stops the USB start is not known. The handler now builds only with `EMU_CHAIN=1` (`tools/env/build_app.sh EMU_CHAIN=1`); every other build leaves IRQ 0 to the SDK, as in run 8 | no USB; a restart about every 9 s |
| 10, 2026-11-11 | run 8's image with a stall check in the scan interrupt (`SMC_STALL_CHECK`, boot-mark builds): after 4 s in which no scan step interrupted the idle task, the next 5 interrupted program counters go into a crash record (cause 2, `stall`) and the chip resets, before the watchdog's 8 s | `logs/sessions/2026-11-11-bm10-log`: boot 5's journal reports boot 4's stall, at 8.6 s: the program counters 0x114182 (×4, the ROM's `os_sem_post`, just after its `local_irq_enable`) and 0x112c5c (`vPortYield`). Something posts a semaphore and yields, again and again, and the idle task never runs. Our one `os_sem_post` in an interrupt is `usb_isr()` (`usb_app.c`, after `dcd_int_handler()`): ⚠️ likely a USB interrupt that keeps coming, which would also explain the slow link. Boot 5 then ran for minutes, the walk without a stop, until a capture opened the port; then it hung again (the port stayed listed). `rom-read-bm-10.bin` (attempt 31: `r` from a script, then a power-on, [recovery.md](recovery.md)): 9 stall records and 5 watchdog resets. The stall samples are mostly the RTOS at work, not one place: the SDK's `sys_timer_task` and `usr_timer_schedule`, the ROM's `xQueueGenericReceive`, `os_sem_pend`/`os_sem_post`, `xTaskResumeAll`; once our flash code (`advance_index`). So the idle task starves under a task-level load, the timers' or the log's, rather than one interrupt (⚠️; the samples fall only where interrupts are on) | the walk, no stop until the capture |
| 11, 2026-11-11 | run 10's image; the stall check no longer calls the RTOS from the interrupt (it keeps the interrupted task's handle), the record adds the USB interrupts in its 4 s (item 45), and the start logs the tasks' handles (item 44) | `rom-read-bm-11.bin`: one soft reset (`R4000`), then the watchdog (`R0400`) at every start, about every 7.5 s, and no stall record: the stall check armed only once the idle task had run, and it never did. The handles: `systimer` 0x20f40c, the busy task of run 10's samples | a restart about every 7.5 s |
| 12, 2026-11-11 | run 11's image with the SDK's 2 ms ADC scan held in its occupy mode after `adc_init()` (`NO_SDK_ADC=1`, `board_smc_mixer.c`): it never touches the converter again | the same cycle: the port on the bus for 6.6 s, off for 1 s, over 90 s (the flash not read: ⚠️ the watchdog again, by the timing). The SDK's ADC scan is not the cause |
| 13, 2026-11-11 | run 11's image (the SDK's ADC scan back); the stall check counts from the scan's first step as well, so a start that never reaches the idle task is caught too, and records whether the idle task ever ran (item 45, word 6) | `rom-read-bm-13.bin`: a stall record (cause 2) at every start, the soft reset, no watchdog. The idle task ran in 1 start of 12. The samples fall in two tasks, at ordinary places: `systimer` (`sys_timer_task`, `usr_timer_schedule`, the ROM's `xQueueGenericReceive`, `xTaskResumeAll`) and `smc_usb` (`usb_task`, `os_sem_pend`, `tu_edpt_stream_read_xfer`, `advance_index`, `spin_lock`, the ROM's `check_usb_sof`), with 50..57 USB interrupts in the 4 s. No task loops: the CPU has no time left over. `TCFG_CLOCK_SYS_HZ` is 24 MHz (the SDK's default; nothing raises it), and the scan interrupt comes every 50 µs | a restart about every 7.5 s |
| 14, 2026-11-11 | run 13's image with the CPU clock raised to 48 MHz first thing in `app_main()` (`SYS_CLOCK_MHZ=48`, `clk_set("sys", …)`); the clock log line now names the system clock | ✅ the restarts stop: the port stays on the bus (2 min watched, then a 40 s capture, then still there). `logs/sessions/2026-11-11-bm14-log`: "clock: system 48000000 Hz; scan timer at 24000000 Hz" (the timer's clock does not follow the CPU's). The scan handler takes 20.3 µs on average of each 50 µs step, 48.3..49.9 µs at most; 358 per mille of the CPU in a window; 36243 steps skipped and 1472 resyncs of the µs clock in 130 s; CPU load 237..388 per mille. 19013 records in 40 s, where run 10's capture got about 900 in 200 s. So at 24 MHz the scan alone needs about 40 µs of each 50 µs step (⚠️ by scaling, not measured) and the tasks above it starve: the cause of runs 8..13's restarts, of the slow link and of the capture's hang. The raw fader lines flood the log (16486 of 18971 records; 58623 lost since the start). The journal names the start's reset cause `0x402` "low voltage", where the boot marks read `R0400` as the watchdog: ❓ which reading is right | the walk, the port stays |
| 15, 2026-11-11 | run 14's image (48 MHz) with the scan handler timed phase by phase (`SCAN_PROFILE=1`, `input_br25.c`), logged each heartbeat | `logs/sessions/2026-11-11-bm15-log`, average of each 50 µs step: the timer's count at the handler's entry 7.9 µs (up to 50), the load sample 7.65, the µs clock 3.4, `input_br25_time_us()` 2.9, `scan_step()` 4.5 (up to 20.9), the shift and LED columns 2.2, the fader ADC 2.0 (up to 34: the raw fader log line). The load sample is one call of the ROM's `xTaskGetCurrentTaskHandle()`, which reads the running task between the ROM's `local_irq_disable()` and `local_irq_enable()`: the two functions the SDK patches through hardware breakpoints | the walk, the port stays |
| 16, 2026-11-11 | run 15's image, the running task read from the RTOS's variable once it agrees with `xTaskGetCurrentTaskHandle()` at the start; the entry split by the task interrupted; a timing of the lock functions at the start | `logs/sessions/2026-11-11-bm16-log`: the check refused, rightly: `CurrentTCB` (0x12558) holds the address of the ROM's variable (0x31c8c), not the task (the SDK's `pvPortSwitch` reads it twice over). The entry: 5.1 µs on steps that interrupted the idle task, 11.3..11.8 µs on the others. The lock timing called the SDK's RAM `local_irq_disable()`/`local_irq_enable()`, not the ROM's: 1.67 µs a pair | the walk, the port stays |
| 17, 2026-11-11 | run 16's image reading `*CurrentTCB`, and timing the ROM's pair too | `logs/sessions/2026-11-11-bm17-log`: "running task: read directly". ✅ Measured at 48 MHz: a lock and unlock 1.6 µs through the SDK's RAM functions, 6.4 µs through the ROM's (the breakpoints: about 2.4 µs more a call); `xTaskGetCurrentTaskHandle()` 6.6 µs. The load sample now takes 1.57 µs; the handler 17.7 µs on average (was 23.3), the scan 301 per mille of the CPU (was 407). The entry: 4.1 µs from the idle task, 12 µs from the others (⚠️ the tasks' sections with interrupts off, the ROM's patched locks among them; not measured one by one). Now about 15 500 steps in 10 s are skipped because the SPI word of the step before is still shifting (0 in run 15): the step comes sooner after the shift than it did. So the scan needs about 24.5 µs of each 50 µs step at 48 MHz with its entry, and would not fit at 24 MHz (⚠️ by scaling) | the walk, the port stays |
| 18, 2026-11-11 | run 17's image with three cheaper steps: the stack sample once a pass (16 steps) instead of each step; the step's time read without `input_br25_time_us()`'s lock; the µs clock's division replaced by a multiply with a reciprocal (`usclock.c`) | `logs/sessions/2026-11-11-bm18-log`: the clock phase 1.78 µs (was 3.4: the stack sample), the time read 2.79 µs (was 2.81: the other two changes gain nothing, and were taken back). The handler 15.9 µs on average (was 17.7), the scan 275 per mille of the CPU (was 301). Code in RAM runs at about 3 cycles an instruction in this handler (a phase's time against its instructions), so a phase costs what its instruction count says; no single call is left that costs more than its size. Still: about 290 µs-clock resyncs and 14 500 skipped steps (SPI still shifting) in 10 s | the walk, the port stays |
| 19, 2026-11-11 | the board's clock at 96 MHz (`TCFG_CLOCK_SYS_HZ`, the owner's choice), the 50 µs step; the raw fader lines off (the matrix's stay on) | `logs/sessions/2026-11-11-bm19-log`: "clock: system 96000000 Hz". ✅ The handler 9.5 µs on average (was 15.9 at 48 MHz), the scan 177 per mille of the CPU, the CPU load 107..119 per mille; the entry 0.8 µs after the idle task, 15 µs after the others; about 35 µs-clock resyncs in 10 s (was 290). The faders parked at their top end (0, 3, 6) sent about 100 events a second: their filtered value dipped under the end's snap and back (a 12 s capture with `raw on`, `2026-11-11-bm19-rawfaders`: raw 1017..1023 at the top, mid-travel noise of up to ±4 counts, which the deadband holds); the log lost 904 records in its first window. Skipped steps (SPI still shifting) about one a pass: 11 956 in 10 s against 11 657 passes | the walk, the port stays |
| 20, 2026-11-11 | leaving a fader's end takes 4 × `fader_deadband` (128) | `logs/sessions/2026-11-11-bm20-log`: ✅ in 30 s at rest 4 fader events after the 8 of the start (was about 3 000), no log record lost | the walk, the port stays |
| 21, 2026-11-11 | the pass's evaluation (buttons, encoders) after the last column's word is out (`scan_pass()`), instead of before it | `logs/sessions/2026-11-11-bm21-log`: ✅ the step phase 2.3 µs on average and at most 2.5 (was 3.7 and 36.5; the 35 µs of the pass now sit in the "out" phase, after the shift); skipped steps about 3 000 in 10 s (was 11 900), a pass every 818 µs on average (ideal 800). Left: a pass takes up to 3.1..4.4 ms now and then, and the µs clock falls up to 2.2..3.6 ms behind about 3.5 times a second (explained by runs 22..26: lost periods and the timer's period, not ms-long holds) | the walk, the port stays |
| 22, 2026-11-11 | the plain hardware-test build (no boot marks: nothing written to flash at start), with a check of gaps between steps on the CPU's tick timer (`TICK_CNT`), reporting gaps over 150 µs by the address they ended at | `logs/sessions/2026-11-11-hwtest22-log`: ✅ starts and runs like the boot-mark builds. Only 10 gaps over 150 µs in 10 s, at most 242 µs, all in the SDK's `sys_timer_task`: no ms-long holds. But the steps fall short: 198 400 in 10 s, not 200 000 | the walk, the port stays |
| 23..24, 2026-11-11 | the check counts lost periods instead: each step places its period's start on the tick timer (23 had it off: `TICK_PRD` is the count less one) | `logs/sessions/2026-11-11-hwtest24-log`: ✅ 1 200..1 500 periods lost in 10 s, which accounts for the missing steps. Each gap ends right after the SDK's `local_irq_enable()`: about 870 in `sys_timer_task` after `timer1_run()` (up to 200 µs), the rest in the ROM's RTOS (`xTaskResumeAll`, queue receive, `prvGetExpectedIdleTime`; up to 100 µs) in the tasks `systimer`, `IDLE`, `smc_usb`, `app_core`. So the SDK's interrupt lock holds priority 7 off too | the walk, the port stays |
| 25, 2026-11-11 | the µs clock counts the lost periods | `logs/sessions/2026-11-11-hwtest25-log`: resyncs 4 in 10 s (was 30..38). Steps and lost periods together still 0.0875% short of 200 000: 1/1200, the timer's period | the walk, the port stays |
| 26, 2026-11-11 | the scan timer's PRD one less (the counter runs 0..PRD) | `logs/sessions/2026-11-11-hwtest26-log`: ✅ no resync in 60 s, drift −750..+250 µs, a pass every 817..820 µs on average and 1.1 ms at most (was 3..4.4 ms); about 1 500 lost periods in 10 s remain, harmless now | the walk, the port stays |
| 27, 2026-11-11 | step 6's corrections from image 26 (`2026-11-11-manual-1..3`): encoders count clockwise up (A is column 2*n*), encoder 8's pair read across the pass boundary, the encoders' LED entries reversed, the fader channels reversed | `logs/sessions/2026-11-11-manual-4-hwtest27-usb10`: ✅ clockwise counts up on all eight, the encoder LEDs walk left to right, faders 1 and 8 on their numbers; a 12-minute run with everything moved: no lost record, no refusal, no resync, `metrics --check` ok. Fast spins of all eight (`2026-11-11-manual-5-encoder-spins`): about 5 revolutions a second drops half-steps, 1 jump on encoder 1 up to 28 on encoder 7, nearly each as the other line switches in the same pass (⚠️ cause). 17 USB bus resets at the first start after the install (1 before and after) | the walk, the port stays |
| 28, 2026-11-11 | the encoder probe (`raw probeN`: one encoder's two columns in turn, each every 100 µs) | `logs/sessions/2026-11-11-probe-enc7`, `-probe-enc1`: ✅ no record lost, no invalid transition at 100 µs; bounce pulses 100–200 µs; the state with both contacts closed 19–22% of a cycle and, in fast spins, under 0.85 ms in a tenth of the cycles: the cause of the lost steps is the 0.8 ms pass ([hwtest.md](hwtest.md), "What the run of 2026-11-11 found") | the walk, the port stays |
| 29, 2026-11-11 | the decoder infers a skipped state (a jump while turning counts two transitions the way the last one went) | `logs/sessions/2026-11-11-hwtest29-spins`, `-probe-enc8`: ✅ encoders 1 and 7 count as the replay, 74 jumps inferred, none lost; encoder 8's contacts drop out (1 199 short pulses against 79–96), and a bounce turned the guess | the walk, the port stays |
| 30, 2026-11-11 | the inference goes by the sum of the recent transitions (±3), not the last one | `logs/sessions/2026-11-11-hwtest30-spins`: ✅ 5 fast revolutions each way equal 5 slow ones back within 1 click on encoders 1, 7 and 8; 20 jumps inferred, none lost | the walk, the port stays |
| 31, 2026-11-11 | three builds with the fader ADC's start delay 15, 4 and 1 (`ADC_DELAY=n`), the faders held still alternately at an end and mid-travel | `logs/sessions/2026-11-11-adc-delay15`, `-delay4`, `-delay1`: ✅ a fader every 3.68 / 1.62 / 1.17 ms; 4 as quiet as 15, no crosstalk; 1 noisier; outliers are the ADC's wrong conversions near mid-scale ([hwtest.md](hwtest.md), "The faders measured") | the walk, the port stays |
| 32, 2026-11-11 | delay 4, a median of 3, filter shift 3, a directional hysteresis of 5 counts | `logs/sessions/2026-11-11-hwtest32-faders-mid`: ✅ every fader mid-travel, no event in 120 s | the walk, the port stays |

A record is 5 bytes: a check byte, the item (`0x31` = 49), the length shifted left by 4 (`10 00`), the data.

Reading run 2: no mark at all means a mark is not stored (or the VM's opening does not finish);
`0x0A` alone, a stop in `board_power_init`; up to `0x0C`, in `adc_init`, whose busy waits on the
ADC's ready bit have no timeout; up to `0x0D`, in `cfg_file_parse`.

### Step 5: the first run

The following first-contact failure is historical; runs 14–26 above resolved the CPU/clock
problems, and image 30 later passed `firstrun.py` with soft/fault/watchdog resets
([hwtest.md](hwtest.md), "On the device: the first run").

First contact, 2026-11-11, boot-mark image 6 (a start without our IRQ 0 handler), `smclog capture` with no
resets: ✅ the link answers, the stream decodes against the dictionary, the boot journal comes
(reset cause `0x440`, "soft reset"; boot 3's crash; boot 2's latest 68 records), the scan runs (50 µs
step, handler max 47 µs, 0 skipped), the faders report, USB and USB MIDI are configured by the host.
The reply to the first command took 6.5 s. Then the device left the bus. From then on it kept
restarting: on the bus for 6 to 11 s, off for about 1.5 s. Three more captures, opened as soon as
the port appeared, received nothing before it left. One start before that had run for 310 s.
⚠️ Why the starts end is not known yet. Boot 2's records stop at 10.8 s with nothing but raw fader values, and image 6's VM shows watchdog
resets after the walk's first step.


`firstrun.py` checks the link, the stream, the boot journal, the metrics and the resets. What its
failures mean:

| Check | If it fails | Register |
|---|---|---|
| PINGs, the stream without bad frames or gaps | the USB driver under load: `cmd metrics` for the driver's counters | U3–U5, U8 |
| "the RAM was kept" after a soft reset | the ROM or the uboot clears that RAM: the keep block has to move | D1 |
| the reset cause | the bits mean something else: the raw value is in the report | D2 |
| the crash record after the fault test | the load did not trap, or our handler did not run, or the frame's order differs | D3–D5 |
| the clock against the host's, within 500 ppm | the timer does not run at the rate `clk_get("timer")` reports | T4 |
| the heartbeat's checks | the scan's handler time, the stacks, the heap | T7, D6 |

A failed diagnostic check (D rows) does not block step 6: note it and go on. A failed link or
stream check does.

### Step 6: the manual procedure

Buttons, encoders, the LED walk, faders, a 10-minute USB run, the timing. It settles the N rows
and T4–T7. Each correction goes into `firmware/input/scan.c`'s tables (with the host tests) and
[hardware.md](hardware.md), and means a new image before step 7.

The fader step decides more than a table: if the eight candidates are not the fader pins (N9),
the ADC driver's channel list is wrong and the raw records show nothing moving. Then the fader
pins have to be found another way (the stock code did not show them).

### Step 7: the controller

Install the `app` variant, run `firstrun.py` on it, then `smcsim device` with the scenario
corpus, then with `--midi coremidi:SMC-Mixer` to check actual host reception as well as the log.
A disagreement between the device and the simulator is a bug in one of the two.

✅ Done on 2026-11-11 (the controller's first image, build `0adc03b0`): `firstrun.py` 32 of 32,
`smcsim device` 12 of 12 with and without `--midi coremidi:SMC-Mixer` (67 of 67 messages
received). The first run of the corpus failed 10 of 12 on a fader left mid-travel (its jitter is
real input); with the faders at an end, none ([core.md](core.md), "Scenarios on the device").

### Later

- **The scan during a real flash write** (T1, T5): ✅ controller 20's configuration saves on
  2026-11-12 and controller 29's state saves on 2026-11-13 measured SDK interrupt holds up to
  about 10 ms ([config.md](config.md)). The priority-7 bypass premise is false.
- **The scan against BLE** (SPEC R-14): ✅ advertising/connection, a fader sweep and a same-traffic
  BLE-on/off comparison ran on controllers 21–23, 2026-11-12 ([ble.md](ble.md)). Physical free-page
  counts, SDK BT task stack depth and current iOS/new-address checks remain separate gaps.
- **The bootloader**: where it goes in flash, and whether the PLL's 48 MHz runs when it starts
  after the factory uboot (U1). A decision of its own; nothing above needs it.
- **iOS**: enumeration and the port names on an iPad; `bMaxPower` (NFR-09).

## The register of assumptions

What the code and the plans take for granted about the device. "Home" is the document that holds
the fact and its mark; "settled by" is the first step that can show it.

Confidence: ✅ confirmed with the stated source (hardware or offline), ⚠️ inferred, ❓ unknown.
Operational success is not proof of every underlying register interpretation; scopes below
preserve that distinction. R8 and other explicitly unverified boundaries remain open.

### Recovery

| ID | Assumption | Mark | Home | Settled by | If false |
|---|---|---|---|---|---|
| R1 | The key `0x16EF` on D+/D− at about 50 kHz, sent while the chip starts, enters the ROM's download mode | ✅ 2026-11-10 | recovery.md | 1 | no recovery path: no write, ever |
| R2 | The chip starts on USB power with the switch off | ✅ false (2026-11-10): it needs the switch | recovery.md, hardware.md | 1 | switch on during the attempt (the procedure now) |
| R3 | The clock calibration needs a bus without other traffic: a port of the Mac itself | ✅ works in a port of the Mac itself (2026-11-10); a hub not tried | recovery.md | 1 | try other ports |
| R4 | The ROM takes `br25loader.bin` as stored, at `0x12000`, started with argument 1 | ✅ 2026-11-10 | recovery.md | 1 | the loader's settings from another source |
| R5 | The loader's commands and replies are as `jlrom` builds and reads them | ✅ 2026-11-10 for `FC09`, `FC0A`, `FC0B`, `FC14`, `FD05`, `FC0E`; `FC15` is not (recovery.md, "First contact") | recovery.md | 1 | fix `jlrom` from the log |
| R6 | The chip key is `0x980F`; the SFC keys blocks from base `0x1000`; sector 0 reads through the `0xFFFF` keystream every 256 bytes | ✅ on the chip (2026-11-10, `compare.py`) | image.md | 1 (`compare.py`) | the image model is wrong: redo image.md and the plan before any write |
| R7 | Running the loader changes nothing the stock firmware minds (it may set the flash's status registers) | ✅ 2026-11-10: the stock runs afterwards, two reads equal | recovery.md | 1 (the stock runs afterwards; two reads equal) | stop; the read itself is not harmless |
| R8 | The mode still works with sector 0 erased | ⚠️ | recovery.md | only by erasing sector 0 | a brick: this is the risk C-6 is about |
| R9 | Erase `FB01` and write `FB04` (no CRC check in the loader) work as `jlrom` builds them, and the loader lifts the flash's write protection itself | ✅ 2026-11-10 (rehearsal 2, sector `0x7F000`) | recovery.md, "Rehearsal 2" | 2 | no write path: no installation this way |
| R10 | The stock firmware's erase `0x21` and write `0x22` work as its code reads: a 4 KB erase, the bytes stored as sent (sector 0 too), the same reply whatever happened | ✅ 2026-11-10 (step 2b: `0x7F000` written and erased, sector 0 erased and rewritten, all checked by reads) | hardware.md, "Stock service protocol" | 2b (`0x7F000`) | no installation without extra hardware |
| R11 | While the stock firmware runs, the flash's write protection covers neither sector 0 nor our sectors (its driver never changes it, and its writes then fail silently) | ✅ sector 0 and `0x7F000` in step 2b, then our first area write/read-check in step 3, 2026-11-10; later browser installs on 2026-11-14/15. Not a map of arbitrary-sector protection | hardware.md, "Stock service protocol" | 2b, 3 | the SysEx path cannot install; the key's path still can |

### The image and the start

| ID | Assumption | Mark | Home | Settled by | If false |
|---|---|---|---|---|---|
| I1 | The uboot follows sector 0's `app_dir_head` to our directory and starts our `app.bin` at its entry | ✅ the raw read after the first power-on, 2026-11-10 (`rom-read-hwtest-1.bin`): our code ran far enough to format its VM | image.md, "Still open" | 4a | does not boot: roll back |
| I2 | Sector 0 is right with only bytes `0x60..0x66` changed (the header's other fields and `uboot.boot`'s CRC are not decoded) | ✅ the raw read after the first power-on, 2026-11-10 (`rom-read-hwtest-1.bin`) | image.md, "Still unknown" | 4a | does not boot: roll back |
| I3 | At `0x41000` the SFC cipher gives the same bytes under both key models | ✅ by construction; on the chip: the raw read after the first power-on, 2026-11-10 (`rom-read-hwtest-1.bin`), the code ran | image.md | 4a | — |
| I4 | Our SDK build starts without `cfg_tool.bin` and `config.dat` | ❌ without `cfg_tool.bin` (run 3); ✅ the SDK's default file passes `cfg_file_parse` (run 4) and later working builds use it. Normal installs omit `config.dat`; its contents remain undecoded | image.md, "Still open" | 4a | always pass the required `--cfg`; do not repeat the archived missing-file plan |
| I5 | On its first run our firmware writes only its own VM; `BTIF` is read, not written (the stock MAC is valid) | ✅ the raw read after the first power-on, 2026-11-10 (`rom-read-hwtest-1.bin`): the flash equals the plan's "after" except 4 bytes `55 AA AA 55` at `0x52000` (the SDK's VM header); `BTIF` unchanged. The run stopped early, so later writes are not seen | image.md, hardware.md | a raw read after step 4 | restore `BTIF` from the raw read |
| I6 | The stock update loader at `0x6E000` and the stock area stay untouched | ✅ by the plan's checks; ✅ the raw read after the first power-on, 2026-11-10 (`rom-read-hwtest-1.bin`) | image.md | a raw read after step 4 | the rollback's CRCs differ: compare range by range |
| P1 | The board settings taken from the SDK's demo board suit the mixer: LDO 1.5 V, VDDIO, the 24 MHz crystal, the charger off | ✅ static against V012 | hardware.md, "Power"; SPEC R-12 | 4 (watch current and temperature) | switch off; roll back |
| P2 | Driving PB4–PB7, PA2, PA5, PC1 and PC3 as outputs, with PB0–PB3 as inputs and the eight fader candidates as analog inputs, fights nothing on the board (that is what our code does; whether they are the right pins is N1, N7, N9) | ✅ panel mapping/operation, 2026-11-11; no conflict observed, not exhaustive electrical proof | hwtest.md | 4, 6 | a pin conflict: switch off |
| P3 | The SDK's watchdog is set to 8 s and only the idle hook clears it | ✅ deliberate watchdog reset, image 30, 2026-11-11 (8.9 s after request); idle-hook mechanism remains static-analysis evidence | link.md, RESET | 5 (`--watchdog`) | the watchdog test hangs until power is removed |

### USB

Home: [usb.md](usb.md), "Behaviour the driver depends on" (its item numbers in brackets) and
"Not checked without hardware".

| ID | Assumption | Mark | Settled by | If false |
|---|---|---|---|---|
| U1 | The PHY's 48 MHz from the PLL runs: in the application from the SDK's clock setup; in the bootloader from what the uboot leaves | ✅ application USB operation since 2026-11-11; ❓ bootloader clock, still unrun | 4b; the bootloader later | the bootloader needs its own clock setup |
| U2 | The start-up order of `dcd_init` (pads, `CON0`, `PHY_ON`, the pull-up) attaches the device; the handler is installed before `tusb_init()` [6] | ✅ application attaches/enumerates, 2026-11-11; exact register rationale from SDK/pad | 4b | no device: compare with the SDK's driver |
| U3 | MaxP written as `0xFF` gives single buffering [1] | ⚠️ SDK, pad | 5, 6 (long transfers) | data shifted by 512 bytes |
| U4 | The RX FIFO holds two packets; a count of 0 with FIFOFull means "wait" [2] | ⚠️ pad | 6 (`rx_waits`, `rx_wait_fails`) | split packets on the CDC OUT |
| U5 | One buffer per direction, rewritten before each packet, is harmless whichever way the pointers behave [3] | ⚠️ | 6 | lost first bytes |
| U6 | Endpoint 0 follows the MUSB peripheral protocol with the SDK's command values; a SETUP before the status is armed can be parked [4] | ⚠️ SDK, pad | 4b–c | enumeration fails |
| U7 | One endpoint number can serve IN and OUT (MIDI on 4) [5] | ✅ deployed shared endpoint: MIDI output/scenarios 2026-11-11 and USB MIDI configuration round trips 2026-11-13; not an exhaustive duplex stress test | 4c, 6 | drop CDC's notification endpoint |
| U8 | Masking IRQ 9 around task-side register access is enough [7]; the IRQ runs at priority 3 beside the scan at 7 | ✅ no corruption in the 12-minute panel/USB run, 2026-11-11, and later protocol runs; no exhaustive interrupt-race proof | 6 (10-minute run; the scan's `late max`) | corrupted transfers under load |
| U9 | macOS and iOS show the jacks' names as port names; `bMaxPower` suits an iPad | ✅ macOS names, 2026-11-11; controller `C1`/display names checked 2026-11-15. ❓ hardware-test `T1`, iOS labels and `bMaxPower` | 4c; iOS later | names set another way |

### Input

Home: [hwtest.md](hwtest.md), "Hardware layout it assumes"; [hardware.md](hardware.md), "Stock
firmware internals". All from static analysis of the stock firmware unless marked.

| ID | Assumption | Mark | Settled by | If false |
|---|---|---|---|---|
| N1 | The 595 chain is on SPI2's port A pins (clock PB6, data PB7), latched by a rising edge of PB4; PB5 low enables the outputs | ✅ the panel works (2026-11-11, step 6; PB5 as /OE still a guess that fits) | 4a, 6 | nothing of the panel works |
| N2 | The word: upper 16 bits select a column (active low, the stock's bit order), lower 16 are LED rows; column *c* is read while the word for *c* − 1 is latched | ✅ step 6.1, 2026-11-11 | 6 (buttons by index) | indexes shifted: fix the tables |
| N3 | Rows on PB0–PB3 with pull-ups, a closed contact reads low | ✅ step 6, raw mode | 6 (raw mode) | invert, or other pins |
| N4 | The button table (`scan_button_pos`) | ✅ all 45 in order, 2026-11-11 | 6.1 | fix the table |
| N5 | Encoders on row 0, encoder *n* on columns 2*n* − 1 and 2*n*, the eighth on 15 and 0; 2 transitions a click | ✅ the columns and the divider (47–49 steps a revolution, no detents); clockwise: column 2*n* leads, so it is A (image 27) | 6.2 | fix columns or the divider |
| N6 | The scan at 0.8 ms a pass loses no encoder step at a normal speed | ✅ at a normal speed, none; fast spins lose steps when the shortest state falls under the pass (✅ the encoder probe, run 28); since 2026-11-23 a 25 µs step (0.4 ms a pass) and skipped-state inference ([hwtest.md](hwtest.md), "Encoders") | 6.2 (`enc_invalid`) | scan the encoder columns more often |
| N7 | LED columns on PA2, PA5, PC1, PC3, active low, one at a time; a cleared row bit lights a LED | ✅ the LED walk, 2026-11-11 | 4a, 6.3 | fix polarity or pins |
| N8 | The LED table's order is the core's LED order (0–44 the buttons, 45–52 the encoders) | ❌ → fixed: the buttons' order holds, the encoders' 8 entries were right to left; reversed in image 27, ✅ left to right | 6.3 | fix `scan_led_pos` |
| N9 | The faders are on PA0, PA1, PA3, PA4, PA6, PC2, PC4, PC5 (the ADC pins nothing else uses), in that order | ✅ all eight, in reverse: faders 1–8 on PC5, PC4, PC2, PA6, PA4, PA3, PA1, PA0 (image 27) | 6.4 | find the pins another way |
| N10 | An ADC conversion with the SDK's setting takes a few scan steps | ✅ original delay 15: each fader every 3.7 ms; delay 4: 1.62 ms at 96 MHz; current 192 MHz with /128: about 1.27 ms (controller 14, core.md). Filter lag is separate | 6.6 (the heartbeat) | faders slower than NFR-01 |
| N11 | Our polled conversions can share the ADC with the SDK's driver: it starts only when the control register reads 0 | ✅ 12 min: 185 steps the SDK had it, 24 conversions lost to it, 0 timeouts | 6.6 (`lost`, `timeouts`) | take the ADC from the SDK's driver, or use it |
| N12 | SPI2 at about 2 MHz suits the 595 chain (the stock's clock is not known) | ✅ step 6 | 6 | a slower clock |

### Interrupts and timing

Home: [hwtest.md](hwtest.md), "While flash is written" and "The platform part".

| ID | Assumption | Mark | Settled by | If false |
|---|---|---|---|---|
| T1 | `request_irq()` gives priority 7, and the SDK's flash path (mask at 6) leaves it running | ❌ bypass premise: run 24 shows the SDK lock holds off priority 7; controller 20/29 flash saves pause the scan up to about 10 ms | 6.6; real flash saves 2026-11-12/13 | the scan stalls during writes (observed); RAM-only rules remain |
| T2 | The lock's test of ICFG bits `0x300` says whether interrupts were on | ⚠️ | 5, 6 (no hangs, no lost events) | a wrong lock: the queue corrupts |
| T3 | SOFT0 is free, and its pending bit is bit 4 of `ILAT_SET` | ❌ bit 4: no wake; fixed to bit 0 (`WAKE_ILAT_BIT`), ✅ controller 4, 2026-11-11, input → core at most 0.45 ms | 4d, 5 (events reach the task without a 10 ms wait) | the control task wakes only on its tick |
| T4 | TIMER3 is free (✅ every build checks) and counts at the rate `clk_get("timer")` reports | ✅ 12 min: no resync, drift −650..+350 µs, steady | 5 (the clock's drift) | wrong times everywhere: fix the rate |
| T5 | Read-modify-write of ports A, B and C in the interrupt does not collide with SDK code doing the same under its own lock | ⚠️ no collision observed in the 12-minute panel run; the original preemption concern relied on T1's disproved bypass premise, not an observed failure. Shared GPIO ownership still matters | 6 (a 10-minute run); cannot be shown absent | atomic set and clear registers, if the chip has them |
| T6 | `xTaskGetHandle("IDLE")` finds the idle task; the ROM functions the interrupt calls are safe there | ✅ the load is reported (about 105 per mille) | 5 (the CPU load is reported) | no load figure |
| T7 | The scan handler takes far less than its 50 µs step, the fader ADC included | ✅ 9.3 µs on average, 49 µs at most (image 27) | 5, 6.6 (the heartbeat) | a longer step |
| T8 | A priority-7 interrupt every 50 µs does not disturb the BLE controller | ✅ controllers 21–23, 2026-11-12: connection, control sweep and same-traffic on/off comparison; scan interference measured, no lost input in those runs. Not every host/radio condition | BLE runs in ble.md | a lower priority, or a slower step |

### Diagnostics

Home: [logging.md](logging.md), "Boot journal".

| ID | Assumption | Mark | Settled by | If false |
|---|---|---|---|---|
| D1 | The ROM and the uboot leave the keep block's RAM alone across a reset | ✅ for a soft reset (`chip_reset()`): on 2026-11-11 boot 4 reported boot 3's crash record and replayed boot 2's latest log records (`logs/sessions/2026-11-11-bm6-first-log`, boot-mark image 6). ✅ after the watchdog and the fault test too (`firstrun.py`, image 30, 2026-11-11) | 5 | move the block; until then no crash record |
| D2 | The reset cause bits mean what the SDK's names say; reading does not clear them | ✅ soft reset 0x440, watchdog 0x404, each named right by `firstrun.py` on image 30 (2026-11-11); a later soft reset no longer shows the watchdog bit. ❓ complete read-clear/accumulation behavior for other sources | 5 | rename from the raw values |
| D3 | The exception frame's order is the ROM handlers' | ✅ the fault test (2026-11-11): `RETI` equals the trace's newest entry, `RETS` the caller's return address | 5 (the fault test) | reorder `diag.h` |
| D4 | A misaligned word load traps, and our handler at IRQ 0 and 1 takes it | ✅ the deliberate load reaches IRQ 1, a few instructions late (2026-11-11, `firstrun.py --resets fault`); ❌ the original IRQ-0 takeover premise, since it is the SDK's breakpoint mechanism (runs 5 to 7) | 5 | preserve SDK IRQ 0; `EMU_CHAIN=1` failed startup (run 9) and is not the default fix |
| D5 | The trace registers hold the program counters from before the exception | ✅ the fault test: newest first, the instruction after the call, `diag_br25_fault`, the call, an earlier branch (2026-11-11) | 5 | drop the trace from the record |
| D6 | The SDK's stack and heap figures are what the disassembly suggests | ✅ sampled application stack figures run on the mixer; heap-free is virtual address space, not free physical RAM (ble.md, "The two heap numbers"). ❓ physical free pages and SDK BT task depth | 5 ("not measured" otherwise) | sample another way |

## Work this plan asks for

Historical proposals from the 2026-11-10 review, not a current unstarted backlog. W1's guarded
write/restore paths ran; a dedicated plan executor is still absent. W4 diagnostics were added
for the recorded bisects. W5's no-key normal path is now the editor's STOCK return and reinstall
over stock USB SysEx/BLE AE40 (2026-11-14), not our unrun DFU bootloader. The original rationale
is retained below; W2/W3 were contingency ideas, not approved new work.

- **W1. The write path in `jlrom`** (already planned: recovery.md, "Rehearsal 2"), and with it one
  command that carries out a plan from `installplan.py` and one that rolls it back, each checking
  by CRC before and after. Steps 3 and every later image depend on it; done by hand, the sector 0
  step is where a slip costs most.
- **W2. A sign of life that needs no USB.** Once the LED walk is seen, LEDs are an output: a few
  of them could show the USB driver's state (attached, reset seen, configured) and a reset
  counter, so that "LEDs walk, no USB device" says where the driver stopped. Small, and it only
  uses what step 4a has already shown to work.
- **W3. A baseline image on a USB stack known to work.** The SDK ships its own USB device stack
  with a CDC class (`apps/common/device/usb/device/`), and the stock firmware is built on that
  SDK. A first image using it instead of our driver would separate "the installation and the
  start work" from "our USB driver works". ⚠️ Not checked: that it builds with our configuration,
  or how much of our link it could carry. Worth it only if step 4 fails at the USB line.
- **W4. Switches for bisecting on the device**: the scan interrupt's priority, the fader ADC and
  the scan itself are constants in `input_br25.c`. As build options (or link commands for the
  ADC) they would let a failing image be narrowed down without editing code at the bench.
- **W5. A shorter loop after the first run.** Every new image goes through the ROM mode: the key,
  `sudo`, the power switch, a person. Steps 6 and 7 each need at least one more image. A way to
  take an image over USB from the running firmware (the bootloader's DFU, or a link command into
  an updater) is the first thing to build once step 5 passes.
