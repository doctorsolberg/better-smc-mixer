# BLE-MIDI

Bluetooth LE MIDI on the controller (SPEC §5.6: FR-IO-03..09; stage 5). This document holds what
the research found, the design, what Bluetooth costs in flash, RAM and CPU, and the steps. Started
2026-11-12. The probe build (the stack and the service, no MIDI through it) ran on the mixer the
same day: "The first run".

Marks: ✅ confirmed (with the source), ⚠️ inferred, ❓ unknown.

## Where it stands

- A first device glue exists behind a build switch, off by default: `BLE=1`
  (`firmware/app/ble/ble_br25.c`, `firmware/app/Makefile`; `BLE=1 tools/env/build_app.sh` builds
  it into `build/app-ble/`, `make device` includes it; it pairs Just Works when asked
  and stores no keys, Q-24; `BLE_SM=0` is refused, "Without the security manager"). It starts the SDK's BLE controller and
  host, serves the BLE-MIDI GATT service, advertises as "Better-SMC-Mixer", asks for a 7.5..15 ms
  connection interval and logs the link's events. ✅ It builds and links with the device toolchain
  (2026-11-12), and ✅ macOS and iOS connect to it on the mixer ("The first run").
- **MIDI flows both ways** since controller 22 (2026-11-12, step 4, "The route"): the core's
  output goes to USB and, while Bluetooth is on, BLE (the BT button's long press turns it off and
  on, as in the stock firmware; FR-IO-04, "Bluetooth on and off"), through a BLE queue that
  merges a continuous control's waiting value with its newer one; MIDI from a central reaches the
  core as `CORE_FROM_BLE`. ✅ On the mixer with macOS: the BLE stream is the USB stream less
  the replaced values, every note there, input → BLE median under 5 ms ("The route on the
  mixer").
- Without `BLE=1` the build is the controller as before (`ble_stub.c`); the SDK's Bluetooth code
  is not even linked (the linker drops it: nothing calls `btstack_init()`).
- **The blocker found and lifted:** with Bluetooth the application did not fit our flash area as
  it was laid out ("Flash"). The owner chose (2026-11-12, SPEC Q-23) to move the configuration's
  slots into the free gap below our area; ✅ saves and reads run there on the mixer the same day
  (controller 21, without Bluetooth).
- **Later evidence:** configuration SysEx, events and named MIDI ran over BLE on controllers
  31–35 (2026-11-13/14, [link.md](link.md)). Browser BLE install, Update and Go back ran on
  2026-11-14 ([app.md](app.md), "Firmware"). Physical BLE E2E (37 checks) and Chrome client
  checks passed on 2026-11-15. ✅ On 2026-11-23 a full Bluetooth firmware cycle on the release
  candidate completed with USB unplugged: Go back over BLE MIDI, stock Web Bluetooth AE40,
  backup, install and read-back, physical off/on, ours over BLE MIDI again with the configuration
  content byte-identical, the revision aside ([integration.md](integration.md), "Hardware
  results, 2026-11-23").

## What the SDK offers

- ✅ The AC63 SDK has **no BLE-MIDI profile** (no example or source names the BLE-MIDI UUIDs).
  It has a generic GATT layer in source form, `apps/common/third_party_profile/jieli/gatt_common/`
  (`le_gatt_common.c`, `le_gatt_server.c`, Apache 2.0): an application gives it an ATT database
  (a byte table), read and write callbacks and an event callback, and advertising data. Its
  examples (`apps/spp_and_le/examples/trans_data/ble_trans.c`) use it; so does our glue, with
  the two files compiled as they are.
- ✅ The start, as the examples do it: the BLE address from the Bluetooth MAC
  (`lib_make_ble_address()` over `bt_get_mac_addr()`, `le_controller_set_mac()`),
  `bt_pll_para()`, `ble_comm_init()`, `btstack_init()`. The stack then posts `BT_STATUS_INIT_OK` as
  a system event (`SYS_BT_EVENT`, `SYS_BT_EVENT_TYPE_CON_STATUS`); a static handler
  (`SYS_EVENT_HANDLER`, run by the SDK's `sys_event` task) sets the name, the database and the
  advertising and enables the module. The examples' application framework (`start_app`, intents)
  is not needed: the static handler table is in our link already.
- ✅ The SDK's tasks for Bluetooth are in our `task_info_table` since stage 1 (`btctrler` 2 KB
  stack, priority 4; `btencry` 2 KB, priority 1; `btstack` 3 KB, priority 3; `app_main.c`). They
  are created only when the stack starts.
- The closed parts (SPEC R-10): `btctrler.a` (the controller, the radio), `btstack.a` (HCI, L2CAP,
  ATT, SM), `crypto_toolbox_Osize.a`. None of it is in the ROM: the ROM's table in the linker script
  names the RTOS, memory and DSP functions only.

## What the stock firmware does

From the V012 application (`materials/re/v012/app.bin`, static analysis) and the flash dump:

- ✅ It has a BLE-MIDI service. Its ATT database (at `0x14A80` in `app.bin`, the SDK's format):
  GAP (`1800`: name, appearance, preferred connection parameters), GATT (`1801`: service changed),
  Device Information (`180A`: manufacturer, model), Battery (`180F`), a JieLi service `AE40`
  (`AE41` write without response, `AE42` notify), **BLE-MIDI** on handles `0x0070..0x0073`
  (service `03B80E5A-…`, characteristic `7772E5DB-…` read / write without response / notify, its
  CCC), and a JieLi service `AE00` (`AE01`, `AE02`: ⚠️ the stock OTA, SincoOTA).
- ⚠️ The stock database marks the BLE-MIDI characteristic's value with flags `0x0116`, without the
  128-bit-UUID bit the SDK's generator sets (`0x0316`); it works because the central reaches it by
  handle. Ours uses `0x0316`.
- ✅ The stock firmware's advertised name: "SMC-Mixer". Ours advertises "Better-SMC-Mixer" since
  controller 28 (the owner, 2026-11-12: a host's list tells the two apart; Q-25). The stock `EXIF` block (`0x7C000`, factory data) holds it
  three times; the SDK's `cfg_tool.bin` in the stock area has only the SDK's default names
  (`AC696X_…`). Our glue sets the name itself and reads neither block.
- ❓ What the stock firmware puts into its advertising packets (the UUID sits next to the code at
  `0x14730`, likely for the advertising data). ✅ The tested vendor BLE→DIN adapter accepted
  changed names and addresses on controllers 25/26 ("The vendor's adapter on the mixer");
  the service-UUID selection rule is inferred from those tests, not a trace of its firmware.
  Our advertising: flags
  and the BLE-MIDI service UUID in the advertising packet, the name in the scan response, as
  Apple's specification asks.

From open-smc-pad (SPEC §3.4; facts, not code): macOS caches a device's GATT database by its
address, so a device that changes its database must change its address (they mix a hash of the
database into the address); batching BLE-MIDI messages while the stack's queue is busy took the
median latency from 757 ms to 23 ms; at 24 MHz a stack call took 9 ms, at 96 MHz 1.3 ms. We run at
192 MHz.

**Our own address** (the owner, 2026-11-14): the SDK's `lib_make_ble_address()` over the MAC in
`BTIF`, the rule M-Vave's firmware uses, then the low bits of one byte flipped (`BLE_ADDR_XOR`, 5
by default; `ble_br25.c`): stable per mixer and never M-Vave's, so a phone or a computer can keep a
separate entry and pairing for each firmware. ✅ On the mixer, 2026-11-14, build `a628aa6a`:
after the page's BLE Update and a power cycle, the owner connected “Better-SMC-Mixer” in Audio
MIDI Setup without forgetting a device; `info` named that build and `stock check` passed at
`0x41000`. iOS with this default is not checked yet. The vendor's adapter takes any address
(✅ R-11 below, controller 26 with the same value).

Separate addresses do not prevent the Mac from reconnecting to M-Vave's firmware automatically.
After the page's Go back over BLE, the owner found no mixer in Chrome's chooser while it was
connected to the Mac; disconnecting and forgetting it made it appear. Disconnecting alone was not
confirmed sufficient. The editor's Bluetooth help distinguishes this from a stale GATT cache;
no-forget switching back to M-Vave's firmware is not confirmed. Before our own address, a Mac that
paired with M-Vave's firmware could keep its database (handles `0x70..`) for our firmware too;
with the shared address, switches on 2026-11-14 needed the Mac's old pairing removed.

## Flash

Measured 2026-11-12 with the device toolchain (`BLE=1 tools/env/build_app.sh`):

| Build | `app.bin` | Growth |
|---|---|---|
| The controller (controller 20, no Bluetooth linked) | 91 560 bytes | |
| `BLE=1`: the stack, the GATT layer, pairing (security manager) | 156 000 bytes | +64.4 KB |
| `BLE=1 BLE_SM=0`: the same without pairing or bonding | 140 072 bytes | +48.5 KB |

So pairing costs about 15.9 KB, the rest of Bluetooth about 48.5 KB. ✅ The build without the
security manager does not start ("Without the security manager"): the saving is not available.

The area our firmware owns starts at `0x41000` (fixed by the SFC keys, [image.md](image.md)) and
holds the directory, `app.bin`, `cfg_tool.bin`, then the SDK's VM (at least 8 KB). Until
2026-11-12 the VM ended at the configuration slots at `0x5E000` (64 KB, [config.md](config.md),
"Storage"), which left 109 440 bytes for `app.bin`: **neither Bluetooth build fit.** Above
`0x6E000` lies the stock update loader (52 KB: `LOADER.BIN`, `ble_app_ota.bin`), then `BTIF` and
`EXIF` (never touched).

| Layout | Largest `app.bin` | Fits |
|---|---|---|
| Until 2026-11-12: VM (8 KB) up to `0x5E000`, slots `0x5E000..0x6DFFF` | 109 440 bytes | neither |
| Slots of 2 × 16 KB at `0x66000..0x6DFFF` (the stock loader kept; the largest configuration, 30 KB, no longer fits a slot) | 142 208 bytes | without pairing only, 2 KB left |
| Slots of 2 × 32 KB at `0x6B000..0x7AFFF`, over the stock loader (`0x6E000` given up), below `BTIF` | 162 688 bytes | both: 6.5 KB left with pairing, 22 KB without |
| **Chosen (Q-23, 2026-11-12):** slots of 2 × 32 KB in the free gap `0x25000..0x34FFF` below our area; VM (8 KB) up to `0x6E000`, the stock loader kept | 175 104 bytes | both: 19 KB left with pairing, 35 KB without |

⚠️ The numbers ahead of the slots are counted from the current plan's layout (directory `0x120`,
`cfg_tool.bin` 0x360, the VM aligned to 4 KB).

The room has since been used: on 2026-11-14, with STOCK (below), the BLE controller's `app.bin` is
172 408 bytes, 2 696 under the limit (`build_app.sh` checks it). The next feature of any size in
the BLE build needs room found first (the VM's 8 KB is the SDK's least; the slots could shrink).

The owner's requirement (2026-11-12, SPEC Q-18): our firmware can be updated and the stock one
brought back without the key. The path that does it needs neither the stock loader nor room for a
second image: the stock application at `0x1000` stays untouched; our firmware points sector 0
back at it (a sector 0 write of our own: STOCK, [image.md](image.md), "Going back"; ✅ on the
mixer over USB and BLE MIDI, 2026-11-14), the stock firmware starts, and its service
write path (`0x21`/`0x22`, ✅ how controller 1 was installed, [bringup.md](bringup.md), "Step 3")
writes a new image of ours and points sector 0 at it again, or leaves the stock firmware running.

So the gap below our area is the best place for the slots, and they are there now: the stock
firmware's directory counts `0x24000..0x7AFFF` as its VM but used only the sector at `0x24000`
(✅ the dumps), and it ran with our area written inside that range (✅ 2026-11-10). What the
listing says about the SDK's driver (2026-11-12, static analysis):

- ✅ Writes: `sfc_write()` programs the flash raw; `sdfile_check_write_protect()` in our build
  returns 0 for every address, so nothing refuses them.
- ✅ Reads: `sfc_read()` reads above a boundary B through the cache's mapping (plain data, the SFC
  decrypting) and below it raw, then XORs the buffer with the ENC keystream of key `0xFFFF`
  (`doe()`). Our raw-written slots below B therefore come back XORed; `flash_br25.c` XORs the same
  keystream again (the transform is its own inverse).
- B is `0x41000`, our area's start (inferred from the mapping: the entry `0x1E00120` is flash
  `0x41120`; ✅ at least above the slots: two saves read back there on 2026-11-12,
  [config.md](config.md), "At the new place"). The firmware cannot log it: the helpers that read it are local to `cpu.a` (neither
  links from our code). If B were at or below the slots, `sfc_read()` would not apply the
  keystream there, ours would spoil the data, and every save would fail its read back, safely.
  The saves and read-backs at the new place settled this behavior
  ([config.md](config.md), "At the new place", 2026-11-12).
