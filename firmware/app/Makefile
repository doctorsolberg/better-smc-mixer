# SMC-Mixer application for BR25 on the JieLi AC63 SDK.
#
# Run in the build container by tools/env/build_app.sh (make -C /src/firmware/app). SDK sources,
# headers, libraries and the linker script template come from an SDK copy (SDK); our sources from
# FW; everything generated goes to OUT.
#
# VARIANT (app/include/variant.h): app, the controller (default), or hwtest, the hardware test
# (docs/hwtest.md). Each variant builds into its own OUT.
#
# Derived from the SDK's apps/hid/board/br25/Makefile (Apache License 2.0, see NOTICE): the
# compiler and linker flags, defines, include paths and libraries are the SDK's. Changes: our
# source list and include directories first; IPRA off (it breaks weak functions); our code built
# with warnings as errors and the SDK headers as system headers; the generated linker script is
# patched (sdk_ld_patch.py) and takes --build-id; objects and outputs in OUT; no post-build
# packing (app.bin is cut by build_app.sh).

SDK ?= /src/build/app/sdk
FW ?= /src/firmware
VARIANT ?= app
OUT ?= /src/build/$(VARIANT)

TOOL_DIR := /opt/jieli/pi32v2/bin
CC := $(TOOL_DIR)/clang
LD := $(TOOL_DIR)/lto-wrapper
SYS_LIB_DIR := $(TOOL_DIR)/../lib/r3
SYS_INC_DIR := $(TOOL_DIR)/../include

ELF := $(OUT)/app.elf

CFLAGS := \
	-target pi32v2 \
	-mcpu=r3 \
	-integrated-as \
	-flto \
	-Wuninitialized \
	-Wno-invalid-noreturn \
	-fno-common \
	-Oz \
	-g \
	-fallow-pointer-null \
	-fprefer-gnu-section \
	-Wno-shift-negative-value \
	-Wundef \
	-Wframe-larger-than=256 \
	-Wincompatible-pointer-types \
	-Wreturn-type \
	-Wimplicit-function-declaration \
	-fms-extensions

SDK_WARN := -w
# SMC_RAM_SECTIONS: what the scan interrupt reaches goes to RAM (firmware/lib/smclog/smc_ram.h).
FW_WARN := -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -DSMC_RAM_SECTIONS

# BOOTMARK=1: the boot marks of the diagnostic build (platform/br25/bootmark_br25.h).
BOOTMARK ?=
DEFINES := \
	-DSUPPORT_MS_EXTENSIONS \
	-DCONFIG_RELEASE_ENABLE \
	-DCONFIG_CPU_BR25 \
	-DCONFIG_PRINT_IN_MASK \
	-DCONFIG_EQ_SUPPORT_ASYNC \
	-DCONFIG_MIXER_CYCLIC \
	-DCONFIG_FREE_RTOS_ENABLE \
	-DCONFIG_MMU_ENABLE \
	-DCONFIG_SBC_CODEC_HW \
	-DCONFIG_MSBC_CODEC_HW \
	-DCONFIG_AEC_M=256 \
	-DCONFIG_AUDIO_ONCHIP \
	-DCONFIG_MEDIA_DEVELOP_ENABLE \
	-D__GCC_PI32V2__ \
	-DCONFIG_NEW_ECC_ENABLE \
	-DEVENT_HANDLER_NUM_CONFIG=2 \
	-DEVENT_TOUCH_ENABLE_CONFIG=0 \
	-DEVENT_POOL_SIZE_CONFIG=256 \
	-DCONFIG_EVENT_KEY_MAP_ENABLE=0 \
	-DTIMER_POOL_NUM_CONFIG=10 \
	-DAPP_ASYNC_POOL_NUM_CONFIG=0 \
	-DSDK_VERSION_CFG_DEFINE=0x240001 \
	-DSDK_VERSION_DATE_DEFINE=20241231 \
	-DVFS_ENABLE=1 \
	-DUSE_SDFILE_NEW=1 \
	-DVFS_FILE_POOL_NUM_CONFIG=0 \
	-DSDFILE_VERSION=0x020000 \
	-DVM_MAX_SIZE_CONFIG=16*1024 \
	-DVM_ITEM_MAX_NUM=256 \
	-DCONFIG_TWS_ENABLE \
	-DCONFIG_HID_CASE_ENABLE \
	-DCONFIG_TRANSFER_ENABLE \
	-DCONFIG_LITE_AEC_ENABLE=0 \
	-DCONFIG_LITE_AUDIO \
	-DCONFIG_LMP_RESET_ENABLE \
	-DCONFIG_LMP_REFRESH_ENCRYPTION_KEY_ENABLE \
	-DCONFIG_LINK_DISTURB_SCAN_ENABLE=0 \
	-DCONFIG_BTCTRLER_TASK_DEL_ENABLE \
	-DCONFIG_UPDATA_ENABLE \
	-DCONFIG_OTA_UPDATA_ENABLE \
	-DCONFIG_ITEM_FORMAT_VM \
	-DTCFG_APP_BT_EN=1 \
	-DUSB_PC_NO_APP_MODE \
	-D__SHELL__

