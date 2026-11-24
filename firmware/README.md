# Firmware

Firmware for the SMC-Mixer (JieLi AC6966C, BR25): the application, the DFU bootloader and the
portable libraries both use. Requirements: [docs/SPEC.md](../docs/SPEC.md); build environment:
[docs/build.md](../docs/build.md); logging: [docs/logging.md](../docs/logging.md).

Status: version 0.1.1 (pre-release). ✅ The hardware-test image and controller have run on the
mixer since 2026-11-11; USB/CDC/MIDI, panel scan, configuration storage, BLE and keyless
installation/return have dated run evidence in their area documents. The defects found by the
2026-11-15 code review (R01–R12) were fixed (2026-11-16..19) and verified on the mixer. On
2026-11-23, on a retail mixer: USB and Bluetooth install/update/go-back cycles with physical
off/on and byte-exact configuration read-back, the end-to-end test 37/37, the editor's self-test
29/29, all 19 core scenarios agreeing with the simulator, and 128 held notes released after a
configuration replacement. The matrix is scanned every 25 µs (0.4 ms a pass) with SPI2 at 4 MHz
([docs/hwtest.md](../docs/hwtest.md)). The DFU bootloader remains an unrun, download-refusing
skeleton.

## Layout

| Path | What |
|---|---|
| `app/` | The application, on the JieLi AC63 SDK (for its BLE stack). It starts the log's µs clock and the boot journal (`platform/br25/diag_br25.c`; the version in `include/app_version.h`), the log, then the build's variant (`include/variant.h`): `controller.c`, the controller (scan, core, USB MIDI; [docs/core.md](../docs/core.md), "On the device"), or `hwtest/`, the hardware test ([docs/hwtest.md](../docs/hwtest.md)), each with the metrics heartbeat (`metrics_app.c`: the BR25 part, stacks sampled, heap; [docs/logging.md](../docs/logging.md), "Metrics"); then USB (`usb/`: TinyUSB with a CDC interface for the command link, [docs/link.md](../docs/link.md), and a USB MIDI function with two ports, [docs/usb.md](../docs/usb.md)); with `BLE=1`, BLE-MIDI on the SDK's stack (`ble/`, [docs/ble.md](../docs/ble.md); otherwise `ble_stub.c`). Files derived from the SDK say so in their header (see `NOTICE`) |
| `core/` | The control core: events in, MIDI and LED states out; pages, presets, Shift, pickup, encoders, buttons, the target store; the factory and test configurations and settings by name (`core_settings.c`, shared by the simulator and CORE_SETUP). Portable C11 ([docs/core.md](../docs/core.md)) |
| `config/` | The configuration's binary form: the device's codec (`cfgbin.c`: checks and decodes what is stored or uploaded, writes the canonical form) and its tables generated from `config/schema.json` (`cfg_schema.h`) ([docs/config.md](../docs/config.md)) |
| `ctl/` | The control path on the device: input events into the core, collected MIDI units into USB/BLE queues with backpressure, LED blinking, a clock that never goes back, configuration replacement/live edits and remote selection between rounds, panel state and named SENT history. Portable C11; the output collection is bounded and configuration changes release held notes in bounded chunks (R01/R02, [docs/core.md](../docs/core.md), "On the device") |
| `midi/` | USB-MIDI 1.0 packets (pack, check and unpack) and the single-producer, single-consumer packet queue between the control task and the USB task; BLE-MIDI packets (`blemidi.c`: encode, decode; [docs/ble.md](../docs/ble.md), "Packets") and the BLE queue with its merge (`bleq.c`, "The route"). Portable C11 ([docs/usb.md](../docs/usb.md), "USB MIDI") |
| `metrics/` | The metrics heartbeat (LOG-06): latency histograms, durations, stack samples, the heartbeat's records; the microsecond clock from a periodic timer (`usclock.c`). Portable C11 ([docs/logging.md](../docs/logging.md), "Metrics") |
| `diag/` | Diagnostics across a reset: the boot journal (LOG-11) and the RAM part of the black box (LOG-10): the block a reset keeps, the crash record, the previous run's last heartbeat. Portable C11 ([docs/logging.md](../docs/logging.md), "Boot journal") |
| `link/` | Commands/replies and panel/SENT events over CDC and SysEx on USB/BLE MIDI; the log stream stays on CDC. `cfglink.c`: configuration read/upload/live edits, manual Save (default), optional autosave, Revert, selection/state and load at start. Portable C11 ([docs/link.md](../docs/link.md)) |
| `store/` | Configuration slots with header-last commit (`cfgstore.c`), panel-state journal (`statestore.c`) and sector-0 boot selection (`bootsel.c`). The platform supplies separately bounded storage/boot read, erase and program operations (`platform/br25/flash_br25.c`, NOR models on the host). Portable C11 ([docs/config.md](../docs/config.md), "Storage"; [docs/image.md](../docs/image.md), "Going back") |
| `input/` | The input layer: the scan (matrix, debounce, encoders, faders, LED frame), the input event queue, input injection (LOG-33). Portable C11 ([docs/hwtest.md](../docs/hwtest.md)) |
| `sim/` | The core simulator for the host (`smcsim`), run by `tools/smcsim`; `devhost`, the controller's portable code behind the command link, the stand-in device for `smcsim device` (LOG-35); with `--keep FILE` it keeps what the device keeps across a reset, SIGUSR1 stands in for a crash, and the link's RESET restarts it in place; `--midi PATH` writes the MIDI it would send over USB to PATH, and `--fault drop-midi` loses every third message; `--flash FILE` keeps the configuration's flash slots in FILE (a NOR model); `cfgtool`, the configuration codec on the host ([docs/config.md](../docs/config.md)) |
| `app/board/` | Board support derived from the SDK's BR25 demo board; power compared statically with stock, panel/fader mapping checked on the mixer on 2026-11-11; unresolved electrical details remain in [docs/hardware.md](../docs/hardware.md) |
| `boot/` | The DFU bootloader: bare metal, no SDK (SPEC D-1, C-4). Own startup (`crt0.S`), linker script, TinyUSB DFU class over the same USB driver the application uses on hardware. The bootloader itself has not run on the mixer; it always stays in DFU and refuses every download |
| `lib/smclog/` | Tokenized logging (SPEC §8): ring, records, frames, the `smclog` section; the flight recorder of the latest records, replayed after a reset; `smc_ram.h`, the marks that keep the scan interrupt's code and tables in RAM in the BR25 build (empty elsewhere) |
| `platform/br25/` | Platform code for BR25: the application's smclog port; `input_br25.c`, the input hardware on the SDK (scan timer, which is also the log's µs clock, SPI2 to the 595s, rows, LED columns, our fader ADC driver; the scan interrupt runs at priority 7 from RAM, [docs/hwtest.md](../docs/hwtest.md), "While flash is written"); `irq_br25.h`, the interrupt lock that holds it off; `diag_br25.c`, the boot journal's BR25 part: the block a reset keeps, the reset cause, our exception handler writing the crash record ([docs/logging.md](../docs/logging.md), "Boot journal"); `flash_br25.c`, the configuration's flash slots through the SDK's flash driver ([docs/config.md](../docs/config.md), "Storage"); `usb/`, our TinyUSB driver for the USB controller, shared by the bootloader and the application ([docs/usb.md](../docs/usb.md)) |
| `port/host/` | Host ports for tests and the simulator (macOS and Linux); this is not native Windows host-test support |
| `tests/` | Host unit tests (`check.h`), among them the USB driver against a controller model (`test_dcd_br25.c`); `tests/scenarios/`: the core's scenarios (the regression corpus, LOG-42); `tests/device/`: toolchain checks built for the device and never run |