- ✅ The stock firmware starts with our saved configuration still below the area: controller 36's
  STOCK back and browser reinstall kept generation 40 (2026-11-14, [app.md](app.md), "Firmware").
  This does not prove that a full stock VM can never grow into that space. SincoOTA V012 → V013
  erased `0x24000..0x7AFFF`, including ours and the saved slots, in the recorded run.

Overwriting the stock loader was a rejected layout option: whether stock starts without it is
unknown. The chosen layout keeps it, so the demonstrated keyless return does not depend on
removing it.

## RAM

### The two heap numbers

`build_app.sh` printed a heap of 36 704 bytes for controller 20 (`_HEAP_BEGIN`..`_HEAP_END`), and
the metrics heartbeat on the mixer reads "heap: 95 608 bytes free". They count different things
(✅ from the disassembly of `main` and `pvPortMalloc`, build `9919619d`):

- `main()` hands `_HEAP_BEGIN`..`_HEAP_END` to the ROM's page allocator (`pmalloc_init`): the
  **physical** RAM, 128-byte pages, 286 pages for controller 20.
- `pvPortMalloc()` (the SDK's `heap_mmu`) is a first-fit allocator over a **virtual** region the
  ROM's `vmalloc` gives it (the task stacks also live at virtual addresses `0x20xxxx`); the MMU
  backs a virtual page with a physical one when it is used. `xPortGetFreeHeapSize()`, which the
  heartbeat reads, returns the free bytes of that virtual region.

So 95 608 is free address space, not free RAM. ⚠️ The physical RAM left is at most the 36 704
bytes, less what tasks and allocations already map; ❓ how much exactly: the heartbeat should read
the ROM's `pmalloc_get_unused_pages()` too (step 2; its signature is ⚠️ not declared in the SDK).

### What Bluetooth takes

| What | Bytes | Source |
|---|---|---|
| Static (`.bss`, `.data`, RAM code): the link's heap shrinks | 36 704 → 34 208: 2 496 | ✅ `build_app.sh`, `BLE=1` (with or without pairing) |
| The three tasks' stacks | 7 168 | ✅ `task_info_table` (words × 4); virtual, mapped when used |
| The tasks' queues | ⚠️ about 2 600 (640 entries) | `task_info_table`; entry size not known |
| ATT send buffer | 2 × (head + 104) ≈ 230 | our `gatt_ctrl_t` |
| The controller's and host's buffers (`malloc` at start) | ❓ | closed libraries |

⚠️ Estimate: 12–16 KB of the 36 KB physical heap. ❓ To be measured on the mixer before
anything else is built on it: the free pages before and after the stack starts, and the deepest
point of the three stacks.

## CPU

Measured for the recorded builds: controller 21's idle connection used 47–54‰ CPU, controller
22's control sweep up to 379‰, and controller 23's same-traffic comparison 336‰ with BLE versus
134‰ off (the dated runs below). This is not a fresh measurement of the latest build.
The scan interrupt runs at priority 7 every 50 µs and holds the
CPU about 11% at 192 MHz; the SDK's interrupt lock (`local_irq_disable()`) holds priority 7 off as
well ([hwtest.md](hwtest.md), "Lock"), so the BT stack's own critical sections will delay the
scan ("late max", "gaps"); and the radio's interrupts sit below 7, so the scan delays them by up
to its handler's time (≈5 µs average, 21 µs at most). SPEC R-14 asks to measure both early: the
scan's late max, skipped steps and gaps with a connection running, and the link's stability.

**Flash writes by the stack.** With pairing, the stack stores bonding keys in the SDK's VM: a flash
write, with interrupts masked; our own flash writes paused the scan up to 10 ms per erase
([config.md](config.md), "The first run"). Without bonding (the default since 2026-11-13, Q-24) the stack writes nothing (✅ controller 28 with macOS, iOS and the
adapter, "Without bonding on the mixer").
❓ Whether it writes anything else (the SDK's `BTIF` item for the MAC is written only when empty,
[hardware.md](hardware.md)).

## Design

The route below describes the implementation; early design choices superseded by the BT button
and sender task are not current settings. The dated runs later retain their original build scope.

```
core output ─► midiq (USB) ─► USB task ─► TinyUSB
            └► bleq (BLE) ─► BLE sender ─► blemidi_encode ─► notify (ATT)
ATT write ─► blemidi_decode ─► CORE_EV_MIDI_IN, transport BLE ─► core (feedback, FR-IO-06)
```

- **Packet framing, portable** (`firmware/midi/blemidi.c`, host tests): Apple's format. Encode:
  a header byte with the timestamp's high 6 bits, a timestamp byte before each message (13-bit
  ms), running status within a packet when the timestamp is unchanged, as many messages as fit in
  `MTU − 3` bytes, SysEx split across packets with timestamps before `F0` and `F7`, a real-time
  byte allowed inside SysEx. Decode: the reverse, tolerant of every form the specification
  allows (running status, timestamps between running-status messages, SysEx continuation
  packets), and robust to garbage (counted, LOG-34).
