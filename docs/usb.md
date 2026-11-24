# USB device controller (BR25)

The BR25's full-speed USB device controller and our TinyUSB driver for it,
[`firmware/platform/br25/usb/`](../firmware/platform/br25/usb/). The bootloader and the
application share the driver (SPEC D-1, §4).

**Status:** written on 2026-11-08, with host tests against a model of the controller
(`firmware/tests/test_dcd_br25.c`, `make -C firmware test`). Both images
link it and build with the JieLi toolchain:

- the bootloader (`tools/env/build_boot.sh`), in place of the stage 1 stub;
- the application (`tools/env/build_app.sh`), with a CDC interface for the command link
  ([link.md](link.md)).

✅ The application driver, CDC and USB MIDI have run on the mixer since 2026-11-11, including
the 12-minute scan/USB run and controller scenarios ([hwtest.md](hwtest.md), [core.md](core.md)).
Physical USB E2E and Chrome client checks followed on 2026-11-15, and on 2026-11-23 browser
firmware updates over USB with physical off/on and configuration read-back, and the end-to-end
test 37/37 ([integration.md](integration.md), "Hardware results, 2026-11-23").
**Our bootloader has not run on the mixer.** The list
at the end separates operational evidence from unmeasured register/reset boundaries.

## Sources

Every fact below has its source marked:

- **SDK**: the AC63 SDK's headers (`include_lib/driver/cpu/br25/asm/br25.h`, `usb.h`,
  `include_lib/driver/device/usb/usb_phy.h`) and its compiled driver (`cpu/br25/liba/cpu.a`:
  `usb.c`, `usb_phy.c`, `gpio.c`). The library ships as LLVM bitcode; we read it as IR, locally
  (`build/re-sdk/`, not part of the repository). The SDK is Apache 2.0; we copy no code from it.
- **pad**: what open-smc-pad measured on the SMC-PAD, the same chip (their `docs/usb-br25.md`).
  A knowledge source only (D-1): no code of theirs is in ours, and each pad fact is ❓ on the
  mixer until our hardware test (B3) shows it.
- **ours**: what we found ourselves, with where.

Register addresses and bit names are in
[`br25_usb_regs.h`](../firmware/platform/br25/usb/br25_usb_regs.h), with their sources.

## The controller

A Mentor MUSB core behind two JieLi blocks (SDK):

- **Register access.** The MUSB registers are byte-wide and reached one at a time through
  `USB_CON1` (`0x1E1804`). To write, store `reg << 8 | data`; to read, store `reg << 8 | 1 << 14`.
  In both cases poll until bit 15 (ACK) is set, at most 20000 times, with a `csync` after the store.
  A read's value is the low byte of `CON1`. JieLi numbers the registers in its own order (`FAddr`
  0, `Power` 1, the interrupt registers 2–6, their enables 7–0x0B, `Index` 0x0E, then the
  indexed CSRs 0x10–0x17). The SDK masks the data to 8 bits, and it checks `CON0.NRST`
  before each access: with the core in reset, `CON1` does nothing.
- **DMA instead of FIFO data registers.** Each endpoint has a RAM buffer address: `EP0_ADR`
  (`0x1E181C`), `EPn_TADR`/`EPn_RADR` (`0x1E1820 + 8(n−1)`, `+4`). Writing `EPn_CNT` (`0x1E1808 +
  4n`) gives a transmit its length.
- **Interrupts.** `IRQ_USB_CTRL` is index 9 and the SOF interrupt is index 8. Reading `CON0`
  acknowledges the controller's interrupt. `INTRUSB`, `INTRTX1` and `INTRRX1` clear on read.
- **Pads.** `USB_IO_CON0` (`0x1E5100`, in the GPIO block) controls the D+/D− pads: digital inputs,
  USB or GPIO mode, slew rate, pull-ups and pull-downs. The 1.5 kΩ D+ pull-up is what attaches the
  device.
- **Clock.** The PHY runs from the PLL's 48 MHz output; the SDK's `usb_phy.c` names the source
  `USB_CLOCK_IN_PLL48M`. The application gets this from the SDK's clock setup. The bootloader sets
  no clock of its own and relies on what the factory uboot leaves running (❓, see the list at the
  end).

**Initialisation** (SDK, `usb_device_mode`/`usb_g_sie_init`; `dcd_init` does the same in this
order):

