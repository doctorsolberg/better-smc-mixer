# better-smc-mixer: notes for coding agents

Alternative firmware for the M-Vave SMC-Mixer (JieLi AC6966C, BR25) and a companion app to
configure it. Aimed at hardware synths and iOS apps, not DAWs.

## Read first

1. The document of the area you touch (table below).
2. [docs/SPEC.md](docs/SPEC.md) only for the requirements you need: it is long. Look a
   requirement up by its ID (`FR-PG-03`, `LOG-07`, `C-6`, `Q-18`).

| Area | Document | Code | Tests |
|---|---|---|---|
| Control core, simulator, scenarios | [docs/core.md](docs/core.md) | `firmware/core/`, `firmware/ctl/`, `firmware/sim/`, `tools/smcsim/` | `firmware/tests/test_core.c`, `test_ctl.c`, `firmware/tests/scenarios/` |
| Configuration: schema, files, binary form | [docs/config.md](docs/config.md) | `config/`, `firmware/config/`, `tools/smcconf/` | `test_cfgbin.c`, `tools/smcconf/test_smcconf.py` |
| Logging, metrics, boot journal | [docs/logging.md](docs/logging.md) | `firmware/lib/smclog/`, `firmware/metrics/`, `firmware/diag/`, `tools/smclog/` | `test_smclog.c`, `test_metrics.c`, `test_diag.c`, `tools/smclog/test_*.py` |
| Command link (USB CDC; SysEx over USB and BLE MIDI) | [docs/link.md](docs/link.md) | `firmware/link/`, `firmware/app/usb/`, `tools/hostmidi/`, `tools/hostserial/` | `test_link.c`, `tools/smcconf/test_device.py`, `tools/hostmidi/test_hostmidi.py`, `tools/hostserial/test_hostserial.py`, `tools/editortest/transcripts.py` |
| Input scan, hardware-test image, first run | [docs/hwtest.md](docs/hwtest.md) | `firmware/input/`, `firmware/platform/br25/input_br25.c`, `firmware/app/hwtest/` | `test_input.c` |
| USB controller driver, USB MIDI | [docs/usb.md](docs/usb.md) | `firmware/platform/br25/usb/`, `firmware/midi/` | `test_dcd_br25.c`, `test_midi.c` |
| BLE-MIDI (stage 5) | [docs/ble.md](docs/ble.md) | `firmware/midi/blemidi.c`, `firmware/midi/bleq.c`, `firmware/app/ble/` (`BLE=1`) | `test_blemidi.c`, `test_bleq.c` |
| Flash image, installation plan, the editor's installer, going back (STOCK) | [docs/image.md](docs/image.md), [docs/app.md](docs/app.md) "Firmware" | `tools/image/`, `firmware/store/bootsel.c`, `app/editor/js/flashimg.js`, `installplan.js`, `stocklink.js`, `release.js` | `tools/image/test_*.py`; `test_bootsel.c`; `tools/editortest/run.mjs` (`install.json`, `stockdev.mjs`) |
| First contact with the mixer: the order of steps, failure branches, the register of unverified assumptions | [docs/bringup.md](docs/bringup.md) | | |
| Recovery (ROM download mode, RP2040 key) | [docs/recovery.md](docs/recovery.md) | `tools/usbkey/`, `tools/jlrom/` | `tools/usbkey/test_*`, `tools/jlrom/test_compare.py` |
| Device facts, stock firmware | [docs/hardware.md](docs/hardware.md) | `tools/stage0/`, `tools/re/` | |
| Build environment, pins of dependencies | [docs/build.md](docs/build.md) | `tools/env/`, `docker/` | |
| Publication checks (nothing private, documents' links, every version agrees on both sides), the dry release package, the fuzzer and the sanitizers | [docs/build.md](docs/build.md), "Checks and tests" | `tools/release/`, `firmware/tests/fuzz_device.c` | `tools/release/test_*.py`; `make sanitize`, `make fuzz` |
| The user guide | [docs/user/README.md](docs/user/README.md) | | |
| The configuration editor (UX, the browser editor, its simulated mixer) | [docs/app.md](docs/app.md) | `app/editor/` | `tools/editortest/` (`test_vectors.py`; `run.mjs` in the build image's Node, `tools/env/test_editor.sh`); the page by hand: [docs/app.md](docs/app.md), "The editor's code" |
| Joining the editor and the firmware: the plan, the test layers (contract, link, product on the mixer, browser) | [docs/integration.md](docs/integration.md) | `tools/editortest/`, `tools/e2e/` (`make e2e`, on the mixer), `app/editor/js/link.js` | `tools/editortest/test_vectors.py`, `run.mjs` (with `link.mjs`), `live.py` (T-2L), `tools/e2e/test_e2e.py` |

Directory by directory: [firmware/README.md](firmware/README.md).

## Hard rules

- **No device actions without the owner's go-ahead in the current session.** Nothing that talks to
  the mixer or any other USB device: no `smcmidi`, no `jlrom`, no serial or MIDI ports. An earlier
  go-ahead does not carry over.
- **Never write the mixer's flash without the owner's go-ahead in the current session** (SPEC C-6,
  Q-18; recovery.md, "Rehearsal 2"). Write paths and authorized hardware runs already exist;
  neither their implementation nor an earlier successful run authorizes another device action. Host-only tests may exercise the simulated write paths.
- **No new dependencies and no downloads.** Allowed: the JieLi toolchain, the JieLi AC63 SDK, the
  frozen TinyUSB copy in `third_party/`, Debian's pinned `gcc-arm-none-eabi`, Node.js and host
  gcc in the build image (SPEC C-4, Q-15, A-20; the host gcc: the owner, 2026-11-14). Python tools use the standard library only. Other projects (open-smc-pad,
  jl-uboot-tool) are sources of facts, never of code.
- **Read-only trees:** `deps/` (the SDK is compiled in place, never edited) and
  `third_party/tinyusb/` (`tools/env/vendor_tinyusb.sh check`).
- **Vendor data stays local** (SPEC C-7): `materials/`, `logs/`, `deps/` and `build/` are not part
  of the repository. Never copy their contents (firmware, dumps, Bluetooth keys, the serial
  number) into a tracked file.
- **Repository files are in English**: documents, comments, tool output, log text.
- **Do not take the owner's decisions.** Write them down as questions: SPEC §13.1 for the
  requirements, a GitHub issue for the rest.
- **The editor and the firmware are one product.** They run on different devices but must match
  exactly: the configuration (schema, file, binary form), the link protocol, what each control
  sends, the behaviour the editor describes in words, and the version numbers
  ([docs/app.md](docs/app.md), "One product"). A change to any of these is made and checked on
  both sides in the same step: the definition, the firmware, the editor, both sides' tests and
  documents. Never leave one side ahead of the other, and never call a part of the contract ✅
  before the editor and the mixer have run it together.
- **No configuration migrations before the first public release**: a format change rewrites
  `config/*.json` and the tests (SPEC FR-APP-22).
- **Keep changes reviewable:** make small changes, run the checks after each, and do not rewrite
  a file wholesale when an edit will do. Write all commit messages in English. Never publish,
  push, or create a remote repository without the owner's authorization.

## Browser co-working (project-specific)

These owner preferences apply to this project only.

- Reuse the initial blank browser tab for the configurator instead of opening a second tab and
  leaving the first blank. Identify the test browser's targets first; close only positively
  identified disposable blank tabs, never personal tabs. Verify that one test tab remains.
  Recheck after each browser/session restart: the harness may attach to a second background tab
  while the visible first tab stays blank. In that case attach explicitly to the original blank
  target with `activate=False`, navigate it, and close the redundant configurator tab only when
  no device operation is running. Reusing whichever tab the harness attached to is insufficient.
- Keep browser automation in the background while the owner uses the keyboard and mouse.
  Attach with `switch_tab(target, activate=False)` and navigate the existing tab with `goto_url`.
  Do not use `Page.bringToFront`, `Target.activateTarget`, foreground desktop delivery or other
  focus-changing actions without the owner's explicit approval for that step.
- Use CDP for page operations and background desktop actions for native permission/chooser
  windows. If an operation cannot work in the background, pause and agree on a safe moment
  instead of stealing focus. Chrome may raise native dialogs itself; do not promise otherwise.
- Verify native permission dismissal and completed device discovery, not just a CDP permission
  value. A background DOM read (`document.hasFocus() === false`) proves that read worked without
  page focus, not that every future dialog will remain in the background.

For a firmware install/rollback cycle on the mixer, see [tools/fwcycle/README.md](tools/fwcycle/README.md).

## Commands

Run from the repository root. Python is always `python3 -I`.

| Command | What | Time |
|---|---|---|
| `make quick` | host tests that take no real time: the C unit tests, the core scenarios, the tools' fast tests, the self-tests of `jlrom` and `smcmidi` (rebuilt if their source changed) | about 10 s |
| `make test` | every host test: `quick` plus the suites that run in real time, and the editor's `MixerDevice` live against `devhost` (`tools/editortest/live.py`, Docker) | about 3 min |
| `make device` | every device build below, each with its image checks | minutes |
| `make check` | `test`, then `device` | |
| `make sanitize` | every C test and the fuzzer with AddressSanitizer and UBSan (`build/host-san`) | about 1 min |
| `make fuzz [CASES=n] [SEED=n]` | the fuzzer of everything a host sends the controller, with the sanitizers | 100 000 cases: about 5 min |
| `make coverage` | line coverage of the firmware's portable code by the C tests and the fuzzer (`build/host-cov/uncovered.txt`) | about 45 s |
| `make linux-py` | the plain Python tests on Linux, in the host test image's Python ([docs/integration.md](docs/integration.md), "Any OS") | about 6 s |
| `make linux` | every host test on Linux, in the host test image (`docker/Dockerfile.host`: gcc, Python, Node on the machine's own platform, no emulation): `make -C firmware test`, the editor's contract tests, T-2L ([docs/build.md](docs/build.md), "The host test image") | about 2 min |
| `make release` | a dry release package in `build/release/`: every image built twice and compared, the editor's static files, the manifest, the publication check on what was packed ([docs/build.md](docs/build.md), "Release packages"). Publishes nothing | about 45 s |
| `make -C firmware sim` | `build/host/smcsim`, `devhost`, `cfgtool` | seconds |
| `python3 -I tools/smcsim/smcsim.py run-all firmware/tests/scenarios` | the core scenarios alone | 1 s |
| `tools/env/build_app.sh` | the application (controller) for the device, with image checks | about 25 s |
| `VARIANT=hwtest tools/env/build_app.sh` | the hardware-test variant | about 25 s |
| `BLE=1 tools/env/build_app.sh` | the controller with BLE-MIDI (`build/app-ble/`, [docs/ble.md](docs/ble.md)) | about 35 s |
| `tools/env/build_boot.sh` | the bootloader, with image checks | 5 s |
| `tools/env/check_device.sh` | portable code on the device toolchain (`-Oz`, LTO, `-Werror`), sizes | |
| `tools/env/build_usbkey.sh` | the RP2040 key firmware and its tests | |
| `python3 -I tools/smcconf/gen.py` | regenerate the files made from `config/schema.json` | |

The device builds need Docker (amd64, emulated through Rosetta on Apple silicon) and `deps/`
(`tools/env/fetch_deps.sh`, a download: ask first).

## Done means

- `make test` passes (`make quick` while working).
- If device code changed (anything under `firmware/` outside `tests/` and `sim/`): `make check`,
  which adds the device builds. Some rules are checked only there (below).
- A new behaviour of the core comes with a scenario in `firmware/tests/scenarios/` (LOG-31) and
  a unit test.
- If the change touches what the editor and the firmware share (the table in
  [docs/app.md](docs/app.md), "One product"): both sides are changed, both sides' checks ran (`tools/editortest/`
  in `make quick`, the page by hand), and that table is current.
- The area's document says what the code now does. Progress is not recorded in SPEC.md.
- Separate current summaries from dated run records; do not present superseded failures as
  current blockers. Keep intent in [IDEA.md](IDEA.md), requirements in SPEC.md, deferred ideas
  in [docs/ideas.md](docs/ideas.md). Do not infer the current device state from an earlier
  installation.
- Facts about the device carry a confidence mark: ✅ confirmed (with the source), ⚠️ inferred,
  ❓ unknown. Host tests passing is not ✅: say "not run on hardware" until it has run there.

## Traps

- **Generated files, never edited by hand:** `firmware/core/core_config.h`,
  `firmware/config/cfg_schema.h`, `config/smc-config.schema.json`, and the field reference at the
  end of `docs/config.md`. Edit `config/schema.json` (our own description; it is not a JSON
  Schema) and run `gen.py`. `make -C firmware test` fails on a stale one.
- **`config/factory.json` and `config/test.json` mirror the C configurations**
  (`core_config_factory.c`, `core_config_test.c`), byte for byte. After changing one:

  ```sh
  make -C firmware sim
  build/host/cfgtool factory /tmp/f.bin && python3 -I tools/smcconf/smcconf.py decode /tmp/f.bin -o config/factory.json
  ```

  (`test` instead of `factory` for the other.) The test configuration stays as it is when the
  factory layout changes: the unit tests and most scenarios run on it.
- **The three configuration codecs must agree**: `firmware/config/cfgbin.c`,
  `tools/smcconf/codec.py` and the editor's `app/editor/js/cfgbin.js` implement the same rules
  (the owner, 2026-11-13). Change all three in one step; `vectors.py --write` then `run.mjs` hold the
  editor's to `codec.py` byte for byte.
- **The core is deterministic and portable** (SPEC D-2, D-3): no clock, no hardware, no platform
  headers, no allocation in `firmware/core/`. Time arrives in events.
- **The core's log text is an interface.** The scenario runner and replay read the `input` and
  `engine` records back from their text (button, encoder, fader, midi in, `out …`, `led N = S`,
  `preset P page G`, `shift N`). `tools/smcsim/corelog.py` is the one place that parses them; a
  test fails if `core.c` no longer holds the format strings it lists. Change the two together.
- **The scan interrupt runs from RAM at priority 7** (hwtest.md, "While flash is written").
  Everything `scan_isr` reaches (`input/scan.c`, the event queue's push, the log, the metrics,
  the µs clock) must be marked `SMC_RAM_CODE`, read tables marked `SMC_RAM_DATA`, use no `switch`
  that compiles to a jump table, and call neither the RTOS nor the SDK's interrupt lock.
  `tools/re/rampath.py` checks it, but only inside `build_app.sh`: the host tests do not.
- **TIMER3 belongs to the scan**: `build_app.sh` fails if any other function loads its address.
- **Log call sites** go into a dictionary tied to the build ID; the builds check that every ID
  the code passes is in it. A new log module is added in `firmware/lib/smclog/smclog_modules.h`.
- **A new source file for the device** is listed in `firmware/app/Makefile` (and
  `tools/env/check_device.sh` if it is portable code), not only in `firmware/Makefile`.
- **Hardware coverage is specific to the build, path and run**, not to the whole tree. The controller has substantial hardware coverage; our
  separate bootloader and other listed paths remain unverified. Do not build new features on
  an unverified layer when the hardware could check it first, or remove a ⚠️ without evidence.