- **Batching** (FR-IO-03): the sender packs waiting messages and sends while the stack can take
  a whole notification (`GATT_COMM_EVENT_CAN_SEND_NOW`, `ble_comm_att_check_send()`); the ATT
  send buffer holds two notifications ("The route").
- **Last value wins** (FR-IO-08): the BLE queue merges a continuous control's waiting value with
  its newer one (the same target: channel, type, number); non-merge groups retain order, while
  NRPN/RPN continuous groups merge only with their selection dependencies preserved. A full
  queue can refuse a whole unit, and a disconnected sender drops queued data. The USB queue
  already holds collected units whole and applies backpressure
  ([core.md](core.md), "The MIDI queue under load"); the BLE queue adds the merge, since BLE
  carries about 10–30 messages per interval at best.
- **Directions** (FR-IO-04): USB always; BLE too while Bluetooth is on. The BT long press
  replaced controller 22's `global.outputs`; there is no such setting in the current schema.
  **Bridge** (FR-IO-05, SHOULD): not implemented.
- **Incoming** (FR-IO-06): BLE MIDI in enters the core as `CORE_EV_MIDI_IN` with transport
  `CORE_FROM_BLE` (index 1, already in the core), like USB's.
- **Tasks:** the GATT callbacks and events run in the SDK's `btstack` task (priority 3, the same as
  our USB task). The decoder runs there and passes messages to the control task through the input
  queue; sending runs in our `smc_ble` task (priority 3), woken by the control task and
  `CAN_SEND_NOW`. Task/SDK safety assumptions remain bounded by those recorded runs.
