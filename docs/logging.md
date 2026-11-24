# Logging: smclog

How the firmware logs and how the host reads the logs (SPEC §8). Device side:
`firmware/lib/smclog/`; host side: `tools/smclog/smclog.py` (Python, standard library only).

**Hardware evidence:** the CDC stream, heartbeat and boot journal ran on the mixer on
2026-11-11; `firstrun.py` checked soft, fault and watchdog resets (image 30, then the controller).
The read-only USB captures on 2026-11-15 add idle stability across clock wraps, not a sustained
MIDI-load test.

## Using it in firmware code

```c
#include "smclog.h"

LOG_INFO(input, "fader %u = %u", idx, value);       /* module, printf-style format, arguments */
LOG_WARN(usb, "endpoint %u stalled", ep);

uint16_t c = smclog_cause_begin();                   /* an input event starts a cause chain */
LOG_DEBUG(engine, "page %u", page);                  /* records written now refer to cause c */
smclog_cause_end();

LOGC_DEBUG(c, midi_usb, "sent %u bytes", n);         /* explicit cause: interrupt handlers */
```

- Levels: `LOG_ERROR`, `LOG_WARN`, `LOG_INFO`, `LOG_DEBUG`, `LOG_TRACE` (`LOGC_*` with an explicit
  cause). `SMCLOG_LEVEL_MAX` drops the higher levels at build time (they generate no code);
  `smclog_set_level(SMCLOG_MOD_x, level)` sets each module's run-time level; INFO by default
  (LOG-08).
- Modules are listed in `smclog_modules.h`. A module name must not be a macro, which is why the
  logger's own module is `smclog`, not `log`.
- The format must be a string literal. Up to 8 arguments: integers up to 64 bits and pointers,
  `float`/`double` (sent as `float`), strings (`char *`, cut to fit the record). Length modifiers
  (`l`, `ll`, `z`, …) are accepted and ignored: the type comes from the argument (`_Generic`), not
  from the format.
- `smclog_write()` may be called from tasks and interrupts. The ring is guarded by
  `smclog_port_lock()`.

## How a call site becomes a record

Each `LOG_*` call puts a description of itself into the section `smclog` (`__DATA,__smclog` on
macOS) and passes its address to `smclog_write()`. The section is not loaded on the device (it is
`INFO` in `smclog.ld`, which the application, bootloader and check linker scripts include). The
section starts at address 0. So the address the code passes is the offset of the description, and
**the record's ID is that offset / 4**. No text reaches the device's flash or the wire (LOG-01).

Call site description, 4-byte aligned, all fields little-endian:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `0x01A54C53` (`"SL"`, `0xA5`, version 1) |
| 4 | 2 | size of the header and the strings |
| 6 | 2 | source line |
| 8 | 1 | level (1 ERROR … 5 TRACE) |
| 9 | 1 | module index (`smclog_modules.h` order) |
| 10 | 2 | reserved, 0 |
| 12 | 4 | argument types: count in bits 0–3, then 2 bits per argument |
| 16 | … | module name, file and format, each NUL-terminated |

Argument types: 0 `int` (zigzag varint), 1 `i64` (zigzag varint of a 64-bit value), 2 `f32`
(4 bytes), 3 `str` (a length byte, then the bytes; bit 7 of the length is set when the string was
cut; at most 127 bytes).

Varints are LEB128: 7 bits per byte, low bits first, bit 7 set on every byte but the last. Zigzag
maps 0, −1, 1, −2, … to 0, 1, 2, 3, …. The decoder masks values printed with `%u`/`%x` to the
argument's width.

## Records (wire format version 1)

| Kind | Byte 0 | Fields after it |
|---|---|---|
| LOG | `0x01` | seq u16, t_us u32, id u16, cause u16, arguments |
| LOST | `0x02` | seq u16, t_us u32, count u32 |
| HEADER | `0x03` | version u8, timer_hz u32, t_us u32, lost_total u32, ring_size u32, build ID length u8, build ID |
| LOG_TRUNC | `0x04` | as LOG; the arguments that did not fit are missing (shown as `<?>`) |
| PAST | `0x80` + kind | a LOG or LOG_TRUNC record of the previous run, replayed at start ("Boot journal" below) |

- `seq` counts LOG and LOST records (wraps at 2^16); the decoder reports a gap as a `gap` record.
- `t_us` is µs since start, from the port's clock (`SMCLOG_TIMER_HZ`). It wraps after 2^32 µs
  (about 71 minutes); the decoder unwraps it.
