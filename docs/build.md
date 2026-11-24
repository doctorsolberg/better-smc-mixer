# Build environment

How the firmware is built (SPEC C-4, NFR-11, stage 1). The JieLi toolchain is an x86_64 Linux
binary, so every device build runs in a Docker container of the amd64 build image
(`docker/Dockerfile.build`); on Apple silicon Docker emulates amd64, through Rosetta since
2026-11-14 (Docker Desktop: "Use Rosetta for x86_64/amd64 emulation on Apple Silicon"; before,
QEMU). The host tests on Linux use a second image on the machine's own platform, with no
emulation (`docker/Dockerfile.host`, below).

Command descriptions were checked against the launchers on 2026-11-15. Device builds and a
twice-built dry package passed on 2026-11-15, with the build image produced from cached layers
after a network metadata check (not a cold offline provisioning proof); the native macOS and
Linux host suites passed too. Release routing (R06) and Linux launcher hashing (R12) are fixed
("No-download preflight", "Release packages"). A successful build is not a hardware test: hardware
results are in [integration.md](integration.md).

## What is pinned

| Dependency | Pin | Source | Check |
|---|---|---|---|
| JieLi toolchain (clang 4.0.1 for pi32v2, GNU ld 2.26.51) | `jieli-linux-toolchains-20260730.1.tar.xz`, 26,027,508 bytes | `jl-update.oss-cn-shenzhen.aliyuncs.com`, linked from [pkgman.jieliapp.com](http://pkgman.jieliapp.com/doc/all) | SHA-256 `a1545cddbc451a06bac43eb5cd96d831470c42a97c5c0b1f98879a20fc7647bd` |
| JieLi post-build tools (`remove_tailing_zeros`, `isd_download`, `ufw_maker`, …) | `jieli-linux-post-build-tools-20260923.1.tar.xz`, 38,662,628 bytes | same | SHA-256 `6949d48c4cd82e274d7943d4ca67f214593fdb4f699ef9f51c2ecc78125ab739` |
| JieLi AC63 SDK (Apache 2.0) | commit `fdc018de81629e21e594d677cf05f87d288be0f0` | [Jieli-Tech/fw-AC63_BT_SDK](https://github.com/Jieli-Tech/fw-AC63_BT_SDK) | the commit hash |
| Base image | `debian:bookworm-slim@sha256:734a72b3…b179bc`, linux/amd64 | Docker Hub | digest in `docker/Dockerfile.build` |

The JieLi archives may be used but not redistributed: they live in `deps/`, which is not part of
the repository. The Debian packages in the image (make, python3, xxd, …) are not pinned, except
the ARM toolchain for the RP2040 recovery key: `gcc-arm-none-eabi=15:12.2.rel1-1` and
`binutils-arm-none-eabi=2.40-2+18+b1` (compiler and libgcc only, no C library). The image tag carries a
hash of the Dockerfile, so a changed Dockerfile builds a new image.

Node.js for the editor's contract tests (SPEC A-20, decided by the owner 2026-11-13; an allowed
tool in the spirit of C-4 and Q-15: Debian's package, no npm, nothing else downloaded): pinned in
the image as `nodejs=18.20.4+dfsg-1~deb12u3` and `libnode108=18.20.4+dfsg-1~deb12u3`
(bookworm-security, the candidate on 2026-11-13; the image rebuilt with it the same day, with the
owner's go-ahead). Debian's security archive keeps only its newest version: when a newer one
replaces it, the pin is moved to it (a rebuild fails until then). `tools/env/test_editor.sh` runs
`tools/editortest/run.mjs` in the host test image (below; only the repository mounted,
read-only) and says SKIPPED without Docker or without Node in the image
([integration.md](integration.md), T-1). `app/editor/package.json` only tells Node the
editor's `.js` files are ES modules; it is not an npm package and has no dependencies.

### The host test image

The host tests on Linux need no x86: `docker/Dockerfile.host` (the owner, 2026-11-14) is the same
Debian base and the same pinned packages as the build image less the ARM toolchain, built for the
machine's own platform (arm64 on Apple silicon), so nothing is emulated. `tools/env/host_image.sh`
names it (`better-smc-mixer-host:<the Dockerfile's hash>`) and builds it when missing. The host
C compiler: Debian's `gcc=4:12.2.0-3`, `gcc-12=12.2.0-14+deb12u1` and
`libc6-dev=2.36-9+deb12u14` (the same versions on arm64 and amd64; the build image has no host
gcc). It runs:

- `tools/env/test_editor.sh` (the editor's contract tests, T-1) and `tools/editortest/live.py`
  (T-2L): its Node;
- `make linux-py` (`tools/env/test_python_linux.sh`): the plain Python suites;
- `make linux` (`tools/env/test_linux.sh`): every host test on Linux. It copies the sources inside
  the container (the repository is mounted read-only; `build/`, `deps/`, `materials/` and `logs/`
  stay out), builds the host programs with the image's gcc (`-j`) and runs
  `make -C firmware test`, the editor's contract tests and the editor live against `devhost` in
  the image's Node (`SMC_NODE`); `tools/env/test_linux.sh quick` runs the quick set only.

On Linux `firmware/Makefile` adds `-Wl,--build-id -D_GNU_SOURCE`: the host log port reads the GNU
build ID (20 bytes; macOS's LC_UUID is 16) and glibc gives `memmem` and the pseudo-terminal calls
only with `_GNU_SOURCE`.

Measured 2026-11-14 on the Mac (10 CPUs): `make linux` 9 min 39 s in the amd64 build image under
QEMU, 2 min 8 s in the native host image; `make linux-py` 75 s → 6 s. A CPU-bound Python loop:
179 ms native, 472 ms amd64 under Rosetta; under QEMU the configuration tools' tests ran 12 times
slower than on the Mac itself. Rosetta made `make release` (every image built twice) 45 s, from
about 3 min, with the images byte for byte those QEMU built.

## Setup

Needs Docker (Docker Desktop on macOS) and about 1.2 GB of disk for `deps/`.
Setup is not an ordinary verification step: downloads or dependency changes require the owner's
explicit permission. `smoke_build.sh` and the normal launchers can also build a missing Docker
image, which can contact registries and package archives. For a no-download session, preflight
the exact tags first; do not invoke setup to discover whether they are cached.

```sh
tools/env/fetch_deps.sh     # download, verify, extract into deps/ (skips what is already there)
tools/env/smoke_build.sh    # check: build an unmodified SDK example for BR25
```

`fetch_deps.sh` refuses an archive whose SHA-256 differs or which has absolute or `..` entries, and
clones the SDK shallow at the pinned commit. Its log goes to stdout; the setup logs from the first
run are in `logs/env/`.

| Path | What |
|---|---|
| `deps/downloads/` | the two JieLi archives as downloaded |
| `deps/jieli-toolchain/jieli-linux-toolchains-20260730.1/` | toolchain: `pi32v2/bin/` is the one BR25 uses (`pi32/`, `q32s/` are other cores) |
| `deps/jieli-post-build/jieli-linux-post-build-tools-20260923.1/` | post-build tools |
| `deps/fw-AC63_BT_SDK/` | SDK checkout (802 MB with `.git`) |

### No-download preflight

From the repository root, this checks only the Dockerfile fingerprints and the local Docker
image inventory. It does not build, pull, inspect devices or read private/vendor inputs:

```sh
python3 -I - <<'PY'
import hashlib
from pathlib import Path
import subprocess
for recipe, prefix in (("host", "better-smc-mixer-host"), ("build", "better-smc-mixer-build")):
    tag = prefix + ":" + hashlib.sha256(Path("docker/Dockerfile." + recipe).read_bytes()).hexdigest()[:12]
    print(tag, flush=True)
    subprocess.run(["docker", "image", "inspect", tag], check=True, stdout=subprocess.DEVNULL)
PY
```

If either check fails, stop the affected Docker path and report the blocker; do not retag an
older recipe or authorize a download implicitly. `tools/env/host_image.sh --no-build` is also a
local-only check of the host tag. Normal `run.sh` and `host_image.sh` automatically build missing
images; the preflight is not itself a network sandbox. Keep the environment stable or use an
explicit no-build/no-pull guard when strict enforcement is required. Like `host_image.sh`,
`run.sh` uses `sha256sum` when available, falling back to `shasum -a 256`. Either utility hashes
the same Dockerfile bytes to the same twelve-character image fingerprint (R12).

## Running the toolchain

`tools/env/run.sh CMD…` runs a command in the `better-smc-mixer-build` image (built from
`docker/Dockerfile.build` on first use) as the calling user, with:

| Host | Container | Why there |
|---|---|---|
| `deps/jieli-toolchain/<version>/` | `/opt/jieli` (read-only) | the SDK Makefiles use `/opt/jieli/pi32v2/bin` |
| `deps/jieli-post-build/<version>/` | `/opt/utils` (read-only) | the SDK post-build script uses `/opt/utils` |
| the repository | `/src` (working directory) | sources and output |

The open file limit is raised to 8192: the toolchain README warns that the LTO link opens many
files and fails with "File format not recognized" otherwise.

## Smoke build

`tools/env/smoke_build.sh` builds the SDK's `ac636n_hid` example (BR25, board `AC636N_DEMO`)
unmodified, to prove the toolchain, the SDK and the post-build tools work together. Nothing is
packed into a flashable image and nothing talks to a device. Log: `logs/env/smoke-build.log`.

What it does, and why:

- **Builds in a copy** (`build/sdk-smoke/`, an APFS clone on macOS). The SDK builds inside its own
  tree: `objs/`, the generated `cpu/br25/sdk.ld`, `cpu/br25/tools/sdk.elf`.
- **`make pre_build`, then only the `sdk.elf` target.** The default target runs the post-build step,
  which races the link under `-j`.
- **Cuts `app.bin` itself** with the same steps as `cpu/br25/tools/download.sh`: `.text`, `.data`,
  `.data_code`, then each overlay with its trailing zero bytes removed (`remove_tailing_zeros -mark
  ff`: an overlay is an `FF FF FF FF` marker followed by zero-filled space, and only the marker
  stays). The SDK's Linux post-build
  ends in `strip-ini` and `host-client`, which the published post-build tools do not include, so it
  cannot run as is. Packing our own image is our code (stage 2), as in open-smc-pad.

Result (2026-11-07): 136 C files compiled and LTO-linked with no errors or warnings in the log;
`sdk.elf` 2,616,492 bytes; `app.bin` 223,516 bytes (`.text` 212,480, `.data` 3,960, `.data_code`
7,036, ten 4-byte overlay markers); `_start` at `0x1E00120`, the stock application entry
(flash `0x1120`, hardware.md).

## TinyUSB

[third_party/tinyusb](../third_party/tinyusb) is a frozen copy of TinyUSB 0.21.0 (commit
`dae3f9a366bfcddbf9dcf1b48d7500286a849539`): the 31 files the firmware needs (device core, MIDI,
CDC, DFU, common code, no-OS abstraction), unmodified, each with its SHA-256 in `MANIFEST`.

```sh
tools/env/vendor_tinyusb.sh check    # the copy matches MANIFEST (build_boot.sh runs it first)
tools/env/vendor_tinyusb.sh update   # fetch the pinned commit again and recopy
```

`update` fetches with git into `deps/tinyusb-upstream`: a partial, sparse checkout of `src/` and
`LICENSE` only, and replaces the frozen copy. It requires separate authorization; ordinary work
keeps `third_party/tinyusb/` read-only and uses `check` only. Log: `logs/env/vendor-tinyusb.log`.

## Application

`tools/env/build_app.sh` builds `firmware/app` against the pinned SDK. Nothing is packed into a
flashable image. Two variants (`firmware/app/include/variant.h`):

- `app` (default): the controller ([core.md](core.md), "On the device"), into `build/app/`, log
  `logs/build/app.log`. It adds the core with the factory configuration (`firmware/core/core.c`,
  `core_config_factory.c`), the control path (`firmware/ctl/ctl.c`) and
  `firmware/app/controller.c`;
- `hwtest`: the hardware test ([hwtest.md](hwtest.md)), `VARIANT=hwtest tools/env/build_app.sh`,
  into `build/hwtest/`, log `logs/build/hwtest.log`. It adds `firmware/app/hwtest/hwtest.c`.

`BLE=1 tools/env/build_app.sh` adds BLE-MIDI to the controller and writes `build/app-ble/`
([ble.md](ble.md)). Output names also include `-bootmark` when `BOOTMARK=1`; that diagnostic
image writes boot marks on startup and is not a normal release image. Unset unrelated routing
variables when selecting a variant. After changing compiler/linker flags or build-time settings
within the same output directory, pass `-B` to force rebuilding: object timestamps do not encode
the command-line flags. No build command authorizes installation or any device action.

Both have the input layer (`firmware/input/scan.c`, `evq.c`, `inject.c`), the BR25 input platform
part (`firmware/platform/br25/input_br25.c`) and USB MIDI (`firmware/midi/usbmidi.c`, `midiq.c`).

- **The SDK tree is only read.**
  - SDK sources are compiled in place. Our `app_config.h` and `board_config.h` come first in the
    include path and shadow the hid example's.
  - Generated files (`sdk.ld`, `sdk_used_list.used`), objects and outputs go to `build/app/` (or
    the variant's directory).
  - The script warns if the SDK checkout was modified.
- **What is built:**
  - the SDK sources listed in `firmware/app/Makefile` (with extra sources for BLE), as they are,
    with `-w`;
  - two files derived from the SDK: the board file, and `lib_update_config.c` with every SDK
    update mode off (SPEC R-13);
  - our code: `app_main.c`, `ble_stub.c`, smclog and its BR25 port, USB (`app/usb/`, the command
    link `link/`, the BR25 USB driver), and the variant's files, with
    `-std=gnu11 -Wall -Wextra -Werror` and the SDK and TinyUSB headers as system headers;
  - TinyUSB's device core, CDC and MIDI classes, as they are, with our `app/usb/tusb_config.h` and no SDK
    headers;
  - all SDK libraries.
  The SDK's compiler and linker flags are kept, with two changes:
  - IPRA off (`--plugin-opt=-enable-ipra=false`): it breaks weak functions (SPEC §3.4);
  - `--build-id=sha1`.
- **The generated `sdk.ld` is patched** (`firmware/app/sdk_ld_patch.py`). Each pattern must match
  exactly once, or the build fails. The patch adds:
  - `INCLUDE smclog.ld`, the log section, not loaded;
  - the build-id note, placed in flash right after the startup code as `__build_id_note`;
  - `.smc_keep` (NOLOAD), the block a reset keeps (the boot journal's, [logging.md](logging.md),
    "Boot journal"), after the RAM code (`data_code_limit_end`) and before the heap.
- **After the link:**
  - `app.bin` is cut as in the smoke build;
  - the log dictionary is written (`app.smclog.json`, LOG-22);
  - the call sites the code references are checked against it;
  - the disassembly (`app.lst`) is checked for TIMER3, the scan timer: in both variants only
    `scan_isr`, `input_br25_clock_start`, `input_br25_start` and `input_br25_time_us` may load its
    address (so the SDK and its libraries leave it alone). This is a direct-address check; reaching
    the register by indexing from another timer's address would escape it;
  - the scan interrupt's path (`tools/re/rampath.py` on `app.lst`): every function `scan_isr`
    reaches must be in RAM or the allowed ROM functions, with no constant loaded from flash and no
    RTOS or SDK interrupt-lock call ([hwtest.md](hwtest.md), "While flash is written"). Our code is
    compiled with `-DSMC_RAM_SECTIONS`, which turns the `SMC_RAM_CODE` and `SMC_RAM_DATA` marks
    on (`firmware/lib/smclog/smc_ram.h`);
  - the RAM layout: `.bss` ends before `data_code_limit_end`, the keep block lies between it and
    `_HEAP_BEGIN`, and the heap is not empty.

What startup the SDK does (`cpu.a`, `startup.S`; seen in the disassembly), which our bootloader
follows:

1. set `sp` and `ssp`;
2. call `boot_info_init`;
3. zero `.bss`, copy `.data` and `.data_code` word by word (`rep` loops);
4. clear bits 8–9 of `icfg`;
5. call `app_bank_init`, `update_result_get` and `maskrom_init`, then jump to `main`.

The SDK's `main` then starts the OS and calls our `app_main()` in the `app_core` task.

Result (2026-11-07):

- `app.elf` 1,196,220 bytes;
- `app.bin` 46,176 bytes: `.text` 36,720, `.data` 2,880, `.data_code` 6,536, overlay markers
  (57,392 bytes before the SDK update modes were turned off);
- `_start` at `0x1E00120`, `__build_id_note` at `0x1E001A0`;
- 4 log call sites;
- a clean rebuild gives an identical `app.bin`.

With USB, the input layer, USB MIDI, the metrics, the boot journal and the scan interrupt in RAM
(2026-11-08):

| Variant | `app.bin` | Log call sites | `.bss` | Keep block | Heap |
|---|---|---|---|---|---|
| `app` (the controller, with the core, CORE_SETUP and the metrics) | 77,116 bytes | 142 | 49 KB (`0xc44c`) | `0x209e0`–`0x20f00` (1,312 bytes) | `0x20f00`–`0x31ae0` (68,576 bytes) |
| `hwtest` | 68,696 bytes | 111 | 18 KB (`0x48ac`) | `0x18e40`–`0x19360` | `0x19360`–`0x31ae0` (100,224 bytes) |

The RAM code (`.data_code`: the SDK's and the scan interrupt's, [hwtest.md](hwtest.md), "While
flash is written") is 0x27BC bytes in `app` and 0x27A0 in `hwtest`; it was 0x1AF0 before the
scan interrupt moved into RAM (3.2 KB more), and the heap is 3.4 KB smaller.

The heap is what the SDK's `malloc` and the OS task stacks take.

The board settings come from the SDK demo board. They were checked against the stock firmware
(power mode, VDDIO, crystal, charger; the empty application drives no pin): [hardware.md](hardware.md),
"Power".

## Bootloader

`tools/env/build_boot.sh` builds `firmware/boot` with the toolchain and TinyUSB only (C-4): no SDK
header, source or library. Log: `logs/build/boot.log`.

This is still a stage-1 DFU skeleton: `boot_decide()` always stays in DFU, download/manifest
callbacks refuse writes, and no application-selection/flash driver is implemented there. It has
not run on the mixer. A successful build or its inclusion in a dry package does not establish an
installed recovery bootloader; the current installation/recovery routes are separate
([image.md](image.md), [recovery.md](recovery.md)).

What it is made of:

- our startup (`crt0.S`), linker script (`boot.ld`) and `main.c`;
- the TinyUSB device core and DFU class;
- smclog;
- our BR25 USB controller driver ([usb.md](usb.md)), built for endpoint 0 only.

The image is linked where the SDK links applications (`0x1E00120`). Where the bootloader really
goes is a stage-2 decision. RAM is 16 KB from `0x10000`, well below the ROM's area at `0x31AE0`.

After the link the script checks:

- only our objects, TinyUSB, libc and compiler-rt are linked;
- `_start` opens `.text`;
- there are no sections besides `.text`, `.data`, `.bss` and `smclog`;
- RAM ends below `0x31AE0`;
- the SHA-1 build-id note is in flash;
- every log ID the code passes is in the dictionary.

Result (2026-11-08, with the USB driver; stage 1 with the placeholder driver was 3,936 bytes):

- `boot.bin` 6,312 bytes (`.text` 6,292, `.data` 20);
- RAM 12,920 bytes: the 8 KB log ring, the 4 KB DFU block buffer, the driver's state and its
  64-byte endpoint 0 buffer; 3,464 bytes left for the stack;
- 32 log call sites;
- from the toolchain only `memcpy` and `memset` (smclog converts its float arguments itself);
- TinyUSB builds with no warnings at `-Wall`;
- a clean rebuild gives an identical `boot.bin`.

## Checks and tests

| Command | What |
|---|---|
| `make quick` | Root target: macOS Swift tool self-tests (no devices; SKIPPED elsewhere), then `make -C firmware quick`, then `tools/env/test_editor.sh`. The firmware target runs portable C suites, generated-file checks, core scenarios, host transcript/tool/model tests, publication/link/version checks and release-package tests. The editor wrapper adds JavaScript contracts and seeded C/Python/editor differential checks in the host test image's Node. This is the shorter inner-loop set, not a fixed-duration guarantee |
| `make test` | Root target: Swift self-tests, `make -C firmware test`, editor contracts/differential checks, then `python3 -I tools/editortest/live.py` (T-2L). The firmware target adds the real-time log/first-run/scenario/configuration suites against host stand-ins and 2,000 seeded fuzzer cases to its quick set. It does not include the root target's JavaScript/live checks. About 3 min on Apple silicon, subject to host scheduling |
| `make device` | Build/check the bootloader, USB controller, hardware-test variant, BLE controller, portable device-toolchain probes and RP2040 key; nothing is installed or run on hardware |
| `make check` | Root host and device targets. Invoke without `-j`: its `test` and `device` prerequisites can otherwise run concurrently in the shared build tree |
| `make sanitize` | Every C test and the fuzzer built with AddressSanitizer and UndefinedBehaviorSanitizer into `build/host-san` (the compiler's own, no download) and run; the fuzzer 3 000 cases. About a minute. First run 2026-11-14: four faults, all in test code (a test's buffer one byte short, reads past a 64-event array, flash models doing pointer arithmetic outside their arrays), fixed; none in the firmware |
| `make fuzz [CASES=n] [SEED=n]` | `firmware/tests/fuzz_device.c` with the sanitizers, 100 000 cases by default from a clock seed: mutated command frames over the CDC and SysEx, configurations (PUT), live edits (SET), selections, raw bytes, MIDI in over USB and BLE, panel events and power cuts with the flash damaged, through the controller's own wiring (link, cfglink, ctl, core, the stores). After each step the running configuration must pass `cfgbin_check()` and encode, every reply frame must be whole, the core may send no message that is not a channel message. The parsers alone too (`cfgbin_decode`, `cfgbin_patch`, BLE-MIDI, USB-MIDI, the 7-bit unpacking). It prints how far the cases reached and fails if a path (a stored start, a load, an edit, a selection) was never reached. A failure prints the seed to replay. `make test` runs 2 000 cases from seed 1 without the sanitizers |
| `make coverage` | line coverage of the firmware's portable code (`core/`, `config/`, `link/`, `ctl/`, `input/`, `midi/`, `lib/smclog/`, `metrics/`, `diag/`) by every C test and 3 000 fuzzer cases: built with `--coverage` in `build/host-cov`, the counters merged by `tools/release/coverage.py` (the compiler's own `gcov`: Apple's llvm-cov, or gcc's on Linux); every line not reached in `build/host-cov/uncovered.txt`. 2026-11-14: 4 519 of 4 676 lines, 96.6%; the configuration codec 98.3%, the link 99.3%, the configuration protocol 95.0%, the control path 91.2%. The lines not reached where a host's bytes arrive are defensive: a running configuration that does not encode, a check that cannot fail after the record checks, an operation refusing after the busy check; the reachable ones got tests (a preset record twice, METRICS_NOW with a payload, SET and REVERT on a host without them). About 45 s |
| `tools/env/check_device.sh` | Small images for the device toolchain, never run. smclog: with LTO and `-Oz`, the log section must not be loaded, the IDs the code passes must equal the dictionary's, and the build-id note must be in flash. The control core, the input layer (scan, event queue, injection) and the configuration codec: build with `-Werror` and link with smclog and the factory configuration, wired as in the hardware-test firmware; all their call sites are in the dictionary; reports their code size and RAM. Log: `logs/env/check-device.log` |
| `tools/env/build_app.sh` (both variants), `tools/env/build_boot.sh` | the image checks above |
| `tools/env/build_usbkey.sh` | the RP2040 key firmware ([recovery.md](recovery.md)): only our objects, TinyUSB and libgcc linked; boot2 in the first 256 bytes with a correct CRC; the vector table at `0x10000100`; only the BOOTSEL reader in RAM; host tests of the key logic and of `uf2.py`. Log: `logs/build/usbkey.log` |

Use `SMC_EDITOR_TESTS=required make test` for acceptance: the editor wrapper and live harness
otherwise report SKIPPED and may exit 0 without Docker/Node. This setting does not make every
platform/private-input skip an error. `make linux` and `make linux-py` can themselves exit 0 with
SKIPPED when Docker is unavailable; inspect the output, not just the exit code. Historical suite
counts, skip counts and durations belong to their dated reports, not a promise about every host.

For explicit documentation/publication reports, run `python3 -I tools/release/check_docs.py`,
`python3 -I tools/release/check_public.py` and `python3 -I tools/release/check_versions.py`.
The privacy scanner uses local stock/private-string inputs where present and reports missing
inputs as skipped checks; zero findings is not proof that those inputs were checked. Keep the
inputs and any sensitive diagnostic details out of public output. Tests are host/model checks
only; even `make device` adds build evidence, not a hardware test.

## The version

One semantic version for the whole product (the owner, 2026-11-14): the firmware, the editor and the
release package carry the same number, 0.x before 1.0. The source is `VERSION` at the repository's
root (`0.1.0`); its copies are `firmware/app/include/app_version.h` (three numbers) and
`app/editor/js/version.js`, and `tools/release/check_versions.py` (`make quick`) fails when one
differs. A release changes all three in one step.

- **The firmware** writes it into the boot journal (`firmware 0.1.0, build tag …`), GET_INFO's
  reply ([link.md](link.md)) and USB's `bcdDevice` (major and minor, [usb.md](usb.md)). devhost
  reports the same. `smclog cmd info` and `firstrun.py` print it.
- **The editor** shows the mixer's version (from GET_INFO) and its own in the device sheet.
- **The package**: `manifest.json`'s `version` and `editor`.
- **Compatibility** does not follow from this number: the editor and the firmware check the
  format and protocol versions (FR-CFG-07, [config.md](config.md), "Versions and migrations").
- Builds between releases carry the version of the release they lead to; the build ID tells them
  apart (LOG-12). The number moves when a release is made, not with each build.

## Release packages

`make release` (`python3 -I tools/release/release.py --twice`) makes a release package in
`build/release/`, the same package the release workflow attaches to a GitHub release and the
Pages site serves ([app.md](app.md), "Hosting on GitHub Pages"); GitHub Actions builds release
packages from tags (`.github/workflows/release.yml`). The script builds every image
(`build_boot.sh`, `build_app.sh` in its three forms), packs them with their log dictionaries, the editor's static files
(`app/editor`'s page, styles and modules; `config/schema.json` and `config/factory.json` in
`editor/data/`, which the page reads when `../../config/` is not there), the user guide's drafts,
`LICENSE`, `NOTICE` and the license texts it names (`licenses/`: newlib's and compiler-rt's notices,
TinyUSB's, and the SDK's where `deps/` is here), then writes `manifest.json` (the product's version,
the five format versions, each image's size, SHA-256 and build ID, every file's size and hash)
and `SHA256SUMS`. `--no-build` packs what `build/`
holds: `python3 -I tools/release/release.py --no-build --out build/release-no-build` is an
assembly/publication check, not fresh image or reproducibility evidence. Do not combine it with
`--twice` for a no-build run: `--twice` still invokes the forced second builds.

The release builder clears only `VARIANT`, `BLE` and `BOOTMARK` before applying each image's
explicit overrides (R06). Ambient application, BLE or diagnostic flags therefore cannot
redirect a release build away from the directory later packaged. Unrelated environment settings
such as PATH are preserved. Host fake-runner regressions cover all combinations of these flags,
ordinary and forced builds, and replacement of planted stale nominal images.
Preflight Docker first: release builds use launchers that may build a missing image automatically.
`--twice` forces the second build only; its first build can reuse existing objects. For a proof
of two from-scratch builds, first call `release.build(force=True)`, then
`release.main(['--no-build', '--twice'])` in the same Python process after importing
`tools/release/release.py`. Run this only after other shared-tree builds/source edits have finished.
Existing artifacts and an assembly-only `--no-build` run are not that proof.

The installable package also needs the already-approved SDK's `cfg_tool.bin` alongside the BLE
controller. The script includes it and declares `manifest.install` only when it is present;
without it, packaging can still succeed with `install: null`. That is not an install-ready package.
Our source has the root MIT `LICENSE`; SDK-derived source keeps Apache-2.0 attribution and the
public text `licenses/jieli-ac63-sdk.txt`, and the images are distributed under the SDK's
Apache-2.0 licence (`NOTICE`). The package script copies its SDK licence from `deps/` when
present, rather than falling back to that public text. Check the actual package's licence
inventory; a successful assembly alone is not a completeness guarantee without the SDK.

The guide's relative links must work in the package, not only in the checkout. Public project
Markdown is included under `reference/` and linked public text assets are copied there as needed;
`docbundle.py` rewrites guide/reference links to shipped files. Code-only references are labelled
source paths rather than broken links. Private/ignored trees and outward symlinks cannot be
pulled in through documentation, and the guide's required Markdown dependencies cannot be
silently omitted. `check_docs.py` checks the assembled package before it replaces an older one.
Source files, license texts and Markdown examples are not rewritten in the checkout.

Assembly happens in a sibling temporary workspace. The output cannot be the checkout or its
parent, a symlink, or an unrelated nonempty directory. Only a recognized previous dry package
may be replaced; assembly failure leaves it intact, commit failure restores it, and failed
rollback keeps its backup outside temporary cleanup. This is local artifact protection, not a
firmware-update atomicity guarantee. The command itself publishes nothing.

- **Reproducible** ✅ (2026-11-14): every image built again from scratch (`make -B`) has the same
  bytes and the same log dictionary: the build ID is a hash of the content, the container always
  builds in `/src`, and no build writes a time into an image. Two builds of each of the four images,
  4 of 4 the same.
- **Later build evidence (2026-11-15):** `make device` and the builder with `--twice` exited 0;
  4/4 images matched their forced second builds. The first application builds reused current
  objects. The required image tag was produced from cached BuildKit layers after a network
  metadata check, not a cold offline build. After the routing and hashing fixes (R06, R12) the
  four images were forced from scratch twice with contaminated routing flags: 4/4 binaries and
  dictionaries matched.
- **The publication check on the package**: `check_public.py`'s checks on every text file, and on
  each image the private strings (as bytes and as UTF-16, as USB strings hold them). Runs of
  32 bytes or more that the stock firmware also holds are counted per image and printed as a note,
  not a failure: they are the JieLi SDK's code and the toolchain's runtime, which the stock firmware
  is built from too (2026-11-14, by the controller's map: compiler-rt's `__adddf3` and `__muldf3`,
  the SDK's `sys_event_task`, `clk_early_init`, `free`, `syscfg_read`; in the BLE build also its
  radio and AES code). 3 runs in the bootloader, 163 in the controller, 346 with BLE, 164 in the
  hardware test. They are covered by the licences `NOTICE` names (the SDK's Apache-2.0 and the
  toolchain runtime's terms).
- **Left empty in the manifest:** the release's date, unless `--date` is given (the release
  workflow gives it; the manifest then says `"dry": false`).

### Publishing

Three GitHub Actions workflows in `.github/workflows/`, with `tools/release/publish.py`:

- **`ci.yml`** (pushes to `main`, pull requests): the publication checks, the release, firmware-cycle
  and Linux host tests, the editor's contract tests, and every image built twice and compared.
- **`release.yml`** (a tag `vX.Y.Z` naming `VERSION`): fetches the pinned toolchain and SDK, runs
  the publication checks, builds and packs every image twice (`release.py --twice --date`), then
  `publish.py expected` requires every image's build ID and SHA-256 to equal the candidate
  verified on the mixer (`release/expected.json`, written by `publish.py record`). It zips the
  package, fills the notes from `release/notes.md` and creates a draft GitHub release, a
  pre-release while the version is 0.x.
- **`pages.yml`** (a release published by the owner): `publish.py site` lays out the Pages site,
  the home page (`site/`) with that release's package at the root (`editor/`, `manifest.json`,
  `firmware/`) and every older published release's package under `/vX.Y.Z/`. The release event
  only starts the workflow again on the default branch (the `github-pages` environment deploys
  from it); running `pages.yml` by hand with a tag redeploys that release.

Releasing a version: set `VERSION` and the documents' version lines (`check_versions.py`),
install the candidate on the mixer and run its checks, `publish.py record build/release
release/expected.json`, commit, tag `vX.Y.Z` and push the tag. Review the draft release and
publish it; the site follows.

`tools/release/test_release.py` also checks image routing with local fake build scripts and runs
the actual launcher against a fake Docker executable with each hash utility alone on PATH.
These tests neither build firmware nor access a device. Synthetic documentation/output-safety tests
run even without device images: offline links, text assets, ignored/symlink boundaries, preserved code
examples, deterministic text packaging, protected directories and replacement/rollback failures.
Where images are built, its additional tests check the manifest and `SHA256SUMS`, versions,
license texts, complete editor, offline reference links and a planted private string in an image.
All are in `make quick`; only the actual-image cases are skipped without those builds.

## Measurements

Apple silicon Mac, Docker 29.9.2 (10 CPUs, 8 GB), amd64 emulated through QEMU (before Rosetta,
2026-11-14; "The host test image" has the newer numbers):

| What | Value |
|---|---|
| Container start + `clang --version` | under 1 s |
| Smoke build from scratch (`-j10`, 136 files + LTO link + `app.bin`) | 70 s |
| Application from scratch (`build_app.sh`, 41 files + LTO link + checks) | 22–28 s |
| Bootloader from scratch (`build_boot.sh`, 11 files + LTO link + checks) | 5 s |
| `make -C firmware test` | about 65 s (most of it in real time: the device runner's corpus 19 s, the first-run tool 13 s; the configuration tools' tests 13 s) |
| First `fetch_deps.sh` (both archives + SDK clone, run in parallel) | about 3.5 min |
| `fetch_deps.sh` again, everything present | under 1 s |
