# Hardware-test firmware

The first application of ours meant to run on the mixer (SPEC stage 2). It drives every input
and LED, logs the raw inputs (LOG-07) and takes injected input (LOG-33). It answers what static
analysis of the stock firmware cannot: the pin map, matrix and LED placement, encoder wiring,
fader ADC channels, scan timing, and how well the USB driver works ([usb.md](usb.md)).

**Status:** first built on 2026-11-08; ✅ the plain hardware-test image, scan, CDC link,
manual panel checks and reset diagnostics ran on the mixer on 2026-11-11. The dated runs below
record initial failures and corrections, not a claim that the current build has repeated them.
The implementation includes:

- the scan layer, the input event queue and input injection, in `firmware/input/`;
- host tests: `firmware/tests/test_input.c`, run by `make -C firmware test`;
- a build with the JieLi toolchain, wired to the core the way the firmware will wire them
  (`tools/env/check_device.sh`);
- the BR25 platform part, `firmware/platform/br25/input_br25.c`: the scan timer, SPI2 to the
  595s, the latch, the rows, the LED columns, the fader ADC;
- the image: the application's `hwtest` variant (`firmware/app/hwtest/hwtest.c`), built by
  `VARIANT=hwtest tools/env/build_app.sh`;
- the command link over USB CDC ([link.md](link.md)): the log stream, LOG_RAW_INPUT,
  METRICS_NOW, INPUT_INJECT, with the host tool's `cmd` and `capture`.

This document records the hardware test and the measurements made with it; installation and
updates are in [image.md](image.md). The product firmware (0.1.0) now scans with a 25 µs step
(a 0.4 ms pass) and SPI2 at 4 MHz ("Encoders" below).

## Structure

```
 scan interrupt (timer)              consumer                        USB (CDC / SysEx)
 ─────────────────────               ─────────                       ─────────────────
 rows = PB0-PB3                      hwtest: log events              INPUT_INJECT → inject_command()
 word = scan_step(...)    ┐          controller: core_event()        LOG_RAW_INPUT → scan.raw_mode
 595s ← word, latch       │            → MIDI, LED frame             smclog drain → host
 scan_pass(...)         ──┼→ evq ──→
 ADC → scan_fader(...)  ──┘     ↑
                                └── inject (task)
```

- **`input/scan.c`** (portable, interrupt context): the input matrix, button debounce, encoder
  decoding, fader filtering, the LED frame. It produces core events (`core/core.h`) into a sink.
- **`input/evq.c`**: the event queue between the scan and injection (producers) and the main
  loop (the consumer). It holds 64 events. A full queue refuses an event and counts it; the scan
  keeps a refused button change or encoder click and tries again on the next pass, so nothing is
  dropped silently (LOG-34). The platform provides the lock (`input_port_lock`: mask the scan and
  USB interrupts).
- **`input/inject.c`**: the INPUT_INJECT command (LOG-33, LOG-34).
- **The consumer** in the hardware-test variant logs events (injected ones marked
  "(injected)") and runs the LED walk; it does not run the core or send performance MIDI.
  The controller variant instead feeds the same event queue into the core (LOG-33).
- **Platform** (`platform/br25/input_br25.c`, below): the scan timer and SPI to the 595s, row and
  LED-column pins, the ADC.
- **USB** (`app/usb/`, [link.md](link.md)): the command link carries the commands in and the log
  out.

## Hardware layout it assumes

From the stock firmware ([hardware.md](hardware.md), static analysis), checked by the manual
procedure on 2026-11-11 (sessions `2026-11-11-manual-1..4`, images 26 and 27): the last column says
what the run found.

| What | Assumed | How the hardware test checks it |
|---|---|---|
| 595 word | upper 16 bits: one column selected (bit low), column *c* = bit 24 + *c* for *c* < 8, bit 16 + *c* − 8 for the rest; lower 16: LED rows, active low | ✅ buttons arrive with the right index; LEDs light at the right place |
| 595 chain | SPI2 port A: clock PB6, data PB7; latch PB4 (rising edge); PB5 low enables the outputs (⚠️ /OE) | anything works at all |
| Column numbering | the stock pipeline: column *c* is read while the word for *c* − 1 is latched | ✅ buttons arrive with the right index |
| Input rows | PB0–PB3, pull-ups, closed reads low | ✅ raw mode: a press changes one bit |
| Buttons | the stock table (`scan_button_pos`), column << 4 \| row, rows 1–3 | ✅ all 45 in order, no bounce |
| Encoders | row 0; encoder *n* on columns 2*n*−1 and 2*n*; encoder 8 on 15 and 0 | ✅ each on its columns. The line on column 2*n* leads when turned clockwise: it is A, so clockwise counts up (image 27). Encoder 8's pair is read across the pass boundary: column 15 of the pass before with column 0 (`raw_last_before`), a step apart, not 15 steps |
| Encoder detents | 2 transitions per click (the stock divider) | ✅ no detents (smooth knobs); 47–49 steps a revolution with the divider of 2, as the stock firmware's DAW mode |
| LED columns | PA2, PA5, PC1, PC3, one at a time, low drives a column, a blank phase between | the LED walk (below) |
| LEDs | the stock table (`scan_led_pos`), row << 4 \| column; its order taken as the core's LED indexes (0–44 the buttons, 45–52 the encoders) | ✅ the buttons' 45 in order; the stock table lists the encoders right to left, so their 8 entries are reversed (image 27: left to right). The 64 raw matrix places light nothing the 53 do not |
| LED polarity | a cleared row bit lights the LED in the driven column | ✅ the LED walk |
| Faders | ADC channels **unknown** (R-3); the candidates are the 8 ADC pins nothing else uses: PA0, PA1, PA3, PA4, PA6, PC2, PC4, PC5 | ✅ each candidate is one fader, in reverse: faders 1–8 (left to right) on PC5, PC4, PC2, PA6, PA4, PA3, PA1, PA0 (`fader_ch`). Bottom reads 0, top 1023, every fader the whole range. At rest mid-travel the raw value wanders ±2..±6 counts (fader 4 the most) |