1. pads: digital inputs on, USB mode, slew rate on, all pulls off;
2. `CON0` clears `NRST`, `CID`, `VBUS` and the SIE pending bit;
3. `CON1` = 0;
4. `PHY_ON`;
5. `CON0 |= NRST|TM1|CID|VBUS` (`0x3C`);
6. interrupt enables cleared, pending flags read away, `EP0_ADR` set;
7. `INTRUSBE` = Reset|Suspend|Resume;
8. `CON0 |= SIEIE`;
9. attach: `Power` = SoftConn (`0x40`) and the D+ pull-up on.

## Behaviour the driver depends on

The numbers are referred to from the code.

1. **MaxP is written as `0xFF`** whatever the descriptor says. SDK: `usb_g_ep_config` passes
   1023, which `CON1` truncates to `0xFF`. Pad: with the real size (64) the controller
   double-buffers the endpoint by itself, and every other packet moves through *address + 512*.
2. **The RX FIFO holds two packets.** Pad: even with MaxP `0xFF`, `RXCSR` reads RxPktRdy |
   FIFOFull. In a run of full packets RxPktRdy can come up while the packet is still streaming
   into RAM, and the count reads 0; moving `RADR` or popping at that moment splits the packet. A
   real zero-length packet comes without FIFOFull. The driver (`rx_count`) re-reads the count, at
   most 1600 times (enough up to 192 MHz), only when it reads 0 *and* FIFOFull is set. A count that stays 0 is taken as a
   zero-length packet and counted in `rx_wait_fails`. SDK: `usb_read_rxcount` returns 0 unless
   RxPktRdy is set, and the SDK's blocking read simply polls again.
3. **One RAM buffer per direction, rewound for every packet.** SDK: the buffer address is
   written before every transmitted packet. On bulk endpoints the SDK ping-pongs `RADR` between two
   buffers 68 bytes apart; on interrupt endpoints it keeps a single buffer and does not rewrite
   it. It points `EP0_ADR` once, then copies to and from that buffer. Pad: the pointers advance
   with each packet, and the SDK's ping-pong loses the first bytes of the next packet when the
   switch lands during its reception. The driver rewrites `EP0_ADR`, `TADR` or `RADR` before each
   packet, whatever the hardware does: on a pointer that does not move, rewriting it is harmless.
   It copies a packet out of the buffer *before* it rewinds and pops; the SDK pops first.
4. **Endpoint 0 is the MUSB peripheral protocol** (SDK command values: ServicedRxPktRdy `0x40`,
   TxPktRdy|DataEnd `0x0A`, ServicedRxPktRdy|DataEnd `0x48`, ServicedRxPktRdy|SendStall `0x60`):
   - a SETUP is left unserviced (the host is NAKed) until the stack answers it;
   - DataEnd goes with the last data packet, and the controller then runs the status stage by
     itself.

   Two orderings are allowed and both are handled (pad, both seen on their device):
   - the host's status OUT can complete in the same interrupt as the last IN data packet;
   - the next SETUP can arrive before the stack, which runs as a task, arms the last transfer's
     status. Such a SETUP is **parked** in the FIFO until that status is armed, or else it would
     be taken as the new request's status (`test_setup_before_status_armed`).
5. **IN and OUT on different endpoint numbers** (pad; e.g. CDC notify `0x81`, OUT `0x02`, IN
   `0x83`). The driver allows the same number in both directions, each with its own buffer and
   state. The pad no longer knows whether the rule is needed. The application breaks it once,
   for USB MIDI (see "Endpoints in the application"); the bootloader has only endpoint 0.
6. **The interrupt handler is installed before `tusb_init()`** (pad). `dcd_init` attaches, and a
   bus reset that reaches an empty vector resets the chip mid-enumeration. The pad saw macOS stop
   looking at the port after a few of these. The bootloader is polled and needs no handler.
7. **Task-side register access masks the interrupt** (SDK: its `usb_write_*`/`usb_read_*`
   functions disable interrupts around `Index` + access; pad). The indexed registers share
   `Index` with the handler. The driver calls `br25_usb_irq_set(false/true)` around every
   task-side sequence; the platform implements it (empty in the polled bootloader).
8. **Packet sizes**: endpoints 1–4, at most 64 bytes, bulk and interrupt only (the driver refuses
   isochronous: the SDK sets an extra `TXCSR`/`RXCSR` bit, `0x4000`, for it, and we do not need
   it).

