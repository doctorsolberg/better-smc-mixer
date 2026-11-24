# USB key firmware for the RP2040: our code, TinyUSB, the compiler and libgcc. No C library, no
# Pico SDK. Run in the build container by tools/env/build_usbkey.sh.

KEY ?= /src/tools/usbkey
TUSB ?= /src/third_party/tinyusb/src
OUT ?= /src/build/usbkey

CROSS := arm-none-eabi-
CC := $(CROSS)gcc

ELF := $(OUT)/usbkey.elf

ARCH := -mcpu=cortex-m0plus -mthumb
CFLAGS := $(ARCH) -Os -g -ffreestanding -fno-builtin -fno-common \
	-ffunction-sections -fdata-sections -fno-tree-loop-distribute-patterns
OWN_WARN := -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter
# TinyUSB is built as it is; its warnings are shown, not fatal.
TUSB_WARN := -std=gnu11 -Wall -Wno-unused-parameter
INCLUDES := -I$(KEY) -I$(TUSB) -isystem $(KEY)/libc

OWN_SRC := main.c key.c dcd_rp2040.c usb_descriptors.c startup.c
TUSB_SRC := tusb.c common/tusb_fifo.c device/usbd.c class/cdc/cdc_device.c

OBJS := $(OUT)/boot2.o \
	$(addprefix $(OUT)/,$(OWN_SRC:.c=.o)) \
	$(addprefix $(OUT)/tusb/,$(TUSB_SRC:.c=.o))

all: $(OUT)/usbkey.uf2

$(OUT)/boot2.o: $(KEY)/boot2.S
	@mkdir -p $(@D)
	$(CC) $(ARCH) -c -o $@ $<

$(OUT)/%.o: $(KEY)/%.c $(wildcard $(KEY)/*.h)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(OWN_WARN) $(INCLUDES) -c -o $@ $<

$(OUT)/tusb/%.o: $(TUSB)/%.c $(KEY)/tusb_config.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(TUSB_WARN) $(INCLUDES) -c -o $@ $<

$(ELF): $(OBJS) $(KEY)/link.ld
	$(CC) $(ARCH) -nostdlib -T $(KEY)/link.ld -Wl,--gc-sections -Wl,--build-id=sha1 \
		-Wl,-Map,$(OUT)/usbkey.map -o $@ $(OBJS) -lgcc

$(OUT)/usbkey.bin: $(ELF)
	$(CROSS)objcopy -O binary $< $@

$(OUT)/usbkey.uf2: $(OUT)/usbkey.bin $(KEY)/uf2.py
	python3 -I $(KEY)/uf2.py $< $@

clean:
	rm -rf $(OUT)

.PHONY: all clean