The two tables are in `input/scan.c`, the fader channels in `platform/br25/input_br25.c`. The
button table matches hardware.md entry for entry. The LED table's original inferred order was
checked and corrected by the LED walk on 2026-11-11 (encoder entries reversed, image 27).

## The scan layer

Per scan step, from the platform's timer interrupt, in the stock firmware's order:

1. read PB0–PB3: the outputs latched at the previous step;
2. latch (PB4 rising edge): the word shifted at the previous step reaches the outputs;
3. `scan_step()` stores the rows and returns the next word;
4. start shifting the word to the 595s;
5. drive the LED column `led_drive` (none while blank).

A word is shifted at one step, latched at the next and read at the one after, so the rows stored as
column *c* were read while the word for column *c* − 1 was latched. The stock firmware does the
same, and its tables (buttons, encoder columns) are in that numbering; the host tests simulate this
pipeline.

After column 15 the pass is evaluated. A full pass is 16 steps; at `SCAN_STEP_US` = 25 µs a step
(50 µs until 2026-11-23), that is one pass every 0.4 ms. The button debounce, the LED column time,
the encoders' inference window and the ADC timeout are defined in time and converted to steps
(`SCAN_PASSES_FOR_US`, `SCAN_STEPS_FOR_US`), so a different step keeps their behaviour; the host
tests run at both steps. The stock firmware runs the steps back to back, as fast as the SPI
shifts (about 8–16 µs a word, ⚠️ its SPI clock is not known); ours are paced by a timer, so the
timing does not depend on the SPI clock and the scan's share of CPU time is bounded.

- **Buttons**: a new state must last `debounce` passes (default 6, i.e. 4.8–5.6 ms; stock: 3 scans
  of 20 ms). A change that reverts sooner is counted in `bounces`. Events are press (1) and release
  (0). Long presses are the core's business, from time.
- **Encoders**: Gray-code decoding at every pass, `enc_transitions` per click (default 2).
  - A jump (both lines changed between two passes) is a state shorter than a pass. While the
    encoder is turning (a transition within `SCAN_ENC_INFER_PASSES`, 8 passes) it counts two
    transitions the way the recent ones went: their sum, held within ±3, must not be 0. It is
    counted in `enc_inferred`. Otherwise, at rest or when the sum is 0, it is counted in
    `enc_invalid`: a lost step, the NFR-03 metric ("What the run of 2026-11-11 found").
  - Clicks are summed per pass into one event. A click the queue refused rides along with the
    next event.
  - The first full pass only records where each encoder stands.
- **Faders** ("The faders measured", below, for why):
  1. a median of the last 3 ADC samples (a single wrong conversion never gets through);
  2. a gate: a sample more than 6 counts from the filter's value is held back until 6 in a row lie
     beyond it on the same side (`SCAN_FADER_GATE_*`): bursts of wrong conversions are dropped, a
     real move waits at most 6 samples (10 ms) at its start;
  3. scaled to 14 bits and filtered, `y += (x − y) / 2^fader_shift` (default 3: about 13 ms at a
     sample every 1.62 ms);
  4. a directional hysteresis, `fader_hysteresis` (default 80 of 16383, 5 counts): while the fader
     keeps its way every count is sent; from rest (no event for `SCAN_FADER_REST_SAMPLES`, 200
     samples, about 0.3 s) or to turn back it must move the hysteresis first.
  The ends 0 and 16383 are always reached (within the hysteresis of them), and leaving an end
  takes twice the hysteresis (10 counts). Without that, a fader parked at its top end sent about
  100 events a second on the mixer: its noise (raw 1017..1023, ✅ run 19) dips the filtered value
  under the snap and back.
- **LEDs**: the core's LED states go into a 4 × 16 frame (`scan_led`). Each LED column is shown
  for `led_steps` steps (default 20 at 25 µs a step: 0.5 ms, so the 4 columns refresh at 500 Hz;
  2 ms and 125 Hz before 2026-11-25), of which the first `led_blank` (default 2: 50 µs) are blank:
  no column pin driven, rows off. The word carries the rows of the column being shown, inverted
  (active low). A column's rows reach the outputs one step after its pin is driven; that step
  still shows the blank rows, so a column never meets another column's rows (`test_leds` follows
  the pipeline through two cycles). Blinking states (slow, fast) will toggle frame bits on a clock
  of their own.