## Driver design

- **TinyUSB's `dcd_*` API** (TinyUSB 0.21, `third_party/tinyusb/src/device/dcd.h`):
  - `dcd_set_address` answers SET_ADDRESS itself (status stage, no event), and the new address
    is written to `FAddr` only when the status IN is over;
  - the stack stalls both EP0 directions on failure, and the driver writes the stall once;
  - EP0 transfers come in parts of at most 64 bytes;
  - a zero-length packet ends a data stage shorter than `wLength` whose length is a multiple of
    64 (the stack asks for it);
  - `dcd_init` attaches.
- **Endpoint 0 state**: idle → SETUP (unserviced) → IN or OUT data → STATUS (DataEnd written) →
  idle. A stall or SetupEnd (the host gave up on the transfer) returns it to idle, and the
  stack's later calls for the dead transfer are dropped.
- **Other endpoints**: one packet in flight per direction. An OUT packet that arrives before the
  stack arms a transfer stays in the controller (the host is NAKed) until it does.
- **Locking** (item 7): when the stack queues an event from task context, its OS layer (`OPT_OS_NONE`)
  calls `dcd_int_disable`/`dcd_int_enable`, which unmasks the interrupt even inside our own masked
  section. So on every task-side path, raising the event is the last thing the driver does.
- **Polled or interrupt-driven**: the bootloader calls `dcd_int_handler(0)` from its main loop.
  The application calls it from the IRQ 9 handler (below).
- **Logging**: module `usb`, cause 0 everywhere (the handler runs in interrupt context,
  [logging.md](logging.md)): bus events, SETUPs, stalls, aborts, register timeouts (the first
  one), RX waits that failed. Packet-level traces are at TRACE level. Counters for the hardware
  test: `br25_usb_stats()` (resets, suspends, resumes, SETUPs, stalls, SetupEnds, packets, RX waits
  and the longest one, failed waits, truncated bytes, register timeouts).

## In the application

`firmware/app/usb/usb_app.c` ([link.md](link.md), "On USB"):

- The SDK's own USB stack is not built: its USB device and OTG sources (`apps/common/device/usb/`)
  are not in the build, and `TCFG_PC_ENABLE` is off. So the controller is ours alone.
- The IRQ 9 handler: priority 3, the SDK's for its USB device (`usb_device.c`). It runs
  `dcd_int_handler(0)`, then posts a semaphore to the USB task. The SDK does the same from its
  own handlers (`led_spi.c`).
- `br25_usb_irq_set()` masks and unmasks IRQ 9 using `irq_br25_enable()` under our interrupt
  lock (`irq_br25.h`), avoiding the ROM's breakpoint-patched SDK lock since controller 10
  ([core.md](core.md), "The USB path's locks").
- The handler is installed and masked before `tusb_init()`: `dcd_init` attaches, and TinyUSB then
  unmasks it through `dcd_int_enable` (item 6).
- TinyUSB runs without an OS layer in the task `smc_usb`. A bus reset does not call
  `tud_umount_cb`, so the task watches `tud_mounted()` and DTR itself to reset the link.

### Endpoints in the application

The controller has endpoints 1–4. CDC needs three (notification IN, data OUT, data IN) and USB
MIDI two (OUT, IN). That is five directions, so item 5 (IN and OUT on different numbers) cannot
hold for all of them. The application shares one number between MIDI's two directions:

| Endpoint | Function | Size |
|---|---|---|
| `0x81` | CDC notification IN (interrupt) | 8 |
| `0x02` | CDC data OUT (bulk) | 64 |
| `0x83` | CDC data IN (bulk) | 64 |
| `0x04` | MIDI OUT, host → device (bulk) | 64 |
| `0x84` | MIDI IN, device → host (bulk) | 64 |

The model test drives both directions of endpoint 4 at once (`test_shared_endpoint`), and
`test_app_descriptors` checks that 4 is the only number used both ways. ✅ The application uses
this shared endpoint on the mixer: MIDI output/scenarios since 2026-11-11, and configuration
round trips over USB MIDI since controller 31, 2026-11-13 ([link.md](link.md), "Over SysEx").
That demonstrates the deployed layout, not an exhaustive simultaneous-direction stress test.

### USB MIDI

