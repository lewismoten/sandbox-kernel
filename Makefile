# SPDX-License-Identifier: MIT
TOOLCHAIN ?= /Applications/ArmGNUToolchain/15.3.rel2/arm-none-eabi/bin/arm-none-eabi-
CC := $(TOOLCHAIN)gcc
OBJCOPY := $(TOOLCHAIN)objcopy
SIZE := $(TOOLCHAIN)size
PYTHON ?= python3
BUILD_ID ?= $(shell $(PYTHON) scripts/build_id.py --hash)
TUSB := third_party/tinyusb/src
PANEL_ROTATE_180 ?= 1
CFLAGS := -DPANEL_ROTATE_180=$(PANEL_ROTATE_180) -mcpu=arm1176jzf-s -marm -mno-unaligned-access -mfloat-abi=soft -std=c11 -Os -g -ffreestanding -fno-builtin -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Wno-misleading-indentation -Iinclude -I$(TUSB) -DRASPPI=1 -include build/build_id.h
LDFLAGS := -nostdlib -Wl,--gc-sections -Tarch/armv6/link.ld
COMMON := arch/armv6/start.S drivers/bcm2835.c drivers/sd.c core/util.c core/divide.c core/store.c
USB := $(TUSB)/tusb.c $(TUSB)/common/tusb_fifo.c $(TUSB)/device/usbd.c $(TUSB)/device/usbd_control.c $(TUSB)/class/cdc/cdc_device.c $(TUSB)/portable/synopsys/dwc2/dcd_dwc2.c $(TUSB)/portable/synopsys/dwc2/dwc2_common.c
KERNEL := $(COMMON) drivers/panel.c core/render.c core/logo_data.c drivers/usb.c core/protocol.c core/main.c $(USB)
HEADERS := $(wildcard include/*.h include/broadcom/*.h)
.PHONY: all clean deps boot test FORCE
all: build/kernel.img build/runtime.img build/runtime.ski
build:
	mkdir -p build
third_party/tinyusb/.fetched:
	$(PYTHON) scripts/fetch.py tinyusb
deps: third_party/tinyusb/.fetched
FORCE:
build/build_id.h: FORCE | build
	$(PYTHON) scripts/build_id.py '$(BUILD_ID)'
build/selector.elf: Makefile build/build_id.h $(COMMON) core/selector.c $(HEADERS) arch/armv6/link.ld | build
	$(CC) $(CFLAGS) -DSELECTOR $(COMMON) core/selector.c $(LDFLAGS) -Wl,--defsym=LOAD_ADDRESS=0x8000,--defsym=MEMORY_LIMIT=0x70000,-Map=build/selector.map -o $@
build/runtime.elf: Makefile build/build_id.h $(filter-out $(USB),$(KERNEL)) $(HEADERS) arch/armv6/link.ld third_party/tinyusb/.fetched | build
	$(CC) $(CFLAGS) $(KERNEL) $(LDFLAGS) -Wl,--defsym=LOAD_ADDRESS=0x100000,--defsym=MEMORY_LIMIT=0x08000000,-Map=build/runtime.map -o $@
build/kernel.img: build/selector.elf
	$(OBJCOPY) -O binary $< $@
	$(SIZE) $<
build/runtime.img: build/runtime.elf
	$(OBJCOPY) -O binary $< $@
	$(SIZE) $<
build/runtime.ski: build/runtime.img scripts/image.py
	$(PYTHON) scripts/image.py pack $< $@ --build-id $(BUILD_ID)
boot: all
	$(PYTHON) scripts/assemble_boot.py
test:
	$(PYTHON) -m unittest discover -s tests -v
clean:
	$(PYTHON) scripts/clean.py