- **The LED light** (SPEC §5.7, `scan_led_light()`), redone on 2026-11-25 after the owner saw
  level 1 flicker on the mixer: with whole steps only, level 1 was one 25 µs step in 8 ms, its
  refresh visible, and a step that came late (up to 200 µs, "Lost periods" below) lit it several
  times longer for once, a flash about every second. Now:
  - each channel (the lit LEDs; the unlit buttons' glow) owes a column's time a light in
    quarter-µs, the share of the full time the square of the level, level 1 kept at 1/71 (the
    owner's dimmest, as checked): 1, 3, 6, 11, 18, 26, 35, 45, 58 and 71 of 71 parts of the 17
    steps a column shows (`light_share`, `scan_light_q()`);
  - it shows in whole words first, then the rest in pulse words of at most 10 µs, one channel's
    rows each, after both channels' whole words; the column's last word, latched at the next
    column's first (blank) step, can be one, with the column driven again for the pulse only;
  - the platform times a pulse inside the interrupt (`led_pulse`, `input_br25.c`): it releases the
    column after that many quarter-µs, so a late step cannot lengthen it;
  - a whole word a late step lengthened is measured (the step times), and what a column's time
    showed beyond what it owed is paid back at its next times (`light_debt`, at most a column's
    time); when the pulses do not fit, a remainder takes one more whole word, paid back the same
    way;
  - level 10 is every word, as without the light; `light_plan()` runs from the scan interrupt
    (in RAM, `rampath`), once a column's time.

  ✅ The mixer, 2026-11-25 (test build `8eeb3ce7`): level 1 shows and does not flicker, only
  barely at the edge of the eye ("quite acceptable"); level 5 clearly brighter, no more flicker;
  the glow at 2 shows on the buttons, not on the encoders, hardly flickers, and a lit M stands
  out. Host tests: `test_led_light` (each level exact on average, within 3 % with every 7th step
  60 µs late; both channels dim at once within 1 %), at 25 and 50 µs a step.
  Two diagnostic build switches (`firmware/app/Makefile`): `LED_DARK=1` never drives a column,
  `LED_DARK=2` also takes PB5 (⚠️ the 595s' /OE) high, which stops the buttons. The bottom row's,
  BT's and Shift's LEDs glow faintly with either (controllers 16 and 17, 2026-11-11):
  [hardware.md](hardware.md), "The faint glow of 13 LEDs".
- **Statistics** (`struct scan_stats`): steps, passes, events by type, bounces, encoder jumps
  lost and inferred, refusals. The statistics records log them; the metrics heartbeat adds the scan's timing
  (LOG-06, [logging.md](logging.md), "Metrics").

All defaults are in `scan_default_params`; they are tuned with raw input mode on the device.

## Raw input mode (LOG-07)

`scan.raw_mode` holds two bits, the same as LOG_RAW_INPUT's: `SCAN_RAW_MATRIX` (1) and
`SCAN_RAW_FADERS` (2). With a bit set, the scan logs that input's changes at INFO, module `input`,
cause 0. The hardware-test build starts with the matrix only. The faders are asked for when
needed: `smclog.py cmd raw off|matrix|faders|on` (`on` is both). The controller starts with
neither. The records:

- `raw: column C rows R` (matrix): each change of a column's raw row bits. These include the
  encoder lines (row 0);
- `raw: fader F adc A` (faders): each change of a fader's raw ADC value. A resting fader's ADC
  noise alone makes a steady stream of these, and on the device each costs the scan interrupt up
  to about 35 µs (run 18, [bringup.md](bringup.md)). That is why they are off until asked for.

Every record has its microsecond timestamp (smclog). These recordings tune debounce, the encoder
divider and the fader filter, and feed replay (LOG-41). A fader's ADC noise can produce many
records. If the ring overflows, the losses are counted (smclog), and the fader records are the
first candidate for decimation.

### Encoder probe

A diagnostic in the same command: `smclog.py cmd raw probeN` (N = 1..8; bits 4–7 of
LOG_RAW_INPUT, [link.md](link.md)). The scan then reads only encoder N's two columns, in turn, so
each line is read every 2 steps (50 µs instead of 400 at the 25 µs step), and logs each change of them; buttons
and encoders are not evaluated, the LEDs and the faders go on. `raw matrix` (or any value
without a probe) returns to the full scan; the encoders are primed again on its third pass, so
nothing the probe left turns into an event (`test_encoder_probe`). The two reads already on their
way when the order changes are not labelled with a column and are dropped (`probe_label`).

## Input injection (LOG-33, LOG-34)

`inject_command(payload)` handles the INPUT_INJECT command. In the hardware test it runs from
the command link ([link.md](link.md); `smclog.py cmd inject ...`). The payload format is
provisional, until the configuration protocol (SPEC §7) defines its own:

| Byte | Meaning |
|---|---|
| 0 | op: `0` off, `1` on (this session), `2` events |
| then, op 2 | 5 bytes per event, at most 32: type (`core_ev_type`), index, b0, b1, b2 |

- **Button**: b0 is `1` for press, `0` for release.
- **Encoder**: b0 | b1 << 8 is the signed click count, non-zero.
- **Fader**: b0 | b1 << 8 is the position, 0–16383.
- **MIDI in**: the index is the transport (`CORE_FROM_USB`, `CORE_FROM_BLE`); b0–b2 are the
  message.

Rules:

- Injection is off at start and after every reboot. It is never seen by power-on decisions (the
  bootloader reads the real keys).
- Every event of a command is checked before any is queued. One invalid event (an index out of
  range, a tick, a zero click) rejects the whole command.
- A full queue refuses the rest of a command. The reply says how many were accepted and how many
  refused, and both are counted.
- A long press is a press, a wait and a release; the host times it.

Reply: status (`ok`, `disabled`, `bad format`, `bad event`, `full`), accepted, refused.

## The platform part (BR25)

`firmware/platform/br25/input_br25.c` drives the scan on the SDK, with the stock firmware's
settings where they are known ([hardware.md](hardware.md)):

- **Scan timer**: TIMER3, period `SCAN_STEP_US`, interrupt priority 7 (the stock matrix
  interrupt has 5, its handler in RAM). The counter runs 0..PRD, so PRD is the period's counts
  less one (✅ run 26: with PRD = 1200 the µs clock fell behind by 1/1201, about four resyncs in
  10 s). It is also the log's µs clock, started first thing in `app_main()`.
  Nothing else in the build uses TIMER3: `build_app.sh` checks the disassembly (only `scan_isr`,
  `input_br25_clock_start` and `input_br25_time_us` may load its address). TIMER0 and TIMER1 are
  the SDK's; TIMER2 and TIMER5 belong to SDK drivers we do not build (`handshake_timer.c`,
  `irflt.c`).
- **The CPU's time**: since controller 15 `app_main()` sets 192 MHz (`SYS_CLOCK_MHZ`, with the
  fader ADC at /128; [core.md](core.md), "The CPU clock"); the measurements below are at the
  clocks they name. The board starts the CPU at `TCFG_CLOCK_SYS_HZ`
  (`board_smc_mixer_cfg.h`), 96 MHz since 2026-11-11 (the owner's choice, with the 50 µs step;
  `CONFIG_BT_NORMAL_HZ` the same, so Bluetooth's start keeps it). It was the SDK's 24 MHz.
  There the scan does not fit its 50 µs step: the idle task never runs and the watchdog
  restarts the chip (bringup.md, runs 8..13). At 48 MHz (`SYS_CLOCK_MHZ=48`, a
  diagnostic switch of `firmware/app/Makefile`: `clk_set("sys", …)` before the scan timer starts)
  the handler takes 20.3 µs of a step on average and up to 49.9 µs, about 36% of the CPU, and the
  start lasts (run 14, ✅ measured on the mixer). The timer's clock stays at 24 MHz either way.
  For comparison: the stock firmware runs at 192 MHz and paces its matrix by SPI2's
  DMA-complete interrupt, no timer ([hardware.md](hardware.md), "Stock firmware internals",
  ⚠️ static analysis). open-smc-pad, on the sibling SMC-PAD (same chip), moved from 24 to
  96 MHz for its Bluetooth stack's sake, and scans from TIMER3 at 4 kHz: each 250 µs tick reads
  the column latched by the previous one and latches the next, so a pass of its 16 columns
  takes 4 ms, against our 0.8 ms (16 steps of 50 µs). Its notes measure the SDK's `systimer`
  task alone at 40% of the CPU at 24 MHz.
  Where the time goes (`SCAN_PROFILE=1`, runs 15..18, 48 MHz, average of a step): the entry
  6.2..6.6 µs (4.1 from the idle task, 11..14 from a busy one), the load sample 1.6, the µs
  clock 1.8 (the stack sample once a pass of 16 steps), `input_br25_time_us()` 2.8,
  `scan_step()` 4.2 (up to 23), the shift and LED columns 2.0, the fader ADC 1.9 (up to 35, the
  raw fader log line): 15.9 µs in the handler, 22 µs with the entry, 27% of the CPU in the
  handler. About 3 cycles an instruction: each phase costs what its length says. At 96 MHz
  (runs 19..21, ✅): the entry 2.3 µs (0.8 from the idle task, 15 from a busy one: the time
  spent there with interrupts off, which does not shrink with the clock), the handler 10.3 µs,
  about 20% of the CPU; the CPU load as a whole 10..12%. The pass's evaluation (`scan_pass()`,
  about 35 µs: 45 buttons and 8 encoders) runs after the last column's word is out (run 21):
  before it, the word went out late at every pass's end and the next step was skipped.
  Lost periods (runs 22..26, ✅): about 1 500 in 10 s (0.75%), each gap at most 200 µs, every
  one ending right after the SDK's `local_irq_enable()`: about 870 in the SDK's `sys_timer_task`
  (after `timer1_run()`, up to 200 µs), the rest in the ROM's RTOS (`xTaskResumeAll`, queue
  receive, the idle task's `prvGetExpectedIdleTime`), at most 100 µs. They cost a step's delay,
  nothing more; the µs clock counts them (no resync in 60 s, drift −750..+250 µs against the
  millisecond reference, a pass every 1.1 ms at most). The ms-long holds seen before were the
  clock, not interrupts: it lost these periods and ran 1/1201 slow (the timer's period).
- **The running task**, for the load samples: read from the RTOS's own variable (`*CurrentTCB`,
  as the SDK's `pvPortSwitch` does), once at the clock's start it equals what
  `xTaskGetCurrentTaskHandle()` returns; else through that function. The ROM's function locks
  through the ROM's `local_irq_disable()`/`local_irq_enable()`, which the SDK patches with
  hardware breakpoints: 6.6 µs a call at 48 MHz, against 1.6 µs for the direct read (✅ run 17).
  The log line "running task: read directly" says which way a start took.
- **SPI2**: port A (PB6 clock, PB7 data), master, one data line, clock idle low, data changed on
  the falling edge, 4 MHz since 2026-11-23 (`clk_get("spi")` / 4 MHz: 64 MHz / 16; the log records
  the result; 2 MHz before). Each word
  is 4 bytes of DMA from RAM. The interrupt is off: the next step checks the pending flag, and a
  step that comes while a word is still shifting is skipped and counted (`spi_busy`), since
  latching then would latch half a word. A 32-bit word takes 8 µs at 4 MHz (16 at 2 MHz), so a skip
  means the step before shifted late in its period. With the 25 µs step: about 1.7% of the steps
  at 2 MHz, 0.5–1.3% at 4 MHz (✅ the mixer, 2026-11-23; LEDs and buttons unchanged by eye and in
  e2e). The step is simply taken again 25 µs later: nothing is lost, the pass lengthens.
- **Start-up**: rows as inputs with pull-ups; LED columns as outputs, high (off); PB4 low; then one
  word with nothing selected and all LEDs off is shifted and latched, and only then is PB5 driven
  low (⚠️ the 595 /OE) and the timer started.
- **Fader ADC**: our own driver, polled from the scan interrupt; the 8 candidate pins are analog
  inputs (no digital input, no pulls). At each step the interrupt either finishes its conversion
  and hands the value to the scan (`scan_fader`), or starts the next fader's if the converter is
  free; the faders take turns, and after each conversion the converter stays idle for a step. The
  SDK's driver (`adc_api.c`: one of its own channels, the battery and the reference, every 2 ms)
  keeps sharing the converter: ours starts only when the control register reads 0, never enables
  the ADC interrupt (the SDK's handler would take the result) and ends a conversion as that
  handler does (CON = 0). If the register no longer holds our setting while ours runs, the SDK
  took the converter: ours is dropped and counted (`lost`). A conversion still running after 40
  steps (2 ms) is given up (`timeouts`). The original SDK setting used /96 and start delay 15;
  current device builds use `ADC_DELAY=4`, and `ADC_DIV=7` (/128)
  with the default 192 MHz CPU (64 MHz ADC source). ✅ Measurements: each fader every 1.62 ms
  at 96 MHz with /96, about 1.27 ms at 192 MHz with /128 ([core.md](core.md), "The CPU clock").
  Each fader is read every (conversion steps + 1) × 8 × `SCAN_STEP_US`; the heartbeat records both.
- **Lock**: `input_port_lock()` is `irq_br25.h`'s: `cli`, and `sti` again only if interrupts were
  on. The SDK's nesting `local_irq_disable()` (the interrupt mask at 6) holds off the scan too:
  ✅ the scan's lost periods end right after `local_irq_enable()` (run 24); why priority 7 is
  masked by it is ❓ (the mask's meaning is from the disassembly only).
- **Lost periods**: the timer has one pending flag, so a step that starts more than a period late
  loses a period. Each step places its period's start on the CPU's tick timer (`TICK_CNT`, the
  RTOS tick's, 10 ms, counting on with interrupts off); two starts more than 1.5 periods apart
  mean a period was lost. The µs clock counts the lost ones too, and the heartbeat logs them by
  site: the address the late step interrupted (where interrupts came back on) and the task
  ("scan periods lost", "scan gap site").
- **Waking the control task**: the interrupt does not call the RTOS (below). After queueing an
  event it raises the software interrupt SOFT0 (`input_br25_wake()`); SOFT0's handler, at
  priority 1, calls the waker the application set (`input_br25_set_waker()`: the controller posts
  its control task's semaphore).
- **CPU load**: at each step the interrupt notes whether the task it interrupted is the RTOS's
  idle task (`IDLE`); the share of the others is the heartbeat's CPU load. Interrupt handlers that
  run while the idle task is current count as idle; the scan interrupt's own time is measured
  apart (the heartbeat's "CPU of our code").
- **Statistics**: steps, skipped steps, the handler's longest run and its longest delay after the
  timer period (timer ticks), the SPI clock, the fader channels sampled; the ADC's counts and the
  load samples per heartbeat window.

### While flash is written

❌ The premise below does not hold: the SDK's `local_irq_disable()` holds the scan off too
(run 24, "Lost periods" above). So the scan pauses while the SDK writes flash instead of running
through it; the RAM-only rules stay (they cost nothing and keep the handler safe if the mask is
ever lifted).

The original rationale inferred from the disassembly was that mask 6 left priority 7 running;
the device disproved that. Configuration saves on 2026-11-12 and panel-state saves on
2026-11-13 measured holds up to about 10 ms per SDK flash command
([config.md](config.md), "The first run", "The panel's state"). Pairing without bonding is now
the default and stores no keys in the recorded BLE runs. The scan's retained RAM-only rules are:

- runs at priority 7;
- reaches only code and constants in RAM or ROM. `SMC_RAM_CODE` (`firmware/lib/smclog/smc_ram.h`)
  puts a function into the SDK's `.volatile_ram_code`; `SMC_RAM_DATA` puts a constant table it
  reads into `.data` (constants otherwise land in flash). The portable code it reaches (scan,
  event queue, log, metrics, µs clock) carries the marks, which are empty outside the BR25 build
  (`SMC_RAM_SECTIONS`). The ROM functions it calls: `memcpy`, `memset`,
  `xTaskGetCurrentTaskHandle`;
- calls neither the RTOS (it may have interrupted the RTOS inside its own critical section) nor
  the SDK's interrupt lock (whose `local_irq_enable()` would lift the mask under the SDK's flash
  write); the control task is woken through SOFT0 (above).

`tools/re/rampath.py` checks the result in every build (`build_app.sh`, both variants): from
`scan_isr` it follows each call and branch of the disassembly and fails on a function in flash, a
constant loaded from flash, a call through a pointer it was not told about (`--indirect`: the
scan's event sink), a ROM function not in the list above, or a call to `os_*` or `local_irq_*`.
The early hardware-test builds checked 23 functions in RAM, 2 in ROM and none in flash, with
RAM code growing from 0x1AF0 to 0x27BC bytes. These are historical sizes, not a current-build
inventory; `build_app.sh` checks each built image's actual paths.

**Evidence and remaining limits:**

- ✅ The scan runs with `request_irq(..., 7, ...)`; ❌ priority 7 does not bypass the SDK's
  interrupt lock (run 24), nor its real flash commands (controller 20/29).
- ✅ SOFT0 is `ILAT_SET` bit 0, not bit 4: bit 4 never ran the handler; controller 4's corrected
  wake reached the core within 0.45 ms (2026-11-11; `WAKE_ILAT_BIT` in `input_br25.c`).
- ⚠️ ICFG bits `0x300` still come from the SDK's `cpu_irq_disabled()`, not a hardware manual;
  successful runs are operational evidence, not a complete specification of that mask.
- Port read/modify/write collision with SDK code has not been shown. The earlier concern about
  preempting an SDK-locked port write relied on the disproved priority-7 premise; keep shared
  GPIO ownership under review without treating the old hypothetical race as an observed failure.

## The hardware-test image

The application's `hwtest` variant: `VARIANT=hwtest tools/env/build_app.sh` builds it into
`build/hwtest/` (`app.elf`, `app.bin`, the log dictionary). It is the stage-1 application (the SDK,
the log) with `app/hwtest/hwtest.c` in place of the empty variant:

- raw input mode on from start;
- every event from the queue logged (`button 3 press`, `encoder 2 -1`, `fader 5 8192`), every
  10 ms;
- the LED walk, one step a second: LEDs 0–52 by the stock table, then every (row, column) of the
  LED matrix, each step logged;
- the scan statistics every second (steps, passes, skipped steps, handler time, events by type,
  bounces, encoder jumps, refusals, queue high-water mark);
- the USB driver's, the command link's and USB MIDI's counters every 10 seconds, and on
  METRICS_NOW, with the metrics heartbeat ([logging.md](logging.md), "Metrics": the scan's timing,
  the µs clock's resyncs and drift, the heap, the USB task's stack, the queue);
- over the command link: the log stream, raw mode, metrics, the boot journal and injection;
  injected events are logged with `(injected)`;
- the boot journal at every start ([logging.md](logging.md), "Boot journal"): the reset cause,
  what the previous run left (its last heartbeat, a crash record, its latest records), the init
  times;
- USB MIDI enumerates as in the application ([usb.md](usb.md), "USB MIDI"). Channel messages from
  the host go into the input queue and are logged (`MIDI in from transport 0: 90 3c 40`); this
  build sends no MIDI.

The core is not wired in: this build is about the hardware, not the controls (the `app` variant
has it: [core.md](core.md), "On the device"). On 2026-11-08 it links to an `app.bin` of 68,464
bytes, with 109 log call sites.

## On the device: the first run

The steps before this one, and what to look at if the device does not show up on USB at all:
[bringup.md](bringup.md).

Once the image runs on the device (stage 2's installer, after C-6), the first thing is one
command:

```sh
python3 -I tools/smclog/firstrun.py --port /dev/cu.usbmodemXXXX --binary build/hwtest/app.elf \
    --session logs/sessions/2026-11-09-first-run --watchdog
```

✅ It ran on the mixer on 2026-11-11 (image 30; sessions `2026-11-11-firstrun-hwtest30`,
`-wdt2`, `-fault2`): every check passed. The soft, fault and watchdog resets each come back in
0.7–2 s (the watchdog 8.9 s after the request) with the RAM kept, the cause named (soft and
fault 0x440, watchdog 0x404) and the latest records replayed; the fault leaves its crash record.
The device's clock runs +17..+19 ppm against the host's. Two of the tool's checks were wrong
and are fixed: the clock's runs are split when the device's time lags far behind the host's, not
only when it goes back (a reset up longer than the run before it), and the fault test's frame
check accepts a load that traps after the function returned (logging.md, "The exception handler
(BR25)").

It checks what can be checked without touching the panel and writes `report.txt` (and
`report.json`) into the session, next to the records:

- the link: versions, the build ID;
- PINGs: round trips, and the device's clock going forward;
- the stream: header first, then over the whole run no bad frame, no gap, no unknown call site;
- the boot journal, the levels, raw input and injection;
- a metrics heartbeat with `smclog metrics --check`'s checks;
- 10 s at rest (`--listen`);
- the device's clock against the host's (drift within 500 ppm, "The device's clock on the host"
  in [logging.md](logging.md)).

Then RESET (soft, then fault; `--watchdog` adds the watchdog test). After each reset it waits for
the device to come back and checks:

- **the RAM was kept**: the boot count goes on;
- **the reset cause**: what the bits say for each kind of reset; the raw value is in the report;
- **the heartbeat**: the one sent just before the reset is reported;
- **the replay**: the latest records are replayed, up to `reset on request`;
- **after the fault test**:
  - the crash record is there, so our exception handler ran;
  - a return address of the frame lies inside `diag_br25_fault()`, so the frame's order in
    `diag.h` holds;
  - the CPU's trace, by name.

The exit status is 1 if a check failed. The first findings to note from it are open questions of
[logging.md](logging.md), "Boot journal": whether the RAM survives a reset, what the reset bits
mean, which register holds the faulting address, and what the trace holds. If "nothing kept"
comes back, the ROM or the factory uboot clears that RAM, and the keep block has to move.

The same run works on the `app` variant (`--binary build/app/app.elf`).

## On the device: the test procedure

The firmware starts with raw mode on for the matrix (the faders: step 4). The host records everything with
`smclog.py capture --port /dev/cu.usbmodem* --dict build/hwtest/app.smclog.json --session DIR`
([link.md](link.md)), and a marker (Enter) goes in before each step. A person (LOG-30
scenario, then LOG-36: the scan layer stays manual) does the following:

1. **Buttons**: press each of the 45 in order; the log must name indexes 0–44 in order.
2. **Encoders**: the knobs have no detents, so turn each one revolution clockwise against a mark
   on its cap, then back; expect about +48 and −48, and `enc_invalid` 0 at a normal speed. Then
   turn fast and note where steps start getting lost (R-4).
3. **LED walk**: the firmware lights LED 0, 1, … 52 in turn for a second each, then every
   (row, column) of the 4 × 16 matrix. The person notes what lights. This confirms or corrects
   `scan_led_pos` and the polarity.
4. **Faders**: `smclog.py cmd raw on` first (the matrix and the faders), then move each fader
   end to end; the raw ADC records show which channel follows it, and its range and noise.
   `smclog.py cmd raw matrix` afterwards.
5. **USB**: enumeration, a 10-minute run with all of the above, then the driver's statistics
   (`smclog.py cmd metrics`; [usb.md](usb.md) and [link.md](link.md), "Not checked without
   hardware").
6. **Timing** (the same session): `smclog.py metrics DIR`. The µs clock should show no resyncs
   at rest and a drift within about 0 to +1000 µs (the reference is in whole milliseconds); a
   drift that grows means the timer's rate is not what `clk_get("timer")` says. The scan's step
   handler should stay below the 25 µs step and its passes near 400 µs; the heap and the USB
   task's stack must give plausible numbers (or "not measured", which says the SDK's stack
   functions are not what the disassembly suggests). The fader ADC: no timeouts, few conversions
   lost to the SDK, the longest conversion in steps and how often each fader was read (this sets
   the faders' latency); the CPU load at rest and while moving everything.
7. **Boot journal**: `smclog.py boot DIR` on the session. The first start after power-on must
   read "power-on" and "nothing kept". The resets are `firstrun.py`'s (above); `smclog.py cmd
   boot` writes the journal again during a capture.

### What the run of 2026-11-11 found

Images 26 and 27, the owner at the bench; sessions `logs/sessions/2026-11-11-manual-1-buttons-encoders`,
`-2-led-walk`, `-3-faders` (image 26) and `-4-hwtest27-usb10` (image 27). The layout table above
carries the facts; besides them:

- **Encoders at speed: the cause, measured.** A slow revolution: 47–49 steps, no jump, on all
  eight. Fast spins lost half-steps (`2026-11-11-manual-5-encoder-spins`: 92 jumps over eight
  encoders, more on the higher numbers). The encoder probe (image 28, sessions
  `2026-11-11-probe-enc7` and `-probe-enc1`) read each line every 100 µs and gives the true count:
  - No invalid transition at 100 µs. Contact bounce exists but is short: 45..79 pulses of
    100–200 µs in a session, rarely caught by a 0.8 ms pass.
  - The four states are not equal: 11 (both contacts closed) is the shortest, 19–22% of a cycle
    (00 27–30%). In fast spins (about 3 revolutions a second on average, 6 at the peak) state 11
    lasts 1.4..1.8 ms at the median and 0.35..0.85 ms in the shortest tenth.
  - So the cause is the sampling rate: when a state is shorter than the 0.8 ms pass, the decoder
    sees two lines change at once (10 → 01) and drops both transitions. Encoder 1 under the probe
    is no better than encoder 7, so the gradient over the encoders' numbers seen before is ⚠️
    most likely the spins' speed, not the parts.
  - Image 27's pairing of encoder 8 a step apart stays right (`test_encoder_fast_turn`).

  The recorded waveforms, sampled again as the scan would (random phase, B read 50 µs before A)
  and decoded, against the true count: steps lost, at the recorded speed (about 3 revolutions a
  second on average over a spin) and at that speed scaled up (⚠️ a model past ×1: bounce does not
  scale with speed):

  | Decoding | ×1 (3 rev/s) | ×1.5 (4.4) | ×2 (5.9) | ×3 (8.8) |
  |---|---|---|---|---|
  | now: a 0.8 ms pass | 1.1–1.9% | 7.7–10.8% | 21–25% | 53% |
  | a one-pass debounce of each line | 1.9–4.8% | 13–17% | 34–36% | 66–69% |
  | the decoder infers a skipped state (a jump counts two transitions the way the encoder last turned, if it turned within 8 passes) | 0.4–0.6% | 0.4–2.9% | 0.9–10.9% | 25–29% |
  | a 25 µs step: a 0.4 ms pass | 0.5–0.6% | 0.6–0.8% | 1.5–2.3% | 8–11% |
  | a 25 µs step and the inference | 0.4–0.6% | 0.4–0.6% | 0.5–0.6% | 0.5–1.2% |

  Ranges: encoder 7, encoder 1. About 0.4% is the comparison's own floor (half-steps at a spin's
  ends). The owner chose the inference (2026-11-11), and on 2026-11-23 the 25 µs step on top
  (the last row): on the mixer, owner spins at peaks of 230–380 clicks a second lost nothing on
  encoders 1 and 7 (build `746dd090`; encoder 8 about 7% short, its known contacts below).
- **Encoder 8's contacts.** Its probe (image 29, `2026-11-11-probe-enc8`, read with
  `-hwtest29-spins`) counts 1 199 pulses of 100–200 µs, about 300 a line, against 79–96 on
  encoders 1 and 7: a contact opening briefly while closed. In fast spins that alone loses about
  6–8% whatever the decoder (the replay above), slow turns 0.5–1.4%.
- **The inference, on the device.** Image 29 took the last transition's direction: on encoders 1
  and 7 its counts equal the replay's, 74 jumps inferred, none lost; on encoder 8 a bounce (+1 −1)
  turned the guess the wrong way. Image 30 takes the sum of the recent transitions, held within
  ±3, instead (`enc_trend`, `test_encoder_skip_after_bounce`). Replayed (losses at ×1 / ×1.5 /
  ×2 / ×3 of the recorded speed): encoder 1 0.3 / 0.4 / 11 / 38% (last direction 0.3 / 0.7 / 14 /
  41%), encoder 7 0.5 / 0.4 / 0.7 / 20% (0.5 / 0.5 / 1.0 / 26%), encoder 8 6.0 / 6.0 / 6.5 / 13%
  (8.2 / 6.9 / 8.5 / 17%). A sum of 1 and of 2 to infer are equal within 1%; 1 is kept. A filter
  that holds a closed line one pass longer was worse and is not used.
  Image 30 on the bench (`2026-11-11-hwtest30-spins`): on encoders 1, 7 and 8, 5 fast revolutions
  each way (35–72 clicks a second on average, peaks 80–114) against 5 slow ones back to the same
  mark: 240 = 240, 241/240, 239 = 239, 239 = 130 + 109; ✅ at most 1 click in 240. 20 jumps
  inferred, none lost. These spins were slower than the probe's (peaks 110–170 clicks a second),
  so the model's ×1.5..×2 figures are not checked on the device yet.
- **Faders at rest.** With the defaults (filter shift 2, deadband 32 in 14-bit units, two raw
  counts), a fader left mid-travel sends events now and then: in 14 s fader 4 sent 158, the others
  0..12. Its raw value spans 521..533 there. Not changed: a larger deadband costs resolution
  (the owner decided against a deadband, SPEC Q-21).
- **The faders measured** (sessions `2026-11-11-adc-delay15`, `-delay4`, `-delay1`: hardware-test
  builds with `ADC_DELAY=15`, `4`, `1`, 60 s each with the faders still, alternately at an end
  and mid-travel; and `-manual-3-faders`):
  - The conversion time is the start delay: n × 8 ADC clocks of 3 µs (32 MHz / 96), 360 µs at the
    SDK's 15. A fader is read every 3.68 ms at 15, 1.62 ms at 4, 1.17 ms at 1 (heartbeats).
  - Delay 4 reads as delay 15: the same noise per sample (σ 0.6–2.5 codes, by fader) and no
    crosstalk from the fader converted before (means within 0.2 codes, the one before at an end
    either way). Delay 1 is noisier (fader 2: σ 1.66 → 3.12).
  - The noise at rest has three parts: ±1 code of the ADC's own; a slow part shared by faders
    converted one after the other (correlation 0.2–0.45 at 0.5 ms apart, none further), growing
    with the position (σ 0.55 at code 60, about 1.1 mid-travel, 1.4 near the top: ⚠️ most likely
    the faders' supply against the ADC's reference); and outliers of 10–55 codes that are the
    successive-approximation ADC's wrong bit decisions near the mid-scale transition: 502 (0x1F6)
    read as 512 (0x200), 479 (0x1DF), 447 (0x1BF), 495 (0x1EF). The noisiest faders in every
    recording sat at codes 500–530. They are single samples, not shared between faders.
  - Filters replayed on these samples (the rest noise as recorded; a move of 400 codes with the
    rest noise added), false events a minute on the four mid-travel faders, and the lag behind
    the true position at each 7-bit step:

    | Filter | ADC delay | Events at rest / min | Lag: fast / 1 s / 10 s move | Steps backwards in a move |
    |---|---|---|---|---|
    | today: shift 2, deadband 2 codes | 15 | 4–786 | 11 / 14 / 34 ms | 17 |
    | the same | 4 | 2–1653 | 5 / 8 / 0–53 ms | 87 |
    | deadband 8 codes (a CC step) | 15 | 0–4 | 12 / 21 / 85 ms | 0 |
    | median of 3, shift 3, hysteresis 4 | 15 | 0 | 29 / 30 / 46 ms | 0 |
    | median of 3, shift 3, hysteresis 5 | 4 | 0 | 13 / 14 / 19 ms | 0 |
    | median of 3, shift 4, hysteresis 4 | 4 | 0 | 25 / 27 / 31 ms | 0 |

    Hysteresis here is directional: while the fader keeps its way every code is sent; turning
    back, or starting from rest, it must move the hysteresis first (5 codes: 0.5% of the travel,
    0.6 of a 7-bit step). The resolution stays the ADC's 1 024 positions; a 14-bit message carries
    no more than that.
  - The owner chose delay 4 with the hysteresis (2026-11-11). On the mixer: hardware-test image 32
    (`2026-11-11-hwtest32-faders-mid`): ✅ all eight faders mid-travel (codes 478–523), no event in
    120 s. The controller then still sent a jump now and then on fader 2 (code 502): bursts of
    wrong conversions, 512 several samples in a row (runs of 2 or more about 50 a minute, of 3–5
    about 15–20, `2026-11-11-app4-fader3`), which a median of 3 lets through; under the scenario
    runner's log traffic they lifted it 5 counts about twice a minute. The gate (above) drops
    bursts of up to 7 in the model, with the same lag on a fast move (13 ms) and 17–21 ms on slow
    ones. Controller 5: ✅ `smcsim device` 12 of 12, three runs in a row, with every fader
    mid-travel, and no fader event from the scan in about 8 minutes.
- **USB and timing**, 12 minutes with everything moved for about 5 of them: 26 092 events, no queue
  refusal (the most queued 27), no log record lost (the ring at most 7 195 of 8 192 bytes),
  719 commands with 719 replies, no bad frame, no USB reset during the run; the µs clock 0
  resyncs, drift −650..+350 µs; passes 821 µs on average, 1.13 ms at most; the CPU load 109 per
  mille over the session, 141 at most; the scan 183 per mille at most; `smclog metrics --check`
  ok. The first start after installing image 27 counted 17 USB bus resets at enumeration (every
  earlier image: 1); a soft reset afterwards counted 1. ⚠️ Cause unknown, not seen again.

Then the automatic part (LOG-35), on the `app` variant (the controller), not this one:
`smcsim device` runs the scenarios of `firmware/tests/scenarios/` with injection and checks that
the device agrees with the simulator ([core.md](core.md), "Scenarios on the device").

## Not done yet

Installation, the scan/ADC/clock and controller scenario runs are no longer untried: see the
2026-11-10/11 ladder in [bringup.md](bringup.md) and the results above. Remaining hardware
questions include encoder 8's contacts (about 7% short in the 2026-11-23 fast spins, where
encoders 1 and 7 lost nothing), the exact PB5 /OE wiring and the coloured LEDs' glow path. The fader filter's moving-position
lag is modelled rather than directly measured, and the 257th panel-state save's sector rollover
has not run on hardware ([config.md](config.md)). Hardware-test USB serial `T1` and showing
both variants separately remain unchecked ([usb.md](usb.md)). Our DFU bootloader is still unrun.
