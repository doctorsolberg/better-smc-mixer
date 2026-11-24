# SMC-Mixer DFU bootloader for BR25, bare metal: the JieLi toolchain and TinyUSB only, no SDK
# (SPEC C-4).
#
# Run in the build container by tools/env/build_boot.sh (make -C /src/firmware/boot). Sources come
# from FW and TUSB; everything generated goes to OUT.

FW ?= /src/firmware
TUSB ?= /src/third_party/tinyusb/src
OUT ?= /src/build/boot

TOOL_DIR := /opt/jieli/pi32v2/bin
CC := $(TOOL_DIR)/clang
LD := $(TOOL_DIR)/lto-wrapper
SYS_LIB_DIR := $(TOOL_DIR)/../lib/r3
SYS_INC_DIR := $(TOOL_DIR)/../include

ELF := $(OUT)/boot.elf

# The application's code generation flags (firmware/app/Makefile), without the SDK's defines.
CFLAGS := \
	-target pi32v2 \
	-mcpu=r3 \
	-integrated-as \
	-flto \
	-Oz \
	-g \
	-fno-common \
	-fallow-pointer-null \
	-fprefer-gnu-section \
	-Wframe-larger-than=256

FW_WARN := -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter
# TinyUSB is built as it is; its warnings are shown, not fatal.
TUSB_WARN := -std=gnu11 -Wall -Wno-unused-parameter

INCLUDES := -I$(FW)/boot -I$(FW)/lib/smclog -I$(FW)/platform/br25/usb -I$(TUSB) -isystem $(SYS_INC_DIR)

ASM_SRC := boot/crt0.S

FW_SRC := \
	boot/main.c \
	boot/boot_port.c \
	platform/br25/usb/dcd_br25.c \
	boot/usb_descriptors.c \
	lib/smclog/smclog.c

TUSB_SRC := \
	tusb.c \
	common/tusb_fifo.c \
	device/usbd.c \
	class/dfu/dfu_device.c

# IPRA off: it breaks weak functions (SPEC §3.4), and TinyUSB is full of them.
LFLAGS := \
	--plugin-opt=mcpu=r3 \
	--plugin-opt=-enable-ipra=false \
	--gc-sections \
	--build-id=sha1 \
	-e _start \
	-L$(FW)/lib/smclog \
	-T$(FW)/boot/boot.ld \
	-M=$(OUT)/boot.map

SYS_LIBS := $(SYS_LIB_DIR)/libc.a $(SYS_LIB_DIR)/libcompiler-rt.a

ASM_OBJS := $(ASM_SRC:%.S=$(OUT)/obj/fw/%.o)
FW_OBJS := $(FW_SRC:%.c=$(OUT)/obj/fw/%.o)
TUSB_OBJS := $(TUSB_SRC:%.c=$(OUT)/obj/tusb/%.o)
OBJS := $(ASM_OBJS) $(FW_OBJS) $(TUSB_OBJS)

VERBOSE ?= 0
Q := $(if $(filter 1,$(VERBOSE)),,@)

.PHONY: all clean
.SUFFIXES:

all: $(ELF)

$(ELF): $(OBJS) $(FW)/boot/boot.ld $(FW)/lib/smclog/smclog.ld
	$(info +LINK $@)
	$(Q)$(LD) -o $@ $(OBJS) $(LFLAGS) $(SYS_LIBS)

$(ASM_OBJS): $(OUT)/obj/fw/%.o: $(FW)/%.S
	@mkdir -p $(@D)
	$(info +AS $<)
	$(Q)$(CC) -target pi32v2 -mcpu=r3 -integrated-as -c $< -o $@

$(FW_OBJS): $(OUT)/obj/fw/%.o: $(FW)/%.c
	@mkdir -p $(@D)
	$(info +CC $<)
	$(Q)$(CC) $(CFLAGS) $(FW_WARN) $(INCLUDES) -MMD -MP -c $< -o $@

$(TUSB_OBJS): $(OUT)/obj/tusb/%.o: $(TUSB)/%.c
	@mkdir -p $(@D)
	$(info +CC $<)
	$(Q)$(CC) $(CFLAGS) $(TUSB_WARN) $(INCLUDES) -MMD -MP -c $< -o $@

clean:
	rm -rf $(OUT)/obj $(ELF) $(OUT)/boot.map

-include $(OBJS:.o=.d)