- **Logs:** the module `ble` (link state, parameters, MTU, encryption) and `midi_ble` (traffic,
  merges, refusals); the heartbeat gains the BLE latency (input → notification) and the queue's
  maxima (LOG-06). The command link and the configuration protocol run over BLE MIDI as SysEx since I-2
  ([link.md](link.md), "Over SysEx"; ✅ on the mixer, controller 31, 2026-11-13, `tools/e2e/e2e.py --ble`).
- **The BT button:** a long press turns Bluetooth on and off (SPEC §5.5); the BT LED blinks while
  advertising and stays lit when connected; implemented and checked on controller 23. The blink is
  twice a second (`CTL_BLINK_BT_US`, 250 ms half period; 4 a second before 2026-11-23, the owner
  found it irritating); the encoders' pickup indication keeps 4 a second.

## Packets

`firmware/midi/blemidi.c` (portable, no allocation; ✅ host tests; ✅ the device toolchain with
`-Werror`; 1 128 bytes of code at `-Oz` in the initial check; in the `BLE=1` controller since
controller 22, 2026-11-12):

- **Sending**, one packet at a time into the caller's buffer (the ATT MTU less 3: 20 bytes before
  the MTU exchange, 101 at our MTU of 104). `blemidi_tx_add()` takes whole messages (channel,
  System Common, real-time) and refuses one, leaving the packet as it is, when it does not fit or
  when its timestamp cannot follow the packet's last one (earlier, or more than one wrap of the
  low 7 bits ahead): then the caller sends and begins another. Running status (status and
  timestamp left out) only right after a channel message with the same status and timestamp;
  every packet starts with a status. `blemidi_tx_sysex()` writes SysEx bytes and returns how many
  fit; the rest goes on in the next packet (data right after the header, the F7 with a
  timestamp, alone if need be). Real-time may go into a SysEx; other messages wait for its end.
