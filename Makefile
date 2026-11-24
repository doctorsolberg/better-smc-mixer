# The project's checks in one place (AGENTS.md, "Commands"; docs/build.md, "Checks and tests").
#
#   make quick    host tests that take no real time: about 10 s, for the inner loop; then the
#                 editor's contract tests in the build image's Node (tools/env/test_editor.sh:
#                 needs Docker, says SKIPPED without it)
#   make test     every host test (about 3 min), with the editor's MixerDevice live against devhost
#                 (tools/editortest/live.py: Docker, SKIPPED without it)
#   make jlrom    build the ROM download tool if its source changed and run its self-test
#                 (docs/recovery.md); part of quick and test, so build/jlrom is never stale
#   make smcmidi  the same for the MIDI tool (the stock firmware's protocol, its erase and write)
#   make device   every device build with its image checks, in the Docker container:
#                 the bootloader, the application in both variants and with BLE, the portable code on the
#                 device toolchain, the RP2040 key. Needs Docker and deps/ (docs/build.md).
#   make check    test, then device: what "done" means for a change to device code
#   make sanitize the C tests and the fuzzer with AddressSanitizer and UBSan (build/host-san)
#   make fuzz [CASES=n] [SEED=n]
#                 the fuzzer of what a host sends the controller, with the sanitizers, at length
#   make coverage line coverage of the firmware's portable code by the host tests and the fuzzer
#                 (build/host-cov; tools/release/coverage.py)
#   make linux-py the plain Python tests on Linux, in the build image (tools/env/test_python_linux.sh)
#   make linux    every host test on Linux, in the build image: its gcc, Python and Node (tools/env/test_linux.sh)
#   make release  a dry release package in build/release/ (tools/release/release.py --twice: every
#                 image built twice and compared, the editor's static files, the manifest, the
#                 publication check on the package). Needs Docker and deps/. Publishes nothing
#   make e2e PORT=/dev/cu.usbmodemNN [E2E_ARGS="--transport cdc" or "--ble PORT" ...]
#                 the editor's configurations on the mixer (tools/e2e/e2e.py; docs/integration.md,
#                 T-3): push and pull, then the MIDI it sends against the simulator's. TALKS TO THE
#                 MIXER: only with the owner's go-ahead in the session (AGENTS.md)
#
# Apart from e2e, nothing here talks to the mixer or to any USB device.

.PHONY: quick test jlrom smcmidi swift-skipped device check e2e sanitize fuzz release linux-py linux coverage

# jlrom and smcmidi are Swift on macOS's IOUSBHost and CoreMIDI: built and self-tested there only,
# until their host parts are portable (docs/integration.md, "Any OS").
ifeq ($(shell uname -s 2>/dev/null),Darwin)
SWIFT_TOOLS := jlrom smcmidi
else
SWIFT_TOOLS := swift-skipped
endif

quick: $(SWIFT_TOOLS)
	$(MAKE) -C firmware quick
	tools/env/test_editor.sh

test: $(SWIFT_TOOLS)
	$(MAKE) -C firmware test
	tools/env/test_editor.sh
	python3 -I tools/editortest/live.py

swift-skipped:
	@echo "SKIPPED: jlrom and smcmidi (macOS only for now: docs/integration.md, \"Any OS\")"

# macOS only (IOUSBHost). The self-test needs no device and no root.
build/jlrom: tools/jlrom/main.swift
	@mkdir -p build
	swiftc -O -swift-version 5 -o $@ $<

jlrom: build/jlrom
	build/jlrom selftest

# macOS only (CoreMIDI). The self-test needs no device.
build/smcmidi: tools/smcmidi/main.swift
	@mkdir -p build
	swiftc -O -swift-version 5 -o $@ $<

smcmidi: build/smcmidi
	build/smcmidi selftest

device:
	tools/env/build_boot.sh
	tools/env/build_app.sh
	VARIANT=hwtest tools/env/build_app.sh
	BLE=1 tools/env/build_app.sh
	tools/env/check_device.sh
	tools/env/build_usbkey.sh

check: test device

sanitize:
	$(MAKE) -C firmware sanitize

fuzz:
	$(MAKE) -C firmware fuzz CASES=$(or $(CASES),100000) SEED=$(SEED)

coverage:
	$(MAKE) -C firmware coverage

linux-py:
	tools/env/test_python_linux.sh

linux:
	tools/env/test_linux.sh

release:
	python3 -I tools/release/release.py --twice

e2e:
	@test -n "$(PORT)" || { echo "make e2e needs PORT=the mixer's CDC port (it talks to the mixer)"; exit 2; }
	$(MAKE) -C firmware sim
	python3 -I tools/e2e/e2e.py --port $(PORT) $(E2E_ARGS)