# Ours first: app_config.h and board_config.h shadow the hid example's.
FW_INCLUDES := \
	-I$(FW)/app/include \
	-I$(FW)/app/board \
	-I$(FW)/app/usb \
	-I$(FW)/app/ble \
	-I$(FW)/lib/smclog \
	-I$(FW)/link \
	-I$(FW)/config \
	-I$(FW)/store \
	-I$(FW)/core \
	-I$(FW)/ctl \
	-I$(FW)/diag \
	-I$(FW)/input \
	-I$(FW)/midi \
	-I$(FW)/metrics \
	-I$(FW)/platform/br25 \
	-I$(FW)/platform/br25/usb

# TinyUSB (third_party/, frozen): its headers as system headers in our code.
TUSB ?= /src/third_party/tinyusb/src
TUSB_INCLUDES := -isystem $(TUSB)

SDK_INCLUDE_DIRS := \
	include_lib \
	include_lib/driver \
	include_lib/driver/device \
	include_lib/driver/cpu/br25 \
	include_lib/system \
	include_lib/system/generic \
	include_lib/system/device \
	include_lib/system/fs \
	include_lib/system/ui \
	include_lib/system/math/cpu/br25 \
	include_lib/btctrler \
	include_lib/btctrler/port/br25 \
	include_lib/update \
	include_lib/agreement \
	include_lib/btstack/third_party/common \
	include_lib/btstack/third_party/rcsp \
	include_lib/media/media_develop \
	include_lib/media/media_develop/media \
	include_lib/media/media_develop/media/cpu/br25 \
	include_lib/media/media_develop/media/cpu/br25/asm \
	apps/hid/include \
	apps/hid/board/br25 \
	apps/common \
	apps/common/include \
	apps/common/third_party_profile/jieli \
	apps/common/third_party_profile/jieli/JL_rcsp \
	apps/common/third_party_profile/jieli/JL_rcsp/rcsp_updata \
	apps/common/third_party_profile/jieli/JL_rcsp/bt_trans_data \
	apps/common/third_party_profile/jieli/gatt_common \
	apps/common/third_party_profile/jieli/online_db \
	apps/common/third_party_profile/jieli/trans_data_demo \
	apps/common/third_party_profile/common \
	apps/common/device \
	apps/common/device/usb \
	include_lib/ai_stack/JL_rcsp \
	include_lib/btstack \
	cpu/br25 \
	include_lib/system/ex_mcu \
	apps/common/code_switch/include \
	apps/common/device/optical_mouse_sensor/include \
	apps/common/device/touch_pad \
	apps/common/audio \
	cpu/br25/audio_dec \
	cpu/br25/audio_enc \
	apps/common/device/usb/device \
	apps/common/device/usb/host

SDK_INCLUDES := $(addprefix -I$(SDK)/,$(SDK_INCLUDE_DIRS)) -I$(SYS_INC_DIR)
SDK_SYSINCLUDES := $(addprefix -isystem $(SDK)/,$(SDK_INCLUDE_DIRS)) -isystem $(SYS_INC_DIR)

# SDK sources compiled as they are, with our configuration headers.
SDK_SRC := \
	apps/common/debug/debug.c \
	apps/common/update/testbox_update.c \
	apps/common/update/uart_update.c \
	apps/common/update/update.c \
	apps/hid/config/lib_btctrler_config.c \
	apps/hid/config/lib_btstack_config.c \
	apps/hid/config/lib_driver_config.c \
	apps/hid/config/lib_media_config.c \
	apps/hid/config/lib_profile_config.c \
	apps/hid/config/lib_system_config.c \
	apps/hid/config/log_config.c \
	apps/hid/modules/misc.c \
	apps/hid/modules/power/app_power_manage.c \
	apps/hid/modules/user_cfg.c \
	apps/hid/version.c \
	cpu/br25/adc_api.c \
	cpu/br25/charge.c \
	cpu/br25/chargestore.c \
	cpu/br25/clock_manager.c \
	cpu/br25/port_wkup.c \
	cpu/br25/pwm_led.c \
	cpu/br25/setup.c \
	cpu/br25/uart_dev.c