- **Receiving**, `blemidi_rx_packet()` per ATT write, into a sink (messages with their 13-bit
  timestamp; SysEx bytes as they come). It takes every form the specification allows: running
  status with or without a timestamp, also across packets and over real-time bytes; the low
  timestamp's wrap; real-time inside SysEx; SysEx continuation packets. Faults are counted
  (`bad`, LOG-34) and decoding goes on: no header, data with no status or before the first
  timestamp, a message cut short (by a timestamp or the packet's end: a message never continues
  into the next packet), a timestamp with nothing after it, F7 outside a SysEx, an undefined
  status, a SysEx cut short by a status (passed on without its F7).
- Tests (`test_blemidi.c`): the byte layouts, running status, the fit at 20 bytes, the timestamp
  rules (one wrap, the 13-bit wrap), the decoder's forms and faults, SysEx across packets at 20 and
  101 bytes (the F7 alone in the last packet, real-time inside), and a round trip of 400
  pseudo-random messages at 5, 20 and 101 bytes (decoded equal, timestamps included, no fault).

## The route (step 4)

Code: `firmware/midi/bleq.c` (the queue and its merge, portable, `test_bleq.c`),
`firmware/ctl/ctl.c` (the unit into both queues, `test_ctl.c`), `firmware/core/core.c` (the merge
marks, `test_core.c`, `test_merge_marks`), `firmware/app/ble/ble_br25.c` (the sender's task
`smc_ble`, the decoder), `firmware/app/controller.c` (the wiring).

- **Merge marks from the core.** Only the core knows which messages are a continuous control's
  value: while a fader or an absolute encoder sends a value of a CC, 14-bit CC, NRPN, RPN, pitch
  bend or channel pressure assignment, `core.out_merge` holds a number of its own (a new one each
  value), the NRPN selection sent with it included; every other message (buttons, notes, program
  changes, relative encoders, the null parameter) carries 0. The control task copies the mark of
  each message of the unit.
- **The BLE queue** (`bleq.h`, 128 messages, 1.7 KB): a unit's messages form groups (a run with
  the same mark is a merge group). A merge group that finds an older one of the same shape (the
  same statuses and controller numbers, and for NRPN and RPN the same parameter number) waiting
  marks it dead and goes at the end: the newest value wins, and no message ever goes ahead of one
  queued after it. Plain groups are never dropped. The NRPN rule keeps the receiver's selection
  what the core believes: a group that selects its parameter passes over other groups only when
  none in between relies on the older one's selection; a data-entry-only group only when none in
  between changes the selection.
- **The race** with the sender (a higher priority task): the sender checks a group's mark at its
  first message and then sends the whole group; a group marked dead after that still goes out
  whole, before its newer value. No lock.
- **Backpressure:** a round of the control task also stops while fewer than 32 messages fit the
  BLE queue (the input queue merges the faders meanwhile, as for USB); the sender wakes it.
  Without a connection, or with notifications off, the sender drops what waits (counted), so BLE
  never holds the USB.
- **The sender** (task `smc_ble`, priority 3, 1 KB stack): woken by the control task and by the
  stack's `GATT_COMM_EVENT_CAN_SEND_NOW`; while the stack's send buffer has room for a whole
  notification (⚠️ its ATT header counted, to be safe), it packs whatever waits into one
  (`blemidi.c`, the ATT MTU less 3 bytes, 20 before the exchange) with the send time as the
  timestamp, and hands it to `ble_comm_att_send_data()` (⚠️ from our task, as the SDK's examples
  call it from theirs). The buffer holds two notifications.
- **The decoder** runs in the stack's task on each write: channel messages go into the input queue
  as `CORE_FROM_BLE`; System Common and real-time are counted. SysEx bytes go through `sxpipe`
  to the USB task's configuration link since controller 31 ([link.md](link.md), "Over SysEx").
- **Bluetooth on and off** (SPEC §5.5, FR-IO-04; the owner's direction of 2026-11-12, replacing
  the `global.outputs` setting of controller 22): the BT button's long press (800 ms, the stock
  firmware's 41 scans of 20 ms, ⚠️ static analysis) turns Bluetooth off and on, at the 800 ms
  mark while the button is still held; a short press does nothing. On: the core's output goes to
  USB and BLE, and MIDI from both reaches it. Off: USB alone; the module stops advertising and lets
  a connected central go (`ble_module_enable(0)` from the sender's task, ⚠️ as the SDK's examples
  call it from theirs), and MIDI from BLE still in flight is ignored (counted). The state is the
  core's (`bt_on`), kept over a new configuration and over a restart or a power-off with the preset and
  the page ([config.md](config.md), "The panel's state"; Q-25 decided 2026-11-12): the stack
  starts advertising or not as it was left; on with nothing saved. The BT LED: dark while off,
  blinking while on with no central, lit while one is connected (`CORE_EV_LINK` from the stack's
  connection and disconnection events). Without `BLE=1` there is no Bluetooth: the long press does
  nothing and the LED stays dark.
