# Host builds of the firmware's portable code: unit tests (SPEC NFR-11).
#
#   make -C firmware test      build and run the C tests (smclog, USB descriptors, core, the BR25
#                              USB driver on a controller model, input, the command link, USB-MIDI
#                              packets, the control path, metrics, the boot journal, the
#                              configuration codec, the RP2040 key's logic), the Python tools'
#                              tests, the core scenarios and the firmware's side of the editor's
#                              contract tests (tools/editortest/test_vectors.py, the link's
#                              transcript transcripts.py against devhost), tools/hostmidi's and tools/e2e's tests:
#                              "quick" plus the slow suites
#   make -C firmware quick     the same without the suites that run in real time (the log tool's,
#                              the first-run tool's, the device scenario runner's, the
#                              configuration tools', smcconf pull/push against devhost): about 10 s,
#                              for the inner loop
#   make -C firmware sim       build the core simulator (build/host/smcsim, docs/core.md)
#   make -C firmware sanitize  the C tests and the fuzzer built with AddressSanitizer and
#                              UndefinedBehaviorSanitizer into build/host-san, and run
#   make -C firmware fuzz [CASES=n] [SEED=n]
#                              the fuzzer (tests/fuzz_device.c) with the sanitizers at length:
#                              100 000 cases from a seed of the clock unless given
#
# Device images are built in the container: tools/env/build_app.sh, tools/env/build_boot.sh.

HOST_CC ?= cc
BUILD ?= ../build/host
HOST_CFLAGS := -std=gnu11 -Wall -Wextra -Werror -O2 -g -fno-common
# Linux (tools/env/test_linux.sh): the GNU build ID the host log port reads (port/host), and glibc's
# full interface (memmem, the pseudo-terminal calls), which macOS gives without asking
ifeq ($(shell uname -s),Linux)
HOST_CFLAGS += -Wl,--build-id -D_GNU_SOURCE
endif
INCLUDES := -Ilib/smclog -Iport/host -Itests

CORE := core/core.c core/core_names.c core/core_config_test.c core/core_config_factory.c \
	core/core_settings.c
CORE_H := core/core.h core/core_config.h core/core_settings.h