# Derived from SDK files, built like them (their code is the SDK's).
DERIVED_SRC := \
	app/board/board_smc_mixer.c \
	app/config/lib_update_config.c

# Our code: the common part and the variant's.
FW_SRC := \
	app/app_main.c \
	app/metrics_app.c \
	app/usb/usb_app.c \
	app/usb/usb_descriptors.c \
	link/link.c \
	link/sxpipe.c \
	lib/smclog/smclog.c \
	midi/midiq.c \
	midi/usbmidi.c \
	metrics/metrics.c \
	metrics/usclock.c \
	input/scan.c \
	input/evq.c \
	input/inject.c \
	diag/diag.c \
	platform/br25/diag_br25.c \
	platform/br25/input_br25.c \
	platform/br25/smclog_port_br25.c \
	platform/br25/usb/dcd_br25.c

# TinyUSB, built as it is: device stack, the CDC and MIDI classes. Its warnings are shown, not
# fatal.
TUSB_SRC := \
	tusb.c \
	common/tusb_fifo.c \
	device/usbd.c \
	class/cdc/cdc_device.c \
	class/midi/midi_device.c
TUSB_WARN := -std=gnu11 -Wall -Wno-unused-parameter

ifeq ($(BOOTMARK),1)
DEFINES += -DSMC_BOOTMARK -DDIAG_STEP_HOOK=bootmark_step -DSMC_STALL_CHECK
endif
# SYS_CLOCK_MHZ=n: the CPU clock set to n MHz first thing in app_main() (app_main.c); 192 by default
# since controller 15 (docs/core.md, "The CPU clock"); SYS_CLOCK_MHZ= (empty) keeps the board's
# TCFG_CLOCK_SYS_HZ (96 MHz), at which the system starts
SYS_CLOCK_MHZ ?= 192
ifneq ($(SYS_CLOCK_MHZ),)
DEFINES += -DSMC_SYS_CLOCK_HZ=$(SYS_CLOCK_MHZ)000000L
endif
# SCAN_PROFILE=1: the scan handler's time phase by phase, logged each heartbeat (input_br25.c); off by default
ifeq ($(SCAN_PROFILE),1)
DEFINES += -DSMC_SCAN_PROFILE
endif
# NO_SDK_ADC=1: the SDK's 2 ms ADC scan held in the occupy mode (board_smc_mixer.c); off by default
ifeq ($(NO_SDK_ADC),1)
DEFINES += -DSMC_NO_SDK_ADC
endif
# ADC_DELAY=n: the fader ADC's start delay, n x 8 ADC clocks (1-15; input_br25.c); 4 by default
ifneq ($(ADC_DELAY),)
DEFINES += -DSMC_ADC_DELAY=$(ADC_DELAY)
endif
# LED_DARK=1: no LED column is ever driven, a diagnostic for the LEDs' faint glow (input_br25.c);
# LED_DARK=2: also PB5 (⚠️ the 595s' /OE) high, so their outputs are off: no button works; off by
# default
ifeq ($(LED_DARK),1)
DEFINES += -DSMC_LED_DARK
endif
ifeq ($(LED_DARK),2)
DEFINES += -DSMC_LED_DARK -DSMC_LED_OE_OFF
endif
# ADC_DIV=n: the fader ADC's clock divider code (input_br25.c): 7 (/128) by default, for the CPU at
# 192 MHz (the ADC's source is then 64 MHz); 6 is the SDK's /96, right for 96 MHz
ADC_DIV ?= 7
ifneq ($(ADC_DIV),)
DEFINES += -DSMC_ADC_DIV=$(ADC_DIV)
endif
# CORE_RAM=0: the control path's code stays in flash; by default it runs from RAM (SMC_HOT,
# smc_ram.h; docs/core.md, "Where the time goes": about 7 KB of the heap)
CORE_RAM ?= 1
ifeq ($(CORE_RAM),1)
DEFINES += -DSMC_CORE_RAM
endif
# CFG_BUF=n: the configuration protocol's transfer buffer in bytes (link/cfglink.h): 16384 by
# default; at most CFGBIN_MAX (30128) is used
ifneq ($(CFG_BUF),)
DEFINES += -DCFGLINK_BUF=$(CFG_BUF)
endif
# CTL_PROBE=1: every 32nd event through the core with interrupts off, timed (ctl.c); off by default
ifeq ($(CTL_PROBE),1)
DEFINES += -DSMC_CTL_PROBE
endif
# EMU_CHAIN=1: our IRQ 0 handler that passes the SDK's breakpoints on (diag_br25.c); off by default
ifeq ($(EMU_CHAIN),1)
DEFINES += -DSMC_EMU_CHAIN
endif