- **Metrics** (LOG-06): the heartbeat adds "latency input to BLE" (input event → its message in a
  notification the stack took; wide buckets <5, <10, <15, <20, <25, <35, <50 ms; `smclog.py`
  checks NFR-02's median ≤ 25 ms) and "BLE: messages, notifications, merged, dropped, queue max,
  refused, in, faults".
- Costs: `app.bin` 160 640 bytes with `BLE=1` (+4.6 KB over controller 21's); `bleq.c` 1 258 bytes
  of code at `-Oz` (controller-22 measurements, not current sizes).

**The output bound (R01, fixed):** a review on 2026-11-15 found that `ctl.c` could truncate valid
output beyond its 32-message collection buffer. Ordinary output is now bounded at 38 messages,
the queues reserve that bound before the core runs, and configuration cleanup drains in bounded
rounds ([core.md](core.md), "On the device"). ✅ On the mixer 2026-11-23: 128 held notes all
released after a configuration replacement. The dated sweeps below predate the fix.

## The route on the mixer (2026-11-12)

Controller 22 with `BLE=1` (160 640 bytes), installed through the key padded to `0x6E000`
(`materials/install/app-22-ble`; `logs/recovery/app-22-ble-write.jsonl`). The owner at the Mac
(Audio MIDI Setup → Bluetooth), sweeping the eight faders fast, pressing buttons, turning
encoders, for about 80 s. Session `logs/sessions/2026-11-12-app22-ble-sweep`: the device's log and
both CoreMIDI sources ("SMC-Mixer Bluetooth" and the USB port) recorded together.

- ✅ **Nothing lost, nothing reordered:** of 14 696 USB messages the BLE source got 10 524
  (72%); the BLE stream is a subsequence of the USB one (every BLE message matched in order);
  all 574 notes in both, the same order; the last value of every one of the 16 targets the same
  on both.
- ✅ **The merge works under load:** 4 179 older values replaced, 10 412 messages in 4 548
  notifications (2.3 per notification on average; up to 3.2 in the busiest window); the BLE queue
  at most 68 waiting, nothing refused, no round held for it.
- ✅ **Latency input → BLE** (the device's side: to the notification the stack took; NFR-02 asks
  a median ≤ 25 ms): 10 412 messages, 94% under 5 ms, p99 ≤ 10 ms, max 17.3 ms. The air adds
  up to one connection interval (7.5 ms) on top. ⚠️ CoreMIDI's times are not arrival times for
  BLE (macOS rebuilds them from the packets' timestamps), so the host's view (BLE 0.3 ms behind
  USB at the median) does not measure the link.
- **Connection:** macOS first at 22.5 ms, then our 7.5 ms accepted, MTU 104 (notifications of
  101 bytes). 1 116 messages dropped before the connection (the controls moved while the Mac was
  not connected), as designed.
- **The scan under BLE traffic:** steps skipped for a busy SPI rose to 0.3–1.7% of the steps in
  the busiest windows (≤ 0.08% connected and idle, "The first run"); late max 49 µs, passes at
  most 1.1 ms apart (0.8 ms nominal). No input lost. ⚠️ Whether USB-only traffic of the same
  density skips as many is not measured (the comparison is the next check).
- **CPU:** load 379‰ in the busiest window (control task 285‰, USB 53‰), 132‰ over the session.
- **Stacks** (deepest point sampled): `smc_ble` 931 of 1 024 bytes free, `smc_ctl` 2 459 of
  3 072, `smc_usb` 1 759 of 2 048.
- Not covered here: MIDI from the central into the core (0 messages in: the Mac sent none; see
  "MIDI in on the mixer"); iOS; the BLE→DIN adapter.

## MIDI in on the mixer (2026-11-12)

Controller 22 `BLE=1`, the Mac connected over Bluetooth, `smcmidi send` to "SMC-Mixer Bluetooth"
(session `logs/sessions/2026-11-12-app22-ble-feedback*`):

- ✅ 240 channel messages sent (notes, CCs, a burst of 200 CCs 2 ms apart, NRPN sequences 3 ms
  apart): 83 writes, 1 077 bytes, 240 messages decoded, 0 faults; the core logged each as
  `midi in 1: …` and set the targets it addresses (`target … (midi in)`).
- ✅ The LEDs follow (the factory configuration, in RAM by CORE_SETUP): a note on 72 lights M1
  (`led 0 = 1`), on 80 lights Play (`led 32 = 1`), their note offs darken them. With the
  configuration stored on the mixer (source 3) the same notes set the targets but no LED changed
  (❓ not looked into: what that configuration's M1 and Play are assigned).

## Bluetooth on and off on the mixer (2026-11-12)

Controller 23 `BLE=1` (build 2e6f4534), the Mac connected over Bluetooth; everything injected over
the command link (LOG-33), both MIDI sources recorded by `smcmidi listen` (session
`logs/sessions/2026-11-12-app23-bt-onoff`): eight faders swept for 10 s (800 events a second),
BT held 1 s, the same sweep, BT held 1 s again.

- ✅ **On:** USB 7 999 messages, BLE 7 996 (3 merged), input → BLE all under 5 ms.
- ✅ **The long press:** `bluetooth off` at the hold, the module stopped advertising, the central
  was let go (`disconnected: reason 16`), `link 1 = 0`. **Off:** USB 8 000 messages, BLE 0; the
  BLE queue got nothing (`bluetooth 0`). **On again:** advertising again; the Mac does not come
  back by itself (as after a restart), Audio MIDI Setup's Connect brings it back and MIDI in
  works again (✅ 2 messages, 0 faults).
- ✅ **The scan's skipped steps are BLE's:** with the same USB traffic, 414 and 642 of ~199 000
  steps (0.2–0.3%) with Bluetooth on and sending, 10 (0.005%) with it off, 4–7 idle. The gap
  sites with Bluetooth on include the BT controller's and stack's tasks (`btctrler`, `btstack`).
  No input lost; late max 49 µs either way.
- **CPU:** 336‰ with BLE sending, 134‰ off with the same USB traffic.
- ✅ The BT LED (blinking, lit, dark) and a physical press of BT: by hand, the owner, the same day.

## The vendor's adapter on the mixer (2026-11-12)

The vendor's BLE→DIN adapter (FR-IO-09, SPEC R-11), one unit, by hand (the owner) with the
device's log kept (sessions `logs/sessions/2026-11-12-app24-*`, `-app25-name-adapter`,
`-app26-addr-adapter`).

- ✅ **It connects** to controller 24 (name "SMC-Mixer") as soon as Bluetooth is on, once it is
  no longer bound to an iPad it knew; notes from the buttons reach its DIN output. Faders and
  encoders are not checked through it yet (they need the configuration editor).
- ✅ **The adapter and the name:** it connects to controller 25, advertising as "BSM-Test", and to
  controller 26, "BSM-New" with another BLE address (`BLE_ADDR_XOR=5`, an address it had never
  seen). So it picks any device advertising the BLE-MIDI service, whatever its name or address
  (one adapter; ⚠️ another unit or firmware of the adapter may differ).
- ✅ **It asks for encryption on every connection:** `encryption: state 0, process 1` after each
  connect: status 0 (success) and process 1, `LINK_ENCRYPTION_PAIR_JUST_WORKS` in the SDK
  (`le_gatt_common.h`): a Just Works pairing each time, never the reconnection with stored keys
  (`LINK_ENCRYPTION_RECONNECT`, 15), also on the second connection to controller 25. ⚠️ So it
  probably keeps no bond, and pairing without bonding (`BLE_BOND=0`) should be enough for it.
- It wrote a system message about 32 times a second in the sessions of 2026-11-12 and 2026-11-13
  (counted as "others" in `BLE MIDI in`). Since controller 30 the heartbeat counts the others by
  kind ("BLE MIDI in, the others: … clock, … active sensing, …"). Connected to controller 30
  (2026-11-13, session `logs/sessions/2026-11-13-app30-adapter-system`) it wrote nothing in 40 s:
  ⚠️ so the stream is likely what a device on the adapter's DIN input sends (clock or active
  sensing), not the adapter's own; ❓ which, until a capture with that device running.
- Unplugging USB while the adapter carried the MIDI froze the controls in controller 23: the host
  had stopped reading and the USB queue held the core. Fixed in controller 24 ([usb.md](usb.md),
  "A host that is gone but looks configured"; ✅ the owner).

## Without the security manager

Controller 27 (`BLE_SM=0`, `CONFIG_BT_SM_SUPPORT_ENABLE=0`) did not start: no USB device after
the install. Controller 24's boot journal named the previous run's end ✅: a crash 331 ms after
start, cause 1, `RETI` in `btstack_linked_list_add_tail` called from `sm_add_event_handler`,
called from `att_server_init`, `R0` = `0x1d0`. In the SDK's library `att_server_init` always
registers with the security manager, whose state (`sm_global_info`) is allocated only by
`sm_init()`; `le_gatt_common.c` skips `sm_init()` when `sm_config` is NULL, so the pointer is
NULL ✅ (the disassembly, `build/app-ble-nosm27/app.lst`). The library cannot be changed
(`deps/` is read-only), so the security manager stays; the build refuses `BLE_SM=0`. What
remained of Q-24 was bonding: `BLE_BOND=0` (a make argument of controller 28, gone since it became
the default) built a controller that pairs when a central asks
(Just Works) and stores no keys, so the stack writes nothing to flash.

## Without bonding on the mixer (2026-11-13)

Controller 28 (`BLE=1 BLE_BOND=0`, build d35566b8, "Better-SMC-Mixer"), by hand (the owner), the
device's log kept (session `logs/sessions/2026-11-13-app28-nobond-hosts`):

- ✅ **macOS** connects (interval 24 × 1.25 ms, then 7.5 ms), no encryption asked; button notes
  go out; with iOS after it, 1 699 messages in 1 638 notifications, 0 refused by the stack (what
  besides the buttons is not in the INFO log).
- ✅ **iOS** connects the same way, no encryption asked; button notes go out.
- ✅ **The BT button** lets the iPhone go (`reason 16`) and advertises again on the next press.
- ✅ **The vendor's adapter** connects (interval 48 × 1.25 ms, then 7.5 ms), pairs Just Works
  (`encryption: state 0, process 1`) and plays the button notes on DIN (the owner). It writes
  32 system messages a second (one per write, 3 bytes each; ⚠️ likely from a device on its DIN
  input, "The vendor's adapter on the mixer"); the scan skips about 0.2% of its steps while they
  come.
- So all three work with no keys stored: no flash write by the stack (no long scan gap in the
  log). Q-24 decided (the owner, 2026-11-13): no bonding, the default build since. ❓ Android is
  not tried yet (no device at hand).

## Steps

1. ~~Research, the probe build, the costs (flash measured; RAM and CPU estimated)~~ 2026-11-12.
2. ✅ in the main (2026-11-12, "The first run"); the connected fader sweep followed on controller
   22. Left: physical free-page counts and the SDK BT tasks' stack depth in the heartbeat.
   The original checklist was: install a `BLE=1` build; check advertising
   and a connection from macOS (Audio MIDI Setup → Bluetooth) and iOS; measure the physical
   pages free before and after the stack starts, the three stacks' depth, the CPU load, the
   scan's late max, skipped steps and gaps with a connection running; `firstrun.py` and
   `smcsim device` still passing. This settles R-14 and the RAM budget before the rest builds on
   it.
3. ~~The framing on the host~~ 2026-11-12: `firmware/midi/blemidi.c` ("Packets" below) with
   `firmware/tests/test_blemidi.c`. No scenario: the framing is not the core's behaviour
   (LOG-31); the device route of step 4 brings one with `CORE_FROM_BLE`.
4. The device route: the BLE queue with the merge, the sender with batching, the decoder into the
   core; the directions setting; latency metrics (code and host tests 2026-11-12, "The route").
   ✅ On the mixer with macOS the same day ("The route on the mixer"): input → BLE median under
   5 ms (NFR-02), eight faders swept with the merge (FR-IO-08), every note there in order. ✅ MIDI
   in from the Mac reaches the core and the LEDs ("MIDI in on the mixer"). The same-traffic
   scan/CPU comparison followed on controller 23 ("Bluetooth on and off on the mixer"). Left:
   fuller iOS musical-load/feedback checks beyond controller 28's button notes.
5. The BT button and LED ("Bluetooth on and off"): code and host tests 2026-11-12, the
   directions setting replaced; ✅ on the mixer the same day by injection ("Bluetooth on and off
   on the mixer"; ✅ the LED and a physical press by hand). ✅ The vendor's BLE→DIN adapter
   connects, whatever the name or address ("The vendor's adapter on the mixer"). ✅ Bonding
   off (`BLE_BOND=0`, controller 28) with macOS, iOS and the adapter, 2026-11-13 ("Without
   bonding on the mixer"); no bonding is the default (Q-24, the owner, 2026-11-13). Then the bridge (FR-IO-05).

## The first run (2026-11-12)

Controller 21 with `BLE=1` (build `2e94fa82`, `app.bin` 156 048 bytes, pairing compiled in),
installed through the key padded to `0x6E000` (`materials/install/app-21-ble`: the plan's checks
pass, area `0x41000..0x6760F`, VM 24 KB; `logs/recovery/app-21-ble-write.jsonl`). The owner at the
Mac and an iPhone. Sessions `logs/sessions/2026-11-12-ble-start`, `-ble-connect`,
`-firstrun-app21-ble`; `logs/device/2026-11-12-app21-ble-scenarios`.

- ✅ **The stack starts** on our board and clock: `btstack_init` 0, advertising; `ble_br25_start()`
  takes 0.44 s of the boot, after USB and the panel are up. The saved configuration at
  `0x25000` loads as without Bluetooth.
- ✅ **macOS** (Audio MIDI Setup, Bluetooth): found and connected; 22.5 ms at first, then our
  7.5 ms accepted (6 × 1.25 ms, timeout 4 s), ATT MTU 104, MIDI notifications on. Shown in MIDI
  Monitor; the controls' MIDI goes to USB only (expected: nothing routes to BLE yet). The Mac's
  disconnect: reason `0x13`, and advertising resumes by itself (`adv_auto_do`).
- ✅ **iOS** (AUM): listed, but the first three connections were dropped by the phone at once
  (30 ms interval, reason `0x13`, no ATT exchange); one such connection also came right after the
  first start, before anyone tried. After "Forget This Device" in the phone's Bluetooth settings,
  AUM connected: 7.5 ms, MTU 104, notifications on. ⚠️ The phone kept a bond from the stock
  firmware for the same address (our address comes from the same MAC rule, "What the stock
  firmware does") and asked for encryption with keys we do not have. Every user who paired the
  stock firmware will meet this once: the manual must say "forget the device first".
- ✅ **No pairing asked** by either host: no encryption event in the log, so BLE-MIDI works
  unpaired on macOS and iOS (Q-24). So the stack wrote no bond to flash: no long scan gaps after
  the connections.
- **The scan** (heartbeats every 10 s): advertising only, steps skipped for a busy SPI 2–20 per
  10 s (0 without Bluetooth), gaps up to 0.15 ms; connected at 7.5 ms, 120–160 skipped per 10 s
  (≤ 0.08%), gaps up to 0.3 ms; the late max 49 µs (25 µs without Bluetooth), the handler's max 36
  µs. No bounces, no lost input. The stack's start costs gaps up to 10 ms in `btstack` and
  `btctrler`, once, in the first 10 s.
- **CPU load** (samples not idle): 40‰ advertising, 47–54‰ connected (controller 20 without
  Bluetooth: ⚠️ not taken from the same heartbeat; to compare).
- **Heap** (virtual, `heap_mmu`): 95 608 → 88 680 bytes free with the stack up (6.9 KB), 264 bytes
  more per connection, given back at the disconnect. ❓ The physical pages and the three tasks'
  stack depth: the heartbeat does not report them yet (step 2's rest).
- ✅ Nothing else broke: `firstrun.py` 32 passed (the resets included), `smcsim device` 15
  scenarios passed with MIDI through CoreMIDI.

## Not verified

The physical RAM the stack takes and the SDK BT tasks' stack depth; a latest-build repeat of
the older CPU/scan load measurements; iOS with the new BLE address and fuller musical traffic
(controller 28's button notes did run); faders and encoders through the vendor's BLE→DIN adapter;
Android; switching back to stock without forgetting; stock discovery in Chrome without
restarting the browser (on 2026-11-23 the stock mixer was listed only after a browser restart).