SMCLOG := lib/smclog/smclog.c port/host/smclog_port_host.c
SMCLOG_H := $(wildcard lib/smclog/*.h port/host/*.h)

TUSB := ../third_party/tinyusb/src

METRICS := metrics/metrics.c metrics/usclock.c
METRICS_H := metrics/metrics.h metrics/usclock.h

DIAG := diag/diag.c
DIAG_H := diag/diag.h

# The configuration's binary form (docs/config.md)
CONFIG := config/cfgbin.c
CONFIG_H := config/cfgbin.h config/cfg_schema.h core/core_config.h

# The configuration in flash: two slots (docs/config.md, "Storage")
STORE := store/cfgstore.c store/statestore.c store/bootsel.c
STORE_H := store/cfgstore.h store/statestore.h store/bootsel.h

.PHONY: test quick sim clean sanitize fuzz coverage

HOST_BINS := $(BUILD)/test_smclog $(BUILD)/test_boot_descriptors $(BUILD)/test_core $(BUILD)/smcsim \
		$(BUILD)/test_dcd_br25 $(BUILD)/test_input $(BUILD)/test_link $(BUILD)/test_app_descriptors \
		$(BUILD)/test_midi $(BUILD)/test_ctl $(BUILD)/test_metrics $(BUILD)/test_diag $(BUILD)/devhost \
		$(BUILD)/test_cfgbin $(BUILD)/cfgtool $(BUILD)/test_key $(BUILD)/test_cfglink \
		$(BUILD)/test_cfgstore $(BUILD)/test_statestore $(BUILD)/test_blemidi $(BUILD)/test_bleq \
		$(BUILD)/test_bootsel $(BUILD)/test_input_step50

# Everything that takes no real time: the C tests, the scenarios, the tools' fast tests.
quick: $(HOST_BINS)
	python3 -I ../tools/smcconf/gen.py --check
	$(BUILD)/test_smclog
	$(BUILD)/test_boot_descriptors
	$(BUILD)/test_app_descriptors
	$(BUILD)/test_core
	$(BUILD)/test_dcd_br25
	$(BUILD)/test_input
	$(BUILD)/test_input_step50
	$(BUILD)/test_link
	$(BUILD)/test_cfglink
	$(BUILD)/test_cfgstore
	$(BUILD)/test_statestore
	$(BUILD)/test_bootsel
	$(BUILD)/test_midi
	$(BUILD)/test_blemidi
	$(BUILD)/test_bleq
	$(BUILD)/test_ctl
	$(BUILD)/test_metrics
	$(BUILD)/test_diag
	$(BUILD)/test_cfgbin
	$(BUILD)/test_key
	python3 -I ../tools/smcsim/smcsim.py run-all tests/scenarios --sim $(BUILD)/smcsim
	python3 -I ../tools/smcsim/test_smcsim.py CoreLog
	python3 -I ../tools/editortest/test_vectors.py
	python3 -I ../tools/editortest/transcripts.py
	python3 -I ../tools/hostmidi/test_hostmidi.py
	python3 -I ../tools/hostserial/test_hostserial.py
	python3 -I ../tools/e2e/test_e2e.py
	python3 -I -m unittest discover -s ../tools/fwcycle -p 'test_*.py'
	python3 -I ../tools/image/test_flashimg.py
	python3 -I ../tools/image/test_installplan.py
	python3 -I ../tools/jlrom/test_compare.py
	python3 -I ../tools/re/test_rampath.py
	python3 -I ../tools/stage0/test_capture_report.py
	python3 -I ../tools/usbkey/test_uf2.py
	python3 -I ../tools/release/test_check_public.py
	python3 -I ../tools/release/test_check_docs.py
	python3 -I ../tools/release/check_versions.py
	python3 -I ../tools/release/test_check_versions.py
	python3 -I ../tools/release/test_release.py

# The suites that run in real time (against devhost on a pseudo-terminal) or fuzz at length.
test: quick $(BUILD)/fuzz_device
	$(BUILD)/fuzz_device 2000 1
	python3 -I ../tools/smclog/test_smclog.py
	python3 -I ../tools/smclog/test_firstrun.py
	python3 -I ../tools/smcsim/test_smcsim.py
	python3 -I ../tools/smcconf/test_smcconf.py
	python3 -I ../tools/smcconf/test_device.py

sim: $(BUILD)/smcsim $(BUILD)/devhost $(BUILD)/cfgtool

# The sanitizers (night plan N-2): every C test and the fuzzer, built apart in build/host-san.
SAN_BUILD := ../build/host-san
SAN_CFLAGS := -std=gnu11 -Wall -Wextra -Werror -O1 -g -fno-common -fsanitize=address,undefined \
	-fno-sanitize-recover=undefined
SAN_TESTS := test_smclog test_boot_descriptors test_core test_dcd_br25 test_input test_link \
	test_app_descriptors test_midi test_ctl test_metrics test_diag test_cfgbin test_key test_cfglink \
	test_cfgstore test_statestore test_blemidi test_bleq test_bootsel
CASES ?= 100000
SEED ?=

sanitize:
	$(MAKE) BUILD=$(SAN_BUILD) HOST_CFLAGS="$(SAN_CFLAGS)" $(addprefix $(SAN_BUILD)/,$(SAN_TESTS) fuzz_device)
	@set -e; for t in $(SAN_TESTS); do echo "$$t (sanitizers)"; $(SAN_BUILD)/$$t > $(SAN_BUILD)/$$t.log 2>&1 \
		|| { tail -20 $(SAN_BUILD)/$$t.log; exit 1; }; done
	$(SAN_BUILD)/fuzz_device 3000 1

# Line coverage (night plan N-12): every C test and the fuzzer built with --coverage in
# build/host-cov, run, and their counters merged by tools/release/coverage.py.
COV_BUILD := ../build/host-cov
COV_CFLAGS := -std=gnu11 -Wall -Wextra -Werror -O0 -g -fno-common --coverage

coverage:
	@mkdir -p $(COV_BUILD) && rm -f $(COV_BUILD)/*.gcda
	$(MAKE) BUILD=$(COV_BUILD) HOST_CFLAGS="$(COV_CFLAGS)" $(addprefix $(COV_BUILD)/,$(SAN_TESTS) fuzz_device)
	@set -e; for t in $(SAN_TESTS); do $(COV_BUILD)/$$t > $(COV_BUILD)/$$t.log 2>&1 \
		|| { tail -20 $(COV_BUILD)/$$t.log; exit 1; }; done
	$(COV_BUILD)/fuzz_device 3000 1 > $(COV_BUILD)/fuzz_device.log
	python3 -I ../tools/release/coverage.py $(COV_BUILD) --report $(COV_BUILD)/uncovered.txt

fuzz:
	$(MAKE) BUILD=$(SAN_BUILD) HOST_CFLAGS="$(SAN_CFLAGS)" $(SAN_BUILD)/fuzz_device
	$(SAN_BUILD)/fuzz_device $(CASES) $(SEED)

# The RP2040 recovery key's logic against a model of the chip (docs/recovery.md); the key's image is
# built and checked by tools/env/build_usbkey.sh, which runs this test too.
USBKEY := ../tools/usbkey
$(BUILD)/test_key: $(USBKEY)/test_key.c $(USBKEY)/key.c $(USBKEY)/key.h
	@mkdir -p $(BUILD)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -o $@ $(USBKEY)/test_key.c $(USBKEY)/key.c

# Small rings, so the tests reach overflow and wrap-around quickly (the recent ring smaller than the
# main one, so a replay fits).
$(BUILD)/test_smclog: tests/test_smclog.c tests/check.h $(SMCLOG) $(SMCLOG_H)
	@mkdir -p $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -DSMCLOG_RING_SIZE=512 -DSMCLOG_RECENT_SIZE=256 $(INCLUDES) -o $@ tests/test_smclog.c $(SMCLOG)

# The bootloader's descriptors with its TinyUSB configuration; TinyUSB headers as system headers.
$(BUILD)/test_boot_descriptors: tests/test_boot_descriptors.c tests/check.h boot/usb_descriptors.c \
		boot/tusb_config.h
	@mkdir -p $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -Itests -Iboot -isystem $(TUSB) -o $@ tests/test_boot_descriptors.c \
		boot/usb_descriptors.c

# The application's descriptors with its TinyUSB configuration.
$(BUILD)/test_app_descriptors: tests/test_app_descriptors.c tests/check.h app/usb/usb_descriptors.c \
		app/usb/tusb_config.h app/usb/usb_app.h link/link.h app/include/app_version.h
	@mkdir -p $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -Itests -Iapp/usb -Iapp/include -Ilink -Ilib/smclog -isystem $(TUSB) -o $@ \
		tests/test_app_descriptors.c app/usb/usb_descriptors.c

# The BR25 USB controller driver against a model of the controller (docs/usb.md), with all five
# endpoints.
DCD_BR25 := platform/br25/usb/dcd_br25.c
DCD_BR25_H := platform/br25/usb/br25_usb_regs.h platform/br25/usb/dcd_br25.h
$(BUILD)/test_dcd_br25: tests/test_dcd_br25.c tests/check.h $(DCD_BR25) $(DCD_BR25_H) tests/dcd_br25/tusb_config.h \
		$(SMCLOG) $(SMCLOG_H)
	@mkdir -p $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -DBR25_USB_MODEL $(INCLUDES) -Itests/dcd_br25 -Iplatform/br25/usb -isystem $(TUSB) \
		-o $@ tests/test_dcd_br25.c $(DCD_BR25) $(SMCLOG)

# The scan layer, the input event queue and input injection (docs/hwtest.md).
INPUT := input/scan.c input/evq.c input/inject.c
INPUT_H := input/scan.h input/evq.h input/inject.h
$(BUILD)/test_input: tests/test_input.c tests/check.h $(INPUT) $(INPUT_H) $(CORE_H) $(SMCLOG) $(SMCLOG_H)
	@mkdir -p $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(INCLUDES) -Icore -Iinput -o $@ tests/test_input.c $(INPUT) $(SMCLOG)

# The same tests at the former 50 µs scan step: the defaults are times, not step counts.
$(BUILD)/test_input_step50: tests/test_input.c tests/check.h $(INPUT) $(INPUT_H) $(CORE_H) $(SMCLOG) $(SMCLOG_H)
	@mkdir -p $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -DSCAN_STEP_US=50 $(INCLUDES) -Icore -Iinput -o $@ tests/test_input.c $(INPUT) $(SMCLOG)

# The command link (docs/link.md); also the fake device of the smclog tool's tests (--serve).
LINK := link/link.c link/sxpipe.c
$(BUILD)/test_link: tests/test_link.c tests/check.h $(LINK) link/link.h link/sxpipe.h $(SMCLOG) $(SMCLOG_H)
	@mkdir -p $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(INCLUDES) -Ilink -o $@ tests/test_link.c $(LINK) $(SMCLOG)

# USB-MIDI packets and the packet queue (firmware/midi).
MIDI := midi/usbmidi.c midi/midiq.c midi/bleq.c
MIDI_H := midi/usbmidi.h midi/midiq.h midi/bleq.h
$(BUILD)/test_midi: tests/test_midi.c tests/check.h $(MIDI) $(MIDI_H)
	@mkdir -p $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -Itests -Imidi -o $@ tests/test_midi.c $(MIDI)

# BLE-MIDI packets (firmware/midi/blemidi.c; docs/ble.md, "Design").
$(BUILD)/test_blemidi: tests/test_blemidi.c tests/check.h midi/blemidi.c midi/blemidi.h
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) -Itests -Imidi -o $@ tests/test_blemidi.c midi/blemidi.c

$(BUILD)/test_bleq: tests/test_bleq.c tests/check.h midi/bleq.c midi/bleq.h
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) -Itests -Imidi -o $@ tests/test_bleq.c midi/bleq.c

# The control path on the device (firmware/ctl): the event queue, the core on the factory
# configuration, MIDI packets out, LEDs, a received configuration.
$(BUILD)/test_ctl: tests/test_ctl.c tests/check.h ctl/ctl.c ctl/ctl.h input/evq.c input/evq.h $(MIDI) \
		$(MIDI_H) $(CORE) $(CORE_H) $(CONFIG) $(CONFIG_H) $(METRICS) $(METRICS_H) $(SMCLOG) $(SMCLOG_H)
	@mkdir -p $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(INCLUDES) -Icore -Iconfig -Ictl -Iinput -Imidi -Imetrics -o $@ \
		tests/test_ctl.c ctl/ctl.c input/evq.c $(MIDI) $(CORE) $(CONFIG) $(METRICS) $(SMCLOG)

# The configuration in flash on a model of NOR flash, with power cuts (firmware/store/cfgstore.c).
$(BUILD)/test_cfgstore: tests/test_cfgstore.c tests/check.h $(STORE) $(STORE_H) $(CONFIG) $(CONFIG_H) $(CORE) \
		$(CORE_H) $(SMCLOG) $(SMCLOG_H)
	@mkdir -p $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(INCLUDES) -Icore -Iconfig -Istore -o $@ tests/test_cfgstore.c $(STORE) $(CONFIG) \
		$(CORE) $(SMCLOG)

# Sector 0's boot pointer back at M-Vave's area (firmware/store/bootsel.c), on flashes from
# tools/image's model.
$(BUILD)/test_bootsel: tests/test_bootsel.c tests/check.h store/bootsel.c store/bootsel.h store/cfgstore.h \
		$(BUILD)/bootsel_vec.bin
	@mkdir -p $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(INCLUDES) -Istore -DBOOTSEL_VEC='"$(abspath $(BUILD))/bootsel_vec.bin"' -o $@ \
		tests/test_bootsel.c store/bootsel.c

$(BUILD)/bootsel_vec.bin: tests/bootsel_vec.py ../tools/image/flashimg.py ../tools/image/installplan.py \
		../tools/image/test_installplan.py
	@mkdir -p $(BUILD)
	python3 -I tests/bootsel_vec.py $@

# The panel's state in flash on the same model, with power cuts (firmware/store/statestore.c).
$(BUILD)/test_statestore: tests/test_statestore.c tests/check.h $(STORE) $(STORE_H) $(CONFIG) $(CONFIG_H) $(CORE) \
		$(CORE_H) $(SMCLOG) $(SMCLOG_H)
	@mkdir -p $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(INCLUDES) -Icore -Iconfig -Istore -o $@ tests/test_statestore.c $(STORE) $(CONFIG) \
		$(CORE) $(SMCLOG)

# The configuration protocol (firmware/link/cfglink.c, docs/link.md, "Configuration") through the
# command link, on the control path with the core.
CFGLINK := link/cfglink.c
$(BUILD)/test_cfglink: tests/test_cfglink.c tests/check.h $(CFGLINK) link/cfglink.h $(LINK) link/link.h \
		ctl/ctl.c ctl/ctl.h input/evq.c input/evq.h $(MIDI) $(MIDI_H) $(CORE) $(CORE_H) $(CONFIG) \
		$(CONFIG_H) $(STORE) $(STORE_H) $(METRICS) $(METRICS_H) $(SMCLOG) $(SMCLOG_H)
	@mkdir -p $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(INCLUDES) -Icore -Iconfig -Istore -Ictl -Iinput -Ilink -Imidi -Imetrics -o $@ \
		tests/test_cfglink.c $(CFGLINK) $(LINK) ctl/ctl.c input/evq.c $(MIDI) $(CORE) $(CONFIG) \
		$(STORE) $(METRICS) $(SMCLOG)

# The metrics' accumulators, the heartbeat's records and the microsecond clock (firmware/metrics).
$(BUILD)/test_metrics: tests/test_metrics.c tests/check.h $(METRICS) $(METRICS_H) $(SMCLOG) $(SMCLOG_H)
	@mkdir -p $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(INCLUDES) -Imetrics -o $@ tests/test_metrics.c $(METRICS) $(SMCLOG)

# What a reset keeps, the crash record and the boot journal (firmware/diag).
$(BUILD)/test_diag: tests/test_diag.c tests/check.h $(DIAG) $(DIAG_H) $(SMCLOG) $(SMCLOG_H)
	@mkdir -p $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(INCLUDES) -Idiag -o $@ tests/test_diag.c $(DIAG) $(SMCLOG)

# The controller's portable code behind the command link, for the device scenario runner's tests
# (tools/smcsim, "smcsim device").
$(BUILD)/devhost: sim/devhost.c ctl/ctl.c ctl/ctl.h input/evq.c input/evq.h input/inject.c \
		input/inject.h $(LINK) link/link.h $(CFGLINK) link/cfglink.h $(MIDI) $(MIDI_H) $(CORE) $(CORE_H) \
		$(CONFIG) $(CONFIG_H) $(STORE) $(STORE_H) $(METRICS) $(METRICS_H) $(DIAG) $(DIAG_H) $(SMCLOG) \
		$(SMCLOG_H) app/include/app_version.h
	@mkdir -p $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(INCLUDES) -Icore -Iconfig -Istore -Ictl -Iinput -Ilink -Imidi -Imetrics -Idiag \
		-o $@ sim/devhost.c ctl/ctl.c input/evq.c input/inject.c $(LINK) $(CFGLINK) $(MIDI) $(CORE) \
		$(CONFIG) $(STORE) $(METRICS) $(DIAG) $(SMCLOG)

# The fuzzer of everything a host sends the controller (tests/fuzz_device.c): the same wiring as
# devhost and test_cfglink, with mutated frames, commands, configurations, MIDI and power cuts.
$(BUILD)/fuzz_device: tests/fuzz_device.c ctl/ctl.c ctl/ctl.h input/evq.c input/evq.h $(LINK) link/link.h \
		$(CFGLINK) link/cfglink.h $(MIDI) $(MIDI_H) midi/blemidi.c midi/blemidi.h $(CORE) $(CORE_H) $(CONFIG) \
		$(CONFIG_H) $(STORE) $(STORE_H) $(METRICS) $(METRICS_H) $(SMCLOG) $(SMCLOG_H)
	@mkdir -p $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(INCLUDES) -Icore -Iconfig -Istore -Ictl -Iinput -Ilink -Imidi -Imetrics -o $@ \
		tests/fuzz_device.c ctl/ctl.c input/evq.c $(LINK) $(CFGLINK) $(MIDI) midi/blemidi.c $(CORE) $(CONFIG) \
		$(STORE) $(METRICS) $(SMCLOG)

# The configuration's binary form (docs/config.md), its tests and the host tool tools/smcconf's
# tests use; cfg_schema.h is generated from config/schema.json (tools/smcconf/gen.py). CONFIG and
# CONFIG_H are defined at the top.
$(BUILD)/test_cfgbin: tests/test_cfgbin.c tests/check.h $(CONFIG) $(CONFIG_H) $(CORE) $(CORE_H) $(SMCLOG) $(SMCLOG_H)
	@mkdir -p $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(INCLUDES) -Icore -Iconfig -o $@ tests/test_cfgbin.c $(CONFIG) $(CORE) $(SMCLOG)

$(BUILD)/cfgtool: sim/cfgtool.c $(CONFIG) $(CONFIG_H) $(CORE) $(CORE_H) $(SMCLOG) $(SMCLOG_H)
	@mkdir -p $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(INCLUDES) -Icore -Iconfig -o $@ sim/cfgtool.c $(CONFIG) $(CORE) $(SMCLOG)

# The control core with the host smclog port (docs/core.md).
$(BUILD)/test_core: tests/test_core.c tests/check.h $(CORE) $(CORE_H) $(SMCLOG) $(SMCLOG_H)
	@mkdir -p $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(INCLUDES) -Icore -o $@ tests/test_core.c $(CORE) $(SMCLOG)

# The core simulator (docs/core.md): the core, the test configuration, the host smclog port; the
# configuration codec for --config.
$(BUILD)/smcsim: sim/smcsim.c $(CORE) $(CORE_H) $(CONFIG) $(CONFIG_H) $(SMCLOG) $(SMCLOG_H)
	@mkdir -p $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(INCLUDES) -Icore -Iconfig -o $@ sim/smcsim.c $(CONFIG) $(CORE) $(SMCLOG)