# BLE=1: BLE-MIDI on the SDK's BLE stack (app/ble/ble_br25.c, docs/ble.md; the SDK's GATT module
# built as it is); off by default, ble_stub.c stands in
BLE ?=
# BLE_SM=0 is refused: without the security manager the SDK crashes at start
ifeq ($(BLE),1)
DEFINES += -DSMC_BLE
ifeq ($(BLE_SM),0)
$(error BLE_SM=0: the SDK's att_server_init needs the security manager (docs/ble.md, "Without the security manager"))
endif
# BLE_NAME=NAME: advertise another name (letters, digits, '-' and '_'; for trying the vendor's
# adapter with it, SPEC R-11)
ifneq ($(BLE_NAME),)
ifneq ($(shell printf '%s' '$(BLE_NAME)' | tr -d 'A-Za-z0-9_-'),)
$(error BLE_NAME: letters, digits, '-' and '_' only)
endif
DEFINES += -DBLE_NAME=\"$(BLE_NAME)\"
endif
# BLE_ADDR_XOR=N (1-15): our BLE address against M-Vave's (the low bits of one byte flipped;
# 5 by default, ble_br25.c); a test build may choose another
ifneq ($(BLE_ADDR_XOR),)
DEFINES += -DBLE_ADDR_XOR=$(BLE_ADDR_XOR)
endif
SDK_SRC += \
	apps/common/third_party_profile/jieli/gatt_common/le_gatt_common.c \
	apps/common/third_party_profile/jieli/gatt_common/le_gatt_server.c
FW_SRC += app/ble/ble_br25.c midi/blemidi.c
else
FW_SRC += app/ble_stub.c
endif

ifeq ($(VARIANT),app)
FW_SRC += \
	app/controller.c \
	core/core.c \
	core/core_config_factory.c \
	core/core_config_test.c \
	core/core_settings.c \
	config/cfgbin.c \
	link/cfglink.c \
	store/cfgstore.c \
	store/statestore.c \
	store/bootsel.c \
	platform/br25/flash_br25.c \
	midi/bleq.c \
	ctl/ctl.c
else ifeq ($(VARIANT),hwtest)
FW_SRC += app/hwtest/hwtest.c
else
$(error VARIANT must be app or hwtest, not '$(VARIANT)')
endif

LIBS := $(addprefix $(SDK)/cpu/br25/liba/, \
	cpu.a system.a btstack.a btctrler.a crypto_toolbox_Osize.a agreement.a rcsp_stack.a update.a \
	aec.a media.a media_app.a libAptFilt_pi32v2_OnChip.a libEchoSuppress_pi32v2_OnChip.a \
	libNoiseSuppress_pi32v2_OnChip.a libSplittingFilter_pi32v2_OnChip.a libFFT_pi32v2_OnChip.a \
	libOpcore_maskrom_pi32v2_OnChip.a lib_resample_cal.a lib_resample_fast_cal.a opus_enc_lib.a \
	speex_enc_lib.a lib_adpcm_ima_code.a lib_adpcm_code.a lc3_codec_lib.a bfilterfun_lib.a \
	wtg_dec_lib.a wtgv2_dec_lib.a wav_dec_lib.a mp3_dec_lib.a lib_pitch_speed.a lib_midi_dec.a)

LFLAGS := \
	--plugin-opt=-pi32v2-always-use-itblock=false \
	--plugin-opt=-enable-ipra=false \
	--plugin-opt=-pi32v2-merge-max-offset=4096 \
	--plugin-opt=-pi32v2-enable-simd=true \
	--plugin-opt=mcpu=r3 \
	--plugin-opt=-global-merge-on-const \
	--plugin-opt=-inline-threshold=5 \
	--plugin-opt=-inline-max-allocated-size=32 \
	--plugin-opt=-inline-normal-into-special-section=true \
	--plugin-opt=-dont-used-symbol-list=malloc,free,sprintf,printf,puts,putchar \
	--plugin-opt=-pi32v2-enable-rep-memop \
	--plugin-opt=-warn-stack-size=256 \
	--sort-common \
	--dont-complain-call-overflow \
	--plugin-opt=-used-symbol-file=$(OUT)/sdk_used_list.used \
	--gc-sections \
	--build-id=sha1 \
	-L$(FW)/lib/smclog \
	--start-group $(LIBS) --end-group \
	-T$(OUT)/sdk.ld \
	-M=$(OUT)/app.map \
	--plugin-opt=mcpu=r3 \
	--plugin-opt=-mattr=+fprev1

SYS_LIBS := $(SYS_LIB_DIR)/libm.a $(SYS_LIB_DIR)/libc.a $(SYS_LIB_DIR)/libm.a $(SYS_LIB_DIR)/libcompiler-rt.a

SDK_OBJS := $(SDK_SRC:%.c=$(OUT)/obj/sdk/%.o)
DERIVED_OBJS := $(DERIVED_SRC:%.c=$(OUT)/obj/fw/%.o)
FW_OBJS := $(FW_SRC:%.c=$(OUT)/obj/fw/%.o)
TUSB_OBJS := $(TUSB_SRC:%.c=$(OUT)/obj/tusb/%.o)
OBJS := $(SDK_OBJS) $(DERIVED_OBJS) $(FW_OBJS) $(TUSB_OBJS)

GEN_FLAGS := $(CFLAGS) -w $(DEFINES) $(FW_INCLUDES) $(SDK_INCLUDES) -D__LD__ -E -P

VERBOSE ?= 0
Q := $(if $(filter 1,$(VERBOSE)),,@)

.PHONY: all clean
.SUFFIXES:

all: $(ELF)

$(OUT)/sdk_used_list.used: $(SDK)/cpu/br25/sdk_used_list.c $(FW)/app/include/app_config.h
	@mkdir -p $(@D)
	$(info +GEN $@)
	$(Q)$(CC) $(GEN_FLAGS) $< -o $@

$(OUT)/sdk.ld: $(SDK)/cpu/br25/sdk_ld.c $(FW)/app/include/app_config.h $(FW)/app/sdk_ld_patch.py
	@mkdir -p $(@D)
	$(info +GEN $@)
	$(Q)$(CC) $(GEN_FLAGS) $< -o $(OUT)/sdk.ld.orig
	$(Q)python3 -I $(FW)/app/sdk_ld_patch.py $(OUT)/sdk.ld.orig $@

$(ELF): $(OBJS) $(OUT)/sdk.ld $(OUT)/sdk_used_list.used $(FW)/lib/smclog/smclog.ld
	$(info +LINK $@)
	$(file >$(OUT)/objs.txt,$(OBJS))
	$(Q)$(LD) -o $@ @$(OUT)/objs.txt $(LFLAGS) $(SYS_LIBS)

$(OUT)/obj/sdk/%.o: $(SDK)/%.c
	@mkdir -p $(@D)
	$(info +CC $<)
	$(Q)$(CC) $(CFLAGS) $(SDK_WARN) $(DEFINES) $(FW_INCLUDES) $(SDK_INCLUDES) -MMD -MP -c $< -o $@

$(DERIVED_OBJS): $(OUT)/obj/fw/%.o: $(FW)/%.c
	@mkdir -p $(@D)
	$(info +CC $<)
	$(Q)$(CC) $(CFLAGS) $(SDK_WARN) $(DEFINES) $(FW_INCLUDES) $(SDK_INCLUDES) -MMD -MP -c $< -o $@

$(FW_OBJS): $(OUT)/obj/fw/%.o: $(FW)/%.c
	@mkdir -p $(@D)
	$(info +CC $<)
	$(Q)$(CC) $(CFLAGS) $(FW_WARN) $(DEFINES) $(FW_INCLUDES) $(SDK_SYSINCLUDES) $(TUSB_INCLUDES) -MMD -MP -c $< -o $@

# TinyUSB needs only our tusb_config.h and the C library: no SDK headers or defines.
$(TUSB_OBJS): $(OUT)/obj/tusb/%.o: $(TUSB)/%.c
	@mkdir -p $(@D)
	$(info +CC $<)
	$(Q)$(CC) $(CFLAGS) $(TUSB_WARN) -I$(FW)/app/usb -I$(TUSB) -isystem $(SYS_INC_DIR) -MMD -MP -c $< -o $@

clean:
	rm -rf $(OUT)/obj $(ELF) $(OUT)/sdk.ld $(OUT)/sdk.ld.orig $(OUT)/sdk_used_list.used

-include $(OBJS:.o=.d)