One USB MIDI 1.0 function (TinyUSB's `midi_device.c`) with two embedded jacks each way, so the
host sees two ports ([SPEC](SPEC.md) FR-IO-01):

| Cable | Port name | What it carries |
|---|---|---|
| 0 | "SMC-Mixer" | performance: the core's MIDI out, MIDI in for LED feedback |
| 1 | "SMC-Mixer Config" | the command/configuration link over SysEx, including panel and SENT events ([link.md](link.md), "Over SysEx") |

The port names are the jacks' strings. ✅ macOS shows them (2026-11-11): CoreMIDI lists two
sources and two destinations, "SMC-Mixer" and "SMC-Mixer Config", under one device; apps that
show only the device's name (ShowMidi) list two entries with the same name, and only the first
carries the controller's MIDI. The hardware test and the controller share the development ID
`1209:0001`; with one serial for both ("0") macOS kept the name it saw first, and a MIDI monitor
showed the controller as "SMC-Mixer hardware test" (2026-11-14). Since then each variant has a
serial of its own (`variant.h`, `usb_serial`): the controller "C1", the hardware test "T1"
(the owner, 2026-11-14). ✅ Controller `C1` checked on macOS, 2026-11-15, build `a628aa6a`:
the connected USB device's registry serial matches `C1`; CoreMIDI's display names are `SMC-Mixer`
and `SMC-Mixer Config`. Its raw names are empty and `Config`, so `tools/hostmidi` now prefers the
display name ([link.md](link.md), "Over SysEx"). The hardware test's `T1`, the two variants shown
side by side and the MIDI monitor's device label are not checked on hardware. The old "0" entry
may stay listed offline in Audio MIDI Setup: remove it there. iOS: not checked yet. The
controller's `bcdDevice` is the product's version, major and minor in BCD (`0x0001` for 0.1.0;
`app_version.h`; the patch number is in GET_INFO); the bootloader's stays `0x0100`. ✅ The
controller's connected registry entry reports `0x0001` (2026-11-15); the bootloader is not run. All TinyUSB calls stay in the USB task (`usb_app.c`, `midi_service()`):

- **Out.** The control task packs the core's messages into 4-byte USB-MIDI packets
  (`midi/usbmidi.c`; the CIN is the status's high nibble for channel messages) and pushes them
  into a single-producer, single-consumer queue of 64 packets (`midi/midiq.c`), the messages of one
  event as one unit, all or none. It then wakes the USB task, which writes packets with
  `tud_midi_packet_write` until TinyUSB's FIFO is full and pops only those taken. TinyUSB's MIDI
  transmit FIFO holds 16 packets (`CFG_TUD_MIDI_TX_BUFSIZE` 64; was 256), one USB packet's worth
  behind the one on its way. The control task takes no event while more than 8 packets wait
  (backpressure); the USB task wakes it (`usb_app_midi.room`) once they have gone
  ([core.md](core.md), "The MIDI queue under load"). A unit that does not fit is refused whole;
  the control task counts it and logs a warning once per run of refusals (none expected).
- **No host.** While MIDI is not configured (no host, or the host has not opened the function),
  the USB task throws queued packets away and counts them, so a controller that is played without
  a host does not send stale MIDI when one appears. "USB MIDI configured by the host" and "USB
  MIDI gone" are logged at the transitions.
- **A host that is gone but looks configured.** Pulling the cable gives no bus reset, so TinyUSB
  keeps the device mounted; its MIDI FIFO filled, the queue behind it too, and the control task,
  waiting for room (backpressure), took no more events: the panel froze until the cable came back
  (✅ seen 2026-11-12, controller 23 on battery with the BLE→DIN adapter). Since controller 24
  the USB task treats a suspended bus as no host ("suspended: USB MIDI as with no host"), and, in
  case the bus does not report the suspend, drops what waits once the host has taken nothing for
  250 ms (`MIDI_STALL_US`; "the host took nothing for 250 ms", counted as "times the host stopped
  reading"). ⚠️ Whether the controller sees a suspend when the cable is pulled is not known yet.
- **In.** Packets from the host are checked (`usbmidi_unpack`: the CIN must match the status, data
  bytes below 0x80). Channel messages on cable 0 go to the variant (the controller turns them into
  core events); SysEx on cable 1 (the config port) goes to the command link over SysEx
  ([link.md](link.md), "Over SysEx"), whose replies go out on cable 1; SysEx on cable 0, system and
  realtime messages and bad packets are counted only. The first bad packet is logged as a
  warning.
- Counters: "USB MIDI: in … channel, … SysEx, … system, … config port, … bad; out …, … dropped (no
  host)" with the link's statistics (`METRICS_NOW`). The control path bounds ordinary output at
  38 messages and reserves that room before the core runs; configuration cleanup drains in
  bounded rounds (R01, fixed: [core.md](core.md), "On the device").

## The model test

`firmware/tests/test_dcd_br25.c` builds the driver for the host (`-DBR25_USB_MODEL`: register
accesses go to the model instead of memory) and plays both the host and the stack:

- the model does the `CON1` protocol (ACK, or none for the timeout test) and the clear-on-read
  interrupt registers;
- `CSR0` follows MUSB semantics, and a SETUP must be serviced before data goes out;
- `TXCSR`/`RXCSR`: one TX packet in flight, a two-packet RX FIFO (FIFOFull), FlushFIFO pops,
  ClrDataTog counted;
- **a packet that lands while its buffer address has not been rewritten since the last one is an
  error** (item 3);
- an RX count can be made to read 0 a given number of times (item 2).

Scenarios:

- init: register state, attach and detach;
- bus reset;
- GET_DESCRIPTOR of 18 bytes, of 100 bytes (64 + 36), exactly 64 of 255 (with the
  zero-length packet) and 64 of 64 (without it);
- SET_ADDRESS (address only after the status, no event);
- SET_CONFIGURATION;
- OUT data stages: one packet; two packets with the second arriving before it is asked for; the
  status completing before it is armed;
- a stall, written once, and SentStall cleared;
- SetupEnd in mid-transfer;
- the parked SETUP, and a late stall that is dropped;
- bulk OUT: two full packets in a row with the count reading 0 five times, a short packet, a
  zero-length packet, two packets waiting before the transfer is armed, a count that stays 0;
- bulk IN of 150 bytes and a zero-length packet;
- OUT and IN on the same endpoint number (USB MIDI's `0x04`/`0x84`): each direction completes
  while the other is busy, separate buffers, a stall in one direction leaves the other alone;
- stall and clear stall, refused endpoint types and sizes, `close_all`;
- suspend, resume, a bus reset with a transfer in flight, interrupt masking;
- a register access that never gets its ACK.

A model checks the driver against *our reading* of the hardware, not the hardware.

## Not checked without hardware

The hardware-test firmware (B3) logs `br25_usb_stats()` and the driver's events. This original
checklist retains the hardware-only questions; it does not mean that USB is wholly untested.
✅ Application enumeration, CDC traffic and USB MIDI ran on 2026-11-11, with later configuration
E2E and browser checks ([integration.md](integration.md)). Those runs support the
driver's operational path, but do not independently prove every pad-derived FIFO/pointer rule,
parked-SETUP occurrence or forced mid-traffic reset. Bootloader clock/enumeration remain unrun;
iOS product use does not by itself verify both port labels or `bMaxPower`.

1. **The clock in the bootloader**: whether the PLL's 48 MHz is running when our code starts after
   the factory uboot. If it is not, the bootloader needs its own clock setup (the SDK's
   `clk_set`/PLL code is the reference).
2. Enumeration on macOS and iOS: the bootloader as DFU, the application as CDC and USB MIDI
   ([link.md](link.md) lists the CDC checks). For MIDI: both ports appear with their names
   (macOS Audio MIDI Setup, AUM on iOS); MIDI both ways on the shared endpoint 4 while the CDC
   log stream runs; nothing queued while no host is there comes out when one appears.
3. Item 1, MaxP `0xFF`: correct data in long IN and OUT transfers.
4. Item 2: RX waits during a run of 64-byte packets (`rx_waits`, `rx_wait_max`). No
   `rx_wait_fails`, and no packet that differs from what was sent.
5. Item 3: whether the pointers really move. Whatever the answer, the rewrites are harmless.
6. Item 4: the parked SETUP actually happens (the log says "setup parked"), and nothing hangs.
7. The `CON1` ACK always comes (`reg_timeouts` stays 0).
8. Suspend and resume. Remote wakeup is not supported (`dcd_remote_wakeup` only logs).
9. Bus reset in the middle of traffic; detach and re-attach.