- `cause` is 0 outside a cause chain (LOG-04).
- A record is at most `SMCLOG_REC_MAX` bytes (96).
- A PAST record keeps its old `seq` and `t_us` (the previous run's clock); the decoder marks it
  `past` and leaves it out of the gap and time tracking.
- HEADER opens every stream (LOG-12): `smclog_stream_header()`. A stream whose build ID differs from
  the dictionary's is an error, not garbage. Records before any header are decoded with a warning.

## RAM ring

- `SMCLOG_RING_SIZE` bytes (a power of two; 8 KB by default, LOG-03). Entries are
  `[length u8][record]`.
- It is zero-initialized, so it works before `smclog_init()`. `smclog_init()` only writes the
  "log start" record.
- When a record does not fit, it is dropped and counted. A LOST record with the count goes in
  before the next record that fits, or is made by `smclog_drain()` once the ring is empty.
- `smclog_drain(out, cap)` (a single consumer) moves whole frames into a transport buffer and never
  splits a frame. A buffer of `SMCLOG_FRAME_MAX` bytes always makes progress.
- `smclog_get_stats()` gives records written, records lost, bytes in use and the high-water mark
  (for the LOG-06 heartbeat, "Metrics" below).
- A second ring, the flight recorder, can be attached (`smclog_recent_attach()`): every record
  also goes there, and it keeps the latest ones across a reset ("Boot journal" below).

## Frames

Each record is sent as one frame:

1. Append CRC-16/CCITT-FALSE of the record (polynomial `0x1021`, initial value `0xFFFF`),
   little-endian.
2. COBS-encode the result.
3. End the frame with `0x00`.

A reader resynchronizes at the next `0x00`. A frame with a bad CRC or bad COBS becomes a
`bad_frame` record.

`smclog_frame()` and `smclog_crc16()` frame other records for the same stream: the command
link's replies ([link.md](link.md)), whose kinds start at `0x10`.

## Build ID and the dictionary

- **Device:** the linker's GNU build-id note (`--build-id=sha1`). The linker scripts place it in
  flash and name it `__build_id_note`; the port reads the ID from there.
- **Host test binaries (macOS):** the Mach-O `LC_UUID`.
- **Host test binaries (Linux):** the ELF GNU build-ID note (`smclog_port_host.c`).

`smclog.py dict BINARY` reads the `smclog` section and the build ID from an ELF or a Mach-O file
and writes the dictionary:

```json
{"format": 1, "build_id": "d1da…56db", "binary": "app.elf", "section": "not loaded",
 "sites": {"0": {"level": "INFO", "module": "boot", "module_index": 0, "file": "…", "line": 43,
                 "format": "…", "types": []}}}
```

The build scripts write it next to the binary (`build/app/app.smclog.json`,
`build/boot/boot.smclog.json`), as LOG-22 asks. Every dictionary must be kept with its binary.

## Host tool

```sh
python3 -I tools/smclog/smclog.py dict build/app/app.elf -o app.smclog.json
python3 -I tools/smclog/smclog.py decode --dict app.smclog.json stream.bin --text
python3 -I tools/smclog/smclog.py capture --port /dev/cu.usbmodemXXXX --dict app.smclog.json \
    --session logs/sessions/2026-11-07-faders --midi SMC-Mixer
python3 -I tools/smclog/smclog.py cmd --port /dev/cu.usbmodemXXXX --dict app.smclog.json level input debug
```

- `decode` reads a raw stream (frames as the device sends them). It writes JSON lines; `--text`
  writes one line per record instead.
- `capture` records a session until Ctrl-C or `--seconds`:
  - it subscribes to the log over the command link and unsubscribes at the end
    ([link.md](link.md)); `--no-subscribe` only listens;
  - the raw stream from a serial port (USB CDC);
  - its decoded records with host timestamps;
  - a PING every second (`--sync`), whose reply holds the host's send time, the device's time and
    the host's receive time: the samples for clock synchronization;
  - markers: type a line and press Enter; an empty line is a plain marker;
  - with `--midi`, the device's MIDI traffic through `smcmidi listen`.

- `cmd` sends one command of the command link and prints the reply ([link.md](link.md)).
- `metrics` sums up the metrics heartbeats of a session ("Metrics" below).
- `boot` reads the boot journals of a session: the reset cause, the previous run's crash record
  and latest records ("Boot journal" below).
- `clock` fits the device's clock to the host's from a session's PINGs ("The device's clock on
  the host" below).
- `firstrun.py`, next to it, runs the first session on the device in one go
  ([hwtest.md](hwtest.md), "The first run").

File-only commands do not require POSIX serial modules: `termios`/`tty` are imported only when
opening a POSIX terminal, not while decoding a log or selecting a MIDI backend. Blocked-import
host tests cover log decoding and WinMM routing; native Windows device operation is not verified.

Host timestamps are `time.monotonic_ns()`, the same clock as `smcmidi`.

Not written yet as general log-analysis commands (LOG-20): `view`, `analyze`, `check`, `diff`,
`report`, and rewriting device records with fitted host times. `metrics --check`, `clock` and
`boot` provide the narrower analyses described here. Core scenarios run in the simulator and
on the device through `smcsim device` (LOG-35, ✅ 2026-11-11;
[core.md](core.md), "Scenarios on the device"), using the session format below.

### The device's clock on the host

`capture` (every second) and `firstrun.py` send PINGs that carry the host's send time. Each reply
gives three times: the host's send time, the device's clock, and the host's receive time. The
device read its clock somewhere between the two host times; the middle is the estimate, best when
the round trip is short.

```sh
python3 -I tools/smclog/smclog.py clock logs/sessions/2026-11-09-first-run
python3 -I tools/smclog/smclog.py clock --json SESSION
```

`clock_fit()` does the following:

- It splits samples into runs when the decoded device clock goes back, or advances by less
  than roughly half the elapsed host time (with the code's 1 ms tolerance). The latter catches
  a reset whose new uptime already exceeds the preceding run's. Raw-u32 wrap is handled by
  the stream decoder, not treated as a reset by this fit.
- Of each second it keeps the sample with the shortest round trip, so a burst of PINGs counts
  once and slow round trips do not pull the line.
- It fits a line by least squares: device µs against host time.

It reports, per run:

- the drift in ppm and its standard error;
- the round trips (min, median, max);
- how far the used samples lie from the line.

`device_to_host_ns()` maps a device time to the host's monotonic clock, the clock of `smcmidi` and
the markers. The drift checks the device's µs clock from outside: the heartbeat's drift only
compares it with the SDK's millisecond clock. A crystal is good to tens of ppm; a wrong timer rate
shows as thousands.

### Session directory (LOG-21, format 1)

| File | What |
|---|---|
| `meta.json` | format, tool, creation time, host, port, build ID, dictionary, scenario, `t_host_start_ns` |
| `device.bin` | the raw stream as received |
| `device.jsonl` | decoded records, each with `t_host_ns` (when its bytes arrived) |
| `markers.jsonl` | `{"t_ns", "text"}` per marker |
| `midi.jsonl` | `smcmidi listen` output (with `--midi`) |

Record kinds in `device.jsonl` and `decode` output:

| Kind | Fields |
|---|---|
| `log` | `seq`, `t_us`, `id`, `cause`, `level`, `module`, `msg`, `args`, `file`, `line` |
| `lost` | `seq`, `t_us`, `count` |
| `header` | `version`, `timer_hz`, `t_us`, `lost_total`, `ring_size`, `build_id` |
| `reply` | `seq`, `cmd`, `status`, `payload` (hex), and the command's fields ([link.md](link.md)) |
| `gap` | `seq`, `missing` |
| `warning` | `msg` |
| `bad_frame`, `partial_frame`, `unknown` | raw bytes for inspection |

A `log` record also has `truncated` when it was LOG_TRUNC, `past` when it was replayed from the
previous run (PAST), and `error` when its ID is not in the dictionary. `--text` prints a past
record's time as `prev` and its seconds in the previous run.

## Metrics (LOG-06)

A **heartbeat** every 10 seconds, and on METRICS_NOW ([link.md](link.md)), writes the metrics of
its **window** (the time since the previous heartbeat) as records of the module `metrics` at INFO.
Each part of the firmware keeps its own window and hands it over when the heartbeat takes it
(`firmware/metrics/metrics.h`; the BR25 part in `firmware/app/metrics_app.c`). In the controller
the control task writes it after a round (the MIDI queue is its own); in the hardware test the
timer task does. The statistics since start (scan, control path, USB, link) are logged next to it
as before.

| Record | What |
|---|---|
| `heartbeat N: W ms; CPU of our code, per mille: scan S, control C, USB U` | the window's length; the share of time our scan interrupt, our control rounds and our USB task ran |
| `scan steps: …, … skipped; handler avg …, max … ns; late max … us` | timer steps (50 µs), steps skipped while SPI was busy, the step handler's run time, how late it started after the timer period |
| `scan passes: N; every min …, avg …, max … us` | the time between full matrix passes (16 steps, 0.8 ms nominal) |
| `CPU load: L per mille (B of N samples not idle)` | the whole CPU's load: at each scan step, whether the task it interrupted was other than the RTOS's idle task (interrupts that run while the idle task is current count as idle) |
| `fader ADC: N samples, each fader every E us; D steps the SDK had it, L lost to it, T timeouts; longest conversion S steps` | our fader conversions ([hwtest.md](hwtest.md), "Fader ADC"): how often each fader was read on average, the steps a conversion was due while the SDK's was running, ours dropped when the SDK took the converter, conversions given up after 2 ms, the longest in scan steps |
| `clock: N resyncs; drift from the reference A..B us` | the µs clock (below): jumps forward, and the clock minus the SDK's millisecond clock at the syncs |
| `latency input to core: …` and `latency input to USB: …` | events; min, avg, max; p50 and p99 as bucket bounds (at most the max) |
| `latency …, events by bucket: <0.25 ms …, <0.5, <1, <2, <3, <5, <10, more` | the histogram (only when there were events) |
| `traffic: N events; MIDI … out, … in, … dropped; queues max: input …, MIDI out …; refused …, …` | input events into the core, USB MIDI packets (dropped: no host), the most waiting in each queue, refusals |
| `latency input to BLE: …` and `…, events by bucket: <5 ms …, <10, <15, <20, <25, <35, <50, more` | `BLE=1` only: input event → its message in a notification the BLE stack took ([ble.md](ble.md), "The route"); wide buckets for SPEC NFR-02 (median ≤ 25 ms) |
| `BLE: N messages in M notifications, … merged, … dropped; queue max …, refused …; … in, … faults` | `BLE=1` only: messages sent and notifications, older values the merge replaced, dropped with no connection, the BLE queue's most waiting and refusals, messages received and the decoder's faults |
| `log: N records, … lost; ring max … of … bytes` | in the window; the ring's high-water mark since start |
| `heap: … bytes free, … lowest` | the SDK's heap now and its lowest since start |
| `stack NAME: … of … bytes free below the deepest point sampled` | per task of ours (`smc_usb`, `smc_ctl`, with `BLE=1` `smc_ble`); or "not measured" when its bounds were not found |

- **Latency.** "Input" is the event's time stamp: the scan interrupt's step where the debounced
  state changed (so the debounce time is before it), the fader sampling, the USB task for MIDI in,
  the command's arrival for injected events (which are counted too). "To core": when the control
  task takes it from the queue. "To USB": when the USB task hands its MIDI packet to TinyUSB (into
  its FIFO; the host's polling comes after, up to a frame on full speed). The packet carries the
  event's time through the MIDI queue; output with no input event (a tick's) is not counted.
  SPEC NFR-01 (≤ 3 ms typical, ≤ 5 ms p99) is about the whole path, so the device's side has to
  stay well inside it.
- **CPU.** Our own code is timed: the scan handler's run time (from the timer's counter;
  entry and exit not included), `ctl_run()` and the USB task's work per wake-up (on the log clock).
  Total CPU load is estimated separately by sampling whether the interrupted task is idle
  (table above); it is not a complete time accounting of SDK tasks or interrupt work.
- **Stacks** are sampled, never written. The SDK's RTOS (from the image's disassembly:
  `port_overwrite.c` in `system.a`) allocates task stacks through the MMU (`pvPortVMallocStack`)
  and at every task switch (`vCreateTaskStack`) moves the pages below the outgoing task's stack
  pointer to the incoming task; it checks a `0x5A5A5A5A` canary at each stack's bottom and resets
  the chip if it is gone. A painted stack would not keep its paint, and painting the bottom would
  reset the chip. Instead every log call (the BR25 port's lock) and every scan interrupt takes the
  address of a local; it counts for the watched stack that holds it (if interrupts run on their
  own stack, the scan's samples count for none). The deepest sample is a lower bound of the use:
  a path that never logs can go deeper. The bounds: `uxTaskStack()` (the TCB's stack base) and
  the size in `app/include/app_tasks.h`; the RTOS's own size (`uxTaskStackDepth()`) is logged if
  it differs. The SDK resets the chip on an overflow it sees at a task switch.
- **Not yet:** battery voltage and temperature (later, SPEC LOG-06), physical free-page counts
  and the SDK Bluetooth tasks' deepest stack use. BLE traffic/latency and the sampled total-load
  estimate are implemented and recorded on the mixer ([ble.md](ble.md), "The route on the mixer").
- ✅ The scan/ADC/latency metrics and sampled application stacks ran on the mixer on
  2026-11-11. The SDK's reported heap-free value is virtual address space, not free physical RAM
  ([ble.md](ble.md), "The two heap numbers"); plausible heartbeat values do not settle that budget.

### The microsecond clock

The log's time stamps on BR25 (and every event's) come from the scan timer, TIMER3
(`firmware/metrics/usclock.h`, `platform/br25/input_br25.c`): its interrupt counts the 50 µs
periods; a read takes the counter, the pending flag and the counter again with interrupts off, so
a period that ended before its interrupt ran is counted, and the time never goes back. The timer
starts first thing in `app_main()` (`input_br25_clock_start()`), before the boot journal; the scan
work in its interrupt starts later. Before that, the SDK's millisecond clock (`jiffies_msec()`) is
used, and the timer clock takes over from it. Every 16 periods (a matrix pass once the scan runs)
the clock is compared with `jiffies_msec()`, which never runs ahead of real time: behind by more
than 2 ms, it jumps forward and counts a resync. Periods lost while the interrupt was held off
are counted in the interrupt itself, from the CPU's tick timer ([hwtest.md](hwtest.md), "Lost
periods"), so the resync is a backstop: none in 60 s on the mixer (run 26). A clock that runs ahead
(a wrong timer rate) is not corrected; the drift in the heartbeat shows it.

### Summary on the host

```sh
python3 -I tools/smclog/smclog.py metrics logs/sessions/2026-11-07-faders
python3 -I tools/smclog/smclog.py metrics --check --json SESSION
```

It reads `device.jsonl` (or any file of decoded records), adds up the latency histograms of all
heartbeats (p50 and p99 from the summed buckets), takes the worst of the rest (CPU share, scan
timing, queues, heap, the least stack headroom), the CPU load's mean and maximum and the fader
ADC's sums (with the slowest "each fader every"), and checks: input → USB p50 ≤ 3 ms and p99 ≤
5 ms (NFR-01 on the device's side), at least 256 bytes of stack headroom per task, and no fader
conversion given up. `--check` exits
with 1 when a check fails.

For a stability capture, exclude buffered history and intervals that began before subscription:

```bash
python3 -I tools/smclog/smclog.py metrics --capture-window --check --json SESSION
```

`--capture-window` is optional; the default still summarizes every heartbeat that was read.
The filter requires one stream header with host timestamps and a fresh clock-sync PING (keep
capture's default `--sync` enabled). It aligns the device epoch with that PING and its echoed host
time, then keeps only time windows whose device start and host arrival bounds are wholly after
capture began. Both bounds matter: after a u32 wrap, old buffered device timestamps can appear
newer than the header. Six host tests cover buffered/partial windows, wraps, delayed first sync,
stale replies, missing metadata, and the CLI/default behavior. A requested `--check` fails if
no complete time window is available; it does not turn missing data into a pass.

The JSON's `capture_window` states how many windows were included/excluded and the initial
cumulative loss count. The raw records are never removed. Firmware lifetime watermarks (such as
heap low-water and sampled stack/ring peaks) can still predate the selected window. No events
means no latency measurement: a passing idle summary is not an input/MIDI performance test.

## Boot journal (LOG-11) and the crash record (LOG-10 in RAM)

Every start writes a **boot journal** (module `boot`, INFO): what the reset was, what the previous
run left behind, and how long each part of the start took. The portable part is
`firmware/diag/` (host tests in `firmware/tests/test_diag.c`); the BR25 part is
`firmware/platform/br25/diag_br25.c`. Nothing of it writes flash (C-6): what survives a reset
stays in RAM that the reset does not clear. The flash half of LOG-10 (a WARN/ERROR ring that
survives power-off, read and cleared by command) is not implemented. C-6 is no longer a blanket
"nothing written yet" status: configuration, panel-state and boot-selection writes have run,
but they did not add that log store or grant permission for a future device session.

### What a reset keeps

`struct diag_keep` (`diag.h`, 1312 bytes on BR25) has a section of its own, `.smc_keep` (NOLOAD).
`firmware/app/sdk_ld_patch.py` adds it to the SDK's linker script after the RAM code
(`data_code_limit_end`, where the SDK's CPU write and PC limits end), before the heap. The startup
code clears only `.bss`. `build_app.sh` checks the order: `.bss` ≤ `data_code_limit_end` ≤ the
block < `_HEAP_BEGIN`.

| Part | What | Valid when |
|---|---|---|
| header | boots since the RAM was last lost; the previous run's last heartbeat (its number and ms since start) | magic and an FNV-1a check |
| crash record | `struct diag_crash`, written by the exception handler just before it resets the chip | its own magic and check |
| recent records | the flight recorder (below) | magic; every entry's length and kind; the build tag |

The checks make RAM that power-on left random, or that something before us overwrote, read as
"nothing kept", never as garbage. The metrics heartbeat (every 10 s) updates the header, so the
next boot knows roughly when this run ended.

### The flight recorder

`smclog_recent_attach()` (`smclog.h`) gives the log a second ring, `struct smclog_recent`
(`SMCLOG_RECENT_SIZE`, 1 KB by default).

- Every record goes there too, including those the main ring had no room for.
- It overwrites its oldest entries, where the main ring drops the newest. So it always holds the
  latest few dozen records.
- Its position (head and tail, 16-bit counts) is one word. A write moves the tail, writes the
  entry, then moves the head, so a reset in the middle loses at most that entry.
- Its tag is the first four bytes of the build ID, read as a little-endian word (a build ID
  starting `aefc2ff8` gives the tag `f82ffcae`).

`diag_boot_begin()` attaches it before anything else is logged. If the previous run's ring is valid
and from the same build, its records go into the main ring first, as PAST records ("Records"
above):

- they keep their old `seq` and `t_us`;
- they take no new `seq`;
- they are not put into the recorder again.

Then the recorder starts empty for this run. Another build's records are not replayed, since the
dictionary would not match; the journal says so.

### The journal's records

An example (made-up values) of a start after a crash:

```
prev 409.870 ...                       the previous run's latest records, replayed
boot 3 since the RAM was lost: reset cause 0x40 (soft reset); firmware 0.1.0-dev, build tag f82ffcae
previous run: last heartbeat 40, 400012 ms after it started
crash in boot 2 at about 409876 ms: cause 1, task smc_ctl, frame at 0x0001f5a0; build tag f82ffcae
crash: RETI ... SSP          crash: EMU_MSG ... trace          crash: R0-R7 ...   crash: R8-R15 ...
crash: stack at 0x0001f7c0: 7 words  (twice)
previous run: its latest 52 log records replayed before this journal
init log: 812 us (done at 1630 us)
init core: ...  init scan: ...  init control task: ...  init usb: ...
boot done: 9120 us after the journal started (at 9938 us), 5 parts timed
```

- **Reset cause.** On BR25 the raw value is P3_RST_SRC (P33 register `0x12`) in bits 0–7 and
  P3_PMU_CON1 (`0x31`) in bits 8–15. The names:

  | Bit | Name | Source |
  |---|---|---|
  | 0 | power-on | VDDIO POR |
  | 1 | low voltage | VDDIO LVD |
  | 2 | watchdog | |
  | 3 | VCM | |
  | 4 | pin reset | PPINR |
  | 5 | system reset | |
  | 6 | soft reset | |
  | 15 | power return | P3_PMU_CON1 bit 7: a wake from soft power-off |

  The SDK reads these registers first, in `main()`. ✅ The soft/fault/watchdog test sequences
  named the causes correctly and a soft reset after the watchdog no longer carried its bit
  (2026-11-11). That does not establish read-clear/accumulation behavior for every source;
  both the raw value and the names remain logged.
- **Previous run.** One of: "nothing kept" (power-on, or the RAM was lost), "ended before its
  first heartbeat", or "last heartbeat N, T ms after it started".
- **Crash record**, if there is one. An ERROR record (cause, task, frame address, the record's
  build tag), then:
  - the frame's special registers;
  - the debug unit's messages and the trace;
  - r0–r15;
  - 14 words of the task stack.

  It is cleared once taken, so it is reported once. Its build tag says which ELF names its
  addresses.
- **Recent records.** None, from another build, or how many were replayed (they come before the
  journal in the stream).
- **Init times.** `init WHAT: N us (done at T us)` per `diag_boot_mark()`: the time since the
  previous mark, or since the journal started.
  - The parts on BR25: `log`, then the variant's (`core`, `scan`, `control task` in the
    controller; `scan` in the hardware test), then `usb`.
  - They are measured on the µs log clock: `app_main()` starts the scan timer's clock
    (`input_br25_clock_start()`) before the journal; the scan itself starts later.
  - There are at most 12 parts; later ones overwrite the last.
- **Boot done.** `boot done: …` ends the journal.

The link's RESET ([link.md](link.md)) makes the resets to check all this with: a soft reset, a
fault test (a deliberate exception: the crash record's path) and a watchdog test.
`firstrun.py` runs them and checks each next boot's journal ([hwtest.md](hwtest.md), "The first
run").

The journal is kept (`struct diag_boot`) and written again on BOOT_LOG (`0x26`,
[link.md](link.md)), after a `boot journal, again on request:` record. That is for a capture that
started late, or whose boot records were lost.

### The exception handler (BR25)

Ours replaces the SDK's on IRQ 1 (exceptions), at priority 2 as the SDK's. The SDK's handler in
this build only resets the chip (its `exception_analyze()` is reduced to `chip_reset()`), so
nothing is lost.

IRQ 0 (emulator exceptions: the CPU's checks that the SDK's `debug_init()` enables, write and PC
limits, stack limits, misaligned access) stays the SDK's. Its `breakpoint_init()` puts
`breakpoint_isr` there and sets three hardware breakpoints on the ROM's `local_irq_disable`,
`local_irq_enable` and `cpu_irq_disabled`; `breakpoint_isr` sends each hit to a RAM replacement.
✅ Taking IRQ 0 over turned every one of those calls into a crash (on the device, 2026-11-11:
[bringup.md](bringup.md), "Step 4", runs 5 to 7). So an emulator exception leaves no crash record
for now: ⚠️ under `breakpoint_isr` it matches no breakpoint and returns to the same instruction,
again and again, until the watchdog resets the chip (from the disassembly). The next boot names
the watchdog. A handler of ours on IRQ 0 that passes the three breakpoints on, and takes anything else as a
crash, is in `diag_br25.c` behind `EMU_CHAIN=1` (`tools/env/build_app.sh EMU_CHAIN=1`). On the
device it stopped the start inside `usb_app_start()` until the watchdog
([bringup.md](bringup.md), "Step 4", run 9), so it is off in every other build.

✅ A misaligned word load does not go to IRQ 0: on the mixer (2026-11-11, `firstrun.py --resets
fault`, image 30) it came to our handler on IRQ 1 with `EMU_MSG` 0x1, and the crash record was
there at the next boot. It trapped late: `RETI` and the trace's newest entry are the instruction
after the call of `diag_br25_fault()`, the trace's next entry is that function, so the load's
exception is taken a few instructions on, after the function returned. A crash record's `RETI`
can therefore point just past the faulting code; the trace shows where it was. ⚠️ The record's task
name was empty (the fault came in the USB task); not looked into yet.

Ours runs from RAM and:

1. pushes the frame as the SDK's does: r0–r15, then RETI, RETE, RETX, RETS, PSR, ICFG, USP, SSP;
2. stops the CPU's trace (ETM_CON bit 0), so that ETM_PC0–3 keep the program counters from
   before the exception;
3. writes the crash record:
   - the time (`jiffies_msec()`), the boot number and the cause (the interrupt);
   - the frame and its address;
   - EMU_MSG, and JL_DEBUG's MSG, PRP_MMU_MSG and PRP_WR_LIMIT_MSG;
   - the trace;
   - 14 words from the task stack pointer (if it points into RAM);
   - the current task's name;
   - the check, written last;
4. resets the chip (`chip_reset()`, in ROM).

It calls only RAM and ROM code, since flash may be busy. A fault inside it resets at once. A
watchdog reset leaves no crash record, only the last heartbeat and the recent records.

**What the device runs settled (2026-11-11, image 30, `firstrun.py`):** ✅ kept RAM and replay
survive soft, fault and watchdog resets; soft/fault report `0x440`, watchdog `0x404`, and a later
soft reset no longer carries the watchdog bit. ✅ The deliberate fault's frame and trace agree
with the handler layout, with the late trap described above ([hwtest.md](hwtest.md), "On the
device: the first run"). These results do not prove every reset source or fault type, retention
through ROM-loader entry or power loss, IRQ-0 exception capture, or a complete task-name record.

### On the host

```sh
python3 -I tools/smclog/smclog.py cmd --port /dev/cu.usbmodemXXXX boot
python3 -I tools/smclog/smclog.py boot logs/sessions/2026-11-09-crash --binary build/app/app.elf
python3 -I tools/smclog/smclog.py boot --json SESSION
```

`boot` reads a session (`device.jsonl`) or a file of decoded records. For each journal it prints:

- the reset bits by name;
- the previous run;
- the crash record;
- the replayed records;
- the parts' times.

With `--binary` (the ELF of the crash record's build) it names the code addresses in the
registers, the trace and the stack (`ctl_run+0x1a`) from the ELF's symbol table. `--json` gives the
same as data.

### Without hardware

`devhost --keep FILE` ([core.md](core.md)) keeps `struct diag_keep` in a file, mapped shared: it
survives the process being killed, as the RAM survives a reset. SIGUSR1 stands in for a crash: a
crash record with made-up registers (cause 9), then exit status 70. The next run reports both.
RESET restarts it in place: it executes itself again on the same terminal and keep file, with
`--reset-cause` set to the bits the device would read (`0x40` soft reset, `0x04` watchdog); the
fault test writes the made-up crash record first.

The tests:

| Test | What it covers |
|---|---|
| `test_diag.c` | the check's vectors; a first boot; a reset after a crash, with replay; damaged blocks; marks and the journal again |
| `test_smclog.c` | the recorder: replay, keeping the latest, a full main ring, damage, wrap-around; `--emit-past` writes a stream with PAST records |
| `test_link.c` | BOOT_LOG, RESET |
| `test_smclog.py` | decoding PAST records; naming addresses; the image's functions (if built); two `devhost` runs on one keep file with a SIGUSR1 crash between them: the second run's journal, its crash record and replayed records, `boot` as text and JSON; the clock fit on made-up PINGs (a known drift, a burst, runs, `clock`) |
| `test_firstrun.py` | the first run against `devhost`: all three resets and each next boot's journal ([link.md](link.md), "Tests") |

## Ports

| Port | Time | Lock | Build ID |
|---|---|---|---|
| `firmware/port/host` (tests, simulator) | `CLOCK_MONOTONIC`, or a test clock (`smclog_host_set_clock`); the simulator uses the events' virtual time | pthread mutex | macOS `LC_UUID`; Linux GNU build-ID note |
| `firmware/platform/br25` (application, SDK) | µs from the scan timer once its clock starts, first thing in `app_main()` ("The microsecond clock" above); `jiffies_msec()` × 1000 before | `cli` with the previous state restored (`irq_br25.h`), which also holds off the scan interrupt at priority 7 (as the SDK's interrupt disable does: [hwtest.md](hwtest.md), "Lock"); with a stack sample, "Metrics" | `__build_id_note` |
| `firmware/boot` (bootloader, no SDK) | 0: no timer driver yet (stage 2) | none: the bootloader is polled and enables no interrupts | `__build_id_note` |

In the application the command link drains the ring over USB CDC once the host subscribes
([link.md](link.md)). Commands/replies and panel/SENT events also use SysEx over USB MIDI and
BLE MIDI, but the log stream does not: LOG_SUBSCRIBE over SysEx remains unavailable (LOG-12).
The bootloader has no log transport yet and has not run on the mixer.

## Tests

```sh
make -C firmware test        # C tests of the ring and encoding, then the decoder tests (and the core's)
tools/env/check_device.sh    # JieLi toolchain: section not loaded, IDs under LTO/-Oz, note in flash
```

The C tests:

- cover levels, IDs, every argument type, overflow and LOST, drain limits, wrap-around, causes and
  the header;
- check frames with their own COBS/CRC parser;
- emit a fixed stream (`test_smclog --emit FILE`).

The Python tests decode that stream against the test binary's own dictionary. They also check that
a tampered build ID is rejected, and run `capture` through a pseudo-terminal with markers.
`build_app.sh` and `build_boot.sh` check that every ID the code passes is in the image's dictionary.
The command link's tests ([link.md](link.md)) run the tool's `cmd` and `capture` against the
firmware's link code over a pseudo-terminal.

The metrics: `firmware/tests/test_metrics.c` (the histogram's buckets and percentiles, durations,
stack samples, the heartbeat's window and the records it writes, the µs clock's reads around the
pending flag, its never going back, its resyncs and drift), `test_ctl.c` (each event's wait, the
packets' event times, the round's run time, the window) and `test_midi.c` (the packets' times).
`test_smclog.py` checks the summary on made-up heartbeats and reads a real one from `devhost`
through the link after injected events (METRICS_NOW).

The boot journal and the flight recorder: "Boot journal" above, "Without hardware".