TinyUSB is a frozen copy in [third_party/tinyusb](../third_party/tinyusb).

## Building

All device builds run in the Docker container (`tools/env/run.sh`). Each writes a log under
`logs/build/` or `logs/env/` and its outputs under `build/`.

From the repository root, `make quick` runs the host tests that take no real time (about 10 s),
`make test` all of them, `make device` every device build below and `make check` both.

| Command | Builds | Output |
|---|---|---|
| `make -C firmware test` | host unit tests (smclog, the bootloader's and the application's USB descriptors, core, the USB driver, the input layer, the command link, the configuration protocol, USB-MIDI packets, the control path, metrics, the boot journal), the decoder and command link tool tests, the core scenarios and the image tools' tests | `build/host/` |
| `make -C firmware sim` | the core simulator, the device stand-in and the configuration codec | `build/host/smcsim`, `build/host/devhost`, `build/host/cfgtool` |
| `tools/env/build_app.sh` | the application (the controller) against the pinned SDK; `VARIANT=hwtest` for the hardware test | `build/app/` (`build/hwtest/`): `app.elf`, `app.bin`, `app.smclog.json`, `app.map`, `app.lst` |
| `tools/env/build_boot.sh` | the bootloader, then checks the image (no SDK linked, sections, RAM, build ID, log IDs) | `build/boot/`: `boot.elf`, `boot.bin`, `boot.smclog.json`, `boot.map` |
| `tools/env/check_device.sh` | smclog and the core on the JieLi toolchain (LTO, `-Oz`) | `build/device-check/` |

Code rules:

- C11 (`gnu11`); our code builds with `-Wall -Wextra -Werror`.
- SDK and TinyUSB sources build as they are.
- IPRA is off in every device link (`-enable-ipra=false`): it breaks weak functions.
- Images are reproducible: a clean rebuild gives the same `app.bin` and `boot.bin`.
